#include "engine_bridge.h"
#include <algorithm>
#include <atomic>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

static constexpr uint16_t color565(uint32_t rgb) {
    return ((rgb >> 8) & 0xf800) | ((rgb >> 5) & 0x07e0) | ((rgb >> 3) & 0x001f);
}

#include "options_ui.inc"

struct PackedImage {
    size_t offset;
    int width, height;
};
struct PackedSprite {
    SpriteID id;
    PackedImage image;
};
struct PackedIndexedImage {
    size_t offset, size;
    int width, height;
};
struct PackedActor {
    PackedIndexedImage image;
    int frame_width, frames;
};
enum LobbyActor { CAT_CLOCK, COFFEE_GUEST, SLEEPY_GUEST, BENCH_FRIENDS, EXPLORER, ELECTRIC_COMPANION,
                  BENCH_DETECTIVE, NEWSPAPER_READER, LOBBY_ACTOR_COUNT };
#include "graphics_layout.inc"
static_assert(sizeof(lobby_actors) / sizeof(*lobby_actors) == LOBBY_ACTOR_COUNT, "Lobby actor pack order");
static uint16_t* lobby_actor_pixels[LOBBY_ACTOR_COUNT];
static uint32_t lobby_actor_ticks;

static const uint8_t* variant_pack;
static uint16_t* variant_receptionists[BRIDGE_LOBBY_COUNT - 1];
static int lobby_scene;
static std::atomic<int> exterior_lobby_level(1);

static void release_variants() {
    for (uint16_t*& pixels : variant_receptionists) {
        free(pixels);
        pixels = NULL;
    }
    for (uint16_t*& pixels : lobby_actor_pixels) {
        free(pixels);
        pixels = NULL;
    }
    lobby_actor_ticks = 0;
    variant_pack = NULL;
    lobby_scene = 0;
    exterior_lobby_level = 1;
}

static uint16_t* expand_indexed_sprite(const uint8_t* pack, const PackedIndexedImage& art) {
    const auto* palette = reinterpret_cast<const uint16_t*>(pack + art.offset);
    const uint8_t* input = pack + art.offset + 512;
    const uint8_t* end = pack + art.offset + art.size;
    const size_t pixels = size_t(art.width) * art.height;
    auto* out = static_cast<uint16_t*>(malloc(pixels * sizeof(uint16_t)));
    if (!out) return NULL;
    size_t written = 0;
    while (written < pixels && input < end) {
        const uint8_t code = *input++;
        const size_t length = (code & 0x7f) + 1;
        const bool run = code & 0x80;
        if (length > pixels - written || size_t(end - input) < (run ? 1 : length)) break;
        for (size_t i = 0; i < length; ++i) out[written++] = palette[input[run ? 0 : i]];
        input += run ? 1 : length;
    }
    if (written == pixels && input == end) return out;
    free(out);
    return NULL;
}

static void draw_indexed_lobby(uint16_t* dst, int stride, int x, int y, int w, int h, int scene) {
    const PackedIndexedImage& art = lobby_variants[scene - 1];
    const auto* palette = reinterpret_cast<const uint16_t*>(variant_pack + art.offset);
    const uint8_t* indices = variant_pack + art.offset + 512;
    for (int row = 0; row < h; ++row) {
        const uint8_t* src = indices + (int64_t(row) * art.height / h) * art.width;
        uint16_t* out = dst + (y + row) * size_t(stride) + x;
        if (w == art.width) {
            for (int col = 0; col < w; ++col) out[col] = palette[src[col]];
        } else {
            for (int col = 0; col < w; ++col) out[col] = palette[src[int64_t(col) * art.width / w]];
        }
    }
}

static const uint16_t* intro_row_pixels = NULL;
static const uint16_t* intro_row_indices = NULL;
static bool compact_inside_levels = false;
static int lobby_texture_width = 0;
static int lobby_texture_height = 0;
static const uint16_t* current_pixels = NULL;
static int img_width = 0;
static int img_height = 0;
static const uint16_t* lobby_pixels = NULL;
static int lobby_width = 0;
static int lobby_height = 0;
typedef enum {
    TRANSITION_LOBBY,
    TRANSITION_STAIRS_MENU,
    TRANSITION_ZOOM_OUT,
    TRANSITION_ZOOM_IN,
    TRANSITION_CABIN_ARRIVAL,
    TRANSITION_OPEN_INSIDE_DOOR,
    TRANSITION_CLOSE_INSIDE_DOOR,
    TRANSITION_COMPLETE
} elevator_transition_phase_t;
static std::atomic<elevator_transition_phase_t> elevator_transition_phase(
    TRANSITION_LOBBY);
static std::atomic<bool> elevator_inside_scene(false);
static const uint16_t* receptionist_pixels = NULL;
static int receptionist_width = 0;
static int receptionist_height = 0;
static int receptionist_frame = 0;
static int receptionist_animation_step = 0;
static int animation_ticks = 0;
static const uint16_t* neko_pixels = NULL;
static int neko_width = 0;
static int neko_height = 0;
static int neko_frame = 0;
static int neko_animation_step = 0;
static int neko_animation_ticks = 0;
static const uint16_t* elevator_door_pixels = NULL;
static int elevator_door_width = 0;
static int elevator_door_height = 0;
static int elevator_door_frame = 0;
static int elevator_door_animation_ticks = 0;
static std::atomic<bool> elevator_door_opening(false);
static std::atomic<bool> elevator_door_closing(false);
static std::atomic<bool> elevator_door_ready(false);
static std::atomic<bool> elevator_door_open_requested(false);
static std::atomic<int> elevator_inside_door_command(-1);
static const uint16_t* elevator_inside_pixels = NULL;
static int elevator_inside_width = 0;
static int elevator_inside_height = 0;
static const uint16_t* elevator_inside_door_pixels = NULL;
static int elevator_inside_door_width = 0;
static int elevator_inside_door_height = 0;
static const uint16_t* elevator_panel_pixels = NULL;
static int elevator_panel_width = 0;
static int elevator_panel_height = 0;
static const uint16_t* elevator_buttons_pixels = NULL;
static int elevator_buttons_width = 0;
static int elevator_buttons_height = 0;
static const uint16_t* elevator_lights_pixels = NULL;
static int elevator_lights_width = 0;
static int elevator_lights_height = 0;
static const uint16_t* elevator_direction_indicator_pixels = NULL;
static int elevator_direction_indicator_width = 0;
static int elevator_direction_indicator_height = 0;
static std::atomic<int> elevator_panel_pressed_button(-1);
static bool elevator_door_buttons_wait_for_release = false;
static std::atomic<int> elevator_current_level(0);
static std::atomic<int> elevator_last_known_level(0);
static std::atomic<int> elevator_target_level(0);
static std::atomic<int> cabin_arrival_signal(0);
static bool exterior_calls_enabled = false;
static std::atomic<int> exterior_call_held(0);
static std::atomic<int> exterior_waiting_for_arrival(0);
static std::atomic<bool> elevator_indicator_test_mode(false);
static int elevator_panel_rendered_button = -1;
static std::atomic<int> elevator_inside_door_frame(0);
static int elevator_inside_door_animation_ticks = 0;
typedef enum {
    DIRECTION_INDICATOR_UNINITIALIZED = -1,
    DIRECTION_INDICATOR_NEUTRAL,
    DIRECTION_INDICATOR_UP,
    DIRECTION_INDICATOR_DOWN,
    DIRECTION_INDICATOR_IDLE,
    DIRECTION_INDICATOR_WAITING
} elevator_direction_indicator_mode_t;
static elevator_direction_indicator_mode_t elevator_direction_indicator_mode =
    DIRECTION_INDICATOR_UNINITIALIZED;
static elevator_direction_indicator_mode_t exterior_direction_mode = DIRECTION_INDICATOR_UNINITIALIZED;
static int exterior_direction_ticks = 0;
static constexpr int exterior_scroll_period = 12, exterior_scroll_delay = 4;
static constexpr int direction_wait_blink_ticks = 30;
static int elevator_direction_indicator_frame = 3;
static int elevator_direction_indicator_animation_ticks = 0;
static int elevator_direction_indicator_offset = 0;
static int cabin_shaft_offset = 0;
static constexpr int cabin_shaft_cycle = 256;
static constexpr int cabin_shaft_speed = 8;
static constexpr int cabin_shaft_ascent_sign = -1;
static constexpr int cabin_brake_ticks = 36;
static constexpr int cabin_stop_ticks = 6;
static int cabin_arrival_floor = 0;
static int cabin_arrival_tick = 0;
static int cabin_arrival_shaft_step = 0;
static int cabin_brake_remainder = 0;
static int elevator_direction_test_phase = 0;
static int elevator_direction_test_phase_ticks = 0;
static bool returning_to_lobby = false;
static std::atomic<int> stairs_pressed(-1);
static std::atomic<int> stairs_request(-1);
static std::atomic<bool> stairs_pointer_down(false);
static bool stairs_captured = false;
static int stairs_pointer_button = -1;
static bool stairs_menu_visible = false;
// During a stairs transition, zero selects the menu and 1–3 select a lobby.
static int stairs_transition_destination = -1;
static int stairs_rendered_button = -1;
static int stairs_press_ticks = 0;
static constexpr int stairs_press_duration = 4;
static std::atomic<bool> lobby_input_activity(false);
static void (*button_sound_callback)(void*) = nullptr;
static void* button_sound_context = nullptr;
static void (*hint_sound_callback)(void*) = nullptr;
static void* hint_sound_context = nullptr;
static void (*scene_sound_callback)(void*) = nullptr;
static void* scene_sound_context = nullptr;
static void (*hall_call_sound_callback)(void*) = nullptr;
static void* hall_call_sound_context = nullptr;
static void (*emergency_sound_callback)(void*) = nullptr;
static void* emergency_sound_context = nullptr;
static void (*stairs_back_sound_callback)(void*) = nullptr;
static void* stairs_back_sound_context = nullptr;
static void (*options_open_sound)(void*) = nullptr;
static void (*options_mute_click)(unsigned, void*) = nullptr;
static void* options_sound_context = nullptr;
static void (*slider_step_sound)(bool, void*) = nullptr;
static void* slider_step_context = nullptr;
static void (*cabin_motion_sound_callback)(unsigned, void*) = nullptr;
static void* cabin_motion_sound_context = nullptr;
static unsigned cabin_motion_volume = 0;
static void (*door_sound_callback)(bool, unsigned, void*) = nullptr;
static void* door_sound_context = nullptr;
static int previous_door_motion;
static constexpr int elevator_hint_interval = 8 * 60;
static constexpr int elevator_hint_hop = 36;
static constexpr int elevator_hint_rise = 9;
static int elevator_hint_idle_ticks = 0;
static int elevator_hint_offset = -1;
static int transition_ticks = 0;
static const uint16_t* level_indicator_pixels = NULL;
static int level_indicator_width = 0;
static int level_indicator_height = 0;
static const uint16_t* elevator_inside_level_indicator_pixels = NULL;
static int elevator_inside_level_indicator_width = 0;
static int elevator_inside_level_indicator_height = 0;
static int level_indicator_frame = 0;
static int level_indicator_animation_ticks = 0;
static std::atomic<int> gpio_level_indicator_frame(3);
static std::atomic<bool> gpio_level_indicator_enabled(false);
static const uint16_t* agent_shadow_pixels = NULL;
static int agent_shadow_width = 0;
static int agent_shadow_height = 0;
static int agent_shadow_frame = 0;
static int agent_shadow_animation_ticks = 0;
static bool agent_shadow_animation_active = false;
struct LayerFrames {
    int receptionist, neko, agent, door, level;
    int inside_door, direction, direction_offset;
    int pressed_button, light_level;
    int door_progress, inside_door_progress, neko_progress, lobby;
    int receptionist_progress, agent_progress;
    uint32_t actor_ticks;
    int stairs_button, stairs_depth;
    elevator_direction_indicator_mode_t exterior_direction;
    int exterior_scroll;
    int shaft_offset;
    int elevator_hint_offset;
};

typedef struct {
    uint16_t* framebuffer;
    int width;
    int height;
    uint32_t revision;
    bool normal_scene, inside;
    bool stairs_screen;
    int stairs_zoom;
    uint32_t options_revision;
    unsigned gear_pose;
    bool hud_visible, options_visible, queue_visible;
    int options_height;
    LayerFrames frames;
} rendered_framebuffer_cache_t;

static rendered_framebuffer_cache_t rendered_framebuffers[2] = {};
static uint32_t render_revision = 1;
static uint8_t next_framebuffer_cache = 0;
static bool frame_was_rendered = false;
static bool frame_needs_render = true;
static uint64_t animation_time_us = UINT64_MAX;
static uint32_t animation_remainder = 0;
static constexpr unsigned nominal_frame_us = 16667;
static uint16_t* background_cache = NULL;
static int background_cache_width = 0;
static int background_cache_height = 0;
static bool background_cache_valid = false;
static bool transition_source_valid = false;
static bool packed_layout = false;
static const BridgePanelGraphics* panel_graphics = NULL;
static const uint16_t* active_background = NULL;
static int dirty_top = 0, dirty_bottom = 0;

struct ElevatorOptions {
    std::atomic<unsigned> sfx{10}, music{10}, door_seconds{3}, capacity{5};
    std::atomic<bool> muted{false}, timer_enabled{false}, queue_enabled{false}, open{false};
};
static ElevatorOptions elevator_options;
static std::atomic<bool> options_dirty{false};
static unsigned options_animation_us = 0;
static constexpr unsigned options_animation_duration_us = 700000;
static std::atomic<int> options_revealed_bottom{0};
static uint32_t options_revision = 0;
static bool options_pointer_down, options_captured;
static int options_slider = -1;

#include "cabin_visuals.inc"

struct ShaftSpan {
    int left, right;
};
static constexpr int shaft_poses = 32;
static constexpr int shaft_band_rows = 3;
struct ShaftRow { ShaftSpan spans[3]; };
struct ShaftBand {
    ShaftRow visible;
    size_t offsets[3];
};
struct CabinShaft {
    ShaftRow* rows;
    ShaftBand* bands;
    uint16_t* frames;
    size_t pose_pixels;
    int width, height;
};
static CabinShaft panel_shaft = {}, software_shaft = {};

static void release_cabin_shaft(CabinShaft& shaft) {
    free(shaft.rows);
    free(shaft.bands);
    free(shaft.frames);
    shaft = {};
}


static const int receptionist_frame_columns = 12;
static const int receptionist_frame_rows = 2;
static const int receptionist_animation_cycle_length =
    receptionist_frame_columns * 5;
static const int receptionist_default_frame_delay = 10;
static int receptionist_frame_delay = receptionist_default_frame_delay;
static const int neko_frame_columns = 6;
static const int neko_animation_sequence[] = {0, 1, 2, 3, 4, 5};
static const int neko_animation_sequence_length =
    sizeof(neko_animation_sequence) / sizeof(neko_animation_sequence[0]);
static const int neko_frame_top = 0;
static const int neko_frame_bottom = 362;
static const int elevator_door_frame_columns = 9;
static const int elevator_door_frame_rows = 1;
static const int elevator_door_animation_frame_count = 8;
static const int elevator_inside_door_frame_columns = 11;
static const int elevator_inside_door_open_frame = 5;
static const int elevator_inside_door_closed_frame = 0;
static const int elevator_direction_indicator_frame_count = 4;
static constexpr int cabin_indicator_square_frame = 2;
static constexpr int cabin_indicator_center_off = elevator_direction_indicator_frame_count;
static const int elevator_direction_indicator_frame_delay = 3;
static const int elevator_direction_test_phase_duration = 4 * 60;
static const int level_indicator_frame_count = 4;
static constexpr int level_indicator_err_frame = 3;
static const int neko_default_frame_delay = 8;
static const int elevator_door_default_frame_delay = 11;
static const int elevator_transition_duration_frames = BRIDGE_ZOOM_STEPS;
static const int elevator_transition_zoom_max = 1120;
static const int level_indicator_default_frame_delay = 8;
static const int agent_shadow_frame_columns = 8;
static const int agent_shadow_default_frame_delay = 10;
static const int agent_shadow_idle_duration_render_frames = 150; //velocidad en la que el agente sombra se mantiene inactivo antes de iniciar la animación medido en base a 60fps
static const int agent_shadow_animation_first_frame = 2;
static int neko_frame_delay = neko_default_frame_delay;
static int elevator_door_frame_delay = elevator_door_default_frame_delay;
static int level_indicator_frame_delay = level_indicator_default_frame_delay;
static int agent_shadow_frame_delay = agent_shadow_default_frame_delay;
// Sizes and positions use pixels from the 1455x1081 roomde.png scene.
static const int receptionist_x = 410;
static const int receptionist_y = 317;
static const int receptionist_render_width = 105;
static const int receptionist_render_height = 145;
static const int neko_x = 480;
static const int neko_y = 130;
static const int neko_render_width = 95;
static const int neko_render_height = 125;
// Position and size are fitted to the elevator opening in roomde.png.
static const int elevator_door_x = 822;
static const int elevator_door_y = 150;
static const int elevator_door_render_width = 212;
static const int elevator_door_render_height = 395;
static const int elevator_inside_door_x = 128;
static const int elevator_inside_door_y = 49;
static const int elevator_inside_door_render_width = 37;
static const int elevator_inside_door_render_height = 29;
static const int elevator_inside_level_indicator_x = 139;
static const int elevator_inside_level_indicator_y = 38;
static const int elevator_inside_level_indicator_render_width = 14;
static const int elevator_inside_level_indicator_render_height = 8;
static const int elevator_inside_level_indicator_frame_count = 4;
static const int elevator_inside_level_indicator_frame_x_offsets[] = {
    209, 208, 207, 93
};
static const int elevator_inside_level_indicator_frame_y_offsets[] = {
    280, 280, 280, 280
};
static const int elevator_inside_level_indicator_frame_widths[] = {
    129, 137, 137, 357
};
static const int elevator_inside_level_indicator_frame_heights[] = {
    160, 162, 160, 160
};
static const int elevator_direction_indicator_left_x = 117;
static const int elevator_direction_indicator_right_x = 168;
static const int elevator_direction_indicator_y = 54;
static const int elevator_direction_indicator_display_width = 8;
static const int elevator_direction_indicator_display_height = 8;
static const int elevator_direction_indicator_frame_width = 7;
static const int elevator_direction_indicator_frame_height = 7;
static const int elevator_direction_indicator_frame_stride = 10;
static const int elevator_direction_indicator_sheet_width = 37;
static const int level_indicator_default_render_width = 101;
static const int level_indicator_default_render_height = 93;
static const int level_indicator_center_x = 940;
static const int level_indicator_panel_width = 100;
static const int level_indicator_y = 160;
static int level_indicator_render_width = level_indicator_default_render_width;
static int level_indicator_render_height = level_indicator_default_render_height;
static const int agent_shadow_x = 1200;
static const int agent_shadow_y = 450;
static const int agent_shadow_render_width = 120;
static const int agent_shadow_render_height = 200;
// The source strip shifts each panel right by 5 px per frame.
static const int level_indicator_frame_x_offsets[level_indicator_frame_count] = {0, 5, 10, 15};
static const int level_indicator_frame_y_offsets[level_indicator_frame_count] = {0, 0, 2, 1};
static const uint16_t level_indicator_face_color = 0x2127;
static const uint16_t level_indicator_number_color = 0xFEAF;
static const uint16_t level_indicator_error_color = 0xFC8E;
typedef struct {
    int x;
    int y;
    int width;
    int height;
} elevator_panel_region_t;
static const elevator_panel_region_t elevator_panel_button_regions[] = {
    {286, 688, 307, 226},
    {286, 432, 306, 228},
    {286, 183, 307, 221},
    {183, 950, 283, 241},
    {485, 950, 283, 241},
    {274, 1235, 401, 282}
};
static constexpr int elevator_emergency_button = BRIDGE_ELEVATOR_EMERGENCY_BUTTON;
static const elevator_panel_region_t elevator_panel_light_regions[] = {
    {609, 761, 90, 72},
    {609, 505, 90, 72},
    {609, 255, 90, 71}
};
static const elevator_panel_region_t elevator_panel_button_sprite_bounds[] = {
    {17, 9, 61, 45},
    {17, 9, 61, 45},
    {17, 10, 61, 44},
    {20, 8, 56, 48},
    {20, 8, 56, 48},
    {8, 4, 80, 56}
};
static const int elevator_panel_reference_width = 948;
static const int elevator_panel_reference_height = 1659;
static const int elevator_panel_source_x = 210;
static const int elevator_panel_source_y = 9;
static const int elevator_panel_source_width = 105;
static const int elevator_panel_source_height = 176;

static void calculate_background_layout(
    int screen_width, int screen_height, int image_width, int image_height,
    int* dest_x, int* dest_y, int* dest_width, int* dest_height) {
    *dest_width = screen_width;
    *dest_height = static_cast<int>(
        static_cast<int64_t>(screen_width) * image_height / image_width);
    if (*dest_height > screen_height) {
        *dest_height = screen_height;
        *dest_width = static_cast<int>(
            static_cast<int64_t>(screen_height) * image_width / image_height);
    }

    *dest_x = (screen_width - *dest_width) / 2;
    *dest_y = (screen_height - *dest_height) / 2;
}

static void calculate_background_layout(
    int screen_width, int screen_height,
    int* dest_x, int* dest_y, int* dest_width, int* dest_height) {
    calculate_background_layout(
        screen_width, screen_height, img_width, img_height,
        dest_x, dest_y, dest_width, dest_height);
}

static constexpr BridgeRect stairs_icon_rect = {27, 8, 78, 82};

static BridgeRect stairs_menu_button_rect(int button) {
    if (button == 0) return {72,48,86,86};
    return {316,126 + (BRIDGE_LOBBY_COUNT - button) * 122,392,90};
}

static BridgeRect scale_stairs_rect(BridgeRect room, BridgeRect rect, int reference_width = 807) {
    const auto scale_x = [=](int x) { return room.x + int(int64_t(x) * room.w / reference_width); };
    const auto scale_y = [room](int y) { return room.y + int(int64_t(y) * room.h / 600); };
    const int x = scale_x(rect.x), y = scale_y(rect.y);
    return {x, y, scale_x(rect.x + rect.w) - x, scale_y(rect.y + rect.h) - y};
}

#include "elevator_requests.inc"

void bridge_init(void) {
    bridge_set_button_sound_callback(nullptr, nullptr);
    bridge_set_hint_sound_callback(nullptr, nullptr);
    bridge_set_scene_sound_callback(nullptr, nullptr);
    bridge_set_hall_call_sound_callback(nullptr, nullptr);
    bridge_set_emergency_sound_callback(nullptr, nullptr);
    bridge_set_stairs_back_sound_callback(nullptr, nullptr);
    bridge_set_options_sound_callbacks(nullptr, nullptr, nullptr);
    bridge_set_slider_sound_callback(nullptr, nullptr);
    reset_elevator_options();
    bridge_set_cabin_motion_sound_callback(nullptr, nullptr);
    cabin_motion_volume = 0;
    bridge_set_door_sound_callback(nullptr, nullptr);
    previous_door_motion = 0;
    release_cabin_shaft(panel_shaft);
    release_cabin_shaft(software_shaft);
    release_variants();
    stairs_pressed = -1;
    stairs_request = -1;
    stairs_pointer_down = false;
    lobby_input_activity.store(false, std::memory_order_relaxed);
    elevator_hint_idle_ticks = 0;
    elevator_hint_offset = -1;
    stairs_captured = false;
    stairs_pointer_button = -1;
    stairs_menu_visible = false;
    stairs_transition_destination = -1;
    stairs_rendered_button = -1;
    stairs_press_ticks = 0;
    animation_time_us = UINT64_MAX;
    animation_remainder = 0;
    packed_layout = false;
    panel_graphics = NULL;
    free(background_cache);
    background_cache = NULL;
    background_cache_width = 0;
    background_cache_height = 0;
    background_cache_valid = false;
    intro_row_pixels = NULL;
    intro_row_indices = NULL;
    current_pixels = NULL;
    img_width = 0;
    img_height = 0;
    lobby_pixels = NULL;
    lobby_width = 0;
    lobby_height = 0;
    receptionist_pixels = NULL;
    receptionist_width = 0;
    receptionist_height = 0;
    receptionist_frame = 0;
    receptionist_animation_step = 0;
    animation_ticks = 0;
    receptionist_frame_delay = receptionist_default_frame_delay;
    neko_pixels = NULL;
    neko_width = 0;
    neko_height = 0;
    neko_frame = 0;
    neko_animation_step = 0;
    neko_animation_ticks = 0;
    elevator_door_pixels = NULL;
    elevator_door_width = 0;
    elevator_door_height = 0;
    elevator_door_frame = 0;
    elevator_door_opening = false;
    elevator_door_closing = false;
    elevator_door_animation_ticks = 0;
    elevator_door_ready.store(false, std::memory_order_relaxed);
    elevator_door_open_requested.store(false, std::memory_order_relaxed);
    elevator_inside_door_command.store(-1, std::memory_order_relaxed);
    elevator_inside_pixels = NULL;
    elevator_inside_width = 0;
    elevator_inside_height = 0;
    elevator_inside_door_pixels = NULL;
    elevator_inside_door_width = 0;
    elevator_inside_door_height = 0;
    elevator_panel_pixels = NULL;
    elevator_panel_width = 0;
    elevator_panel_height = 0;
    elevator_buttons_pixels = NULL;
    elevator_buttons_width = 0;
    elevator_buttons_height = 0;
    elevator_lights_pixels = NULL;
    elevator_lights_width = 0;
    elevator_lights_height = 0;
    elevator_direction_indicator_pixels = NULL;
    elevator_direction_indicator_width = 0;
    elevator_direction_indicator_height = 0;
    elevator_panel_pressed_button.store(-1, std::memory_order_relaxed);
    elevator_door_buttons_wait_for_release = false;
    elevator_current_level.store(0, std::memory_order_relaxed);
    elevator_last_known_level.store(0, std::memory_order_relaxed);
    elevator_target_level.store(0, std::memory_order_relaxed);
    exterior_calls_enabled = false;
    exterior_call_held = 0;
    exterior_waiting_for_arrival = 0;
    elevator_indicator_test_mode.store(false, std::memory_order_relaxed);
    elevator_panel_rendered_button = -1;
    elevator_inside_door_frame = 0;
    elevator_inside_door_animation_ticks = 0;
    elevator_direction_indicator_mode = DIRECTION_INDICATOR_UNINITIALIZED;
    exterior_direction_mode = DIRECTION_INDICATOR_UNINITIALIZED;
    exterior_direction_ticks = 0;
    elevator_direction_indicator_frame = 3;
    elevator_direction_indicator_animation_ticks = 0;
    elevator_direction_indicator_offset = 0;
    cabin_shaft_offset = 0;
    cabin_arrival_signal.store(0, std::memory_order_relaxed);
    cabin_arrival_floor = 0;
    cabin_arrival_tick = 0;
    cabin_arrival_shaft_step = 0;
    cabin_brake_remainder = 0;
    elevator_direction_test_phase = 0;
    elevator_direction_test_phase_ticks = 0;
    elevator_inside_scene = false;
    returning_to_lobby = false;
    elevator_transition_phase = TRANSITION_LOBBY;
    transition_ticks = 0;
    transition_source_valid = false;
    level_indicator_pixels = NULL;
    elevator_inside_level_indicator_pixels = NULL;
    elevator_inside_level_indicator_width = 0;
    elevator_inside_level_indicator_height = 0;
    level_indicator_width = 0;
    level_indicator_height = 0;
    level_indicator_frame = 0;
    level_indicator_animation_ticks = 0;
    gpio_level_indicator_frame.store(3, std::memory_order_relaxed);
    gpio_level_indicator_enabled.store(false, std::memory_order_relaxed);
    agent_shadow_pixels = NULL;
    agent_shadow_width = 0;
    agent_shadow_height = 0;
    agent_shadow_frame = 0;
    agent_shadow_animation_ticks = 0;
    agent_shadow_animation_active = false;
    neko_frame_delay = neko_default_frame_delay;
    elevator_door_frame_delay = elevator_door_default_frame_delay;
    level_indicator_frame_delay = level_indicator_default_frame_delay;
    agent_shadow_frame_delay = agent_shadow_default_frame_delay;
    level_indicator_render_width = level_indicator_default_render_width;
    level_indicator_render_height = level_indicator_default_render_height;
    memset(rendered_framebuffers, 0, sizeof(rendered_framebuffers));
    render_revision = 1;
    next_framebuffer_cache = 0;
    frame_was_rendered = false;
    frame_needs_render = true;
}

