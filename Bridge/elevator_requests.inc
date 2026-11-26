// The input loop owns these records. The render task sees only the options,
// the published indicators, queue count and elevator command atomics.
static constexpr unsigned max_cabin_requests = 5;
static constexpr uint64_t request_pulse_us = 200000;
static constexpr uint64_t queue_collection_us = 2000000;

struct RequestWait {
    uint64_t started_at = 0;
    bool started = false;

    bool ready(uint64_t now, uint64_t delay) {
        if (!started) {
            started_at = now;
            started = true;
        }
        return now - started_at >= delay;
    }
};

static struct {
    int floors[max_cabin_requests] = {};
    unsigned count = 0;
    int active_floor = 0, arriving_floor = 0;
    bool collecting = false;
    RequestWait wait;
} cabin_requests;

static int hall_request_floor;
static RequestWait hall_request_wait;
static int scheduled_output_floor;
static uint64_t scheduled_output_until;
static bool scheduled_floor_press_seen;
static bool emergency_wait_for_release;
static bool emergency_pulse_pending;
static uint64_t emergency_pulse_until;
static std::atomic<bool> cabin_emergency_latched{false};
static std::atomic<elevator_direction_indicator_mode_t> cabin_interrupted_direction{DIRECTION_INDICATOR_NEUTRAL};
static std::atomic<unsigned> visible_queue_count{0};
static std::atomic<bool> waiting_hall_request{false};
static std::atomic<bool> waiting_cabin_request{false};

static void publish_queue_count() {
    visible_queue_count.store(cabin_requests.count, std::memory_order_release);
    options_dirty.store(true, std::memory_order_release);
}

static void reset_hall_request() {
    hall_request_floor = 0;
    hall_request_wait = {};
    waiting_hall_request.store(false, std::memory_order_release);
}

static void clear_pending_cabin_requests() {
    cabin_requests.count = cabin_requests.active_floor ? 1 : 0;
    cabin_requests.collecting = false;
    cabin_requests.wait = {};
    publish_queue_count();
}

static void clear_cabin_requests() {
    waiting_cabin_request.store(false, std::memory_order_release);
    if (cabin_requests.count || cabin_requests.arriving_floor) {
        cabin_requests = {};
        publish_queue_count();
    }
}

static void stop_cabin_for_emergency(elevator_direction_indicator_mode_t direction, int confirmed_floor) {
    const bool moving = direction == DIRECTION_INDICATOR_UP || direction == DIRECTION_INDICATOR_DOWN;
    const bool needs_recovery = moving || cabin_emergency_latched.load(std::memory_order_acquire);
    if (moving)
        cabin_interrupted_direction.store(direction, std::memory_order_release);
    cabin_emergency_latched.store(needs_recovery, std::memory_order_release);
    emergency_wait_for_release = true;
    emergency_pulse_pending = true;
    elevator_target_level.store(0, std::memory_order_release);
    cabin_arrival_signal.store(0, std::memory_order_release);
    elevator_inside_door_command.store(!needs_recovery && confirmed_floor > 0 ? 3 : -1,
                                      std::memory_order_release);
    exterior_waiting_for_arrival = 0;
    exterior_call_held = 0;
    reset_hall_request();
    clear_cabin_requests();
    scheduled_output_floor = 0;
}

static void reset_elevator_options() {
    elevator_options.sfx = 10;
    elevator_options.music = 10;
    elevator_options.door_seconds = 3;
    elevator_options.capacity = max_cabin_requests;
    elevator_options.muted = false;
    elevator_options.timer_enabled = false;
    elevator_options.queue_enabled = false;
    elevator_options.open = false;
    options_dirty = false;
    options_animation_us = 0;
    options_revealed_bottom = 0;
    options_revision = 0;
    options_pointer_down = options_captured = false;
    options_slider = -1;
    cabin_requests = {};
    visible_queue_count = 0;
    waiting_cabin_request = false;
    reset_hall_request();
    scheduled_floor_press_seen = false;
    scheduled_output_floor = 0;
    scheduled_output_until = 0;
    emergency_wait_for_release = emergency_pulse_pending = false;
    emergency_pulse_until = 0;
    cabin_emergency_latched = false;
    cabin_interrupted_direction = DIRECTION_INDICATOR_NEUTRAL;
}

static bool queue_cabin_floor(int level, bool recovering) {
    const bool stacking = elevator_options.queue_enabled.load();
    const unsigned capacity = stacking ? elevator_options.capacity.load() : 1;
    if (cabin_requests.count >= capacity) return false;
    if (!stacking && (elevator_target_level.load() != 0 || (!recovering && elevator_current_level.load() == level)))
        return false;

    if (!cabin_requests.count && !cabin_requests.arriving_floor) {
        cabin_requests.wait = {};
        cabin_requests.collecting = stacking;
    }
    cabin_requests.floors[cabin_requests.count++] = level;
    publish_queue_count();
    return true;
}

static void pulse_floor_request(int floor, uint64_t now) {
    elevator_target_level.store(floor, std::memory_order_release);
    scheduled_output_floor = floor;
    scheduled_output_until = now + request_pulse_us;
}