void bridge_set_receptionist_frame_delay(int render_frames) {
    if (render_frames > 0) {
        receptionist_frame_delay = render_frames;
        animation_ticks = 0;
    }
}

void bridge_set_neko_frame_delay(int render_frames) {
    if (render_frames > 0) {
        neko_frame_delay = render_frames;
        neko_animation_ticks = 0;
    }
}

void bridge_set_elevator_door_frame_delay(int render_frames) {
    if (render_frames > 0) {
        elevator_door_frame_delay = render_frames;
        elevator_door_animation_ticks = 0;
    }
}

void bridge_set_agent_shadow_frame_delay(int render_frames) {
    if (render_frames > 0) {
        agent_shadow_frame_delay = render_frames;
        agent_shadow_animation_ticks = 0;
    }
}

bool bridge_touch_elevator_door(int x, int y, int screen_width, int screen_height) {
    if (x < 0 || y < 0 || screen_width <= 0 || screen_height <= 0 || elevator_options.open.load()) {
        return false;
    }

    int dest_x, dest_y, dest_width, dest_height;

    if (elevator_inside_scene) {
        if (elevator_transition_phase != TRANSITION_COMPLETE ||
            cabin_emergency_latched.load(std::memory_order_acquire) ||
            elevator_target_level.load(std::memory_order_acquire) != 0 ||
            elevator_inside_door_frame != elevator_inside_door_open_frame ||
            elevator_inside_door_pixels == NULL ||
            elevator_inside_door_width <= 0 ||
            elevator_inside_door_height <= 0 ||
            elevator_inside_width <= 0 || elevator_inside_height <= 0 ||
            lobby_pixels == NULL || lobby_width <= 0 || lobby_height <= 0) {
            return false;
        }

        calculate_background_layout(
            screen_width, screen_height, elevator_inside_width,
            elevator_inside_height, &dest_x, &dest_y, &dest_width, &dest_height);
        const int sprite_width = static_cast<int>(
            static_cast<int64_t>(elevator_inside_door_render_width) *
                dest_width / elevator_inside_width);
        const int sprite_height = static_cast<int>(
            static_cast<int64_t>(elevator_inside_door_render_height) *
                dest_height / elevator_inside_height);
        const int sprite_x = dest_x + static_cast<int>(
            static_cast<int64_t>(elevator_inside_door_x) *
                dest_width / elevator_inside_width);
        const int sprite_y = dest_y + static_cast<int>(
            static_cast<int64_t>(elevator_inside_door_y) *
                dest_height / elevator_inside_height);
        if (x < sprite_x || x >= sprite_x + sprite_width ||
            y < sprite_y || y >= sprite_y + sprite_height) {
            return false;
        }

        clear_cabin_requests();
        elevator_inside_door_command.store(5, std::memory_order_release);
        return true;
    }

    if (elevator_transition_phase != TRANSITION_LOBBY ||
        elevator_door_opening || elevator_door_closing ||
        elevator_inside_pixels == NULL || elevator_inside_door_pixels == NULL ||
        !elevator_door_ready.load(std::memory_order_acquire) ||
        lobby_width <= 0 || lobby_height <= 0) {
        return false;
    }

    calculate_background_layout(
        screen_width, screen_height, lobby_width, lobby_height,
        &dest_x, &dest_y, &dest_width, &dest_height);
    const int sprite_width = static_cast<int>(
        static_cast<int64_t>(elevator_door_render_width) * dest_width / lobby_width);
    const int sprite_height = static_cast<int>(
        static_cast<int64_t>(elevator_door_render_height) * dest_height / lobby_height);
    const int sprite_x = dest_x + static_cast<int>(
        static_cast<int64_t>(elevator_door_x) * dest_width / lobby_width);
    const int sprite_y = dest_y + static_cast<int>(
        static_cast<int64_t>(elevator_door_y) * dest_height / lobby_height);

    if (x < sprite_x || x >= sprite_x + sprite_width ||
        y < sprite_y || y >= sprite_y + sprite_height) {
        return false;
    }

    if (exterior_calls_enabled) {
        const int floor = exterior_lobby_level.load(std::memory_order_acquire);
        const int current = elevator_current_level.load(std::memory_order_acquire);
        if (!current) return false;
        const int target = elevator_target_level.load(std::memory_order_acquire);
        if (target && target != floor) return false;
        if (current != floor) {
            if (hall_request_floor) return true;
            if (elevator_options.timer_enabled.load() && !target) {
                hall_request_floor = floor;
                waiting_hall_request.store(true, std::memory_order_release);
                if (hall_call_sound_callback) hall_call_sound_callback(hall_call_sound_context);
                return true;
            }
            elevator_target_level.store(floor, std::memory_order_release);
            const bool new_call = exterior_waiting_for_arrival.exchange(floor, std::memory_order_acq_rel) != floor;
            exterior_call_held.store(floor, std::memory_order_release);
            if (new_call && hall_call_sound_callback) hall_call_sound_callback(hall_call_sound_context);
            return true;
        }
        if (hall_request_floor) reset_hall_request();
    }
    elevator_door_open_requested.store(true, std::memory_order_release);
    return true;
}

static elevator_direction_indicator_mode_t elevator_travel_direction() {
    const int current = elevator_current_level.load(std::memory_order_acquire);
    const int target = elevator_target_level.load(std::memory_order_acquire);
    const int origin = current > 0 ? current : elevator_last_known_level.load(std::memory_order_acquire);
    const bool recovering = cabin_emergency_latched.load(std::memory_order_acquire);
    if (recovering && !target) return DIRECTION_INDICATOR_NEUTRAL;
    if (target > 0 && origin > 0) {
        if (target > origin) return DIRECTION_INDICATOR_UP;
        if (target < origin) return DIRECTION_INDICATOR_DOWN;
        if (recovering && !current) {
            const auto interrupted = cabin_interrupted_direction.load(std::memory_order_acquire);
            if (interrupted == DIRECTION_INDICATOR_UP) return DIRECTION_INDICATOR_DOWN;
            if (interrupted == DIRECTION_INDICATOR_DOWN) return DIRECTION_INDICATOR_UP;
        }
    }
    return current > 0 ? DIRECTION_INDICATOR_IDLE : DIRECTION_INDICATOR_NEUTRAL;
}

static elevator_direction_indicator_mode_t elevator_display_direction() {
    const auto actual = elevator_travel_direction();
    if (actual == DIRECTION_INDICATOR_UP || actual == DIRECTION_INDICATOR_DOWN ||
        !elevator_indicator_test_mode.load(std::memory_order_acquire)) return actual;
    constexpr elevator_direction_indicator_mode_t demo[] = {
        DIRECTION_INDICATOR_UP, DIRECTION_INDICATOR_DOWN, DIRECTION_INDICATOR_IDLE,
    };
    return demo[elevator_direction_test_phase];
}

static bool elevator_is_travelling(elevator_direction_indicator_mode_t direction) {
    return direction == DIRECTION_INDICATOR_UP || direction == DIRECTION_INDICATOR_DOWN;
}

void bridge_set_button_sound_callback(void (*callback)(void*), void* context) {
    button_sound_callback = callback;
    button_sound_context = context;
}

void bridge_set_hint_sound_callback(void (*callback)(void*), void* context) {
    hint_sound_callback = callback;
    hint_sound_context = context;
}

void bridge_set_scene_sound_callback(void (*callback)(void*), void* context) {
    scene_sound_callback = callback;
    scene_sound_context = context;
}

void bridge_set_hall_call_sound_callback(void (*callback)(void*), void* context) {
    hall_call_sound_callback = callback;
    hall_call_sound_context = context;
}

void bridge_set_emergency_sound_callback(void (*callback)(void*), void* context) {
    emergency_sound_callback = callback;
    emergency_sound_context = context;
}

void bridge_set_stairs_back_sound_callback(void (*callback)(void*), void* context) {
    stairs_back_sound_callback = callback;
    stairs_back_sound_context = context;
}

void bridge_set_options_sound_callbacks(void (*open)(void*),
                                       void (*mute_click)(unsigned, void*), void* context) {
    options_open_sound = open;
    options_mute_click = mute_click;
    options_sound_context = context;
}

void bridge_set_slider_sound_callback(void (*step)(bool, void*), void* context) {
    slider_step_sound = step;
    slider_step_context = context;
}

unsigned bridge_get_sfx_volume(void) {
    return elevator_options.muted.load(std::memory_order_acquire) ? 0 : elevator_options.sfx.load();
}

unsigned bridge_get_music_volume(void) {
    return elevator_options.muted.load(std::memory_order_acquire) ? 0 : elevator_options.music.load();
}

void bridge_set_cabin_motion_sound_callback(void (*callback)(unsigned, void*), void* context) {
    cabin_motion_sound_callback = callback;
    cabin_motion_sound_context = context;
    if (callback) callback(cabin_motion_volume, context);
}

void bridge_set_door_sound_callback(void (*callback)(bool, unsigned, void*), void* context) {
    door_sound_callback = callback;
    door_sound_context = context;
}

static void play_button_sound() {
    if (button_sound_callback) button_sound_callback(button_sound_context);
}

static void play_panel_button_sound(int button) {
    if (button == elevator_emergency_button && emergency_sound_callback)
        emergency_sound_callback(emergency_sound_context);
    else
        play_button_sound();
}

static int cabin_panel_button_at(int x, int y, int sw, int sh) {
    if (x < 0 || y < 0 || sw <= 0 || sh <= 0 || !elevator_inside_scene ||
        !elevator_panel_pixels || elevator_panel_width <= 0 || elevator_panel_height <= 0 ||
        elevator_inside_width <= 0 || elevator_inside_height <= 0) return -1;

    BridgeRect cabin;
    calculate_background_layout(sw, sh, elevator_inside_width, elevator_inside_height,
                                &cabin.x, &cabin.y, &cabin.w, &cabin.h);
    const int w = elevator_panel_source_width * cabin.w / elevator_inside_width;
    const int h = elevator_panel_source_height * cabin.h / elevator_inside_height;
    x -= cabin.x + elevator_panel_source_x * cabin.w / elevator_inside_width;
    y -= cabin.y + elevator_panel_source_y * cabin.h / elevator_inside_height;
    if (x < 0 || x >= w || y < 0 || y >= h) return -1;
    const int panel_x = x * elevator_panel_reference_width / w;
    const int panel_y = y * elevator_panel_reference_height / h;
    for (size_t i = 0; i < sizeof(elevator_panel_button_regions) / sizeof(elevator_panel_button_regions[0]); ++i) {
        const auto r = elevator_panel_button_regions[i];
        if (panel_x >= r.x && panel_x < r.x + r.width && panel_y >= r.y && panel_y < r.y + r.height)
            return static_cast<int>(i);
    }
    return -1;
}

bool bridge_touch_elevator_panel(int x, int y, bool pressed, int sw, int sh, int confirmed_floor) {
    if (!pressed) {
        elevator_door_buttons_wait_for_release = false;
        emergency_wait_for_release = false;
        scheduled_floor_press_seen = false;
        elevator_panel_pressed_button.store(-1, std::memory_order_release);
        return false;
    }
    if (emergency_wait_for_release) return true;

    const auto phase = elevator_transition_phase.load(std::memory_order_acquire);
    const bool settling = phase == TRANSITION_CABIN_ARRIVAL;
    const bool can_press = phase == TRANSITION_COMPLETE || settling;
    const bool panel_available = can_press || phase == TRANSITION_OPEN_INSIDE_DOOR ||
        phase == TRANSITION_CLOSE_INSIDE_DOOR;
    const int button = panel_available ? cabin_panel_button_at(x, y, sw, sh) : -1;
    const bool emergency = button == elevator_emergency_button;
    const bool floor_button = button >= 0 && button < BRIDGE_LOBBY_COUNT;
    const bool stacking = elevator_options.queue_enabled.load();
    const auto direction = elevator_travel_direction();
    const bool moving = elevator_is_travelling(direction);
    const bool recovery = cabin_emergency_latched.load(std::memory_order_acquire);
    const bool door_button = button == 3 || button == 4;
    const bool cancel_departure = button == 3 && confirmed_floor > 0 && !moving && !settling && !recovery &&
        (phase == TRANSITION_CLOSE_INSIDE_DOOR || cabin_requests.count > 0) &&
        elevator_inside_door_frame.load(std::memory_order_acquire) > elevator_inside_door_closed_frame;
    if (door_button && (moving || settling || recovery || elevator_door_buttons_wait_for_release))
        elevator_door_buttons_wait_for_release = true;
    if (button < 0 || (elevator_options.open.load() && !emergency) ||
        (door_button && elevator_door_buttons_wait_for_release) ||
        (!can_press && !emergency && !cancel_departure && !(floor_button && (stacking || recovery)))) {
        elevator_panel_pressed_button.store(-1, std::memory_order_release);
        return false;
    }
    if (elevator_panel_pressed_button.load(std::memory_order_acquire) == button) return true;

    if (emergency) {
        stop_cabin_for_emergency(direction, confirmed_floor);
    } else if (floor_button) {
        if (scheduled_floor_press_seen) return true;
        scheduled_floor_press_seen = true;
        if (!queue_cabin_floor(button + 1, recovery)) {
            elevator_panel_pressed_button.store(-1, std::memory_order_release);
            return true;
        }
    } else if (door_button) {
        if (cancel_departure) clear_cabin_requests();
        elevator_inside_door_command.store(button, std::memory_order_release);
    }
    elevator_panel_pressed_button.store(button, std::memory_order_release);
    play_panel_button_sound(button);
    return true;
}

void bridge_note_interaction(void) {
    lobby_input_activity.store(true, std::memory_order_release);
}

static void apply_options_slider(int row, int x) {
    const int segments = row == 3 ? 5 : 10;
    const int minimum = row < 2 ? 0 : 1;
    const int offset = std::max(0, std::min(options_slider_width, x - options_bar_x));
    int value = std::max(minimum, (offset * segments + options_slider_width / 2) / options_slider_width);
    if (row == 3) value = std::max(value, int(cabin_requests.count));
    unsigned previous;
    switch (row) {
        case 0: previous = elevator_options.sfx.exchange(value); break;
        case 1: previous = elevator_options.music.exchange(value); break;
        case 2: previous = elevator_options.door_seconds.exchange(value); break;
        default: previous = elevator_options.capacity.exchange(value); break;
    }
    const bool unmuted = row < 2 && value > 0 && elevator_options.muted.exchange(false);
    if (previous != unsigned(value) || unmuted) options_dirty.store(true, std::memory_order_release);
    if (previous != unsigned(value) && slider_step_sound)
        slider_step_sound(unsigned(value) > previous, slider_step_context);
}

bool bridge_touch_options(int x, int y, bool pressed, int sw, int sh) {
    if (!pressed) {
        const bool handled = options_captured;
        options_pointer_down = options_captured = false;
        options_slider = -1;
        return handled;
    }
    if (!variant_pack || sw <= 0 || sh <= 0) return false;
    const auto phase = elevator_transition_phase.load(std::memory_order_acquire);
    if (phase != TRANSITION_LOBBY || elevator_inside_scene.load(std::memory_order_acquire))
        return false;

    BridgeRect viewport;
    calculate_background_layout(sw, sh, BRIDGE_PANEL_WIDTH, BRIDGE_PANEL_HEIGHT,
        &viewport.x, &viewport.y, &viewport.w, &viewport.h);
    if (!viewport.w || !viewport.h) return false;
    const int px = int(int64_t(x - viewport.x) * BRIDGE_PANEL_WIDTH / viewport.w);
    const int py = int(int64_t(y - viewport.y) * BRIDGE_PANEL_HEIGHT / viewport.h);
    if (options_pointer_down) {
        if (options_slider >= 0) apply_options_slider(options_slider, px);
        return options_captured;
    }
    options_pointer_down = true;
    const bool open = elevator_options.open.load();
    const int visible_bottom = options_revealed_bottom.load(std::memory_order_acquire);
    const bool close_visible = visible_bottom >= options_close.y + options_close.h;
    if (options_contains(gear_tile, px, py) || (open && close_visible && options_contains(options_close, px, py))) {
        options_captured = true;
        elevator_options.open.store(!open);
        play_button_sound();
        if (!open && options_open_sound) options_open_sound(options_sound_context);
        options_dirty.store(true, std::memory_order_release);
        return true;
    }
    if (options_contains(speaker_tile, px, py)) {
        options_captured = true;
        const bool muting = !elevator_options.muted.load();
        const unsigned volume = elevator_options.sfx.load();
        elevator_options.muted.store(muting);
        if (options_mute_click) options_mute_click(volume, options_sound_context);
        else if (!muting) play_button_sound();
        options_dirty.store(true, std::memory_order_release);
        return true;
    }
    if (!open) return false;
    options_captured = true;
    const OptionsLayout layout = {elevator_options.timer_enabled.load(), elevator_options.queue_enabled.load()};
    for (int row = 2; row < 4; ++row) {
        const int y = layout.checkbox_y(row);
        if (visible_bottom < y + 23 ||
            !options_contains({options_toggle_x-10,y-options_check_touch_radius,options_check_width+20,
                               2*options_check_touch_radius},px,py))
            continue;
        if (row == 2) elevator_options.timer_enabled.store(!layout.timer);
        else {
            elevator_options.queue_enabled.store(!layout.fifo);
            if (layout.fifo) clear_pending_cabin_requests();
        }
        play_button_sound();
        options_dirty.store(true, std::memory_order_release);
        return true;
    }
    for (int row = 0; row < 4; ++row) {
        const int y = layout.slider_y(row);
        if (layout.has_slider(row) && visible_bottom >= y + 22 &&
            options_contains({options_bar_x-21,y-options_slider_touch_radius,options_slider_width+42,
                              2*options_slider_touch_radius},px,py)) {
            options_slider = row;
            apply_options_slider(row, px);
            break;
        }
    }
    return true;
}

bool bridge_touch_stairs(int x, int y, bool pressed, int screen_width, int screen_height) {
    if (pressed) bridge_note_interaction();
    const auto phase = elevator_transition_phase.load(std::memory_order_acquire);
    const bool menu = phase == TRANSITION_STAIRS_MENU;
    const bool lobby = phase == TRANSITION_LOBBY && !elevator_door_opening &&
        !elevator_door_closing && !elevator_door_open_requested;
    const bool available = (menu || lobby) && !elevator_options.open.load() && variant_pack &&
        lobby_width > 0 && lobby_height > 0 && screen_width > 0 && screen_height > 0;
    int hit = -1;
    if ((pressed || stairs_captured) && available && x >= 0 && y >= 0) {
        BridgeRect room;
        calculate_background_layout(screen_width, screen_height,
                                    menu ? BRIDGE_PANEL_WIDTH : lobby_width,
                                    menu ? BRIDGE_PANEL_HEIGHT : lobby_height,
                                    &room.x, &room.y, &room.w, &room.h);
        for (int button = 0; button <= (menu ? BRIDGE_LOBBY_COUNT : 0); ++button) {
            const BridgeRect bounds = menu
                ? scale_stairs_rect(room, stairs_menu_button_rect(button), BRIDGE_PANEL_WIDTH)
                : scale_stairs_rect(room, stairs_icon_rect);
            if (x >= bounds.x && x < bounds.x + bounds.w && y >= bounds.y && y < bounds.y + bounds.h) {
                hit = button;
                break;
            }
        }
    }
    if (!pressed) {
        const bool captured = stairs_captured;
        if (captured && hit >= 0 && hit == stairs_pointer_button) {
            if (lobby) reset_hall_request();
            stairs_request.store(hit, std::memory_order_release);
        }
        stairs_pressed = -1;
        stairs_pointer_down = false;
        stairs_captured = false;
        stairs_pointer_button = -1;
        return captured;
    }
    if (!stairs_pointer_down) {
        stairs_pointer_button = hit;
        stairs_captured = available && (hit >= 0 || menu);
        stairs_pointer_down = true;
        if (menu && hit == 0 && stairs_back_sound_callback)
            stairs_back_sound_callback(stairs_back_sound_context);
        else if (hit >= 0)
            play_button_sound();
    }
    stairs_pressed.store(stairs_captured && hit == stairs_pointer_button ? hit : -1,
                         std::memory_order_release);
    return stairs_captured;
}

int bridge_get_pressed_elevator_button(void) {
    return elevator_panel_pressed_button.load(std::memory_order_acquire);
}

void bridge_set_exterior_calls_enabled(bool enabled) {
    exterior_calls_enabled = enabled;
}

void bridge_release_exterior_call(void) {
    exterior_call_held = 0;
}

int bridge_get_elevator_output_button(void) {
    const int cabin_button = bridge_get_pressed_elevator_button();
    if (cabin_button == elevator_emergency_button || emergency_pulse_pending || emergency_pulse_until)
        return elevator_emergency_button;
    if (scheduled_output_floor) return scheduled_output_floor - 1;
    if (cabin_emergency_latched.load(std::memory_order_acquire)) return -1;
    if (cabin_button >= 0) return cabin_button < BRIDGE_LOBBY_COUNT ? -1 : cabin_button;
    const int hall_level = exterior_call_held.load(std::memory_order_acquire);
    return elevator_transition_phase == TRANSITION_LOBBY ? hall_level-1 : -1;
}

bool bridge_set_elevator_level(int level) {
    if (level < 0 || level > 3) {
        return false;
    }
    const bool inside = elevator_inside_scene.load(std::memory_order_acquire);
    const bool recovery = level > 0 && cabin_emergency_latched.load(std::memory_order_acquire) &&
        elevator_target_level.load(std::memory_order_acquire) == level;
    const auto recovery_direction = recovery ? elevator_travel_direction() : DIRECTION_INDICATOR_NEUTRAL;
    bool recovered = false;
    bool reached_target = false;
    const int previous_contact = elevator_current_level.exchange(level, std::memory_order_acq_rel);
    int displayed_level = level;
    if (level > 0) {
        const int previous_level = elevator_last_known_level.exchange(level, std::memory_order_acq_rel);
        if (exterior_call_held.load(std::memory_order_acquire) == level) bridge_release_exterior_call();
        int target = level;
        reached_target = elevator_target_level.compare_exchange_strong(target, 0, std::memory_order_acq_rel);
        if (reached_target && inside) {
            if (recovery) {
                recovered = true;
            } else {
                cabin_arrival_signal.store(previous_level < level ? level : -level, std::memory_order_release);
            }
        }
    } else if (elevator_target_level.load(std::memory_order_acquire) > 0) {
        // A momentary button can be released before the cabin leaves the switch.
        displayed_level = elevator_last_known_level.load(std::memory_order_acquire);
    }
    gpio_level_indicator_frame.store(displayed_level == 0 ? level_indicator_err_frame : displayed_level - 1,
                                     std::memory_order_release);
    gpio_level_indicator_enabled.store(true, std::memory_order_release);
    if (recovered) {
        cabin_emergency_latched.store(false, std::memory_order_release);
        cabin_interrupted_direction.store(DIRECTION_INDICATOR_NEUTRAL, std::memory_order_release);
        cabin_arrival_signal.store(recovery_direction == DIRECTION_INDICATOR_DOWN ? -level : level,
                                   std::memory_order_release);
    }
    return level > 0 && (inside ? reached_target : level != previous_contact);
}

void bridge_set_elevator_indicator_test_mode(bool enabled) {
    elevator_indicator_test_mode.store(enabled, std::memory_order_release);
}

void bridge_set_level_indicator_frame_delay(int render_frames) {
    if (render_frames > 0) {
        level_indicator_frame_delay = render_frames;
        level_indicator_animation_ticks = 0;
    }
}

void bridge_set_level_indicator_size(int width, int height) {
    if (width > 0 && height > 0) {
        level_indicator_render_width = width;
        level_indicator_render_height = height;
        memset(rendered_framebuffers, 0, sizeof(rendered_framebuffers));
        frame_needs_render = true;
    }
}

void bridge_set_sprite(SpriteID id, const uint16_t* pixel_data, int width, int height) {
    packed_layout = false;
    panel_graphics = NULL;
    memset(rendered_framebuffers, 0, sizeof(rendered_framebuffers));
    if (id == SPRITE_BACKGROUND && pixel_data != NULL && width > 0 && height > 0) {
        lobby_pixels = pixel_data;
        lobby_width = width;
        lobby_height = height;
        lobby_texture_width = width;
        lobby_texture_height = height;
        current_pixels = pixel_data;
        img_width = width;
        img_height = height;
        background_cache_valid = false;
        frame_needs_render = true;
    } else if (id == SPRITE_RECEPTIONIST && pixel_data != NULL &&
               width > 0 && height > 0 &&
               width >= receptionist_frame_columns &&
               height >= receptionist_frame_rows) {
        receptionist_pixels = pixel_data;
        receptionist_width = width;
        receptionist_height = height;
        receptionist_frame = 0;
        receptionist_animation_step = 0;
        animation_ticks = 0;
        frame_needs_render = true;
    } else if (id == SPRITE_NEKO && pixel_data != NULL &&
               width >= neko_frame_columns && height > 0) {
        neko_pixels = pixel_data;
        neko_width = width;
        neko_height = height;
        neko_frame = 0;
        neko_animation_step = 0;
        neko_animation_ticks = 0;
        frame_needs_render = true;
    } else if (id == SPRITE_ELEVATOR_DOOR && pixel_data != NULL &&
               width >= elevator_door_frame_columns &&
               height >= elevator_door_frame_rows) {
        elevator_door_pixels = pixel_data;
        elevator_door_width = width;
        elevator_door_height = height;
        elevator_door_frame = 0;
        elevator_door_opening = false;
        elevator_door_closing = false;
        elevator_door_animation_ticks = 0;
        elevator_door_open_requested.store(false, std::memory_order_relaxed);
        elevator_door_ready.store(true, std::memory_order_release);
        frame_needs_render = true;
    } else if (id == SPRITE_LEVEL_INDICATOR && pixel_data != NULL &&
               width > 0 && height > 0 && width % level_indicator_frame_count == 0) {
        level_indicator_pixels = pixel_data;
        level_indicator_width = width;
        level_indicator_height = height;
        level_indicator_frame = 0;
        level_indicator_animation_ticks = 0;
        frame_needs_render = true;
    } else if (id == SPRITE_ELEVATOR_INSIDE_LEVEL_INDICATOR &&
               pixel_data != NULL && width > 0 &&
               width % elevator_inside_level_indicator_frame_count == 0 &&
               height > 0) {
        elevator_inside_level_indicator_pixels = pixel_data;
        compact_inside_levels = false;
        elevator_inside_level_indicator_width = width;
        elevator_inside_level_indicator_height = height;
        frame_needs_render = true;
    } else if (id == SPRITE_AGENT_SHADOW && pixel_data != NULL &&
               width > 0 && height > 0 &&
               width % agent_shadow_frame_columns == 0) {
        agent_shadow_pixels = pixel_data;
        agent_shadow_width = width;
        agent_shadow_height = height;
        agent_shadow_frame = 0;
        agent_shadow_animation_ticks = 0;
        agent_shadow_animation_active = false;
        frame_needs_render = true;
    } else if (id == SPRITE_ELEVATOR_INSIDE_BACKGROUND &&
               pixel_data != NULL && width > 0 && height > 0) {
        elevator_inside_pixels = pixel_data;
        elevator_inside_width = width;
        elevator_inside_height = height;
        frame_needs_render = true;
    } else if (id == SPRITE_ELEVATOR_INSIDE_DOOR &&
               pixel_data != NULL &&
               width >= elevator_inside_door_frame_columns && height > 0) {
        elevator_inside_door_pixels = pixel_data;
        elevator_inside_door_width = width;
        elevator_inside_door_height = height;
        elevator_inside_door_frame = 0;
        elevator_inside_door_animation_ticks = 0;
    } else if (id == SPRITE_ELEVATOR_PANEL &&
               pixel_data != NULL && width > 0 && height > 0) {
        elevator_panel_pixels = pixel_data;
        elevator_panel_width = width;
        elevator_panel_height = height;
        frame_needs_render = true;
    } else if (id == SPRITE_ELEVATOR_BUTTONS && pixel_data != NULL &&
               width > 0 && width % 6 == 0 && height > 0 && height % 3 == 0) {
        elevator_buttons_pixels = pixel_data;
        elevator_buttons_width = width;
        elevator_buttons_height = height;
        frame_needs_render = true;
    } else if (id == SPRITE_ELEVATOR_LIGHTS && pixel_data != NULL &&
               width > 0 && height > 0 && height % 2 == 0) {
        elevator_lights_pixels = pixel_data;
        elevator_lights_width = width;
        elevator_lights_height = height;
        frame_needs_render = true;
    } else if (id == SPRITE_ELEVATOR_DIRECTION_INDICATOR &&
               pixel_data != NULL &&
               width >= elevator_direction_indicator_sheet_width &&
               height >= elevator_direction_indicator_frame_height) {
        elevator_direction_indicator_pixels = pixel_data;
        elevator_direction_indicator_width = width;
        elevator_direction_indicator_height = height;
        elevator_direction_indicator_frame = 3;
        elevator_direction_indicator_animation_ticks = 0;
        frame_needs_render = true;
    }
}

size_t bridge_graphics_pack_size(void) {
    return graphics_bytes;
}

bool bridge_bind_graphics(const void* pack, size_t bytes) {
    if (!pack || reinterpret_cast<uintptr_t>(pack) % alignof(uint16_t) ||
        bytes < graphics_bytes || memcmp(pack, graphics_tag, sizeof(graphics_tag) - 1)) {
        return false;
    }
    const uint8_t* data = static_cast<const uint8_t*>(pack);
    const uint16_t* indices = reinterpret_cast<const uint16_t*>(data + intro_indices.offset);
    for (int i = 0; i < intro_indices.width * intro_indices.height; ++i) {
        if (indices[i] >= intro_rows.height) return false;
    }
    uint16_t* sprites[BRIDGE_LOBBY_COUNT - 1] = {};
    for (int i = 0; i < BRIDGE_LOBBY_COUNT - 1; ++i) {
        sprites[i] = expand_indexed_sprite(data, receptionist_variants[i]);
        if (!sprites[i]) {
            for (uint16_t* ptr : sprites) free(ptr);
            return false;
        }
    }
    uint16_t* actors[LOBBY_ACTOR_COUNT] = {};
    for (int i = 0; i < LOBBY_ACTOR_COUNT; ++i) {
        actors[i] = expand_indexed_sprite(data, lobby_actors[i].image);
        if (!actors[i]) {
            for (uint16_t* ptr : sprites) free(ptr);
            for (uint16_t* ptr : actors) free(ptr);
            return false;
        }
    }
    release_variants();
    variant_pack = data;
    memcpy(variant_receptionists, sprites, sizeof(sprites));
    memcpy(lobby_actor_pixels, actors, sizeof(actors));
    for (const PackedSprite& sprite : packed_sprites) {
        bridge_set_sprite(sprite.id,
            reinterpret_cast<const uint16_t*>(data + sprite.image.offset),
            sprite.image.width, sprite.image.height);
    }
    // All lobby placement and hit testing use the original design coordinates.
    lobby_width = img_width = 1455;
    lobby_height = img_height = 1081;
    packed_layout = true;
    compact_inside_levels = true;
    intro_row_pixels = reinterpret_cast<const uint16_t*>(data + intro_rows.offset);
    intro_row_indices = indices;
    return true;
}

static void draw_transparent_sprite(uint16_t*, int, int, const uint16_t*, int, int,
                                    int, int, int, int, int, int, int, int);

static void draw_cabin_panel_base(uint16_t* dst, int sw, int sh, BridgeRect cabin) {
    draw_transparent_sprite(dst, sw, sh, elevator_panel_pixels, elevator_panel_width, elevator_panel_height,
        0, 0, elevator_panel_width, elevator_panel_height,
        cabin.x + int(int64_t(elevator_panel_source_x) * cabin.w / elevator_inside_width),
        cabin.y + int(int64_t(elevator_panel_source_y) * cabin.h / elevator_inside_height),
        int(int64_t(elevator_panel_source_width) * cabin.w / elevator_inside_width),
        int(int64_t(elevator_panel_source_height) * cabin.h / elevator_inside_height));
}

static_assert(sizeof(shaft_picture) == BRIDGE_PANEL_WIDTH * BRIDGE_PANEL_HEIGHT,
              "Shaft artwork must match the original panel framing");

static uint16_t mix_rgb565(uint16_t a, uint16_t b, unsigned weight);

// The chain guides divide the wall plate into its four planes. Measure
// depth along those guides so adjoining planes use the same sample at a corner.
static constexpr int shaft_radius_units = 1024;
static constexpr int shaft_half_width = BRIDGE_PANEL_WIDTH / 2;
static constexpr int shaft_far_radius = 244;
// At this depth the full cross-section still fits inside the source art.
static constexpr int shaft_near_radius = 838;
static constexpr int shaft_top_origin = 270, shaft_top_range = 326;
static constexpr int shaft_bottom_origin = 342, shaft_bottom_range = 311;

static int shaft_top_y(int radius) {
    return shaft_top_origin - shaft_top_range * radius / shaft_radius_units;
}

static int shaft_bottom_y(int radius) {
    return shaft_bottom_origin + shaft_bottom_range * radius / shaft_radius_units;
}

static int shaft_side_x(int radius) {
    return shaft_half_width - shaft_half_width * radius / shaft_radius_units;
}

template<size_t N>
static int shaft_align_between_guides(int value, const int (&output)[N], const int (&source)[N]) {
    size_t upper = 1;
    while (upper < N - 1 && value >= output[upper]) ++upper;
    const size_t lower = upper - 1;
    const int width = output[upper] - output[lower];
    const int offset = std::max(0, std::min(value - output[lower], width));
    return source[lower] + offset * (source[upper] - source[lower]) / std::max(1, width);
}

struct ShaftWallGuides {
    int side[7];
    int ceiling[4];
    int floor[4];
};

static ShaftWallGuides shaft_wall_guides(int radius) {
    const int x = shaft_side_x(radius);
    const int top = shaft_top_y(radius), bottom = shaft_bottom_y(radius);
    // Place the internal guides on the long beams of the original plate.
    // Interpolating between them keeps each beam in its own strip of artwork.
    return {{top, -3 + x * 241 / 420, 164 + x * 114 / 420, 341 - x * 26 / 420,
             510 - x * 157 / 420, 599 - x * 231 / 420, bottom},
            {x, (top + 175) * 420 / 373, 326 + top * 104 / 198, shaft_half_width},
            {x, (761 - bottom) * 420 / 353, 316 + (600 - bottom) * 102 / 192, shaft_half_width}};
}

static bool prepare_cabin_shaft(CabinShaft& shaft, uint16_t* background, int sw, int sh, BridgeRect cabin) {
    BridgeRect art;
    calculate_background_layout(sw, sh, BRIDGE_PANEL_WIDTH, BRIDGE_PANEL_HEIGHT, &art.x, &art.y, &art.w, &art.h);
    if (art.w <= 0 || art.h <= 0) return true;
    const int bands = sh / shaft_band_rows + (sh % shaft_band_rows != 0);
    bool rebuild = shaft.width != sw || shaft.height != sh || !shaft.frames;
    if (shaft.width != sw || shaft.height != sh) {
        release_cabin_shaft(shaft);
        shaft.rows = static_cast<ShaftRow*>(calloc(sh, sizeof(*shaft.rows)));
        shaft.bands = static_cast<ShaftBand*>(malloc(size_t(bands) * sizeof(*shaft.bands)));
        if (!shaft.rows || !shaft.bands) {
            release_cabin_shaft(shaft);
            return false;
        }
        shaft.width = sw;
        shaft.height = sh;
    }
    const int split = std::min(sw, cabin.x + int(int64_t(elevator_panel_source_x) * cabin.w / elevator_inside_width));
    for (int y = 0; y < sh; ++y) {
        uint16_t* row = background + size_t(y) * sw;
        ShaftRow visible = {};
        if (y >= art.y && y < art.y + art.h) {
            int left = 0, right = split;
            while (left < right && !row[left]) ++left;
            while (right > left && !row[right - 1]) --right;
            int panel_left = split, panel_right = sw;
            while (panel_left < panel_right && !row[panel_left]) ++panel_left;
            while (panel_right > panel_left && !row[panel_right - 1]) --panel_right;
            visible = {{{0, left}, {right, panel_left}, {panel_right, sw}}};
            const int sy = int(int64_t(y - art.y) * BRIDGE_PANEL_HEIGHT / art.h);
            const uint8_t* source = shaft_picture + sy * BRIDGE_PANEL_WIDTH;
            for (ShaftSpan& span : visible.spans) {
                span.left = std::max(span.left, art.x);
                span.right = std::min(span.right, art.x + art.w);
                if (span.left >= span.right) {
                    span = {};
                    continue;
                }
                for (int x = span.left; x < span.right; ++x) {
                    const int sx = art.w == BRIDGE_PANEL_WIDTH ? x - art.x :
                        int(int64_t(x - art.x) * BRIDGE_PANEL_WIDTH / art.w);
                    row[x] = shaft_palette[source[sx]];
                }
            }
        }
        if (memcmp(&shaft.rows[y], &visible, sizeof(visible))) rebuild = true;
        shaft.rows[y] = visible;
    }
    if (!rebuild) return true;

    // Store only the background spans visible around the cabin and keypad.
    // Three output rows share one sample to fit all loop frames in PSRAM.
    size_t pixels = 0;
    for (int y = 0; y < sh; y += shaft_band_rows) {
        ShaftBand& band = shaft.bands[y / shaft_band_rows];
        for (int i = 0; i < 3; ++i) {
            auto run = shaft.rows[y].spans[i];
            for (int other = y + 1; other < std::min(y + shaft_band_rows, sh); ++other) {
                const auto& next = shaft.rows[other].spans[i];
                if (next.left < next.right) {
                    if (run.left == run.right) run = next;
                    else run = {std::min(run.left, next.left), std::max(run.right, next.right)};
                }
            }
            band.visible.spans[i] = run;
            band.offsets[i] = pixels;
            pixels += run.right - run.left;
        }
    }
    if (pixels > SIZE_MAX / (shaft_poses * sizeof(*shaft.frames))) return false;
    free(shaft.frames);
    shaft.frames = nullptr;
    shaft.pose_pixels = pixels;
    if (!pixels) return true;
    shaft.frames = static_cast<uint16_t*>(malloc(shaft_poses * pixels * sizeof(*shaft.frames)));
    if (!shaft.frames) return false;

    constexpr int half_height = BRIDGE_PANEL_HEIGHT / 2;
    constexpr float repeat_depth = 0.75f;
    constexpr float seam_depth = 0.07f;
    struct DepthSample {
        int source_radius;
        unsigned blend, shade;
    };
    struct ShaftColumn {
        int along, depth_radius, top_link, bottom_link, link_radius, link_phase;
        bool right;
        unsigned link_shade;
    };
    auto* depths = static_cast<DepthSample*>(malloc((shaft_radius_units + 1) * sizeof(DepthSample)));
    auto* guides = static_cast<ShaftWallGuides*>(malloc((shaft_radius_units + 1) * sizeof(ShaftWallGuides)));
    auto* columns = static_cast<ShaftColumn*>(malloc(size_t(art.w) * sizeof(ShaftColumn)));
    if (!depths || !guides || !columns) {
        free(depths);
        free(guides);
        free(columns);
        free(shaft.frames);
        shaft.frames = nullptr;
        return false;
    }
    for (int r = 0; r <= shaft_radius_units; ++r) guides[r] = shaft_wall_guides(r);
    constexpr int links_per_repeat = 9;
    for (int x = 0; x < art.w; ++x) {
        const int px = int(int64_t(x) * BRIDGE_PANEL_WIDTH / art.w);
        auto& column = columns[x];
        column = {};
        const int near_edge = 82, far_edge = 420;
        const int along = std::min(px, BRIDGE_PANEL_WIDTH - 1 - px);
        column.along = along;
        column.right = px >= shaft_half_width;
        column.depth_radius = shaft_radius_units * (shaft_half_width - along) / shaft_half_width;
        if (along < near_edge || along > far_edge) continue;
        const float t = float(along - near_edge) / (far_edge - near_edge);
        const float depth = 1.0f / (1.0f - 0.65f * t) - 1.0f;
        column.top_link = int(-4 + 215 * t);
        column.bottom_link = int(603 - 205 * t);
        column.link_radius = int(11 - 8 * t);
        column.link_phase = int(depth * links_per_repeat * shaft_link_width / repeat_depth);
        column.link_shade = unsigned(256 - 92 * t);
    }
    const auto sample = [guides](const ShaftColumn& column, int y, int radius, int source_radius,
                                  bool side, bool upper) {
        const auto& destination = guides[radius];
        const auto& source = guides[source_radius];
        int x;
        if (side) {
            x = source.floor[0];
            y = shaft_align_between_guides(y, destination.side, source.side);
        } else {
            x = upper ? shaft_align_between_guides(column.along, destination.ceiling, source.ceiling)
                      : shaft_align_between_guides(column.along, destination.floor, source.floor);
            y = upper ? source.side[0] : source.side[6];
        }
        x = std::min(shaft_half_width - 1, x);
        if (column.right) x = BRIDGE_PANEL_WIDTH - 1 - x;
        return shaft_palette[shaft_picture[y * BRIDGE_PANEL_WIDTH + x]];
    };
    for (int pose = 0; pose < shaft_poses; ++pose) {
        // Repeat the depth along each ray to keep both directions looping.
        const float offset = repeat_depth * pose / shaft_poses;
        const int link_offset = links_per_repeat * shaft_link_width * pose / shaft_poses;
        for (int r = shaft_far_radius + 1; r <= shaft_radius_units; ++r) {
            const float inverse_radius = float(shaft_radius_units) / r;
            const float z = inverse_radius - 1.0f + offset;
            const float depth = z - int(z / repeat_depth) * repeat_depth;
            const float seam = (depth - (repeat_depth - seam_depth)) / seam_depth;
            const float t = std::max(0.0f, std::min(1.0f, seam));
            depths[r] = {int(shaft_near_radius / (1.0f + depth)),
                         unsigned(t * t * (3.0f - 2.0f * t) * 256),
                         unsigned(std::min(256, (r - shaft_far_radius) * 256 / 340))};
        }
        uint16_t* frame = shaft.frames + pose * pixels;
        for (int y = 0; y < sh; y += shaft_band_rows) {
            const auto& band = shaft.bands[y / shaft_band_rows];
            const int py = std::max(0, std::min(art.h - 1, y + shaft_band_rows / 2 - art.y));
            const int sy = int(int64_t(py) * BRIDGE_PANEL_HEIGHT / art.h);
            const bool upper = sy < half_height;
            const int radius_y = upper ? (shaft_top_origin - sy) * shaft_radius_units / shaft_top_range
                                       : (sy - shaft_bottom_origin) * shaft_radius_units / shaft_bottom_range;
            for (const ShaftSpan& run : band.visible.spans) {
                for (int x = run.left; x < run.right; ++x) {
                    const auto& column = columns[x - art.x];
                    const bool side = column.depth_radius >= radius_y;
                    const int radius = side ? column.depth_radius : radius_y;
                    if (radius <= shaft_far_radius) {
                        *frame++ = 0;
                        continue;
                    }
                    const DepthSample& depth = depths[radius];
                    uint16_t color = sample(column, sy, radius, depth.source_radius, side, upper);
                    if (depth.blend)
                        color = mix_rgb565(color, sample(column, sy, radius, shaft_near_radius, side, upper), depth.blend);
                    if (depth.shade < 256) color = mix_rgb565(0, color, depth.shade);
                    const int across = upper ? sy - column.top_link : column.bottom_link - sy;
                    const int width = column.link_radius;
                    if (across >= -width && across < width) {
                        const int v = (across + width) * shaft_link_height / (2 * width);
                        const int u = (column.link_phase + link_offset) % shaft_link_width;
                        const uint32_t link = shaft_link_pixels[v * shaft_link_width + u];
                        if (link >> 16)
                            color = mix_rgb565(color, mix_rgb565(0, uint16_t(link), column.link_shade), link >> 16);
                    }
                    *frame++ = color;
                }
            }
        }
    }
    free(columns);
    free(guides);
    free(depths);
    return true;
}

static int shaft_pose(int phase) {
    return phase * shaft_poses / cabin_shaft_cycle;
}

static void draw_cabin_shaft(const CabinShaft& shaft, uint16_t* dst, int sw, int sh, int pose) {
    if (!shaft.frames) return;
    const uint16_t* frame = shaft.frames + pose * shaft.pose_pixels;
    for (int y = 0; y < sh; ++y) {
        const auto& band = shaft.bands[y / shaft_band_rows];
        for (int i = 0; i < 3; ++i) {
            const auto& span = shaft.rows[y].spans[i];
            if (span.left == span.right) continue;
            const size_t index = size_t(y) * sw + span.left;
            const size_t bytes = size_t(span.right - span.left) * sizeof(*dst);
            const uint16_t* source = frame + band.offsets[i] + (span.left - band.visible.spans[i].left);
            memcpy(dst + index, source, bytes);
        }
    }
    dirty_top = 0;
    dirty_bottom = sh;
}

static bool prepare_cabin_background(CabinShaft& shaft, uint16_t* scratch, int width, int height) {
    if (!scratch || width <= 0 || height <= 0 || !elevator_inside_pixels) return false;
    BridgeRect cabin;
    calculate_background_layout(width, height, elevator_inside_width, elevator_inside_height,
                                &cabin.x, &cabin.y, &cabin.w, &cabin.h);
    memset(scratch, 0, size_t(width) * height * sizeof(*scratch));
    draw_transparent_sprite(scratch, width, height, elevator_inside_pixels, elevator_inside_width, elevator_inside_height,
                            0, 0, elevator_inside_width, elevator_inside_height, cabin.x, cabin.y, cabin.w, cabin.h);
    draw_cabin_panel_base(scratch, width, height, cabin);
    return prepare_cabin_shaft(shaft, scratch, width, height, cabin);
}

bool bridge_prepare_software_cabin(uint16_t* scratch, int width, int height) {
    return prepare_cabin_background(software_shaft, scratch, width, height);
}

static bool prepare_background_cache(int screen_width, int screen_height) {
    if (background_cache_valid && background_cache_width == screen_width &&
        background_cache_height == screen_height) {
        return true;
    }

    const size_t pixel_count = static_cast<size_t>(screen_width) * screen_height;
    uint16_t* cache = static_cast<uint16_t*>(
        realloc(background_cache, pixel_count * sizeof(*background_cache)));
    if (cache == NULL) {
        return false;
    }

    background_cache = cache;
    if (elevator_inside_scene) {
        if (!bridge_prepare_software_cabin(background_cache, screen_width, screen_height)) return false;
        background_cache_width = screen_width;
        background_cache_height = screen_height;
        background_cache_valid = true;
        return true;
    }
    const size_t cache_size = pixel_count * sizeof(*background_cache);
    memset(background_cache, 0, cache_size);

    int dest_x, dest_y, dest_width, dest_height;
    calculate_background_layout(
        screen_width, screen_height, &dest_x, &dest_y, &dest_width, &dest_height);
    if (lobby_scene > 0) {
        draw_indexed_lobby(background_cache, screen_width, dest_x, dest_y, dest_width, dest_height, lobby_scene);
        background_cache_width = screen_width;
        background_cache_height = screen_height;
        background_cache_valid = true;
        return true;
    }
    const int tex_w = lobby_texture_width;
    const int tex_h = lobby_texture_height;
    for (int y = 0; y < dest_height; ++y) {
        const int source_y = static_cast<int>(
            static_cast<int64_t>(y) * tex_h / dest_height);
        if (tex_w == dest_width) {
            const uint16_t* src = current_pixels + source_y * tex_w;
            uint16_t* dst = background_cache + (dest_y + y) * screen_width + dest_x;
            for (int x = 0; x < dest_width; ++x) dst[x] = src[x] == 0xF81F ? 0 : src[x];
            continue;
        }
        if (y && source_y == (y - 1) * (int64_t)tex_h / dest_height) {
            uint16_t* dst = background_cache + (dest_y + y) * screen_width + dest_x;
            memcpy(dst, dst - screen_width, dest_width * sizeof(*dst));
            continue;
        }
        for (int x = 0; x < dest_width; ++x) {
            const int source_x = static_cast<int>(
                static_cast<int64_t>(x) * tex_w / dest_width);
            const uint16_t color = current_pixels[source_y * tex_w + source_x];
            if (color != 0xF81F) {
                background_cache[(dest_y + y) * screen_width + dest_x + x] = color;
            }
        }
    }

    background_cache_width = screen_width;
    background_cache_height = screen_height;
    background_cache_valid = true;
    return true;
}

// Restore changed regions from the clean scene before drawing their
// transparent layers. Overlapping layers must be redrawn together.
static void restore_region(bool partial, uint16_t* framebuffer, int sw, int sh,
                           int x, int y, int w, int h) {
    if (!partial) return;
    const int left = x < 0 ? 0 : x, top = y < 0 ? 0 : y;
    const int right = x + w > sw ? sw : x + w, bottom = y + h > sh ? sh : y + h;
    if (left >= right || top >= bottom) return;
    if (top < dirty_top) dirty_top = top;
    if (bottom > dirty_bottom) dirty_bottom = bottom;
    for (int row = top; row < bottom; ++row) {
        const size_t start = (size_t)row * sw + left;
        memcpy(framebuffer + start, active_background + start, (right - left) * sizeof(*framebuffer));
    }
}

static BridgeRect frozen_layers[32];
static int frozen_layer_count;
static bool collect_layers;

// Only overlay areas are sampled from a frozen scene. Initialize each touched
// cache-line-wide tile once so overlapping transparent sprites retain their order.
static uint32_t initialized_tiles[(BRIDGE_PANEL_HEIGHT + 7) / 8];
struct DoorPatch { BridgeRect area; uint16_t* pixels; };
static DoorPatch open_doors[BRIDGE_LOBBY_COUNT + 1];
static const DoorPatch* frozen_door;

static void draw_elevator_panel_controls(uint16_t*, int, int, int, int, int, int, const LayerFrames&);