static void complete_cabin_request(int floor) {
    for (unsigned i = 1; i < cabin_requests.count; ++i)
        cabin_requests.floors[i - 1] = cabin_requests.floors[i];
    --cabin_requests.count;
    cabin_requests.active_floor = 0;
    cabin_requests.arriving_floor = floor;
    cabin_requests.collecting = false;
    cabin_requests.wait = {};
    publish_queue_count();
}

static void advance_cabin_recovery(uint64_t now, int confirmed_floor) {
    if (!cabin_requests.count || cabin_requests.active_floor || confirmed_floor < 0 ||
        (confirmed_floor == 0 && elevator_last_known_level.load(std::memory_order_acquire) == 0) ||
        elevator_transition_phase.load(std::memory_order_acquire) != TRANSITION_COMPLETE) return;
    const int door = elevator_inside_door_frame.load(std::memory_order_acquire);
    const int floor = cabin_requests.floors[0];
    if (confirmed_floor != floor && door != elevator_inside_door_closed_frame) {
        if (door == elevator_inside_door_open_frame)
            elevator_inside_door_command.store(4, std::memory_order_release);
        return;
    }
    if (confirmed_floor == floor) {
        complete_cabin_request(floor);
        cabin_emergency_latched.store(false, std::memory_order_release);
        cabin_interrupted_direction.store(DIRECTION_INDICATOR_NEUTRAL, std::memory_order_release);
        if (door == elevator_inside_door_closed_frame)
            elevator_inside_door_command.store(3, std::memory_order_release);
    } else {
        cabin_requests.active_floor = floor;
        pulse_floor_request(floor, now);
    }
}

static bool advance_cabin_requests(uint64_t now, int confirmed_floor, uint64_t door_delay) {
    const auto phase = elevator_transition_phase.load(std::memory_order_acquire);
    if (cabin_requests.active_floor) {
        if (confirmed_floor == cabin_requests.active_floor && elevator_target_level.load() == 0)
            complete_cabin_request(confirmed_floor);
        else
            return false;
    }
    if (phase != TRANSITION_COMPLETE || confirmed_floor < 1 || elevator_target_level.load() != 0) return false;

    const int door = elevator_inside_door_frame.load(std::memory_order_acquire);
    if (cabin_requests.arriving_floor) {
        if (confirmed_floor != cabin_requests.arriving_floor || door != elevator_inside_door_open_frame)
            return false;
        cabin_requests.arriving_floor = 0;
    }
    if (!cabin_requests.count) return false;

    const bool filling_batch = cabin_requests.collecting && elevator_options.queue_enabled.load() &&
        cabin_requests.count < elevator_options.capacity.load();
    const uint64_t delay = door_delay ? door_delay : (filling_batch ? queue_collection_us : 0);
    const int floor = cabin_requests.floors[0];
    if (!cabin_requests.wait.ready(now, delay)) return door_delay && floor != confirmed_floor;

    if (floor == confirmed_floor) {
        complete_cabin_request(floor);
        if (door == elevator_inside_door_closed_frame)
            elevator_inside_door_command.store(3, std::memory_order_release);
        return false;
    }
    if (door != elevator_inside_door_closed_frame) {
        if (door == elevator_inside_door_open_frame)
            elevator_inside_door_command.store(4, std::memory_order_release);
        return false;
    }
    cabin_requests.active_floor = floor;
    pulse_floor_request(floor, now);
    return false;
}

void bridge_update_elevator_controls_at(uint64_t now_us, int confirmed_floor) {
    if (scheduled_output_floor &&
        (confirmed_floor == scheduled_output_floor || now_us >= scheduled_output_until))
        scheduled_output_floor = 0;
    if (emergency_pulse_pending) {
        emergency_pulse_until = now_us + request_pulse_us;
        emergency_pulse_pending = false;
    }
    if (elevator_panel_pressed_button.load() == elevator_emergency_button) return;
    if (emergency_pulse_until) {
        if (now_us >= emergency_pulse_until) emergency_pulse_until = 0;
        // Leave one input poll with all output pins low after releasing the stop.
        return;
    }
    if (cabin_emergency_latched.load(std::memory_order_acquire)) {
        advance_cabin_recovery(now_us, confirmed_floor);
        return;
    }
    const uint64_t delay = elevator_options.timer_enabled.load() ?
        elevator_options.door_seconds.load() * 1000000ULL : 0;
    if (elevator_inside_scene.load(std::memory_order_acquire)) {
        waiting_cabin_request.store(advance_cabin_requests(now_us, confirmed_floor, delay),
                                    std::memory_order_release);
        return;
    }
    if (!hall_request_floor) return;
    if (elevator_transition_phase.load() != TRANSITION_LOBBY || hall_request_floor != exterior_lobby_level.load()) {
        reset_hall_request();
        return;
    }
    const int floor = hall_request_floor;
    if ((confirmed_floor != floor && !hall_request_wait.ready(now_us, delay)) ||
        confirmed_floor < 1 || elevator_target_level.load() != 0) return;

    reset_hall_request();
    if (confirmed_floor == floor) {
        elevator_door_open_requested.store(true, std::memory_order_release);
    } else {
        exterior_waiting_for_arrival.store(floor, std::memory_order_release);
        pulse_floor_request(floor, now_us);
    }
}