static void record_layer(int x, int y, int w, int h) {
    if (!collect_layers || w <= 0 || h <= 0) return;
    const int sw = BRIDGE_PANEL_WIDTH, sh = BRIDGE_PANEL_HEIGHT;
    const int l = x < 0 ? 0 : x, t = y < 0 ? 0 : y;
    const int r = x + w > sw ? sw : x + w, b = y + h > sh ? sh : y + h;
    if (l >= r || t >= b) return;
    for (int ty = t / 8; ty <= (b - 1) / 8; ++ty) {
        for (int tx = l / 32; tx <= (r - 1) / 32; ++tx) {
            const uint32_t bit = uint32_t(1) << tx;
            if (initialized_tiles[ty] & bit) continue;
            initialized_tiles[ty] |= bit;
            for (int row = ty * 8; row < ty * 8 + 8 && row < sh; ++row) {
                int offset = row * sw + tx * 32;
                uint16_t* dst = panel_graphics->snapshot + offset;
                memcpy(dst, active_background + offset, 32 * sizeof(*dst));
                if (!frozen_door) continue;
                const BridgeRect& door = frozen_door->area;
                if (row >= door.y && row < door.y + door.h) {
                    const int from = door.x > tx * 32 ? door.x : tx * 32;
                    const int to = door.x + door.w < tx * 32 + 32 ? door.x + door.w : tx * 32 + 32;
                    if (to > from) memcpy(dst + from - tx * 32,
                        frozen_door->pixels + (row - door.y) * door.w + from - door.x, (to-from) * sizeof(*dst));
                }
            }
        }
    }
    for (int i = 0; i < frozen_layer_count; ++i) {
        const BridgeRect& r = frozen_layers[i];
        if (x >= r.x && y >= r.y && x + w <= r.x + r.w && y + h <= r.y + r.h) return;
    }
    if (frozen_layer_count == 32) {
        frozen_layers[0] = {0, 0, BRIDGE_PANEL_WIDTH, BRIDGE_PANEL_HEIGHT};
        frozen_layer_count = 1;
    } else {
        frozen_layers[frozen_layer_count++] = {x, y, w, h};
    }
}

#include "stairs_sprites.inc"

static uint16_t mix_rgb565(uint16_t a, uint16_t b, unsigned weight);

static void draw_stairs_icon(uint16_t* dst, int sw, int sh, BridgeRect room, int depth, bool partial) {
    const BridgeRect r = stairs_icon_rect;
    const BridgeRect icon = scale_stairs_rect(room, r);
    restore_region(partial, dst, sw, sh, icon.x, icon.y, icon.w, icon.h);
    record_layer(icon.x, icon.y, icon.w, icon.h);
    const bool native = room.w == 807 && room.h == BRIDGE_PANEL_HEIGHT;
    const auto paint = [=](BridgeRect art, uint16_t color, unsigned tint = 0) {
        BridgeRect r = art;
        if (native) {
            r.x += room.x;
            r.y += room.y;
        } else {
            r = scale_stairs_rect(room, art);
        }
        const int left = r.x < 0 ? 0 : r.x, right = r.x + r.w > sw ? sw : r.x + r.w;
        const int top = r.y < 0 ? 0 : r.y, bottom = r.y + r.h > sh ? sh : r.y + r.h;
        for (int y = top; y < bottom; ++y) {
            for (int x = left; x < right; ++x) {
                const int offset = y * sw + x;
                dst[offset] = tint ? mix_rgb565(active_background[offset], color, tint) : color;
            }
        }
    };
    const int shift = depth * 5 / stairs_press_duration;
    const int sprite_x = r.x + (r.w - stair_sprite_width) / 2;
    const auto draw_sprite = [&](bool cast_shadow) {
        const int x = sprite_x + (cast_shadow ? (5 - shift) * 4 / 5 : 0);
        const int y = r.y + 2 + (cast_shadow ? 5 : shift);
        for (int row = 0; row < stair_sprite_height; ++row) {
            const char* pixels = stair_step_sprite[row];
            for (int first = 0; first < stair_sprite_width;) {
                const char texel = pixels[first];
                int end = first + 1;
                if (cast_shadow) {
                    while (end < stair_sprite_width && (pixels[end] == '0') == (texel == '0')) ++end;
                    if (texel != '0') paint({x + first, y + row, end - first, 1}, 0x2108, 100);
                } else {
                    while (end < stair_sprite_width && pixels[end] == texel) ++end;
                    if (texel != '0') {
                        const int index = texel <= '9' ? texel - '0' : texel - 'a' + 10;
                        paint({x+first,y+row,end-first,1}, stair_wood_palette[index]);
                    }
                }
                first = end;
            }
        }
    };
    if (shift < 5) draw_sprite(true);
    draw_sprite(false);
}

static constexpr int hint_width = 19, hint_height = 19, hint_pixel_size = 3;
// Aim the tip at the door seam. The bounce follows the same diagonal.
static constexpr int hint_seam_x = 516, hint_seam_y = 262, hint_y = 312;
static constexpr int hint_tip_x = (hint_width - 1) * hint_pixel_size + hint_pixel_size / 2;
static constexpr int hint_tip_y = hint_pixel_size / 2;
static constexpr int hint_x = hint_seam_x - (hint_y + hint_tip_y - hint_seam_y) - hint_tip_x;

static BridgeRect elevator_hint_bounds(BridgeRect room) {
    return scale_stairs_rect(room, {hint_x, hint_y - elevator_hint_rise,
        hint_width * hint_pixel_size + elevator_hint_rise,
        hint_height * hint_pixel_size + elevator_hint_rise});
}

static void draw_elevator_hint(uint16_t* dst, int sw, int sh, BridgeRect room, int offset) {
    // One cell per pixel of the reference arrow, with the lobby floor keyed out.
    static constexpr char sprite[hint_height][hint_width + 1] = {
        ".......############",
        "......#sgggggggggg#",
        ".....#sssggggggggg#",
        ".....#ssssgggggggg#",
        "......#sssgggggggg#",
        ".......#ssgggggghg#",
        ".......#sggggggghg#",
        "......#ggggggggghg#",
        ".....#ggggggghhhgg#",
        "....#ggggggghssshg#",
        "...#ggggggghsssssh#",
        "..#gggggggh###sss#.",
        ".#ddgggggh#..#ss#..",
        "#ddddgggh#....##...",
        "#dddddgg#..........",
        ".#ddddd#...........",
        "..#ddd#............",
        "...#d#.............",
        "....#..............",
    };
    for (int row = 0; row < hint_height; ++row) {
        for (int first = 0; first < hint_width;) {
            const char texel = sprite[row][first];
            int end = first + 1;
            while (end < hint_width && sprite[row][end] == texel) ++end;
            if (texel != '.') {
                const uint16_t color = texel == '#' ? color565(0x10344a) :
                    texel == 'd' ? color565(0x39aaa4) : texel == 's' ? color565(0x206d7b) :
                    texel == 'h' ? color565(0xa4ffde) : color565(0x52deac);
                const auto run = scale_stairs_rect(room, {hint_x + offset + first * hint_pixel_size,
                    hint_y - offset + row * hint_pixel_size, (end - first) * hint_pixel_size, hint_pixel_size});
                const int left = std::max(0, run.x), right = std::min(sw, run.x + run.w);
                if (left < right) {
                    for (int line = std::max(0, run.y); line < std::min(sh, run.y + run.h); ++line)
                        std::fill(dst + size_t(line) * sw + left, dst + size_t(line) * sw + right, color);
                }
            }
            first = end;
        }
    }
}

static void draw_stairs_menu(uint16_t* dst, int sw, int sh, int zoom, int brightness, int previous_zoom) {
    BridgeRect screen;
    calculate_background_layout(sw, sh, BRIDGE_PANEL_WIDTH, BRIDGE_PANEL_HEIGHT,
                                &screen.x, &screen.y, &screen.w, &screen.h);
    const auto paint_at_zoom = [&](BridgeRect art, uint16_t pixel, int factor) {
        const BridgeRect r = scale_stairs_rect(screen, art, BRIDGE_PANEL_WIDTH);
        const auto scale = [factor](int value, int center) {
            return center + int(int64_t(value-center) * factor / 1000);
        };
        int left = scale(r.x, sw/2), right = scale(r.x+r.w, sw/2);
        int top = scale(r.y, sh/2), bottom = scale(r.y+r.h, sh/2);
        if (left < 0) left = 0;
        if (right > sw) right = sw;
        if (top < 0) top = 0;
        if (bottom > sh) bottom = sh;
        if (left >= right || top >= bottom) return;
        if (top < dirty_top) dirty_top = top;
        if (bottom > dirty_bottom) dirty_bottom = bottom;
        for (int y = top; y < bottom; ++y) {
            uint16_t* line = dst + size_t(y) * sw;
            for (int x = left; x < right; ++x) line[x] = pixel;
        }
    };
    const BridgeRect current = stairs_menu_button_rect(lobby_scene + 1);
    const BridgeRect marker = {current.x-64,current.y+18,44,44};
    if (previous_zoom) {
        for (int button = 0; button <= BRIDGE_LOBBY_COUNT; ++button)
            paint_at_zoom(stairs_menu_button_rect(button), 0, previous_zoom);
        paint_at_zoom(marker, 0, previous_zoom);
    }
    if (!brightness) return;
    const auto paint = [&](BridgeRect art, uint16_t color) {
        paint_at_zoom(art, mix_rgb565(0, color, brightness * 256 / 255), zoom);
    };
    constexpr uint16_t arrow_colors[] = {
        0, color565(0x52280e), color565(0x94520f), color565(0xb37515),
        color565(0xe5a827), color565(0xffd65a), color565(0xfff2b5),
    };
    const BridgeRect back = stairs_menu_button_rect(0);
    constexpr int arrow_scale = 3;
    const int arrow_x = back.x + (back.w - stair_return_width * arrow_scale) / 2;
    const int arrow_top = back.y + (back.h - stair_return_height * arrow_scale - stairs_press_duration) / 2;
    const int arrow_y = arrow_top + (stairs_rendered_button == 0 ? stairs_press_ticks : 0);
    for (int row = 0; row < stair_return_height; ++row)
        for (int col = 0; col < stair_return_width; ++col)
            if (stair_return_sprite[row][col] != '0')
                paint({arrow_x+col*arrow_scale,arrow_y+row*arrow_scale,arrow_scale,arrow_scale},
                      arrow_colors[stair_return_sprite[row][col]-'0']);

    struct Palette { uint16_t border, face, bevel, letters; };
    constexpr Palette palettes[] = {
        {color565(0xbf473e), color565(0xffe48f), color565(0xf39869), color565(0x652734)},
        {color565(0x7360ce), color565(0xbfcfff), color565(0x9584e4), color565(0x33286e)},
        {color565(0x008080), color565(0xacffdb), color565(0x50c878), color565(0x00484c)},
    };
    constexpr uint16_t shine = color565(0xfffae4);
    constexpr uint8_t letter_l[7] = {16,16,16,16,16,16,31};
    constexpr uint8_t letter_e[7] = {31,16,16,30,16,16,31};
    constexpr uint8_t letter_v[7] = {17,17,17,17,17,10,4};
    const auto glyph = [&](int x, int y, int unit, const uint8_t* rows, uint16_t color) {
        for (int row = 0; row < 7; ++row)
            for (int col = 0; col < 5; ++col)
                if (rows[row] & (16 >> col)) paint({x+col*unit,y+row*unit,unit,unit}, color);
    };
    for (int button = 1; button <= BRIDGE_LOBBY_COUNT; ++button) {
        const BridgeRect r = stairs_menu_button_rect(button);
        const Palette p = palettes[button-1];
        const int shift = button == stairs_rendered_button ? stairs_press_ticks * 6 / stairs_press_duration : 0;
        const int y = r.y + shift, bottom = r.y + r.h;
        const int face_bottom = y + r.h - 14;
        // Separate bands avoid repainting the face beneath each bevel.
        paint({r.x+10,face_bottom,r.w-20,bottom-face_bottom}, p.letters);
        paint({r.x+5,face_bottom-6,r.w-10,6}, p.letters);
        paint({r.x+12,y,r.w-24,6}, p.border);
        paint({r.x+6,y+6,6,6}, p.border);
        paint({r.x+r.w-12,y+6,6,6}, p.border);
        paint({r.x,y+12,6,face_bottom-y-24}, p.border);
        paint({r.x+r.w-6,y+12,6,face_bottom-y-24}, p.border);
        paint({r.x+12,y+6,r.w-24,6}, p.face);
        paint({r.x+6,y+12,r.w-12,face_bottom-y-24}, p.face);
        paint({r.x+12,face_bottom-12,r.w-24,6}, p.bevel);
        paint({r.x+15,y+6,38,5}, shine);
        paint({r.x+60,y+6,6,5}, shine);
        const uint8_t* letters[] = {letter_l,letter_e,letter_v,letter_e,letter_l};
        const int label_x = r.x+(r.w-246)/2;
        for (int i = 0; i < 5; ++i) glyph(label_x+i*36, y+18, 6, letters[i], p.letters);
        glyph(label_x+216, y+18, 6, stair_floor_sprites[button-1], p.letters);
    }
    constexpr char light[11][12] = {
        "00011111000",
        "00144444100",
        "01554443310",
        "14554433321",
        "14444333321",
        "14443333321",
        "14333333221",
        "13333332221",
        "01333222210",
        "00122222100",
        "00011111000",
    };
    constexpr uint16_t light_colors[] = {
        0, color565(0x8d560e), color565(0xe4a123), color565(0xffd445),
        color565(0xffe982), color565(0xfff9d2),
    };
    for (int y = 0; y < 11; ++y)
        for (int x = 0; x < 11; ++x)
            if (light[y][x] != '0')
                paint({marker.x+x*4,marker.y+y*4,4,4}, light_colors[light[y][x]-'0']);
}

static void draw_transparent_sprite(
    uint16_t* framebuffer, int screen_width, int screen_height,
    const uint16_t* pixels, int image_width, int image_height,
    int source_left, int source_top, int source_width, int source_height,
    int dest_x, int dest_y, int dest_width, int dest_height) {
    if (pixels == NULL || image_width <= 0 || image_height <= 0 ||
        source_left < 0 || source_top < 0 ||
        source_width <= 0 || source_height <= 0 ||
        source_left + source_width > image_width ||
        source_top + source_height > image_height ||
        dest_width <= 0 || dest_height <= 0) {
        return;
    }

    record_layer(dest_x, dest_y, dest_width, dest_height);
    const int first_x = dest_x < 0 ? -dest_x : 0;
    const int last_x = dest_x + dest_width > screen_width
        ? screen_width - dest_x : dest_width;
    const int first_y = dest_y < 0 ? -dest_y : 0;
    const int last_y = dest_y + dest_height > screen_height
        ? screen_height - dest_y : dest_height;
    if (first_x >= last_x || first_y >= last_y) {
        return;
    }

    if (source_width == dest_width && source_height == dest_height) {
        for (int y = first_y; y < last_y; ++y) {
            const uint16_t* src = pixels + (source_top + y) * image_width + source_left;
            uint16_t* dst = framebuffer + (dest_y + y) * screen_width + (dest_x + first_x);
            for (int x = first_x; x < last_x; ++x) {
                const uint16_t color = src[x];
                if (color != 0xF81F) dst[x - first_x] = color;
            }
        }
        return;
    }

    const int source_x_step = source_width / dest_width;
    const int source_x_remainder_step = source_width % dest_width;
    const int64_t first_source_x_numerator =
        static_cast<int64_t>(first_x) * source_width;
    const int first_source_x =
        source_left + static_cast<int>(first_source_x_numerator / dest_width);
    const int first_source_x_remainder =
        static_cast<int>(first_source_x_numerator % dest_width);
    const int source_y_step = source_height / dest_height;
    const int source_y_remainder_step = source_height % dest_height;
    const int64_t first_source_y_numerator =
        static_cast<int64_t>(first_y) * source_height;
    int source_row = source_top +
        static_cast<int>(first_source_y_numerator / dest_height);
    int source_y_remainder =
        static_cast<int>(first_source_y_numerator % dest_height);

    for (int y = first_y; y < last_y; ++y) {
        const int dest_row = dest_y + y;
        int source_col = first_source_x;
        int source_x_remainder = first_source_x_remainder;
        for (int x = first_x; x < last_x; ++x) {
            const int dest_col = dest_x + x;
            const uint16_t color = pixels[
                source_row * image_width + source_col];
            if (color != 0xF81F) {
                framebuffer[dest_row * screen_width + dest_col] = color;
            }
            source_col += source_x_step;
            source_x_remainder += source_x_remainder_step;
            if (source_x_remainder >= dest_width) {
                ++source_col;
                source_x_remainder -= dest_width;
            }
        }

        source_row += source_y_step;
        source_y_remainder += source_y_remainder_step;
        if (source_y_remainder >= dest_height) {
            ++source_row;
            source_y_remainder -= dest_height;
        }
    }
}

static constexpr int neko_cell_width = 52;
static constexpr int neko_cell_height = 69;
static constexpr int neko_paw_top[] = {15, 18, 25, 34, 25, 18};

static bool interpolate_neko() {
    return packed_layout && neko_width == neko_frame_columns * neko_cell_width &&
        neko_height == neko_cell_height;
}

static int paw_velocity(int before, int after) {
    return before * after > 0 ? 2 * before * after * 256 / (before + after) : 0;
}

static uint16_t mix_rgb565(uint16_t a, uint16_t b, unsigned weight) {
    const unsigned inverse = 256 - weight;
    const unsigned red = ((a >> 11) * inverse + (b >> 11) * weight + 128) >> 8;
    const unsigned green = (((a >> 5) & 63) * inverse + ((b >> 5) & 63) * weight + 128) >> 8;
    const unsigned blue = ((a & 31) * inverse + (b & 31) * weight + 128) >> 8;
    return (red << 11) | (green << 5) | blue;
}

// The uniform atlases share the same registered poses. These are the
// changing pairs in the talking and blinking portions of the authored cycle.
static int receptionist_blend_ticks() {
    return packed_layout && (receptionist_frame < 10 ||
           (receptionist_frame >= 15 && receptionist_frame <= 18)) ? animation_ticks : 0;
}

struct PaperTip { int x, y; };
struct PaperFold { int left, right, top, rise, fall, bottom; };
static constexpr PaperFold agent_fold = {7,63,43,48,68,88};
static constexpr PaperFold detective_fold = {2,37,21,26,43,54};
static constexpr PaperFold reader_fold = {2,57,49,53,67,79};
static constexpr PaperTip detective_page_tips[] = {{7,33}, {11,29}, {16,26}, {6,33}, {7,34}};
static constexpr PaperTip reader_page_tips[] = {{7,55}, {8,55}, {10,52}, {5,55}, {5,57}};
static constexpr PaperTip paper_tips[] = {
    {54, 55}, {54, 55}, {38, 51}, {34, 47}, {35, 49}, {13, 47}, {54, 56}, {54, 55}
};

static unsigned pose_weight(int tick, int duration) {
    const unsigned t = static_cast<unsigned>(int64_t(tick) * 256 / duration);
    return t * t * (768 - 2*t) >> 16;
}

static bool paper_pixel(uint16_t color) {
    return color != 0xF81F && (color >> 11) >= 13 &&
        ((color >> 5) & 63) >= 25 && (color & 31) >= 12;
}

// For the newspaper, align the page fold before blending the two poses. The
// warp fades into the shoulders, fixed left page and lap to keep them steady.
static void draw_inbetween(uint16_t* dst, int sw, int sh, BridgeRect r,
                           const uint16_t* from, const uint16_t* to, int stride,
                           unsigned weight, const PaperTip* tips = NULL,
                           const PaperFold& fold = agent_fold) {
    const int x0 = r.x < 0 ? -r.x : 0, x1 = r.x + r.w > sw ? sw - r.x : r.w;
    const int y0 = r.y < 0 ? -r.y : 0, y1 = r.y + r.h > sh ? sh - r.y : r.h;
    if (x0 >= x1 || y0 >= y1) return;
    record_layer(r.x, r.y, r.w, r.h);

    int x_weight[66] = {};
    int shift_x[2] = {}, shift_y[2] = {};
    if (tips) {
        const int dx = tips[1].x - tips[0].x, dy = tips[1].y - tips[0].y;
        const int pivot = tips[0].x + dx * int(weight) / 256;
        for (int x = fold.left + 1; x < fold.right; ++x)
            x_weight[x] = x <= pivot ? (x-fold.left)*256/(pivot-fold.left) :
                                      (fold.right-x)*256/(fold.right-pivot);
        const int travel_x = dx < -24 ? -24 : (dx > 24 ? 24 : dx);
        shift_x[0] = -travel_x * int(weight);
        shift_x[1] = travel_x * int(256 - weight);
        shift_y[0] = -dy * int(weight);
        shift_y[1] = dy * int(256 - weight);
    }
    for (int y = y0; y < y1; ++y) {
        uint16_t* out = dst + (r.y + y) * sw + (r.x + x0);
        unsigned row_weight = 0;
        if (tips && y > fold.top && y < fold.bottom) {
            if (y < fold.rise) row_weight = (y-fold.top)*256/(fold.rise-fold.top);
            else if (y > fold.fall) row_weight = (fold.bottom-y)*256/(fold.bottom-fold.fall);
            else row_weight = 256;
        }
        for (int x = x0; x < x1; ++x) {
            uint16_t a, b;
            if (tips && row_weight && x_weight[x]) {
                const int influence = (row_weight * x_weight[x]) >> 8;
                const int ax = x + ((shift_x[0] * influence + 32768) >> 16);
                const int ay = y + ((shift_y[0] * influence + 32768) >> 16);
                const int bx = x + ((shift_x[1] * influence + 32768) >> 16);
                const int by = y + ((shift_y[1] * influence + 32768) >> 16);
                a = (ax < 0 || ax >= r.w || ay < 0 || ay >= r.h) ? 0xF81F : from[ay * stride + ax];
                b = (bx < 0 || bx >= r.w || by < 0 || by >= r.h) ? 0xF81F : to[by * stride + bx];
            } else {
                a = from[y * stride + x];
                b = to[y * stride + x];
            }
            if (row_weight && a != b &&
                (a == 0xF81F || b == 0xF81F || paper_pixel(a) != paper_pixel(b))) {
                // A sheet crossing the glove is an occlusion. Keep its edge
                // opaque instead of dissolving the page through the hand.
                a = b = weight < 128 ? a : b;
            }
            if (a == b) {
                if (a != 0xF81F) out[x-x0] = a;
            } else {
                if (a == 0xF81F) a = out[x-x0];
                if (b == 0xF81F) b = out[x-x0];
                out[x-x0] = mix_rgb565(a, b, weight);
            }
        }
    }
}

struct ActorPose { int from, to; unsigned weight; };
struct ActorKey { int tick, frame; };
struct SpritePoint { int x, y; };
static constexpr int coffee_period = 360;
static constexpr int coffee_lift = 26;
static constexpr int coffee_hold = 16;
static constexpr int coffee_lower = 28;
static constexpr ActorKey sleep_keys[] = {{0,0}, {493,0}, {507,1}, {557,1}, {573,0}};
// The two cels close different eyes, with separate pauses between blinks.
static constexpr ActorKey bench_keys[] = {
    {0,0}, {178,0}, {181,1}, {185,1}, {189,0}, {294,0}, {297,2}, {301,2}, {305,0}
};
static constexpr ActorKey newspaper_keys[] = {{0,0}, {338,0}, {348,1}, {358,2},
                                              {368,3}, {378,4}, {390,0}};
static constexpr SpritePoint coffee_rims[] = {{22,79}, {22,72}, {22,66}};
// Positions in the lobby's native 807×600 artwork, excluding the side bars.
// Keep the sleeper's full frame clear of the receptionist's redraw area.
static constexpr SpritePoint detective_origin = {507, 408};
static constexpr SpritePoint actor_origins[] = {{295,41}, {651,239}, {162,181}, {602,309},
                                                {511,307}, {466,330}, detective_origin, {648,260}};

template <size_t N>
static ActorPose actor_pose(int tick, const ActorKey (&keys)[N], bool continuous = false) {
    for (size_t i = 1; i < N; ++i) {
        if (tick >= keys[i].tick) continue;
        const int from = keys[i - 1].frame, to = keys[i].frame;
        const int elapsed = tick - keys[i-1].tick, duration = keys[i].tick - keys[i-1].tick;
        return {from, to, from == to ? 0U :
            (continuous ? unsigned(elapsed * 256 / duration) : pose_weight(elapsed, duration))};
    }
    return {keys[N-1].frame, keys[N-1].frame, 0};
}

static ActorPose coffee_pose(uint32_t tick) {
    constexpr int first_lift = coffee_period - coffee_lift;
    if (tick < first_lift) return {0, 0, 0};
    const int phase = (tick - first_lift) % coffee_period;
    int travel;
    if (phase < coffee_lift) {
        travel = 2 * int(pose_weight(phase, coffee_lift));
    } else if (phase < coffee_lift + coffee_hold) {
        return {2, 2, 0};
    } else if (phase < coffee_lift + coffee_hold + coffee_lower) {
        travel = 512 - 2 * int(pose_weight(phase - coffee_lift - coffee_hold, coffee_lower));
    } else {
        return {0, 0, 0};
    }
    // The middle drawing guides the wrist through both movements without
    // restarting the acceleration curve halfway through the gesture.
    if (travel >= 512) return {2, 2, 0};
    const int lower = travel / 256;
    return {lower, lower + 1, static_cast<unsigned>(travel % 256)};
}

static uint16_t sample_actor_column(const uint16_t* source, int stride, int x,
                                    int y256, uint16_t background) {
    const int y = y256 >> 8;
    const unsigned fraction = y256 & 255;
    const uint16_t a = source[y * stride + x];
    if (!fraction) return a == 0xF81F ? background : a;
    const uint16_t b = source[(y + 1) * stride + x];
    if (a == b) return a == 0xF81F ? background : a;
    return mix_rgb565(a == 0xF81F ? background : a, b == 0xF81F ? background : b, fraction);
}

static void draw_coffee_pose(uint16_t* dst, int sw, int sh, BridgeRect r, const uint16_t* pixels,
                             int stride, ActorPose pose) {
    const int distance = coffee_rims[pose.to].y - coffee_rims[pose.from].y;
    const int shift_from = -distance * int(pose.weight);
    const int shift_to = distance * int(256 - pose.weight);
    const int rim = coffee_rims[pose.from].y + (distance * int(pose.weight) + (distance < 0 ? -128 : 128)) / 256;
    const uint16_t* from = pixels + pose.from * r.w;
    const uint16_t* to = pixels + pose.to * r.w;
    record_layer(r.x, r.y, r.w, r.h);
    const int left = r.x < 0 ? -r.x : 0, right = r.x + r.w > sw ? sw - r.x : r.w;
    for (int y = 0; y < r.h; ++y) {
        if (r.y + y < 0 || r.y + y >= sh) continue;
        // Keep the face registered. Only the mug and sleeve travel with
        // the moving rim, with a feathered lower edge at the elbow.
        const int offset = y - rim;
        const int feather = offset < -1 || offset >= 17 ? 0 :
            (offset < 3 ? (offset+1)*9/4 : offset <= 8 ? 9 : 17-offset);
        for (int x = left; x < right; ++x) {
            uint16_t& out = dst[(r.y + y) * sw + r.x + x];
            const int reach = x < 32 ? 10 : x < 42 ? 42 - x : 0;
            const int influence = feather * reach;
            const uint16_t a = sample_actor_column(from, stride, x, y*256 + shift_from*influence/90, out);
            const uint16_t b = sample_actor_column(to, stride, x, y*256 + shift_to*influence/90, out);
            out = (y < rim-1 && y < 71) ? (pose.weight < 128 ? a : b) :
                (a == b ? a : mix_rgb565(a, b, pose.weight));
        }
    }
}

static void actor_pixel(uint16_t* dst, int sw, int sh, BridgeRect r, SpritePoint p,
                        uint16_t color, unsigned opacity) {
    if (!opacity || p.x < 0 || p.x >= r.w || p.y < 0 || p.y >= r.h) return;
    const int x = r.x + p.x, y = r.y + p.y;
    if (x >= 0 && x < sw && y >= 0 && y < sh) {
        uint16_t& pixel = dst[y * sw + x];
        pixel = mix_rgb565(pixel, color, opacity);
    }
}

// Coordinates in 1/256-pixel units give small moving strokes continuous coverage.
static void actor_subpixel(uint16_t* dst, int sw, int sh, BridgeRect r, SpritePoint p,
                           uint16_t color, unsigned opacity) {
    const int x = p.x >> 8, y = p.y >> 8;
    const unsigned fx = p.x & 255, fy = p.y & 255;
    actor_pixel(dst, sw, sh, r, {x, y}, color, opacity*(256-fx)*(256-fy) >> 16);
    actor_pixel(dst, sw, sh, r, {x+1, y}, color, opacity*fx*(256-fy) >> 16);
    actor_pixel(dst, sw, sh, r, {x, y+1}, color, opacity*(256-fx)*fy >> 16);
    actor_pixel(dst, sw, sh, r, {x+1, y+1}, color, opacity*fx*fy >> 16);
}

static void actor_line(uint16_t* dst, int sw, int sh, BridgeRect r, SpritePoint a,
                       SpritePoint b, uint16_t color, unsigned opacity) {
    if (!opacity) return;
    const int dx = b.x - a.x, dy = b.y - a.y;
    const int length = abs(dx) > abs(dy) ? abs(dx) : abs(dy);
    const int segments = (length + 255) / 256;
    if (!segments) {
        actor_subpixel(dst, sw, sh, r, a, color, opacity);
        return;
    }
    for (int i = 0; i <= segments; ++i) {
        actor_subpixel(dst, sw, sh, r, {a.x + dx*i/segments, a.y + dy*i/segments}, color, opacity);
    }
}

static void draw_actor_effect(uint16_t* dst, int sw, int sh, BridgeRect r, LobbyActor actor,
                              ActorPose pose, uint32_t tick) {
    if (actor == COFFEE_GUEST) {
        const SpritePoint a = coffee_rims[pose.from], b = coffee_rims[pose.to];
        const int x = a.x * int(256-pose.weight) + b.x * int(pose.weight);
        const int y = a.y * int(256-pose.weight) + b.y * int(pose.weight);
        // Three poses of a curling steam ribbon, followed by a dissipating tip.
        // All coordinates are relative to the rim and interpolate in subpixels.
        static constexpr SpritePoint steam_frames[][4] = {
            {{0,0}, {-1,-2}, {0,-4}, {1,-5}},
            {{0,0}, {-2,-4}, {0,-8}, {3,-10}},
            {{0,0}, {-3,-4}, {-1,-9}, {4,-13}},
            {{0,0}, {-4,-3}, {-2,-7}, {4,-12}},
        };
        for (int wisp = 0; wisp < 2; ++wisp) {
            constexpr int frame_ticks = 28;
            const int age = (tick + wisp*42) % (3*frame_ticks);
            const int from = age/frame_ticks;
            const unsigned blend = pose_weight(age%frame_ticks, frame_ticks);
            const unsigned alpha = age < 12 ? age*19 : (84-age)*228/72;
            const int rise = age * 256 / 8;
            const int spread = wisp ? -5*256 : 0;
            SpritePoint previous = {x + spread, y - 2*256 - rise};
            for (int i = 1; i < 4; ++i) {
                const SpritePoint a = steam_frames[from][i], b = steam_frames[from+1][i];
                const SpritePoint next = {
                    x + spread + a.x * int(256-blend) + b.x * int(blend),
                    y - 2*256 - rise + a.y * int(256-blend) + b.y * int(blend),
                };
                const unsigned coverage = alpha * (5-i)/4;
                actor_line(dst, sw, sh, r, {previous.x+176,previous.y}, {next.x+176,next.y}, 0xb69a, coverage/2);
                actor_line(dst, sw, sh, r, previous, next, 0xffdf, coverage);
                previous = next;
            }
        }
    } else if (actor == SLEEPY_GUEST) {
        const unsigned awake = pose.from ? 256-pose.weight : pose.weight;
        const unsigned sleep = pose.to == pose.from ? (pose.from ? 0 : 256) : 256-awake;
        if (!sleep) return;
        const int pulse = tick % 96;
        const int t = pulse < 48 ? pulse : 96-pulse;
        const int radius = 512 + 4 * int(pose_weight(t, 48));
        const SpritePoint nose = {17, 91};
        for (int y = -7; y <= 7; ++y) {
            for (int x = 0; x <= 13; ++x) {
                const int dx = x*256-radius, dy = y*256;
                const int edge = (radius*radius - dx*dx - dy*dy) / (2*radius);
                const unsigned coverage = static_cast<unsigned>(edge < -128 ? 0 : (edge > 128 ? 256 : edge + 128));
                const unsigned opacity = edge < 256 ? sleep/2 : sleep/4;
                actor_pixel(dst, sw, sh, r, {nose.x-x, nose.y+y}, 0xcfff, opacity*coverage >> 8);
            }
        }
        actor_subpixel(dst, sw, sh, r, {nose.x*256-radius-256, (nose.y-1)*256}, 0xffff, sleep*3/4);
        for (int i = 0; i < 3; ++i) {
            const int t = (tick + i*34) % 102;
            const int x = 22*256-t*256/10, y = 64*256-t*256/3;
            const int w = i == 0 ? 5*256 : 3*256;
            const unsigned opacity = sleep*(t < 12 ? t*7 : (102-t))/106;
            actor_line(dst, sw, sh, r, {x,y}, {x+w,y}, 0xefff, opacity);
            actor_line(dst, sw, sh, r, {x+w,y}, {x,y+w}, 0xefff, opacity);
            actor_line(dst, sw, sh, r, {x,y+w}, {x+w,y+w}, 0xefff, opacity);
        }
    } else if (actor == NEWSPAPER_READER) {
        constexpr SpritePoint lobes[] = {{-3,0}, {0,-2}, {3,0}, {1,1}};
        const int phase = tick % 390;
        for (int i = 0; i < 2; ++i) {
            const int age = phase - 140 - i * 19;
            if (age < 0 || age >= 58) continue;
            const unsigned opacity = age < 9 ? pose_weight(age, 9) : pose_weight(58 - age, 49);
            const int cx = 19 * 256 - age * 34;
            const int cy = 46 * 256 - age * 122;
            const int radius = 420 + age * 16;
            const int left = cx / 256 - 9, top = cy / 256 - 8;
            for (int y = top; y <= top + 15; ++y) {
                for (int x = left; x <= left + 18; ++x) {
                    int margin = 0;
                    for (const SpritePoint lobe : lobes) {
                        const int dx = x * 256 - cx - lobe.x * radius / 4;
                        const int dy = y * 256 - cy - lobe.y * radius / 4;
                        const int coverage = radius + 128 - (dx * dx + dy * dy + radius * radius) / (2 * radius);
                        if (coverage > margin) margin = coverage;
                    }
                    if (margin > 0) {
                        const unsigned alpha = opacity * (margin < 256 ? margin : 256) * 3 / 1024;
                        actor_pixel(dst, sw, sh, r, {x,y}, y * 256 > cy ? 0xae18 : 0xf7ff, alpha);
                    }
                }
            }
        }
    }
}

static void draw_lobby_actors(uint16_t* dst, int sw, int sh, BridgeRect room, bool partial,
                              int floor, uint32_t tick) {
    const int first = floor == 1 ? CAT_CLOCK : EXPLORER;
    const int end = floor == 1 ? EXPLORER : LOBBY_ACTOR_COUNT;
    BridgeRect regions[LOBBY_ACTOR_COUNT];
    // Restore all overlapping areas before drawing any of the sprites.
    for (int i = first; i < end; ++i) {
        const PackedActor& art = lobby_actors[i];
        const SpritePoint origin = actor_origins[i];
        regions[i] = {room.x + origin.x*room.w/807, room.y + origin.y*room.h/600,
                      art.frame_width*room.w/807, art.image.height*room.h/600};
        const BridgeRect r = regions[i];
        restore_region(partial, dst, sw, sh, r.x, r.y, r.w, r.h);
    }
    const int clock = (tick % 120) * lobby_actors[CAT_CLOCK].frames;
    const ActorPose poses[] = {
        {clock/120, (clock/120+1) % lobby_actors[CAT_CLOCK].frames, static_cast<unsigned>(clock%120*256/120)},
        coffee_pose(tick),
        actor_pose(tick%600, sleep_keys),
        actor_pose(tick%435, bench_keys),
        {0,0,0},
        {0,0,0},
        actor_pose((tick+179)%539, newspaper_keys, true),
        actor_pose(tick%470, newspaper_keys, true),
    };
    for (int i = first; i < end; ++i) {
        const PackedActor& art = lobby_actors[i];
        const BridgeRect r = regions[i];
        const ActorPose pose = poses[i];
        const uint16_t* pixels = lobby_actor_pixels[i];
        const bool native = r.w == art.frame_width && r.h == art.image.height;
        if (native && pose.from != pose.to && pose.weight > 0 && pose.weight < 256) {
            if (i == COFFEE_GUEST) {
                draw_coffee_pose(dst, sw, sh, r, pixels, art.image.width, pose);
            } else {
                const bool detective = i == BENCH_DETECTIVE;
                const PaperTip* page = detective ? detective_page_tips :
                    (i == NEWSPAPER_READER ? reader_page_tips : nullptr);
                const PaperTip tips[] = {page ? page[pose.from] : detective_page_tips[0],
                                         page ? page[pose.to] : detective_page_tips[0]};
                draw_inbetween(dst, sw, sh, r, pixels + pose.from*art.frame_width,
                    pixels + pose.to*art.frame_width, art.image.width, pose.weight,
                    page ? tips : nullptr, detective ? detective_fold : reader_fold);
            }
        } else {
            const int frame = pose.weight >= 128 ? pose.to : pose.from;
            draw_transparent_sprite(dst, sw, sh, pixels, art.image.width, art.image.height,
                frame*art.frame_width, 0, art.frame_width, art.image.height, r.x, r.y, r.w, r.h);
        }
        if (active_background && (i == BENCH_FRIENDS || i == EXPLORER ||
                                  i == ELECTRIC_COMPANION || i == BENCH_DETECTIVE)) {
            // Restore the foreground backrest over the seated figures.
            SpritePoint edge_origin = {476,383};
            if (i == BENCH_FRIENDS) edge_origin = {638,402};
            if (i == BENCH_DETECTIVE) edge_origin = {485,483};
            const bool blue_bench = i == BENCH_FRIENDS;
            const bool center_bench = i == EXPLORER || i == ELECTRIC_COMPANION;
            const int first_column = blue_bench ? 598 : (center_bench ? 458 : 488);
            const int last_column = blue_bench ? 706 : 568;
            const int look_above = i == BENCH_DETECTIVE ? 1 : 2;
            const int look_below = (center_bench ? 13 : 5) * room.h / 600;
            const int max_red = blue_bench ? 10 : 7;
            for (int col = 0; col < r.w; ++col) {
                const int x = r.x + col;
                if (x < 0 || x >= sw) continue;
                const int room_x = (x - room.x) * 807 / room.w;
                if (room_x < first_column || room_x > last_column) continue;
                const int dx = room_x - edge_origin.x;
                const int slope = dx * 5 / 9;
                int edge = room.y + (edge_origin.y - slope) * room.h / 600;
                // Snap the mask to the bench's dark outline in the backdrop.
                const int last = edge + look_below;
                bool found_outline = false;
                for (int y = edge - look_above; y <= last && y < sh; ++y) {
                    if (y < 0) continue;
                    const uint16_t pixel = active_background[y * sw + x];
                    if ((pixel >> 11) <= max_red && ((pixel >> 5) & 63) <= 25) {
                        edge = y;
                        found_outline = true;
                        break;
                    }
                }
                if (!found_outline) continue;
                for (int y = r.y + r.h - 1; y >= edge && y >= r.y && y >= 0; --y) {
                    if (y < sh) dst[y * sw + x] = active_background[y * sw + x];
                }
            }
        }
        if (native) draw_actor_effect(dst, sw, sh, r, static_cast<LobbyActor>(i), pose, tick);
    }
}

static void draw_neko_pose(uint16_t* dst, int sw, int sh, BridgeRect r, int progress) {
    constexpr int feather_start = 38, feather_end = 42, shoulder_y = 48;
    const int count = neko_animation_sequence_length;
    const int i = neko_animation_step;
    const int a = neko_paw_top[neko_frame], b = neko_paw_top[neko_animation_sequence[(i+1)%count]];
    const int prev = neko_paw_top[neko_animation_sequence[(i+count-1)%count]];
    const int next = neko_paw_top[neko_animation_sequence[(i+2)%count]];
    const int u = static_cast<int>(int64_t(progress) * 256 / neko_frame_delay);
    const int u2 = u * u / 256, u3 = u2 * u / 256;
    // Monotone cubic interpolation stops smoothly at the top and bottom of
    // the wave and keeps the timing of each authored pose.
    const int paw_y = a * 256 + (b-a) * (3*u2 - 2*u3)
        + paw_velocity(a-prev, b-a) * (u - 2*u2 + u3) / 256
        + paw_velocity(b-a, next-b) * (u3 - u2) / 256;
    const int pose = neko_animation_sequence[(i + (u >= 128)) % count];
    const int shift = neko_paw_top[pose] * 256 - paw_y;
    const int x0 = r.x < 0 ? -r.x : 0, x1 = r.x + r.w > sw ? sw - r.x : r.w;
    const int y0 = r.y < 0 ? -r.y : 0, y1 = r.y + r.h > sh ? sh - r.y : r.h;
    if (x0 >= x1 || y0 >= y1) return;

    record_layer(r.x, r.y, r.w, r.h);
    const uint16_t* frame = neko_pixels + pose * neko_cell_width;
    for (int y = y0; y < y1; ++y) {
        uint16_t* out = dst + (r.y + y) * sw + (r.x + x0);
        const int sy = y * neko_cell_height / r.h;
        // Only the paw moves. Taper the warp into the body and shoulder.
        const int row_shift = sy * 256 <= paw_y ? shift :
            (sy < shoulder_y ? shift * (shoulder_y - sy) * 256 / (shoulder_y*256 - paw_y) : 0);
        for (int x = x0; x < x1; ++x) {
            const int sx = x * neko_cell_width / r.w;
            int sample_y = sy * 256;
            if (sx > feather_start)
                sample_y += sx >= feather_end ? row_shift :
                    row_shift * (sx - feather_start) / (feather_end - feather_start);
            if (sample_y < 0 || sample_y >= neko_cell_height * 256) continue;
            const int row = sample_y / 256;
            const unsigned fraction = sample_y % 256;
            uint16_t color = frame[row * neko_width + sx];
            if (fraction && row + 1 < neko_cell_height) {
                uint16_t adjacent = frame[(row + 1) * neko_width + sx];
                if (color == adjacent && color == 0xF81F) continue;
                if (color == 0xF81F) color = out[x-x0];
                if (adjacent == 0xF81F) adjacent = out[x-x0];
                color = mix_rgb565(color, adjacent, fraction);
                out[x-x0] = color;
            } else if (color != 0xF81F) {
                out[x-x0] = color;
            }
        }
    }
}

// Aperture edges measured in the packed cells. Fixed anchors keep the jambs
// still, while the intermediate poses move the leaves at panel resolution.
static constexpr uint8_t exterior_edges[][2] = {
    {63, 63}, {59, 67}, {54, 73}, {48, 78}, {43, 84}, {37, 89}, {32, 95}, {28, 98}
};
static constexpr uint8_t interior_edges[][2] = {
    {19, 20}, {18, 21}, {16, 23}, {13, 26}, {10, 29}, {7, 32}
};

struct DoorSample { int x, shift_y; };

static DoorSample door_sample(int px, const int* pose, const int* source, bool inside) {
    int segment = 0;
    while (segment < 4 && px >= pose[segment + 1]) ++segment;
    if (!inside && segment == 2) return {-1, 0};
    const int start = source[segment] * 256;
    const int sx = start + (px - pose[segment]) * (source[segment + 1] - source[segment]) * 256 /
        (pose[segment + 1] - pose[segment]);
    // The exterior sprite follows the isometric slope of the doorway.
    const int shear = (sx - px) * 45;
    const int shift_y = inside ? 0 : (shear + (shear < 0 ? -82 * 128 : 82 * 128)) / (82 * 256);
    return {sx / 256, shift_y};
}

static uint16_t sample_door(const uint16_t* frame, int stride, int height,
                            int y, DoorSample sample) {
    y += sample.shift_y;
    if (sample.x < 0 || y < 0 || y >= height) return 0xF81F;
    return frame[y * stride + sample.x];
}

static void draw_door_inbetween(uint16_t* dst, int sw, int sh, const uint16_t* pixels,
                                int stride, int cell_w, int cell_h, BridgeRect r,
                                bool inside, int frame, int progress) {
    constexpr int subpixel = 256;
    const int delay = elevator_door_frame_delay;
    const auto* edges = inside ? interior_edges : exterior_edges;
    const int closest = frame + (progress >= delay - progress);
    const int source[] = {0, inside ? 5 : 22, edges[closest][0], edges[closest][1],
                          inside ? 35 : 105, cell_w};
    int pose[] = {0, source[1] * subpixel, 0, 0, source[4] * subpixel, cell_w * subpixel};
    for (int side = 0; side < 2; ++side) {
        const int from = edges[frame][side] * subpixel;
        const int delta = (edges[frame + 1][side] - edges[frame][side]) * subpixel;
        pose[side + 2] = from + int(int64_t(delta) * progress / delay);
    }

    // Cache the two horizontal samples for each column. The isometric row
    // offset is computed once per column as well.
    static DoorSample samples[2][BRIDGE_PANEL_WIDTH];
    const int left = r.x < 0 ? -r.x : 0;
    const int right = r.x + r.w > sw ? sw - r.x : r.w;
    const int top = r.y < 0 ? -r.y : 0;
    const int bottom = r.y + r.h > sh ? sh - r.y : r.h;
    if (left >= right || top >= bottom) return;
    for (int x = left; x < right; ++x) {
        for (int sample = 0; sample < 2; ++sample) {
            const int px = int((int64_t(x) * subpixel + sample * subpixel / 2) * cell_w / r.w);
            samples[sample][x - left] = door_sample(px, pose, source, inside);
        }
    }
    const uint16_t* atlas_frame = pixels + closest * cell_w;
    for (int row = top; row < bottom; ++row) {
        const int y = int(int64_t(row) * cell_h / r.h);
        uint16_t* out = dst + (r.y + row) * sw + (r.x + left);
        for (int col = 0; col < right - left; ++col) {
            uint16_t a = sample_door(atlas_frame, stride, cell_h, y, samples[0][col]);
            uint16_t b = sample_door(atlas_frame, stride, cell_h, y, samples[1][col]);
            if (a == b) {
                if (a != 0xF81F) out[col] = a;
            } else {
                if (a == 0xF81F) a = out[col];
                if (b == 0xF81F) b = out[col];
                // Average RGB565 channels independently for fractional edge coverage.
                out[col] = (a & b) + (((a ^ b) & 0xF7DE) >> 1);
            }
        }
    }
}

#include "exterior_direction_sprites.inc"

static void draw_exterior_direction(uint16_t* dst, int sw, int sh, BridgeRect room,
                                    const LayerFrames& state) {
    constexpr int height = exterior_display_face_height + (exterior_display_width-1)*3/5;
    constexpr uint16_t transparent = 0xf81f;
    constexpr uint16_t finishes[BRIDGE_LOBBY_COUNT][12] = {
        {transparent, color565(0x291622), color565(0x4f253a), color565(0x783950),
         color565(0x59293f), color565(0x102821), color565(0x355344), color565(0xc28196),
         color565(0x53283a), color565(0xffedb4), color565(0xe6b16c), color565(0xaa6a55)},
        {transparent, color565(0x211337), color565(0x423168), color565(0x7867ad),
         color565(0x433463), color565(0x142530), color565(0x365466), color565(0xbbaff2),
         color565(0x5d488b), color565(0xeedcff), color565(0xc1a4ef), color565(0x8464bb)},
        {transparent, color565(0x07292d), color565(0x124950), color565(0x488f81),
         color565(0x235a59), color565(0x0b2d2a), color565(0x376c59), color565(0xa7eacd),
         color565(0x205c58), color565(0xe0ffcb), color565(0x87e3a9), color565(0x4aa489)},
    };
    const uint16_t* const palette = finishes[state.lobby];
    uint16_t sprite[height * exterior_display_width];
    for (uint16_t& pixel : sprite) pixel = transparent;
    const bool up = state.exterior_direction == DIRECTION_INDICATOR_UP;
    const bool down = state.exterior_direction == DIRECTION_INDICATOR_DOWN;
    const bool idle = state.exterior_direction == DIRECTION_INDICATOR_IDLE;
    const bool waiting = state.exterior_direction == DIRECTION_INDICATOR_WAITING;
    const bool show_marker = !waiting || state.exterior_scroll == 0;
    for (int y = 0; y < exterior_display_face_height; ++y) {
        const int marker_y = y - 13;
        for (int x = 0; x < exterior_display_width; ++x) {
            const char texel = exterior_display_face[y][x];
            const int index = texel <= '9' ? texel-'0' : texel-'a'+10;
            uint16_t color = palette[index];
            if ((up || down) && x >= 8 && x < 17 && y >= 9 && y < 27) {
                const int offset = up ? state.exterior_scroll : exterior_scroll_period - state.exterior_scroll;
                const int row = (y-9+offset) % exterior_scroll_period;
                if (row < 6 && (exterior_up_chevron[up ? row : 5-row] & (0x100 >> (x-8))))
                    color = color565(0x6bfaa0);
            } else if (show_marker && !up && !down && x >= 8 && x < 17 && marker_y >= 0 && marker_y < 9 &&
                       (exterior_idle_marker[marker_y] & (0x100 >> (x-8)))) {
                const int from_center = abs(x-12) + abs(marker_y-4);
                color = !(idle || waiting) ? color565(0x29604c) :
                    from_center == 4 ? color565(0x39ba76) :
                    from_center <= 1 ? color565(0xb6ffd0) : color565(0x6bfaa0);
            }
            sprite[(y+x*3/5)*exterior_display_width+x] = color;
        }
    }
    const BridgeRect r = scale_stairs_rect(room, {562,204,exterior_display_width,height});
    if (!r.w || !r.h) return;
    if (r.y < dirty_top) dirty_top = r.y;
    if (r.y+r.h > dirty_bottom) dirty_bottom = r.y+r.h;
    // Every pose paints the same housing and glass, so no backdrop restore
    // is needed between glyph changes, including where the door atlas overlaps.
    draw_transparent_sprite(dst, sw, sh, sprite, exterior_display_width, height,
                            0, 0, exterior_display_width, height, r.x, r.y, r.w, r.h);
}

static void draw_scene_door(uint16_t* dst, int sw, int sh, bool inside, int frame,
                            int x, int y, int dw, int dh, bool partial, BridgeRect* bounds = NULL,
                            int progress = 0) {
    const uint16_t* pixels = inside ? elevator_inside_door_pixels : elevator_door_pixels;
    const int tw = inside ? elevator_inside_door_width : elevator_door_width;
    const int th = inside ? elevator_inside_door_height : elevator_door_height;
    const int cols = inside ? elevator_inside_door_frame_columns : elevator_door_frame_columns;
    const int iw = inside ? elevator_inside_width : lobby_width;
    if (!pixels || tw <= 0 || th <= 0 || iw <= 0 || frame < 0 || frame >= cols) return;
    const int ih = inside ? elevator_inside_height : lobby_height;
    if (ih <= 0) return;
    const int dx = x + int(int64_t(inside ? elevator_inside_door_x : elevator_door_x) * dw / iw);
    const int dy = y + int(int64_t(inside ? elevator_inside_door_y : elevator_door_y) * dh / ih);
    const int w = int(int64_t(inside ? elevator_inside_door_render_width : elevator_door_render_width) * dw / iw);
    const int h = int(int64_t(inside ? elevator_inside_door_render_height : elevator_door_render_height) * dh / ih);
    const int sx = frame * tw / cols;
    restore_region(partial, dst, sw, sh, dx, dy, w, h);
    if (bounds) *bounds = {dx, dy, w, h};
    if (collect_layers && frame == (inside ? elevator_inside_door_open_frame : elevator_door_animation_frame_count - 1)) return;
    const int cell_w = tw / cols;
    const int poses = inside ? int(sizeof(interior_edges) / sizeof(*interior_edges)) :
                               int(sizeof(exterior_edges) / sizeof(*exterior_edges));
    const bool supported_atlas = inside ? cell_w == 38 && th == 29 : cell_w == 117 && th == 219;
    if (progress && packed_layout && supported_atlas && frame + 1 < poses &&
        w > 0 && h > 0 && w <= BRIDGE_PANEL_WIDTH) {
        record_layer(dx, dy, w, h);
        draw_door_inbetween(dst, sw, sh, pixels, tw, cell_w, th, {dx, dy, w, h}, inside, frame, progress);
    } else {
        draw_transparent_sprite(dst, sw, sh, pixels, tw, th, sx, 0, (frame+1)*tw/cols-sx, th, dx, dy, w, h);
    }
}

bool bridge_prepare_zoom_scene(int scene) {
    if (!panel_graphics || scene < 0 || scene > BRIDGE_CABIN) return false;
    const bool inside = scene == BRIDGE_CABIN;
    const int w = BRIDGE_PANEL_WIDTH, h = BRIDGE_PANEL_HEIGHT;
    int x, y, dw, dh;
    calculate_background_layout(w, h, inside ? elevator_inside_width : lobby_width,
                                inside ? elevator_inside_height : lobby_height, &x, &y, &dw, &dh);
    uint16_t* dst = panel_graphics->snapshot;
    memcpy(dst, inside ? panel_graphics->cabin : panel_graphics->lobbies[scene], w * h * sizeof(*dst));
    draw_scene_door(dst, w, h, inside, inside ? elevator_inside_door_open_frame : elevator_door_animation_frame_count - 1,
                    x, y, dw, dh, false, &open_doors[scene].area);
    DoorPatch& patch = open_doors[scene];
    const BridgeRect& r = patch.area;
    free(patch.pixels);
    patch.pixels = static_cast<uint16_t*>(malloc(r.w * r.h * sizeof(*dst)));
    if (!patch.pixels) return false;
    for (int row = 0; row < r.h; ++row)
        memcpy(patch.pixels + row * r.w, dst + (r.y + row) * w + r.x, r.w * sizeof(*dst));
    if (inside) {
        LayerFrames idle = {};
        idle.pressed_button = idle.light_level = -1;
        draw_elevator_panel_controls(dst, w, h,
            x + elevator_panel_source_x * dw / elevator_inside_width,
            y + elevator_panel_source_y * dh / elevator_inside_height,
            elevator_panel_source_width * dw / elevator_inside_width,
            elevator_panel_source_height * dh / elevator_inside_height, idle);
    }
    return true;
}

bool bridge_prepare_closed_lobby_zoom(int scene, BridgeRect* door) {
    if (!panel_graphics || scene < 0 || scene >= BRIDGE_LOBBY_COUNT || !door) return false;
    const int w = BRIDGE_PANEL_WIDTH, h = BRIDGE_PANEL_HEIGHT;
    int x, y, dw, dh;
    calculate_background_layout(w, h, lobby_width, lobby_height, &x, &y, &dw, &dh);
    uint16_t* dst = panel_graphics->snapshot;
    memcpy(dst, panel_graphics->lobbies[scene], size_t(w) * h * sizeof(*dst));
    draw_scene_door(dst, w, h, false, 0, x, y, dw, dh, false, door);
    return true;
}

static void release_door_patches() {
    for (DoorPatch& patch : open_doors) { free(patch.pixels); patch = {}; }
}

bool bridge_prepare_panel_graphics(const BridgePanelGraphics* g) {
    release_door_patches();
    panel_graphics = NULL;
    if (!g) release_cabin_shaft(panel_shaft);
    if (!packed_layout || !g || !g->cabin || !g->snapshot || !g->intro_doors ||
        !g->copy_columns || !g->zoom_fade) return false;
    const int w = BRIDGE_PANEL_WIDTH, h = BRIDGE_PANEL_HEIGHT;
    const size_t bytes = w * h * sizeof(uint16_t);
    int x, y, dw, dh;
    calculate_background_layout(w, h, lobby_width, lobby_height, &x, &y, &dw, &dh);
    for (int scene = 0; scene < BRIDGE_LOBBY_COUNT; ++scene) {
        if (!g->lobbies[scene]) return false;
        memset(g->lobbies[scene], 0, bytes);
        if (scene == 0) {
            draw_transparent_sprite(g->lobbies[scene], w, h, lobby_pixels, lobby_texture_width, lobby_texture_height,
                0, 0, lobby_texture_width, lobby_texture_height, x, y, dw, dh);
        } else {
            draw_indexed_lobby(g->lobbies[scene], w, x, y, dw, dh, scene);
        }
    }
    if (!prepare_cabin_background(panel_shaft, g->cabin, w, h)) return false;
    memset(g->intro_doors, 0, bytes);
    for (int row = 0; row < h; ++row) {
        const int index = intro_row_indices[2 * intro_indices.width + row];
        memcpy(g->intro_doors + row * w + (w - intro_rows.width) / 2,
               intro_row_pixels + index * intro_rows.width, intro_rows.width * sizeof(uint16_t));
    }
    panel_graphics = g;
    return true;
}

static int get_elevator_direction_indicator_scroll_period(int dest_height) {
    return static_cast<int>(
        static_cast<int64_t>(elevator_direction_indicator_display_height) *
            dest_height / img_height);
}

static void draw_elevator_direction_indicator(
    uint16_t* framebuffer, int screen_width, int screen_height,
    int dest_x, int dest_y, int dest_width, int dest_height,
    int indicator_scene_x) {
    if (elevator_direction_indicator_pixels == NULL ||
        elevator_direction_indicator_width <= 0 ||
        elevator_direction_indicator_height <= 0) {
        return;
    }

    const int display_x = dest_x + static_cast<int>(
        static_cast<int64_t>(indicator_scene_x) *
            dest_width / img_width);
    const int display_y = dest_y + static_cast<int>(
        static_cast<int64_t>(elevator_direction_indicator_y) *
            dest_height / img_height);
    const int display_width = static_cast<int>(
        static_cast<int64_t>(elevator_direction_indicator_display_width) *
            dest_width / img_width);
    const int display_height = static_cast<int>(
        static_cast<int64_t>(elevator_direction_indicator_display_height) *
            dest_height / img_height);
    if (display_width <= 0 || display_height <= 0) {
        return;
    }

    const int clip_left = display_x < 0 ? 0 : display_x;
    const int clip_right = display_x + display_width > screen_width
        ? screen_width : display_x + display_width;
    const int clip_top = display_y < 0 ? 0 : display_y;
    const int clip_bottom = display_y + display_height > screen_height
        ? screen_height : display_y + display_height;
    record_layer(clip_left, clip_top, clip_right - clip_left, clip_bottom - clip_top);
    if (clip_top < dirty_top) dirty_top = clip_top;
    if (clip_bottom > dirty_bottom) dirty_bottom = clip_bottom;
    for (int y = clip_top; y < clip_bottom; ++y) {
        for (int x = clip_left; x < clip_right; ++x) {
            framebuffer[y * screen_width + x] = 0;
        }
    }

    const bool center_off = elevator_direction_indicator_frame == cabin_indicator_center_off;
    const int atlas_frame = center_off ? cabin_indicator_square_frame : elevator_direction_indicator_frame;
    const int frame_x = atlas_frame * elevator_direction_indicator_frame_stride;
    if (atlas_frame >= cabin_indicator_square_frame) {
        constexpr int width = elevator_direction_indicator_frame_width;
        constexpr int height = elevator_direction_indicator_frame_height;
        uint16_t corners[width * height];
        if (center_off) {
            for (int row = 0; row < height; ++row) {
                memcpy(corners + row * width, elevator_direction_indicator_pixels +
                       row * elevator_direction_indicator_width + frame_x, width * sizeof(*corners));
            }
            // Frame 2 has a 3-by-3 square between the four corner marks.
            for (int row = 2; row < 5; ++row) std::fill_n(corners + row * width + 2, 3, uint16_t(0));
        }
        draw_transparent_sprite(
            framebuffer, screen_width, screen_height,
            center_off ? corners : elevator_direction_indicator_pixels,
            center_off ? width : elevator_direction_indicator_width,
            center_off ? height : elevator_direction_indicator_height,
            center_off ? 0 : frame_x, 0, width, height,
            display_x, display_y, display_width, display_height);
        return;
    }

    const int sprite_width = display_width;
    const int sprite_height = sprite_width * 5 /
        elevator_direction_indicator_frame_width;
    const int period = get_elevator_direction_indicator_scroll_period(dest_height);
    if (sprite_width <= 0 || sprite_height <= 0 || period <= 0) {
        return;
    }

    const int phase = elevator_direction_indicator_offset % period;
    const int sprite_x = display_x;
    for (int copy = -1; copy <= 1; ++copy) {
        const int sprite_y = elevator_direction_indicator_frame == 0
            ? display_y + display_height - phase + copy * period
            : display_y - sprite_height + phase + copy * period;
        const int draw_left = sprite_x > clip_left ? sprite_x : clip_left;
        const int draw_right = sprite_x + sprite_width < clip_right
            ? sprite_x + sprite_width : clip_right;
        const int draw_top = sprite_y > clip_top ? sprite_y : clip_top;
        const int draw_bottom = sprite_y + sprite_height < clip_bottom
            ? sprite_y + sprite_height : clip_bottom;
        for (int y = draw_top; y < draw_bottom; ++y) {
            const int source_y = 1 +
                (y - sprite_y) * 5 / sprite_height;
            for (int x = draw_left; x < draw_right; ++x) {
                const int source_column = frame_x +
                    (x - sprite_x) * elevator_direction_indicator_frame_width /
                        sprite_width;
                const uint16_t color = elevator_direction_indicator_pixels[
                    source_y * elevator_direction_indicator_width + source_column];
                if (color != 0xF81F) {
                    framebuffer[y * screen_width + x] = color;
                }
            }
        }
    }
}

static void draw_elevator_panel_controls(
    uint16_t* framebuffer, int screen_width, int screen_height,
    int panel_x, int panel_y, int panel_width, int panel_height,
    const LayerFrames& state) {
    if (elevator_panel_width <= 0 || elevator_panel_height <= 0) {
        return;
    }

    if (elevator_buttons_pixels != NULL && elevator_buttons_width >= 6 &&
        elevator_buttons_height >= 3) {
        const int frame_width = elevator_buttons_width / 6;
        const int frame_height = elevator_buttons_height / 3;
        for (int i = 0; i < 6; ++i) {
            const elevator_panel_region_t& region =
                elevator_panel_button_regions[i];
            const elevator_panel_region_t& sprite_bounds =
                elevator_panel_button_sprite_bounds[i];
            const int row = i == state.pressed_button ? 1 : 0;
            if (collect_layers && row == 0) continue;
            const int dest_x = panel_x + static_cast<int>(
                static_cast<int64_t>(region.x) * panel_width /
                    elevator_panel_reference_width);
            const int dest_y = panel_y + static_cast<int>(
                static_cast<int64_t>(region.y) * panel_height /
                    elevator_panel_reference_height);
            const int dest_width = static_cast<int>(
                static_cast<int64_t>(region.width) * panel_width /
                    elevator_panel_reference_width);
            const int dest_height = static_cast<int>(
                static_cast<int64_t>(region.height) * panel_height /
                    elevator_panel_reference_height);
            draw_transparent_sprite(
                framebuffer, screen_width, screen_height,
                elevator_buttons_pixels, elevator_buttons_width,
                elevator_buttons_height,
                i * frame_width + sprite_bounds.x,
                row * frame_height + sprite_bounds.y,
                sprite_bounds.width, sprite_bounds.height,
                dest_x, dest_y, dest_width, dest_height);
        }
    }

    if (elevator_lights_pixels != NULL && elevator_lights_width > 0 &&
        elevator_lights_height >= 2) {
        const int frame_height = elevator_lights_height / 2;
        for (int i = 0; i < 3; ++i) {
            const elevator_panel_region_t& region =
                elevator_panel_light_regions[i];
            const int row = state.light_level >= 0
                ? (state.light_level == i) : (state.pressed_button == i);
            if (collect_layers && row == 0) continue;
            const int dest_x = panel_x + static_cast<int>(
                static_cast<int64_t>(region.x) * panel_width /
                    elevator_panel_reference_width);
            const int dest_y = panel_y + static_cast<int>(
                static_cast<int64_t>(region.y) * panel_height /
                    elevator_panel_reference_height);
            const int dest_width = static_cast<int>(
                static_cast<int64_t>(region.width) * panel_width /
                    elevator_panel_reference_width);
            const int dest_height = static_cast<int>(
                static_cast<int64_t>(region.height) * panel_height /
                    elevator_panel_reference_height);
            draw_transparent_sprite(
                framebuffer, screen_width, screen_height,
                elevator_lights_pixels, elevator_lights_width,
                elevator_lights_height, 2, row * frame_height + 2,
                elevator_lights_width - 4, frame_height - 4,
                dest_x, dest_y, dest_width, dest_height);
        }
    }
}

void bridge_zoom_values(int step, int* zoom, int* brightness) {
    const int n = BRIDGE_ZOOM_STEPS;
    const int t = step < 0 ? 0 : (step > n ? n : step);
    const int weight = t * t * (3 * n - 2 * t);
    const int denominator = n * n * n;
    // Retain fractional easing until the final RGB / scale conversion.
    *zoom = 1000 + ((elevator_transition_zoom_max - 1000) * weight + denominator / 2) / denominator;
    *brightness = (255 * (denominator - weight) + denominator / 2) / denominator;
}

static void select_lobby(int level) {
    const int requested = packed_layout && variant_pack && level > 0 ? level - 1 : 0;
    if (requested == lobby_scene) return;
    lobby_scene = requested;
    exterior_lobby_level.store(lobby_scene+1, std::memory_order_release);
    lobby_actor_ticks = 0;
    background_cache_valid = false;
    frame_needs_render = true;
}

static bool at_cabin_arrival_floor(int floor) {
    return floor > 0 && elevator_current_level.load(std::memory_order_acquire) == floor &&
        elevator_target_level.load(std::memory_order_acquire) == 0;
}

static void advance_cabin_shaft(int step) {
    if (!step) return;
    cabin_shaft_offset = (cabin_shaft_offset + cabin_shaft_cycle + step) % cabin_shaft_cycle;
    frame_needs_render = true;
}

static bool advance_cabin_door_close(int& motion, unsigned& ticks) {
    if (elevator_inside_door_frame == elevator_inside_door_closed_frame) return true;
    motion = -1;
    ticks = (elevator_inside_door_frame - elevator_inside_door_closed_frame) *
        elevator_door_frame_delay - elevator_inside_door_animation_ticks;
    frame_needs_render = true;
    if (++elevator_inside_door_animation_ticks >= elevator_door_frame_delay) {
        elevator_inside_door_animation_ticks = 0;
        --elevator_inside_door_frame;
    }
    return elevator_inside_door_frame == elevator_inside_door_closed_frame;
}

static void advance_animation(int screen_width, int screen_height) {
    int door_motion = 0;
    unsigned door_ticks = 0;
    if (elevator_indicator_test_mode.load(std::memory_order_acquire) &&
        ++elevator_direction_test_phase_ticks >= elevator_direction_test_phase_duration) {
        elevator_direction_test_phase_ticks = 0;
        elevator_direction_test_phase = (elevator_direction_test_phase + 1) % 3;
    }
    if (elevator_transition_phase == TRANSITION_LOBBY) {
        if (lobby_scene > 0) {
            ++lobby_actor_ticks;
            frame_needs_render = true;
        }
    }
    const int stairs_choice = stairs_request.exchange(-1, std::memory_order_acquire);
    const bool menu = elevator_transition_phase == TRANSITION_STAIRS_MENU;
    const bool stable_lobby = elevator_transition_phase == TRANSITION_LOBBY &&
        !elevator_door_opening && !elevator_door_closing && !elevator_door_open_requested;
    if (stairs_choice >= 0 && (menu || (stable_lobby && stairs_choice == 0))) {
        exterior_waiting_for_arrival = 0;
        bridge_release_exterior_call();
        stairs_transition_destination = menu ? (stairs_choice ? stairs_choice : lobby_scene+1) : 0;
        stairs_rendered_button = stairs_choice;
        stairs_press_ticks = stairs_press_duration;
        elevator_transition_phase = TRANSITION_ZOOM_OUT;
        transition_ticks = 0;
        transition_source_valid = false;
    } else if (menu || stable_lobby) {
        const int stairs_button = stairs_pressed.load(std::memory_order_acquire);
        if (stairs_button >= 0 && stairs_button != stairs_rendered_button) {
            stairs_rendered_button = stairs_button;
            stairs_press_ticks = 0;
        }
        if (stairs_button >= 0 && stairs_press_ticks < stairs_press_duration) {
            ++stairs_press_ticks;
            frame_needs_render = true;
        } else if (stairs_button < 0 && stairs_press_ticks > 0) {
            --stairs_press_ticks;
            if (!stairs_press_ticks) stairs_rendered_button = -1;
            frame_needs_render = true;
        }
    }

    const bool emergency = cabin_emergency_latched.load(std::memory_order_acquire);
    if (emergency || gpio_level_indicator_enabled.load(std::memory_order_acquire)) {
        const int requested_frame = emergency ? level_indicator_err_frame :
            gpio_level_indicator_frame.load(std::memory_order_acquire);
        if (level_indicator_frame != requested_frame) {
            level_indicator_frame = requested_frame;
            level_indicator_animation_ticks = 0;
            frame_needs_render = true;
        }
    }

    if (elevator_transition_phase == TRANSITION_LOBBY &&
        receptionist_pixels != NULL) {
        ++animation_ticks;
        if (receptionist_blend_ticks()) frame_needs_render = true;
        if (animation_ticks >= receptionist_frame_delay) {
            animation_ticks = 0;
            receptionist_animation_step =
                (receptionist_animation_step + 1) %
                receptionist_animation_cycle_length;
            if (receptionist_animation_step <
                receptionist_frame_columns * 4) {
                receptionist_frame =
                    receptionist_animation_step % receptionist_frame_columns;
            } else {
                receptionist_frame = receptionist_frame_columns +
                    receptionist_animation_step -
                    receptionist_frame_columns * 4;
            }
            frame_needs_render = true;
        }
    }

    if (elevator_transition_phase == TRANSITION_LOBBY && lobby_scene == 0 && neko_pixels != NULL) {
        if (interpolate_neko()) frame_needs_render = true;
        ++neko_animation_ticks;
        if (neko_animation_ticks >= neko_frame_delay) {
            neko_animation_ticks = 0;
            neko_animation_step =
                (neko_animation_step + 1) % neko_animation_sequence_length;
            neko_frame = neko_animation_sequence[neko_animation_step];
            frame_needs_render = true;
        }
    }

    if (elevator_transition_phase == TRANSITION_LOBBY) {
        const int floor = lobby_scene+1;
        const int target = elevator_target_level.load(std::memory_order_acquire);
        const bool at_door = !exterior_calls_enabled ||
            (elevator_current_level.load(std::memory_order_acquire) == floor &&
             (target == 0 || target == floor));
        if (!at_door && elevator_door_opening) {
            elevator_door_opening = false;
            elevator_door_closing = elevator_door_frame > 0;
            elevator_door_animation_ticks = 0;
            frame_needs_render = true;
        }
        const bool tap_requested = elevator_door_open_requested.exchange(false, std::memory_order_acquire);
        const bool arrived_for_call = exterior_waiting_for_arrival.load(std::memory_order_acquire) == floor;
        if (at_door && (tap_requested || arrived_for_call) &&
            !elevator_door_opening && !elevator_door_closing &&
            elevator_door_frame < elevator_door_animation_frame_count - 1) {
            elevator_door_opening = true;
            elevator_door_animation_ticks = 0;
        }
    }

    if (elevator_transition_phase == TRANSITION_LOBBY &&
        elevator_door_pixels != NULL && elevator_door_opening) {
        door_motion = 1;
        door_ticks = (elevator_door_animation_frame_count - 1 - elevator_door_frame) *
            elevator_door_frame_delay - elevator_door_animation_ticks;
        frame_needs_render = true;
        ++elevator_door_animation_ticks;
        if (elevator_door_animation_ticks >= elevator_door_frame_delay) {
            elevator_door_animation_ticks = 0;
            ++elevator_door_frame;
            if (elevator_door_frame >= elevator_door_animation_frame_count - 1) {
                exterior_waiting_for_arrival = 0;
                elevator_door_opening = false;
                elevator_transition_phase = TRANSITION_ZOOM_OUT;
                transition_ticks = 0;
                transition_source_valid = false;
            }
        }
    }
    if (elevator_transition_phase == TRANSITION_LOBBY &&
        elevator_door_pixels != NULL && elevator_door_closing) {
        door_motion = -1;
        door_ticks = elevator_door_frame * elevator_door_frame_delay - elevator_door_animation_ticks;
        frame_needs_render = true;
        ++elevator_door_animation_ticks;
        if (elevator_door_animation_ticks >= elevator_door_frame_delay) {
            elevator_door_animation_ticks = 0;
            if (elevator_door_frame > 0) {
                --elevator_door_frame;
            }
            if (elevator_door_frame == 0) {
                elevator_door_closing = false;
            }
        }
    }

    const bool inside_cabin = elevator_inside_scene.load(std::memory_order_acquire);
    const auto cabin_direction = inside_cabin ? elevator_travel_direction() : DIRECTION_INDICATOR_NEUTRAL;
    if (inside_cabin && emergency &&
        (elevator_transition_phase == TRANSITION_CABIN_ARRIVAL ||
         elevator_transition_phase == TRANSITION_OPEN_INSIDE_DOOR)) {
        cabin_arrival_floor = 0;
        elevator_inside_door_animation_ticks = 0;
        elevator_transition_phase = elevator_inside_door_frame == elevator_inside_door_closed_frame
            ? TRANSITION_COMPLETE : TRANSITION_CLOSE_INSIDE_DOOR;
        frame_needs_render = true;
    }
    if (inside_cabin && (elevator_transition_phase == TRANSITION_COMPLETE ||
                         elevator_transition_phase == TRANSITION_CLOSE_INSIDE_DOOR)) {
        const int arrival = cabin_arrival_signal.exchange(0, std::memory_order_acquire);
        if (arrival != 0) {
            cabin_arrival_floor = std::abs(arrival);
            cabin_arrival_shaft_step = arrival > 0 ? cabin_shaft_ascent_sign : -cabin_shaft_ascent_sign;
            cabin_arrival_tick = 0;
            cabin_brake_remainder = 0;
            elevator_transition_phase = TRANSITION_CABIN_ARRIVAL;
            frame_needs_render = true;
        } else if (elevator_transition_phase == TRANSITION_COMPLETE &&
                   elevator_is_travelling(cabin_direction) &&
                   elevator_inside_door_frame > elevator_inside_door_closed_frame) {
            elevator_inside_door_animation_ticks = 0;
            elevator_transition_phase = TRANSITION_CLOSE_INSIDE_DOOR;
        }
    }

    const int inside_door_command =
        elevator_inside_door_command.exchange(-1, std::memory_order_acquire);
    if (inside_cabin && inside_door_command >= 3 && !elevator_is_travelling(cabin_direction)) {
        const auto phase = elevator_transition_phase.load(std::memory_order_acquire);
        const int door_frame = elevator_inside_door_frame.load(std::memory_order_acquire);
        const bool reversing = phase == TRANSITION_CLOSE_INSIDE_DOOR || phase == TRANSITION_CABIN_ARRIVAL;
        if (inside_door_command == 3 && (phase == TRANSITION_COMPLETE ||
            (reversing && elevator_current_level.load(std::memory_order_acquire) > 0))) {
            if (reversing && elevator_inside_door_animation_ticks > 0) {
                // Preserve the visible position when changing from closing to opening.
                --elevator_inside_door_frame;
                elevator_inside_door_animation_ticks = elevator_door_frame_delay - elevator_inside_door_animation_ticks;
            }
            cabin_arrival_floor = 0;
            elevator_transition_phase = elevator_inside_door_frame == elevator_inside_door_open_frame
                ? TRANSITION_COMPLETE : TRANSITION_OPEN_INSIDE_DOOR;
            frame_needs_render = true;
        } else if ((inside_door_command == 4 || inside_door_command == 5) &&
                   phase == TRANSITION_COMPLETE && door_frame == elevator_inside_door_open_frame) {
            elevator_inside_door_animation_ticks = 0;
            returning_to_lobby = inside_door_command == 5;
            if (inside_door_command == 5) {
                elevator_panel_pressed_button.store(-1, std::memory_order_release);
                elevator_panel_rendered_button = -1;
                background_cache_valid = false;
                transition_source_valid = false;
                elevator_transition_phase = TRANSITION_ZOOM_OUT;
                transition_ticks = 0;
            } else {
                elevator_transition_phase = TRANSITION_CLOSE_INSIDE_DOOR;
            }
            frame_needs_render = true;
        }
    }

    if (!emergency && !gpio_level_indicator_enabled.load(std::memory_order_acquire) &&
        (level_indicator_pixels != NULL ||
         elevator_inside_level_indicator_pixels != NULL)) {
        ++level_indicator_animation_ticks;
        if (level_indicator_animation_ticks >= level_indicator_frame_delay) {
            level_indicator_animation_ticks = 0;
            level_indicator_frame =
                (level_indicator_frame + 1) % level_indicator_frame_count;
            frame_needs_render = true;
        }
    }

    if (elevator_inside_scene &&
        (elevator_transition_phase == TRANSITION_COMPLETE ||
         elevator_transition_phase == TRANSITION_CABIN_ARRIVAL ||
         elevator_transition_phase == TRANSITION_CLOSE_INSIDE_DOOR)) {
        const auto requested_mode = cabin_direction == DIRECTION_INDICATOR_IDLE &&
            waiting_cabin_request.load(std::memory_order_acquire)
            ? DIRECTION_INDICATOR_WAITING : cabin_direction;

        if (requested_mode != elevator_direction_indicator_mode) {
            elevator_direction_indicator_mode = requested_mode;
            elevator_direction_indicator_frame =
                requested_mode == DIRECTION_INDICATOR_UP ? 0
                : requested_mode == DIRECTION_INDICATOR_DOWN ? 1
                : requested_mode == DIRECTION_INDICATOR_NEUTRAL ? 3 : cabin_indicator_square_frame;
            elevator_direction_indicator_animation_ticks = 0;
            elevator_direction_indicator_offset = 0;
            frame_needs_render = true;
        } else if (elevator_is_travelling(requested_mode)) {
            const int sign = requested_mode == DIRECTION_INDICATOR_UP ? cabin_shaft_ascent_sign : -cabin_shaft_ascent_sign;
            advance_cabin_shaft(sign * cabin_shaft_speed);
            ++elevator_direction_indicator_animation_ticks;
            if (elevator_direction_indicator_animation_ticks >=
                elevator_direction_indicator_frame_delay) {
                elevator_direction_indicator_animation_ticks = 0;
                int layout_x, layout_y, layout_width, layout_height;
                calculate_background_layout(
                    screen_width, screen_height, img_width, img_height,
                    &layout_x, &layout_y, &layout_width, &layout_height);
                const int period =
                    get_elevator_direction_indicator_scroll_period(layout_height);
                if (period > 0) {
                    elevator_direction_indicator_offset =
                        (elevator_direction_indicator_offset + 1) % period;
                    frame_needs_render = true;
                }
            }
        } else if (requested_mode == DIRECTION_INDICATOR_WAITING) {
            ++elevator_direction_indicator_animation_ticks;
            if (elevator_direction_indicator_animation_ticks >=
                direction_wait_blink_ticks) {
                elevator_direction_indicator_animation_ticks = 0;
                elevator_direction_indicator_frame = elevator_direction_indicator_frame == cabin_indicator_square_frame
                    ? cabin_indicator_center_off : cabin_indicator_square_frame;
                frame_needs_render = true;
            }
        }
    }

    if (elevator_transition_phase == TRANSITION_LOBBY && lobby_scene == 0 &&
        agent_shadow_pixels != NULL) {
        ++agent_shadow_animation_ticks;
        if (packed_layout && agent_shadow_animation_active) frame_needs_render = true;
        if (!agent_shadow_animation_active &&
            agent_shadow_animation_ticks >= agent_shadow_idle_duration_render_frames) {
            agent_shadow_animation_ticks = 0;
            agent_shadow_frame = agent_shadow_animation_first_frame;
            agent_shadow_animation_active = true;
            frame_needs_render = true;
        } else if (agent_shadow_animation_active &&
                   agent_shadow_animation_ticks >= agent_shadow_frame_delay) {
            agent_shadow_animation_ticks = 0;
            if (agent_shadow_frame + 1 >= agent_shadow_frame_columns) {
                agent_shadow_frame = 0;
                agent_shadow_animation_active = false;
            } else {
                ++agent_shadow_frame;
            }
            frame_needs_render = true;
        }
    }

    if (elevator_transition_phase == TRANSITION_ZOOM_OUT) {
        if (transition_ticks == 0 && scene_sound_callback)
            scene_sound_callback(scene_sound_context);
        ++transition_ticks;
        if (transition_ticks >= elevator_transition_duration_frames) {
            if (stairs_transition_destination >= 0) {
                stairs_menu_visible = stairs_transition_destination == 0;
                if (!stairs_menu_visible) select_lobby(stairs_transition_destination);
                stairs_rendered_button = -1;
                stairs_press_ticks = 0;
            } else if (returning_to_lobby) {
                const int level = elevator_last_known_level.load(std::memory_order_acquire);
                if (level > 0) select_lobby(level);
                current_pixels = lobby_pixels;
                img_width = lobby_width;
                img_height = lobby_height;
                elevator_inside_scene = false;
                elevator_door_frame = elevator_door_animation_frame_count - 1;
                elevator_door_animation_ticks = 0;
                elevator_door_opening = false;
                elevator_door_closing = true;
            } else {
                current_pixels = elevator_inside_pixels;
                img_width = elevator_inside_width;
                img_height = elevator_inside_height;
                elevator_inside_scene = true;
                elevator_inside_door_frame = elevator_inside_door_open_frame;
                elevator_inside_door_animation_ticks = 0;
            }
            background_cache_valid = false;
            elevator_transition_phase = TRANSITION_ZOOM_IN;
            transition_ticks = 0;
            transition_source_valid = false;
        }
        frame_needs_render = true;
    } else if (elevator_transition_phase == TRANSITION_ZOOM_IN) {
        ++transition_ticks;
        if (transition_ticks >= elevator_transition_duration_frames) {
            if (stairs_transition_destination >= 0) {
                elevator_transition_phase = stairs_menu_visible ? TRANSITION_STAIRS_MENU : TRANSITION_LOBBY;
                stairs_transition_destination = -1;
            } else if (returning_to_lobby) {
                elevator_transition_phase = TRANSITION_LOBBY;
                returning_to_lobby = false;
                elevator_door_open_requested.store(
                    false, std::memory_order_release);
            } else {
                elevator_transition_phase = TRANSITION_CLOSE_INSIDE_DOOR;
            }
            transition_ticks = 0;
        }
        frame_needs_render = true;
    } else if (elevator_transition_phase == TRANSITION_CABIN_ARRIVAL) {
        const bool closed = advance_cabin_door_close(door_motion, door_ticks);
        const int stop_after = cabin_brake_ticks + cabin_stop_ticks;
        if (cabin_arrival_tick < stop_after) ++cabin_arrival_tick;
        if (cabin_arrival_tick <= cabin_brake_ticks) {
            // Keep the fractional travel so the scroll slows smoothly to zero.
            cabin_brake_remainder += cabin_shaft_speed * (cabin_brake_ticks - cabin_arrival_tick);
            advance_cabin_shaft(cabin_arrival_shaft_step * (cabin_brake_remainder / cabin_brake_ticks));
            cabin_brake_remainder %= cabin_brake_ticks;
        } else if (closed && cabin_arrival_tick == stop_after && at_cabin_arrival_floor(cabin_arrival_floor)) {
            elevator_inside_door_animation_ticks = 0;
            elevator_transition_phase = TRANSITION_OPEN_INSIDE_DOOR;
            frame_needs_render = true;
        }
    } else if (elevator_transition_phase == TRANSITION_OPEN_INSIDE_DOOR) {
        frame_needs_render = true;
        if (cabin_arrival_floor && !at_cabin_arrival_floor(cabin_arrival_floor)) {
            // Close safely and retry opening once the selected floor is confirmed again.
            if (elevator_inside_door_animation_ticks > 0) {
                ++elevator_inside_door_frame;
                elevator_inside_door_animation_ticks = elevator_door_frame_delay - elevator_inside_door_animation_ticks;
            }
            elevator_transition_phase = TRANSITION_CABIN_ARRIVAL;
        } else {
            door_motion = 1;
            door_ticks = (elevator_inside_door_open_frame - elevator_inside_door_frame) *
                elevator_door_frame_delay - elevator_inside_door_animation_ticks;
            if (++elevator_inside_door_animation_ticks >= elevator_door_frame_delay) {
                elevator_inside_door_animation_ticks = 0;
                if (elevator_inside_door_frame < elevator_inside_door_open_frame) {
                    ++elevator_inside_door_frame;
                }
                if (elevator_inside_door_frame == elevator_inside_door_open_frame) {
                    cabin_arrival_floor = 0;
                    elevator_transition_phase = TRANSITION_COMPLETE;
                }
            }
        }
    } else if (elevator_transition_phase == TRANSITION_CLOSE_INSIDE_DOOR) {
        if (advance_cabin_door_close(door_motion, door_ticks)) {
            elevator_transition_phase = TRANSITION_COMPLETE;
        }
    }

    unsigned motion_volume = 0;
    if (inside_cabin) {
        const auto phase = elevator_transition_phase.load(std::memory_order_acquire);
        if (phase == TRANSITION_CABIN_ARRIVAL && cabin_arrival_tick < cabin_brake_ticks) {
            motion_volume = 100 * (cabin_brake_ticks - cabin_arrival_tick) / cabin_brake_ticks;
        } else if ((phase == TRANSITION_COMPLETE || phase == TRANSITION_CLOSE_INSIDE_DOOR) &&
                   elevator_is_travelling(cabin_direction)) {
            motion_volume = 100;
        }
    }
    if (motion_volume != cabin_motion_volume) {
        cabin_motion_volume = motion_volume;
        if (cabin_motion_sound_callback) cabin_motion_sound_callback(motion_volume, cabin_motion_sound_context);
    }

    if (door_motion != previous_door_motion) {
        if (door_ticks && door_sound_callback)
            door_sound_callback(door_motion > 0, door_ticks, door_sound_context);
        previous_door_motion = door_motion;
    }

    const int pressed_button =
        elevator_panel_pressed_button.load(std::memory_order_acquire);
    if (pressed_button != elevator_panel_rendered_button) {
        elevator_panel_rendered_button = pressed_button;
        frame_needs_render = true;
    }
    if (!elevator_inside_scene && !stairs_menu_visible && variant_pack) {
        const bool waiting = elevator_transition_phase == TRANSITION_LOBBY &&
            elevator_options.timer_enabled.load() && waiting_hall_request.load();
        const auto direction = waiting ? DIRECTION_INDICATOR_WAITING : elevator_display_direction();
        if (direction != exterior_direction_mode) {
            exterior_direction_mode = direction;
            exterior_direction_ticks = 0;
            frame_needs_render = true;
        } else if (elevator_transition_phase == TRANSITION_LOBBY &&
                   (waiting || elevator_is_travelling(direction))) {
            const int delay = waiting ? direction_wait_blink_ticks : exterior_scroll_delay;
            const int phases = waiting ? 2 : exterior_scroll_period;
            exterior_direction_ticks = (exterior_direction_ticks + 1) % (delay * phases);
            if (exterior_direction_ticks % delay == 0) frame_needs_render = true;
        }
    }
}

static void update_elevator_hint(unsigned ticks, bool during_intro) {
    const bool activity = lobby_input_activity.exchange(false, std::memory_order_acq_rel);
    int offset = -1;
    if (during_intro || activity || stairs_pointer_down || elevator_options.open.load() ||
        waiting_hall_request.load() || !variant_pack ||
        elevator_transition_phase != TRANSITION_LOBBY || elevator_door_open_requested ||
        elevator_door_opening || elevator_door_closing || exterior_waiting_for_arrival) {
        elevator_hint_idle_ticks = 0;
    } else {
        elevator_hint_idle_ticks += ticks;
        if (elevator_hint_idle_ticks >= elevator_hint_interval) {
            const int age = (elevator_hint_idle_ticks - elevator_hint_interval) % elevator_hint_hop;
            elevator_hint_idle_ticks = elevator_hint_interval + age;
            constexpr int peak = elevator_hint_hop * elevator_hint_hop / 4;
            offset = (elevator_hint_rise * age * (elevator_hint_hop - age) + peak / 2) / peak;
        }
    }
    if (offset != elevator_hint_offset) {
        const bool appeared = elevator_hint_offset < 0 && offset >= 0;
        elevator_hint_offset = offset;
        frame_needs_render = true;
        if (appeared && hint_sound_callback) hint_sound_callback(hint_sound_context);
    }
}

static void update_options_animation(unsigned elapsed_us) {
    const unsigned previous = options_animation_us;
    if (elevator_transition_phase.load() != TRANSITION_LOBBY || elevator_inside_scene.load()) {
        elevator_options.open.store(false);
        options_animation_us = 0;
    } else if (elevator_options.open.load()) {
        options_animation_us = std::min(options_animation_duration_us, options_animation_us + elapsed_us);
    } else {
        options_animation_us -= std::min(options_animation_us, elapsed_us);
    }
    const bool changed = options_dirty.exchange(false, std::memory_order_acq_rel);
    if (changed) ++options_revision;
    if (changed || previous != options_animation_us) frame_needs_render = true;
}

static void draw_options_overlay(uint16_t* framebuffer, int sw, int sh, bool repaint,
                                 bool lobby_hud, bool people_visible, const OptionsLayout& layout,
                                 int height, unsigned gear_pose) {
    BridgeRect viewport;
    calculate_background_layout(sw, sh, BRIDGE_PANEL_WIDTH, BRIDGE_PANEL_HEIGHT,
        &viewport.x, &viewport.y, &viewport.w, &viewport.h);
    const OptionsPainter painter = {framebuffer, sw, viewport, repaint ? 0 : dirty_top, repaint ? sh : dirty_bottom};
    const auto repaint_tile = [&](BridgeRect r) {
        const auto tile = painter.screen_rect(r);
        const int top = std::max(painter.top, tile.y);
        const int bottom = std::min(painter.bottom, tile.y + tile.h);
        if (top >= bottom || tile.w <= 0) return false;
        dirty_top = std::min(dirty_top, top);
        dirty_bottom = std::max(dirty_bottom, bottom);
        return true;
    };
    const OptionsView view = {bridge_get_sfx_volume(), bridge_get_music_volume(),
        elevator_options.door_seconds.load(), visible_queue_count.load(), elevator_options.capacity.load(),
        elevator_options.muted.load()};
    if (lobby_hud && repaint_tile(gear_tile)) draw_gear_tile(painter, gear_pose);
    if (lobby_hud && repaint_tile(speaker_tile)) draw_speaker_tile(painter, view.muted);
    if (people_visible && repaint_tile(people_tile)) draw_people_tile(painter, view);
    if (height && repaint_tile({scroll_left,scroll_top,scroll_width,height}))
        draw_options_panel(painter, view, layout, height);
}

static bool render_frame_ticks(uint16_t* framebuffer, int screen_width, int screen_height, unsigned ticks,
                               unsigned elapsed_us, bool during_intro = false) {
    frame_was_rendered = false;
    if (framebuffer == NULL || screen_width <= 0 || screen_height <= 0) {
        return false;
    }
    if (static_cast<size_t>(screen_width) >
        SIZE_MAX / static_cast<size_t>(screen_height)) {
        return false;
    }
    const size_t pixel_count = static_cast<size_t>(screen_width) * screen_height;
    if (pixel_count > SIZE_MAX / sizeof(uint16_t)) {
        return false;
    }

    for (unsigned i = 0; i < ticks; ++i) advance_animation(screen_width, screen_height);
    update_elevator_hint(ticks, during_intro);
    // The scroll follows the display refresh even when no simulation tick elapsed.
    update_options_animation(elapsed_us);
    if (frame_needs_render) {
        ++render_revision;
        if (render_revision == 0) {
            render_revision = 1;
            memset(rendered_framebuffers, 0, sizeof(rendered_framebuffers));
        }
        frame_needs_render = false;
    }

    int cache_index = -1;
    for (int i = 0; i < 2; ++i) {
        if (rendered_framebuffers[i].framebuffer == framebuffer) {
            cache_index = i;
            break;
        }
        if (rendered_framebuffers[i].framebuffer == NULL && cache_index < 0) {
            cache_index = i;
        }
    }
    if (cache_index < 0) {
        cache_index = next_framebuffer_cache;
        next_framebuffer_cache ^= 1;
    }

    rendered_framebuffer_cache_t* cache = &rendered_framebuffers[cache_index];
    const bool inside = elevator_inside_scene.load(std::memory_order_acquire);
    const bool zooming = elevator_transition_phase == TRANSITION_ZOOM_OUT ||
                         elevator_transition_phase == TRANSITION_ZOOM_IN;
    // The intro doors cover this scene, including its corner icons, as they open.
    const bool hud_visible = !inside && variant_pack &&
        elevator_transition_phase.load() == TRANSITION_LOBBY;
    const bool people_visible = !during_intro && !zooming && variant_pack && inside &&
        elevator_options.queue_enabled.load();
    const OptionsLayout options_layout = {elevator_options.timer_enabled.load(), elevator_options.queue_enabled.load()};
    const int options_height = hud_visible && !during_intro
        ? options_layout.reveal_height(options_animation_us, options_animation_duration_us) : 0;
    const unsigned gear_pose = settings_gear_pose(options_animation_us, options_animation_duration_us);
    const bool menu_visible = options_height > 0;
    options_revealed_bottom.store(menu_visible ? scroll_top + options_height - scroll_roller_h : 0,
                                  std::memory_order_release);
    const bool overlay_changed = cache->hud_visible != hud_visible ||
        cache->options_visible != menu_visible || cache->queue_visible != people_visible;
    if (cache->framebuffer == framebuffer &&
        cache->width == screen_width &&
        cache->height == screen_height &&
        !overlay_changed &&
        cache->revision == render_revision) {
        return true;
    }
    const bool erase_overlay = (cache->hud_visible && !hud_visible) ||
        (cache->queue_visible && (!people_visible || cache->options_revision != options_revision));
    const auto finish_overlay = [&](bool full) {
        if (hud_visible || people_visible)
            draw_options_overlay(framebuffer, screen_width, screen_height,
                full || overlay_changed || cache->options_revision != options_revision,
                hud_visible, people_visible, options_layout, options_height, gear_pose);
        cache->options_revision = options_revision;
        cache->gear_pose = gear_pose;
        cache->hud_visible = hud_visible;
        cache->options_visible = menu_visible;
        cache->options_height = options_height;
        cache->queue_visible = people_visible;
    };
    const int zoom_progress = elevator_transition_phase == TRANSITION_ZOOM_OUT
        ? transition_ticks : elevator_transition_duration_frames - transition_ticks;
    if (stairs_menu_visible) {
        const bool known_menu_buffer = cache->stairs_screen && cache->framebuffer == framebuffer &&
            cache->width == screen_width && cache->height == screen_height && !erase_overlay;
        dirty_top = known_menu_buffer ? screen_height : 0;
        dirty_bottom = known_menu_buffer ? 0 : screen_height;
        if (!known_menu_buffer) {
            if (panel_graphics && screen_width == BRIDGE_PANEL_WIDTH &&
                screen_height == BRIDGE_PANEL_HEIGHT && panel_graphics->clear_screen) {
                if (!panel_graphics->clear_screen(framebuffer)) return false;
            } else {
                memset(framebuffer, 0, pixel_count * sizeof(*framebuffer));
            }
        }
        int menu_zoom, menu_brightness;
        bridge_zoom_values(zooming ? zoom_progress : 0, &menu_zoom, &menu_brightness);
        draw_stairs_menu(framebuffer, screen_width, screen_height, menu_zoom, menu_brightness,
                         known_menu_buffer ? cache->stairs_zoom : 0);
        finish_overlay(!known_menu_buffer);
        cache->normal_scene = false;
        cache->stairs_screen = true;
        cache->stairs_zoom = menu_brightness ? menu_zoom : 0;
        cache->framebuffer = framebuffer;
        cache->width = screen_width;
        cache->height = screen_height;
        cache->revision = render_revision;
        frame_was_rendered = true;
        return true;
    }
    const int scene = inside ? BRIDGE_CABIN : lobby_scene;
    const bool accelerated = panel_graphics && screen_width == BRIDGE_PANEL_WIDTH && screen_height == BRIDGE_PANEL_HEIGHT;
    const bool fixed_layout = packed_layout && screen_width == BRIDGE_PANEL_WIDTH &&
        screen_height == BRIDGE_PANEL_HEIGHT && !zooming &&
        level_indicator_render_width == level_indicator_default_render_width &&
        level_indicator_render_height == level_indicator_default_render_height;
    const bool partial = fixed_layout && !erase_overlay && (accelerated || background_cache_valid) &&
        cache->normal_scene && cache->revision &&
        cache->framebuffer == framebuffer && cache->width == screen_width &&
        cache->height == screen_height && cache->inside == inside &&
        (inside || cache->frames.lobby == lobby_scene);
    const bool repair_scroll = partial && cache->options_height && cache->options_height != options_height;
    const bool exterior_between = elevator_door_animation_ticks > 0;
    const bool inside_between = elevator_inside_door_animation_ticks > 0;
    const bool closing_inside = elevator_transition_phase == TRANSITION_CLOSE_INSIDE_DOOR ||
        elevator_transition_phase == TRANSITION_CABIN_ARRIVAL;
    const int door_progress = exterior_between && elevator_door_closing ?
        elevator_door_frame_delay - elevator_door_animation_ticks : elevator_door_animation_ticks;
    const int inside_progress = inside_between && closing_inside ?
        elevator_door_frame_delay - elevator_inside_door_animation_ticks : elevator_inside_door_animation_ticks;
    const LayerFrames previous = cache->frames;
    LayerFrames state = {receptionist_frame, neko_frame, agent_shadow_frame,
        elevator_door_frame - (exterior_between && elevator_door_closing), level_indicator_frame,
        elevator_inside_door_frame.load() - (inside_between && closing_inside),
        elevator_direction_indicator_frame, elevator_direction_indicator_offset,
        elevator_panel_rendered_button, cabin_emergency_latched.load(std::memory_order_acquire)
            ? level_indicator_err_frame : gpio_level_indicator_enabled.load()
            ? gpio_level_indicator_frame.load() : -1, door_progress, inside_progress,
        interpolate_neko() ? neko_animation_ticks : 0, lobby_scene,
        receptionist_blend_ticks(),
        packed_layout && agent_shadow_animation_active ? agent_shadow_animation_ticks + 1 : 0, lobby_actor_ticks,
        stairs_rendered_button, stairs_press_ticks, exterior_direction_mode,
        exterior_direction_ticks / (exterior_direction_mode == DIRECTION_INDICATOR_WAITING
            ? direction_wait_blink_ticks : exterior_scroll_delay), cabin_shaft_offset, elevator_hint_offset};
    const bool door_or_level = !partial || repair_scroll || previous.door != state.door ||
        previous.door_progress != state.door_progress || previous.level != state.level;
    dirty_top = partial ? screen_height : 0;
    dirty_bottom = partial ? 0 : screen_height;
    cache->normal_scene = false;
    cache->stairs_screen = false;
    cache->framebuffer = framebuffer;
    cache->width = screen_width;
    cache->height = screen_height;
    cache->revision = 0;

    const bool transition_active = zooming;
    if (!accelerated && (background_cache_width != screen_width ||
        background_cache_height != screen_height)) {
        transition_source_valid = false;
    }
    const bool capture_transition_source =
        transition_active && !transition_source_valid;
    const bool compose_scene = !transition_active || capture_transition_source;

    const bool stairs_lobby_transition = stairs_transition_destination >= 0 && !inside;
    if (accelerated && capture_transition_source && !inside && panel_graphics->start_zoom) {
        // The panel can prepare the stationary background while the CPU
        // freezes the moving layers in the separate snapshot.
        panel_graphics->start_zoom(framebuffer, zoom_progress, scene, stairs_lobby_transition);
    }

    uint16_t* output = framebuffer;
    if (accelerated) {
        active_background = inside ? panel_graphics->cabin : panel_graphics->lobbies[scene];
        if (capture_transition_source) {
            framebuffer = panel_graphics->snapshot;
            frozen_layer_count = 0;
            if (!inside) {
                frozen_door = stairs_lobby_transition ? nullptr : &open_doors[scene];
                memset(initialized_tiles, 0, sizeof(initialized_tiles));
                collect_layers = true;
            }
        }
    } else {
        active_background = background_cache;
    }
    if (compose_scene && !partial) {
        if (accelerated) {
            if ((!capture_transition_source || inside) &&
                !panel_graphics->copy_columns(framebuffer, 0, active_background, 0, screen_width)) return false;
        } else if (current_pixels != NULL && img_width > 0 && img_height > 0) {
            if (!prepare_background_cache(screen_width, screen_height)) {
                return false;
            }
            active_background = background_cache;
            memcpy(framebuffer, background_cache,
                   pixel_count * sizeof(*framebuffer));
        } else {
            memset(framebuffer, 0, pixel_count * sizeof(*framebuffer));
        }
    }

    if (compose_scene && current_pixels != NULL &&
        img_width > 0 && img_height > 0) {
        int dest_x, dest_y, dest_width, dest_height;
        calculate_background_layout(
            screen_width, screen_height, &dest_x, &dest_y, &dest_width, &dest_height);
        if (repair_scroll) {
            // The paper is opaque. Only the old roller and the newly exposed
            // area need the lobby underneath to be restored.
            const int start = std::max(0, scroll_roller_offset(std::min(options_height, cache->options_height)) - 4);
            restore_region(true, framebuffer, screen_width, screen_height, scroll_left, scroll_top + start,
                           scroll_width, cache->options_height - start);
            dirty_bottom = std::max(dirty_bottom, scroll_top + options_height);
        }
        if (partial && hud_visible && (cache->options_revision != options_revision || cache->gear_pose != gear_pose)) {
            restore_region(true, framebuffer, screen_width, screen_height, gear_tile.x, gear_tile.y,
                           gear_tile.w, gear_tile.h);
            if (cache->options_revision != options_revision)
                restore_region(true, framebuffer, screen_width, screen_height, speaker_tile.x, speaker_tile.y,
                               speaker_tile.w, speaker_tile.h);
        }

        if ((!partial || previous.inside_door != state.inside_door ||
             previous.inside_door_progress != state.inside_door_progress) && inside) {
            draw_scene_door(framebuffer, screen_width, screen_height, true, state.inside_door,
                            dest_x, dest_y, dest_width, dest_height, partial, NULL, state.inside_door_progress);
        }

        if ((!partial || previous.direction != state.direction || previous.direction_offset != state.direction_offset) &&
            elevator_inside_scene &&
            elevator_direction_indicator_pixels != NULL &&
            elevator_direction_indicator_width > 0 &&
            elevator_direction_indicator_height > 0) {
            draw_elevator_direction_indicator(
                framebuffer, screen_width, screen_height,
                dest_x, dest_y, dest_width, dest_height,
                elevator_direction_indicator_left_x);
            draw_elevator_direction_indicator(
                framebuffer, screen_width, screen_height,
                dest_x, dest_y, dest_width, dest_height,
                elevator_direction_indicator_right_x);
        }

        if ((!partial || previous.level != state.level) && elevator_inside_scene &&
            elevator_inside_level_indicator_pixels != NULL &&
            elevator_inside_level_indicator_width > 0 &&
            elevator_inside_level_indicator_height > 0) {
            const int frame_width =
                elevator_inside_level_indicator_width /
                elevator_inside_level_indicator_frame_count;
            const int display_width = static_cast<int>(
                static_cast<int64_t>(
                    elevator_inside_level_indicator_render_width) *
                    dest_width / img_width);
            const int display_height = static_cast<int>(
                static_cast<int64_t>(
                    elevator_inside_level_indicator_render_height) *
                    dest_height / img_height);
            const int display_x = dest_x + static_cast<int>(
                static_cast<int64_t>(elevator_inside_level_indicator_x) *
                    dest_width / img_width);
            const int display_y = dest_y + static_cast<int>(
                static_cast<int64_t>(elevator_inside_level_indicator_y) *
                    dest_height / img_height);
            const int frame = level_indicator_frame;
            const int source_width = compact_inside_levels ? frame_width :
                elevator_inside_level_indicator_frame_widths[frame];
            const int source_height = compact_inside_levels ? elevator_inside_level_indicator_height :
                elevator_inside_level_indicator_frame_heights[frame];
            int sprite_width = display_width;
            int sprite_height = static_cast<int>(
                (static_cast<int64_t>(source_height) * sprite_width +
                 source_width / 2) / source_width);
            if (sprite_height > display_height) {
                sprite_height = display_height;
                sprite_width = static_cast<int>(
                    (static_cast<int64_t>(source_width) * sprite_height +
                     source_height / 2) / source_height);
            }
            const int sprite_x =
                display_x + (display_width - sprite_width) / 2;
            const int sprite_y =
                display_y + (display_height - sprite_height) / 2;
            restore_region(partial, framebuffer, screen_width, screen_height,
                           display_x, display_y, display_width, display_height);
            draw_transparent_sprite(
                framebuffer, screen_width, screen_height,
                elevator_inside_level_indicator_pixels,
                elevator_inside_level_indicator_width,
                elevator_inside_level_indicator_height,
                level_indicator_frame * frame_width +
                    (compact_inside_levels ? 0 : elevator_inside_level_indicator_frame_x_offsets[frame]),
                compact_inside_levels ? 0 : elevator_inside_level_indicator_frame_y_offsets[frame],
                source_width, source_height,
                sprite_x, sprite_y, sprite_width, sprite_height);
        }

        if ((!partial || previous.pressed_button != state.pressed_button ||
             previous.light_level != state.light_level) &&
            elevator_inside_scene && elevator_panel_pixels != NULL &&
            elevator_panel_width > 0 && elevator_panel_height > 0) {
            const int panel_width = static_cast<int>(
                static_cast<int64_t>(elevator_panel_source_width) *
                    dest_width / img_width);
            const int panel_height = static_cast<int>(
                static_cast<int64_t>(elevator_panel_source_height) *
                    dest_height / img_height);
            const int panel_x = dest_x + static_cast<int>(
                static_cast<int64_t>(elevator_panel_source_x) *
                    dest_width / img_width);
            const int panel_y = dest_y + static_cast<int>(
                static_cast<int64_t>(elevator_panel_source_y) *
                    dest_height / img_height);
            restore_region(partial, framebuffer, screen_width, screen_height,
                           panel_x, panel_y, panel_width, panel_height);
            draw_elevator_panel_controls(
                framebuffer, screen_width, screen_height,
                panel_x, panel_y, panel_width, panel_height,
                state);
        }

        if (inside && (!partial || shaft_pose(previous.shaft_offset) != shaft_pose(state.shaft_offset) ||
                       previous.pressed_button != state.pressed_button ||
                       previous.light_level != state.light_level)) {
            const CabinShaft& shaft = accelerated ? panel_shaft : software_shaft;
            draw_cabin_shaft(shaft, framebuffer, screen_width, screen_height, shaft_pose(state.shaft_offset));
        }

        if (door_or_level && !inside && !(accelerated && stairs_lobby_transition)) {
            draw_scene_door(framebuffer, screen_width, screen_height, false, state.door,
                            dest_x, dest_y, dest_width, dest_height, partial, NULL, state.door_progress);
        }

        if (door_or_level && !elevator_inside_scene &&
            level_indicator_pixels != NULL && level_indicator_width > 0 &&
            level_indicator_height > 0) {
            const int frame_width = level_indicator_width / level_indicator_frame_count;
            const int sprite_width = static_cast<int>(
                static_cast<int64_t>(level_indicator_render_width) * dest_width / img_width);
            const int sprite_height = static_cast<int>(
                static_cast<int64_t>(level_indicator_render_height) * dest_height / img_height);
            const int visible_width = static_cast<int>(
                static_cast<int64_t>(sprite_width) * level_indicator_panel_width / frame_width);
            const int sprite_x = dest_x + static_cast<int>(
                static_cast<int64_t>(level_indicator_center_x) * dest_width / img_width) -
                visible_width / 2;
            const int sprite_y = dest_y + static_cast<int>(
                static_cast<int64_t>(level_indicator_y) * dest_height / img_height);
            if (sprite_width <= 0 || sprite_height <= 0) return false;
            record_layer(sprite_x, sprite_y, sprite_width, sprite_height);
            const int source_x_step = frame_width / sprite_width;
            const int source_x_remainder_step = frame_width % sprite_width;

            for (int y = 0; y < sprite_height; ++y) {
                const int source_y = static_cast<int>(
                    static_cast<int64_t>(y) * level_indicator_height / sprite_height);
                const int dest_row = sprite_y + y;
                if (dest_row < 0 || dest_row >= screen_height) {
                    continue;
                }

                int source_x = 0;
                int source_x_remainder = 0;
                for (int x = 0; x < sprite_width; ++x) {
                    const int dest_col = sprite_x + x;
                    if (dest_col < 0 || dest_col >= screen_width) {
                        source_x += source_x_step;
                        source_x_remainder += source_x_remainder_step;
                        if (source_x_remainder >= sprite_width) {
                            ++source_x;
                            source_x_remainder -= sprite_width;
                        }
                        continue;
                    }

                    uint16_t color =
                        level_indicator_pixels[source_y * level_indicator_width + source_x];
                    if (color == level_indicator_number_color ||
                        color == level_indicator_error_color) {
                        color = level_indicator_face_color;
                    }
                    if (color != 0xF81F) {
                        framebuffer[dest_row * screen_width + dest_col] = color;
                    }
                    source_x += source_x_step;
                    source_x_remainder += source_x_remainder_step;
                    if (source_x_remainder >= sprite_width) {
                        ++source_x;
                        source_x_remainder -= sprite_width;
                    }
                }
            }

            for (int y = 0; y < sprite_height; ++y) {
                const int base_source_y = static_cast<int>(
                    static_cast<int64_t>(y) * level_indicator_height / sprite_height);
                const int source_y =
                    base_source_y - level_indicator_frame_y_offsets[level_indicator_frame];
                if (source_y < 0 || source_y >= level_indicator_height) {
                    continue;
                }

                const int dest_row = sprite_y + y;
                if (dest_row < 0 || dest_row >= screen_height) {
                    continue;
                }

                int base_source_x = 0;
                int source_x_remainder = 0;
                for (int x = 0; x < sprite_width; ++x) {
                    const int dest_col = sprite_x + x;
                    if (dest_col < 0 || dest_col >= screen_width) {
                        base_source_x += source_x_step;
                        source_x_remainder += source_x_remainder_step;
                        if (source_x_remainder >= sprite_width) {
                            ++base_source_x;
                            source_x_remainder -= sprite_width;
                        }
                        continue;
                    }

                    const int frame_source_x = base_source_x +
                        level_indicator_frame_x_offsets[level_indicator_frame];
                    if (frame_source_x < frame_width) {
                        const int source_x = level_indicator_frame * frame_width +
                            frame_source_x;
                        const uint16_t color =
                            level_indicator_pixels[source_y * level_indicator_width + source_x];
                        if (color == level_indicator_number_color ||
                            color == level_indicator_error_color) {
                            framebuffer[dest_row * screen_width + dest_col] = color;
                        }
                    }
                    base_source_x += source_x_step;
                    source_x_remainder += source_x_remainder_step;
                    if (source_x_remainder >= sprite_width) {
                        ++base_source_x;
                        source_x_remainder -= sprite_width;
                    }
                }
            }
        }

        if ((!partial || repair_scroll || previous.receptionist != state.receptionist ||
             previous.receptionist_progress != state.receptionist_progress) && !elevator_inside_scene &&
            receptionist_pixels != NULL && receptionist_width > 0 &&
            receptionist_height > 0) {
            const int frame_column = receptionist_frame % receptionist_frame_columns;
            const int frame_row = receptionist_frame / receptionist_frame_columns;
            const int frame_x = frame_column * receptionist_width /
                receptionist_frame_columns;
            const int frame_width =
                (frame_column + 1) * receptionist_width /
                    receptionist_frame_columns -
                frame_x;
            const int frame_y =
                frame_row * receptionist_height / receptionist_frame_rows;
            const int frame_height =
                (frame_row + 1) * receptionist_height / receptionist_frame_rows -
                frame_y;
            const int sprite_width = static_cast<int>(
                static_cast<int64_t>(receptionist_render_width) * dest_width / img_width);
            const int sprite_height = static_cast<int>(
                static_cast<int64_t>(receptionist_render_height) * dest_height / img_height);
            const int sprite_x = dest_x + static_cast<int>(
                static_cast<int64_t>(receptionist_x) * dest_width / img_width);
            const int sprite_y = dest_y + static_cast<int>(
                static_cast<int64_t>(receptionist_y) * dest_height / img_height);
            restore_region(partial, framebuffer, screen_width, screen_height,
                           sprite_x, sprite_y, sprite_width, sprite_height);
            const uint16_t* atlas = lobby_scene > 0 ? variant_receptionists[lobby_scene - 1] : receptionist_pixels;
            if (state.receptionist_progress && sprite_width == frame_width && sprite_height == frame_height &&
                frame_width == 58 && frame_height == 80) {
                const uint16_t* frame = atlas + frame_y * receptionist_width + frame_x;
                draw_inbetween(framebuffer, screen_width, screen_height,
                    {sprite_x, sprite_y, sprite_width, sprite_height}, frame, frame + frame_width,
                    receptionist_width, pose_weight(state.receptionist_progress, receptionist_frame_delay));
            } else {
                draw_transparent_sprite(framebuffer, screen_width, screen_height,
                    atlas, receptionist_width, receptionist_height, frame_x, frame_y, frame_width, frame_height,
                    sprite_x, sprite_y, sprite_width, sprite_height);
            }
        }

        if ((!partial || repair_scroll || previous.neko != state.neko || previous.neko_progress != state.neko_progress) &&
            !inside && lobby_scene == 0 &&
            neko_pixels != NULL && neko_width > 0 && neko_height > 0) {
            const int frame_x =
                neko_frame * neko_width / neko_frame_columns;
            const int frame_width =
                (neko_frame + 1) * neko_width / neko_frame_columns - frame_x;
            const int frame_height = (neko_height < neko_frame_bottom ? neko_height :
                                      neko_frame_bottom) - neko_frame_top;
            const int sprite_width = static_cast<int>(
                static_cast<int64_t>(neko_render_width) * dest_width / img_width);
            const int sprite_height = static_cast<int>(
                static_cast<int64_t>(neko_render_height) * dest_height / img_height);
            const int sprite_x = dest_x + static_cast<int>(
                static_cast<int64_t>(neko_x) * dest_width / img_width);
            const int sprite_y = dest_y + static_cast<int>(
                static_cast<int64_t>(neko_y) * dest_height / img_height);
            restore_region(partial, framebuffer, screen_width, screen_height,
                           sprite_x, sprite_y, sprite_width, sprite_height);
            if (state.neko_progress && sprite_width > 0 && sprite_height > 0) {
                draw_neko_pose(framebuffer, screen_width, screen_height,
                               {sprite_x, sprite_y, sprite_width, sprite_height}, state.neko_progress);
            } else {
                draw_transparent_sprite(
                    framebuffer, screen_width, screen_height,
                    neko_pixels, neko_width, neko_height,
                    frame_x, neko_frame_top, frame_width, frame_height,
                    sprite_x, sprite_y, sprite_width, sprite_height);
            }
        }

        if ((!partial || repair_scroll || previous.agent != state.agent || previous.agent_progress != state.agent_progress) &&
            !inside && lobby_scene == 0 &&
            agent_shadow_pixels != NULL && agent_shadow_width > 0 &&
            agent_shadow_height > 0) {
            const int frame_x =
                agent_shadow_frame * agent_shadow_width / agent_shadow_frame_columns;
            const int frame_width =
                (agent_shadow_frame + 1) * agent_shadow_width /
                    agent_shadow_frame_columns -
                frame_x;
            const int sprite_width = static_cast<int>(
                static_cast<int64_t>(agent_shadow_render_width) * dest_width / img_width);
            const int sprite_height = static_cast<int>(
                static_cast<int64_t>(agent_shadow_render_height) * dest_height / img_height);
            const int sprite_x = dest_x + static_cast<int>(
                static_cast<int64_t>(agent_shadow_x) * dest_width / img_width);
            const int sprite_y = dest_y + static_cast<int>(
                static_cast<int64_t>(agent_shadow_y) * dest_height / img_height);
            restore_region(partial, framebuffer, screen_width, screen_height,
                           sprite_x, sprite_y, sprite_width, sprite_height);
            if (state.agent_progress && agent_shadow_frame_delay > 1 &&
                sprite_width == frame_width && sprite_height == agent_shadow_height &&
                frame_width == 66 && sprite_height == 111) {
                const int previous_pose = agent_shadow_frame == agent_shadow_animation_first_frame
                    ? 0 : agent_shadow_frame - 1;
                const unsigned weight = pose_weight(state.agent_progress - 1, agent_shadow_frame_delay - 1);
                const PaperTip tips[] = {paper_tips[previous_pose], paper_tips[agent_shadow_frame]};
                draw_inbetween(framebuffer, screen_width, screen_height,
                    {sprite_x, sprite_y, sprite_width, sprite_height},
                    agent_shadow_pixels + previous_pose * frame_width, agent_shadow_pixels + frame_x,
                    agent_shadow_width, weight, tips);
            } else {
                draw_transparent_sprite(framebuffer, screen_width, screen_height,
                    agent_shadow_pixels, agent_shadow_width, agent_shadow_height, frame_x, 0,
                    frame_width, agent_shadow_height, sprite_x, sprite_y, sprite_width, sprite_height);
            }
        }
        const BridgeRect room = {dest_x, dest_y, dest_width, dest_height};
        const bool hint_visible = !inside && state.elevator_hint_offset >= 0;
        const bool hint_changed = !inside && previous.elevator_hint_offset != state.elevator_hint_offset;
        // The floor 3 bench companion shares part of the hint's redraw area.
        // Clear the old hint before drawing the actors, then paint the hint on top.
        if (partial && hint_changed) {
            const auto bounds = elevator_hint_bounds(room);
            restore_region(true, framebuffer, screen_width, screen_height, bounds.x, bounds.y, bounds.w, bounds.h);
        }
        const bool redraw_actors = !inside && lobby_scene > 0 &&
            (!partial || repair_scroll || previous.actor_ticks != state.actor_ticks || hint_changed);
        if (redraw_actors) {
            draw_lobby_actors(framebuffer, screen_width, screen_height, room, partial, lobby_scene, state.actor_ticks);
        }
        if (!inside && variant_pack && (door_or_level ||
            previous.exterior_direction != state.exterior_direction || previous.exterior_scroll != state.exterior_scroll)) {
            draw_exterior_direction(framebuffer, screen_width, screen_height,
                                    {dest_x, dest_y, dest_width, dest_height}, state);
        }
        if (!inside && (!zooming || stairs_lobby_transition) && variant_pack &&
            (!partial || previous.stairs_button != state.stairs_button || previous.stairs_depth != state.stairs_depth)) {
            const int depth = state.stairs_button == 0 ? state.stairs_depth : 0;
            draw_stairs_icon(framebuffer, screen_width, screen_height, {dest_x, dest_y, dest_width, dest_height},
                             depth, partial);
        }
        if (hint_visible && (!partial || repair_scroll || hint_changed || redraw_actors)) {
            draw_elevator_hint(framebuffer, screen_width, screen_height, room, state.elevator_hint_offset);
        }
    }

    if (capture_transition_source) {
        if (accelerated) {
            collect_layers = false;
            framebuffer = output;
        } else {
            memcpy(background_cache, framebuffer, pixel_count * sizeof(*framebuffer));
            background_cache_valid = false;
        }
        transition_source_valid = true;
    }

    if (elevator_transition_phase == TRANSITION_ZOOM_OUT ||
        elevator_transition_phase == TRANSITION_ZOOM_IN) {
        int zoom, brightness;
        bridge_zoom_values(zoom_progress, &zoom, &brightness);
        const int center_x = screen_width / 2;
        const int center_y = screen_height / 2;
        if (accelerated) {
            if (!panel_graphics->zoom_fade(framebuffer, panel_graphics->snapshot,
                                           zoom_progress, scene, frozen_layers, frozen_layer_count,
                                           stairs_lobby_transition)) return false;
        } else if (!brightness) {
            memset(framebuffer, 0, pixel_count * sizeof(*framebuffer));
        } else if (zoom == 1000 && brightness == 255) {
            memcpy(framebuffer, background_cache, pixel_count * sizeof(*framebuffer));
        } else {
            // The transform is separable. Compute screen columns once, and fold
            // red and green into one small lookup table that fits in CPU cache.
            static uint16_t source_columns[BRIDGE_PANEL_WIDTH];
            static uint16_t red_green[2048];
            uint16_t blue[32];
            for (int r = 0; r < 32; ++r) {
                blue[r] = r * brightness / 255;
                for (int g = 0; g < 64; ++g) {
                    red_green[r * 64 + g] = (blue[r] << 11) | ((g * brightness / 255) << 5);
                }
            }
            const bool mapped = screen_width <= BRIDGE_PANEL_WIDTH;
            if (mapped) {
                for (int x = 0; x < screen_width; ++x) {
                    source_columns[x] = center_x + (x - center_x) * 1000 / zoom;
                }
            }
            int previous_y = -1;
            for (int y = 0; y < screen_height; ++y) {
                const int sy = center_y + static_cast<int64_t>(y - center_y) * 1000 / zoom;
                uint16_t* dst = framebuffer + (size_t)y * screen_width;
                if (sy == previous_y) {
                    memcpy(dst, dst - screen_width, screen_width * sizeof(*dst));
                    continue;
                }
                previous_y = sy;
                const uint16_t* src = background_cache + (size_t)sy * screen_width;
                for (int x = 0; x < screen_width; ++x) {
                    const int sx = mapped ? source_columns[x] :
                        center_x + static_cast<int64_t>(x - center_x) * 1000 / zoom;
                    const uint16_t pixel = src[sx];
                    dst[x] = red_green[pixel >> 5] | blue[pixel & 31];
                }
            }
        }
    }

    finish_overlay(!partial);
    cache->normal_scene = fixed_layout;
    cache->inside = inside;
    cache->frames = state;
    cache->revision = render_revision;
    frame_was_rendered = true;
    return true;
}

void bridge_frame_rows(int* top, int* bottom) {
    // Even if an older buffer already contains the desired pixels, the LCD
    // still needs to select it. One valid line is sufficient for that swap.
    *top = dirty_bottom > dirty_top ? dirty_top : 0;
    *bottom = dirty_bottom > dirty_top ? dirty_bottom : 1;
}

bool bridge_render_frame(uint16_t* framebuffer, int width, int height) {
    return render_frame_ticks(framebuffer, width, height, 1, nominal_frame_us);
}

bool bridge_render_frame_at(uint16_t* framebuffer, int width, int height, uint64_t now_us) {
    unsigned ticks = 1;
    unsigned elapsed_us = nominal_frame_us;
    if (animation_time_us != UINT64_MAX && now_us >= animation_time_us) {
        const uint64_t elapsed = now_us - animation_time_us;
        // Resume after long pauses without running minutes of stale animation.
        elapsed_us = std::min<uint64_t>(elapsed, 1000000);
        const uint32_t scaled = elapsed_us * 60 + animation_remainder;
        ticks = scaled / 1000000;
        animation_remainder = scaled % 1000000;
    } else {
        animation_remainder = 0;
    }
    animation_time_us = now_us;
    return render_frame_ticks(framebuffer, width, height, ticks, elapsed_us);
}

uint32_t bridge_frame_revision(void) {
    return render_revision;
}

bool bridge_frame_was_rendered(void) {
    return frame_was_rendered;
}

int bridge_intro_frame_at(uint32_t elapsed_ms) {
    return elapsed_ms < 1000 ? 0 : (elapsed_ms < 1015 ? 1 : 2);
}

bool bridge_render_intro(uint16_t* framebuffer, uint32_t elapsed_ms) {
    if (!framebuffer || !intro_row_indices) {
        return false;
    }
    const int frame = bridge_intro_frame_at(elapsed_ms);
    const bool native_intro = panel_graphics && panel_graphics->intro_doors;
    const bool opening = elapsed_ms >= BRIDGE_INTRO_OPEN_MS;
    if (opening) {
        uint16_t* scene = native_intro ? panel_graphics->snapshot : framebuffer;
        if (!render_frame_ticks(scene, BRIDGE_PANEL_WIDTH, BRIDGE_PANEL_HEIGHT, 1, 0, true)) return false;
    } else if (native_intro && frame == 2) {
        return panel_graphics->copy_columns(framebuffer, 0, panel_graphics->intro_doors, 0, BRIDGE_PANEL_WIDTH);
    } else {
        memset(framebuffer, 0, BRIDGE_PANEL_WIDTH * BRIDGE_PANEL_HEIGHT * sizeof(*framebuffer));
    }

    const int width = intro_rows.width;
    const int x = (BRIDGE_PANEL_WIDTH - width) / 2;
    const int half = width / 2;
    const int center = x + half;
    const int clearance = center > BRIDGE_PANEL_WIDTH - center ? center : BRIDGE_PANEL_WIDTH - center;
    const int64_t duration = BRIDGE_INTRO_END_MS - BRIDGE_INTRO_OPEN_MS;
    const int64_t progress = elapsed_ms >= BRIDGE_INTRO_END_MS ? duration :
        (opening ? elapsed_ms - BRIDGE_INTRO_OPEN_MS : 0);
    const int shift = static_cast<int>(clearance * progress * progress * (3 * duration - 2 * progress) /
                                        (duration * duration * duration));
    const int left = x - shift;
    const int skip_left = left < 0 ? -left : 0;
    const int right = x + half + shift;
    const int right_count = right + width - half <= BRIDGE_PANEL_WIDTH
        ? width - half : BRIDGE_PANEL_WIDTH - right;
    if (opening && native_intro) {
        // Partition each row into visible strips. DMA writes every pixel once.
        const int sw = BRIDGE_PANEL_WIDTH;
        const int left_start = left < 0 ? 0 : left;
        const int left_end = center - shift > 0 ? center - shift : 0;
        const int right_start = right < sw ? right : sw;
        const int right_end = right_count > 0 ? right_start + right_count : sw;
        const uint16_t* scene = panel_graphics->snapshot;
        const uint16_t* doors = panel_graphics->intro_doors;
        auto copy = panel_graphics->copy_columns;
        return copy(framebuffer, 0, scene, 0, left_start) &&
               copy(framebuffer, left_start, doors, x + skip_left, left_end - left_start) &&
               copy(framebuffer, left_end, scene, left_end, right_start - left_end) &&
               copy(framebuffer, right_start, doors, center, right_end - right_start) &&
               copy(framebuffer, right_end, scene, right_end, sw - right_end);
    }
    for (int y = 0; y < BRIDGE_PANEL_HEIGHT; ++y) {
        const int index = intro_row_indices[frame * intro_indices.width + y];
        const uint16_t* row = intro_row_pixels + index * width;
        uint16_t* dest = framebuffer + y * BRIDGE_PANEL_WIDTH;
        if (half > skip_left) {
            memcpy(dest + (left + skip_left), row + skip_left,
                   (half - skip_left) * sizeof(*dest));
        }
        if (right_count > 0) {
            memcpy(dest + right, row + half, right_count * sizeof(*dest));
        }
    }
    // The doors have modified this buffer after the live scene was cached.
    for (rendered_framebuffer_cache_t& cache : rendered_framebuffers) {
        if (cache.framebuffer == framebuffer) cache.revision = 0;
    }
    return true;
}

void bridge_cleanup(void) {
    bridge_set_button_sound_callback(nullptr, nullptr);
    bridge_set_hint_sound_callback(nullptr, nullptr);
    bridge_set_scene_sound_callback(nullptr, nullptr);
    bridge_set_hall_call_sound_callback(nullptr, nullptr);
    bridge_set_emergency_sound_callback(nullptr, nullptr);
    bridge_set_stairs_back_sound_callback(nullptr, nullptr);
    bridge_set_options_sound_callbacks(nullptr, nullptr, nullptr);
    bridge_set_slider_sound_callback(nullptr, nullptr);
    bridge_set_cabin_motion_sound_callback(nullptr, nullptr);
    bridge_set_door_sound_callback(nullptr, nullptr);
    release_cabin_shaft(panel_shaft);
    release_cabin_shaft(software_shaft);
    release_variants();
    release_door_patches();
    panel_graphics = NULL;
    free(background_cache);
    background_cache = NULL;
    background_cache_width = 0;
    background_cache_height = 0;
    background_cache_valid = false;
    intro_row_pixels = NULL;
    intro_row_indices = NULL;
    transition_source_valid = false;
    current_pixels = NULL;
    lobby_pixels = NULL;
    lobby_width = 0;
    lobby_height = 0;
    receptionist_pixels = NULL;
    neko_pixels = NULL;
    elevator_door_pixels = NULL;
    elevator_door_closing = false;
    level_indicator_pixels = NULL;
    elevator_inside_level_indicator_pixels = NULL;
    elevator_inside_level_indicator_width = 0;
    elevator_inside_level_indicator_height = 0;
    agent_shadow_pixels = NULL;
    elevator_inside_pixels = NULL;
    elevator_inside_door_pixels = NULL;
    elevator_inside_door_command.store(-1, std::memory_order_relaxed);
    elevator_inside_scene = false;
    returning_to_lobby = false;
    elevator_panel_pixels = NULL;
    elevator_buttons_pixels = NULL;
    elevator_lights_pixels = NULL;
    elevator_direction_indicator_pixels = NULL;
    elevator_direction_indicator_width = 0;
    elevator_direction_indicator_height = 0;
    elevator_panel_pressed_button.store(-1, std::memory_order_relaxed);
    elevator_current_level.store(0, std::memory_order_relaxed);
    elevator_last_known_level.store(0, std::memory_order_relaxed);
    elevator_target_level.store(0, std::memory_order_relaxed);
    elevator_indicator_test_mode.store(false, std::memory_order_relaxed);
    elevator_panel_rendered_button = -1;
    memset(rendered_framebuffers, 0, sizeof(rendered_framebuffers));
    render_revision = 1;
    next_framebuffer_cache = 0;
    frame_was_rendered = false;
    frame_needs_render = true;
}
