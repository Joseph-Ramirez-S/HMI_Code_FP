// main.c
#include "main.h"
#include "esp_lcd_panel_ops.h" 
#include "esp_heap_caps.h"     
#include "esp_partition.h"
#include "esp_system.h"
#include "engine_bridge.h"
#include "panel_graphics.h"
#include "intro_audio.h"
#include "audio_sd.h"
#include "driver/gpio.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
#include "freertos/semphr.h"

#define GPIO_OUT_LEVEL_1 GPIO_NUM_50
#define GPIO_OUT_LEVEL_2 GPIO_NUM_51
#define GPIO_OUT_LEVEL_3 GPIO_NUM_53
#define GPIO_OUT_EMERGENCY GPIO_NUM_5
#define GPIO_IN_LEVEL_1 GPIO_NUM_25
#define GPIO_IN_LEVEL_2 GPIO_NUM_27
#define GPIO_IN_LEVEL_3 GPIO_NUM_28

#define INPUT_POLL_HZ 90U
#define FRAME_BUDGET_US (1000000U / 60U)
#define FRAME_STATS_INTERVAL_US 5000000LL
#define ANIMATION_TASK_STACK_SIZE 16384U
// --- VARIABLES EXTERNAS ---
extern esp_lcd_panel_handle_t panel_handle;

static esp_ldo_channel_handle_t ldo3 = NULL;
static esp_ldo_channel_handle_t ldo4 = NULL;

static bool touch_available = false;
static atomic_bool intro_complete = ATOMIC_VAR_INIT(false);

_Static_assert(H_size == BRIDGE_PANEL_WIDTH && V_size == BRIDGE_PANEL_HEIGHT,
               "Prepare the graphics for the configured LCD resolution");

static esp_err_t map_graphics(void) {
    const esp_partition_t *partition = esp_partition_find_first(
        (esp_partition_type_t)0x40, ESP_PARTITION_SUBTYPE_ANY, "graphics");
    if (!partition) return ESP_ERR_NOT_FOUND;
    const size_t size = bridge_graphics_pack_size();
    if (size > partition->size) return ESP_ERR_INVALID_SIZE;

    const void *data = NULL;
    esp_partition_mmap_handle_t handle;
    esp_err_t err = esp_partition_mmap(partition, 0, size, ESP_PARTITION_MMAP_DATA,
                                       &data, &handle);
    if (err != ESP_OK) return err;
    if (!bridge_bind_graphics(data, size)) {
        esp_partition_munmap(handle);
        return ESP_ERR_INVALID_VERSION;
    }
    // The Bridge reads these flash pages for the lifetime of the display task.
    MAIN_INFO("Graficos RGB565 en flash: %u bytes. PSRAM libre: %u bytes",
              (unsigned)size, (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    return ESP_OK;
}

static void wait_for_input_poll(TickType_t *last_wake,
                                         uint32_t *tick_remainder) {
    *tick_remainder += configTICK_RATE_HZ % INPUT_POLL_HZ;
    TickType_t frame_ticks = configTICK_RATE_HZ / INPUT_POLL_HZ;
    if (*tick_remainder >= INPUT_POLL_HZ) {
        ++frame_ticks;
        *tick_remainder -= INPUT_POLL_HZ;
    }
    if (frame_ticks == 0) {
        frame_ticks = 1;
    }
    vTaskDelayUntil(last_wake, frame_ticks);
}

static esp_err_t elevator_gpio_init(void) {
    const gpio_config_t output_config = {
        .pin_bit_mask = (1ULL << GPIO_OUT_LEVEL_1) |
                        (1ULL << GPIO_OUT_LEVEL_2) |
                        (1ULL << GPIO_OUT_LEVEL_3) |
                        (1ULL << GPIO_OUT_EMERGENCY),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&output_config);
    if (err != ESP_OK) {
        return err;
    }

    const gpio_num_t output_pins[] = {
        GPIO_OUT_LEVEL_1, GPIO_OUT_LEVEL_2,
        GPIO_OUT_LEVEL_3, GPIO_OUT_EMERGENCY
    };
    for (size_t i = 0; i < sizeof(output_pins) / sizeof(output_pins[0]); ++i) {
        err = gpio_set_level(output_pins[i], 0);
        if (err != ESP_OK) {
            return err;
        }
    }

    const gpio_config_t input_config = {
        .pin_bit_mask = (1ULL << GPIO_IN_LEVEL_1) |
                        (1ULL << GPIO_IN_LEVEL_2) |
                        (1ULL << GPIO_IN_LEVEL_3),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    return gpio_config(&input_config);
}

static void update_elevator_gpio_outputs(void) {
    const int pressed_button = bridge_get_elevator_output_button();
    const gpio_num_t output_pins[] = {
        GPIO_OUT_LEVEL_1, GPIO_OUT_LEVEL_2,
        GPIO_OUT_LEVEL_3, GPIO_OUT_EMERGENCY
    };
    const int button_indices[] = {0, 1, 2, BRIDGE_ELEVATOR_EMERGENCY_BUTTON};
    gpio_num_t selected_pin = GPIO_NUM_NC;
    bool other_outputs_low = true;
    for (size_t i = 0; i < sizeof(output_pins) / sizeof(output_pins[0]); ++i) {
        if (pressed_button == button_indices[i]) {
            selected_pin = output_pins[i];
            continue;
        }
        const esp_err_t err = gpio_set_level(output_pins[i], 0);
        if (err != ESP_OK) {
            MAIN_ERROR("No se pudo bajar GPIO%d: %s", output_pins[i], esp_err_to_name(err));
            other_outputs_low = false;
        }
    }
    if (other_outputs_low && selected_pin != GPIO_NUM_NC) {
        const esp_err_t err = gpio_set_level(selected_pin, 1);
        if (err != ESP_OK) {
            MAIN_ERROR("No se pudo activar GPIO%d: %s", selected_pin, esp_err_to_name(err));
        }
    }
}

static int update_elevator_gpio_inputs(void) {
    static int candidate = -2, stable = -2;
    static unsigned samples;
    const unsigned contacts = (gpio_get_level(GPIO_IN_LEVEL_1) != 0) |
        ((gpio_get_level(GPIO_IN_LEVEL_2) != 0) << 1) |
        ((gpio_get_level(GPIO_IN_LEVEL_3) != 0) << 2);
    const int level = contacts == 1 ? 1 : contacts == 2 ? 2 : contacts == 4 ? 3 : contacts == 0 ? 0 : -1;
    if (candidate != level) {
        candidate = level;
        samples = 0;
    }
    if (samples < 3 && ++samples == 3 && stable != candidate) {
        stable = candidate;
        // A brief or ambiguous contact must not dispatch the next queued trip.
        const bool play_arrival = bridge_set_elevator_level(stable > 0 ? stable : 0);
        if (play_arrival && atomic_load_explicit(&intro_complete, memory_order_acquire))
            arrival_audio_request();
    }
    return samples == 3 ? stable : -1;
}

// Display initialization and submission run on the same core as the DSI DMA ISR.
static StaticSemaphore_t refresh_storage;
static SemaphoreHandle_t refresh_signal;
static atomic_uint refresh_count = ATOMIC_VAR_INIT(0);

static bool IRAM_ATTR refresh_done(esp_lcd_panel_handle_t panel,
                                   esp_lcd_dpi_panel_event_data_t *event, void *ctx) {
    BaseType_t wake = pdFALSE;
    atomic_fetch_add_explicit(&refresh_count, 1, memory_order_relaxed);
    xSemaphoreGiveFromISR(refresh_signal, &wake);
    return wake == pdTRUE;
}

static uint32_t lcd_refreshes(void) {
    return atomic_load_explicit(&refresh_count, memory_order_relaxed);
}

static void wait_for_refresh(uint32_t *previous) {
    uint32_t current;
    while ((current = lcd_refreshes()) == *previous) {
        if (xSemaphoreTake(refresh_signal, pdMS_TO_TICKS(250)) != pdTRUE &&
            lcd_refreshes() == *previous) {
            MAIN_ERROR("El LCD no ha completado un refresco en 250 ms");
        }
    }
    *previous = current;
}

static void init_fail_handler(const char *module_name, esp_err_t err);

static void animation_game_task(void *pvParameters) {
    esp_err_t err = display_init();
    if (err != ESP_OK) init_fail_handler("LCD", err);
    refresh_signal = xSemaphoreCreateBinaryStatic(&refresh_storage);
    const esp_lcd_dpi_panel_event_callbacks_t callbacks = {.on_refresh_done = refresh_done};
    err = esp_lcd_dpi_panel_register_event_callbacks(panel_handle, &callbacks, NULL);
    if (err != ESP_OK) init_fail_handler("refresco DPI", err);
    err = panel_graphics_init();
    if (err != ESP_OK) MAIN_ERROR("Aceleracion DMA2D no disponible: %s; usando CPU", esp_err_to_name(err));
    void *framebuffers[2] = { NULL, NULL };
    err = esp_lcd_dpi_panel_get_frame_buffer(
        panel_handle, 2, &framebuffers[0], &framebuffers[1]);
    if (err != ESP_OK || framebuffers[0] == NULL || framebuffers[1] == NULL) {
        MAIN_ERROR("No se pudieron obtener los framebuffers del LCD: %s",
                   esp_err_to_name(err == ESP_OK ? ESP_ERR_NO_MEM : err));
        vTaskDelete(NULL);
        return;
    }

    err = intro_audio_prepare();
    if (err != ESP_OK) MAIN_INFO("Audio microSD no disponible: %s", esp_err_to_name(err));
    err = set_lcd_blight(100);
    if (err != ESP_OK) init_fail_handler("LCD Backlight", err);

    uint8_t back_buffer_index = 1;
    uint32_t last_refresh = lcd_refreshes();
    unsigned opening_frames = 0;
    int64_t opening_start_us = 0;
    int displayed_intro_frame = -1;
    int64_t intro_start_us = 0;
    for (;;) {
        wait_for_refresh(&last_refresh);
        const int64_t elapsed_us = intro_start_us ? esp_timer_get_time() - intro_start_us : 0;
        const uint32_t elapsed_ms = (uint32_t)(elapsed_us / 1000);
        const int frame_index = bridge_intro_frame_at(elapsed_ms);
        const bool opening = elapsed_ms >= BRIDGE_INTRO_OPEN_MS;
        const bool finished = elapsed_ms >= BRIDGE_INTRO_END_MS;
        if (opening || frame_index != displayed_intro_frame) {
            const bool first_frame = displayed_intro_frame < 0;
            uint16_t *framebuffer = (uint16_t *)framebuffers[back_buffer_index];
            if (!bridge_render_intro(framebuffer, elapsed_ms)) {
                MAIN_ERROR("No se pudo componer la introduccion");
                vTaskDelete(NULL);
                return;
            }
            displayed_intro_frame = frame_index;
            err = esp_lcd_panel_draw_bitmap(
                panel_handle, 0, 0, H_size, V_size, framebuffer);
            if (err != ESP_OK) {
                MAIN_ERROR("Error transmitiendo la introduccion al LCD: %s",
                           esp_err_to_name(err));
                vTaskDelete(NULL);
                return;
            }
            if (first_frame) {
                intro_start_us = esp_timer_get_time();
                intro_audio_start();
            }
            // Wait for a boundary strictly after publishing the new buffer
            // before writing to the buffer the DMA was previously scanning.
            last_refresh = lcd_refreshes();
            back_buffer_index ^= 1;
            if (opening) {
                if (!opening_frames) opening_start_us = elapsed_us;
                ++opening_frames;
            }
        }

        if (finished) {
            break;
        }
    }
    const int64_t opening_us = esp_timer_get_time() - intro_start_us - opening_start_us;
    MAIN_INFO("Apertura de introduccion: %u cuadros en %u ms",
              opening_frames, (unsigned)(opening_us / 1000));
    panel_graphics_finish_intro();
    err = audio_sd_start_upload_service();
    if (err != ESP_OK) MAIN_ERROR("Transferencia de audio no disponible: %s", esp_err_to_name(err));
    MAIN_INFO("PSRAM al terminar la introduccion: libre %u bytes, minimo desde el arranque %u bytes",
              (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
              (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM));
    atomic_store_explicit(&intro_complete, true, memory_order_release);

    uint32_t frames_in_window = 0, submitted = 0, over_budget = 0;
    uint32_t first_refresh = lcd_refreshes(), presented_revision = 0;
    uint32_t max_render_us = 0, max_submit_us = 0;
    int pending_top = V_size, pending_bottom = 0;
    int64_t window_start_us = esp_timer_get_time();

    for (;;) {
        wait_for_refresh(&last_refresh);
        const int64_t frame_start_us = esp_timer_get_time();
        if (panel_handle == NULL) {
            MAIN_ERROR("El panel LCD no esta disponible para transmitir cuadros");
            vTaskDelete(NULL);
            return;
        }
        uint16_t *framebuffer = (uint16_t *)framebuffers[back_buffer_index];
        if (!bridge_render_frame_at(framebuffer, H_size, V_size, (uint64_t)frame_start_us)) {
            MAIN_ERROR("No se pudo componer el cuadro de animacion");
            vTaskDelete(NULL);
            return;
        }

        const int64_t render_end_us = esp_timer_get_time();
        const uint32_t revision = bridge_frame_revision();
        if (bridge_frame_was_rendered()) {
            int top, bottom;
            bridge_frame_rows(&top, &bottom);
            if (top < pending_top) pending_top = top;
            if (bottom > pending_bottom) pending_bottom = bottom;
        }
        // An unchanged front image can leave newly written rows in the
        // back buffer's CPU cache. Flush them on its next real presentation.
        if (pending_bottom > pending_top && revision != presented_revision) {
            err = esp_lcd_panel_draw_bitmap(
                panel_handle, 0, pending_top, H_size, pending_bottom, framebuffer);
            if (err != ESP_OK) {
                MAIN_ERROR("Error transmitiendo cuadro al LCD: %s",
                           esp_err_to_name(err));
                vTaskDelete(NULL);
                return;
            }
            presented_revision = revision;
            pending_top = V_size;
            pending_bottom = 0;
            last_refresh = lcd_refreshes();
            ++submitted;
            back_buffer_index ^= 1;
        }

        const int64_t frame_end_us = esp_timer_get_time();
        const uint32_t render_us = (uint32_t)(render_end_us - frame_start_us);
        const uint32_t submit_us = (uint32_t)(frame_end_us - render_end_us);
        if (render_us > max_render_us) max_render_us = render_us;
        if (submit_us > max_submit_us) max_submit_us = submit_us;
        if (render_us + submit_us > FRAME_BUDGET_US) ++over_budget;
        ++frames_in_window;
        const int64_t elapsed_us = frame_end_us - window_start_us;
        if (elapsed_us >= FRAME_STATS_INTERVAL_US) {
            const uint32_t now_refresh = lcd_refreshes();
            const unsigned lcd = (unsigned)((uint64_t)(now_refresh - first_refresh) * 10000000 / elapsed_us);
            const unsigned engine = (unsigned)((uint64_t)frames_in_window * 10000000 / elapsed_us);
            const unsigned updates = (unsigned)((uint64_t)submitted * 10000000 / elapsed_us);
            MAIN_INFO("LCD %u.%u Hz | motor %u.%u Hz | enviados %u.%u FPS | "
                      "max render/envio %u/%u us | >16.67 ms %u/%u",
                      lcd / 10, lcd % 10, engine / 10, engine % 10, updates / 10, updates % 10,
                      max_render_us, max_submit_us, over_budget, frames_in_window);
            first_refresh = now_refresh;
            frames_in_window = submitted = over_budget = max_render_us = max_submit_us = 0;
            window_start_us = frame_end_us;
        }
    }
}

// ---------------------------------------------------------
// 4. INICIALIZACIÓN DEL SISTEMA HARDWARE
// ---------------------------------------------------------
static void init_fail_handler(const char *module_name, esp_err_t err) {
    for (;;) {
        MAIN_ERROR("[%s] init failed: %d", module_name, err);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

static void system_init(void) {
    esp_err_t err = ESP_OK;

    err = elevator_gpio_init();
    if (err != ESP_OK) init_fail_handler("GPIO del ascensor", err);

    esp_ldo_channel_config_t ldo3_cof = { .chan_id = 3, .voltage_mv = 2500, };
    err = esp_ldo_acquire_channel(&ldo3_cof, &ldo3);
    if (err != ESP_OK) init_fail_handler("ldo3", err);

    esp_ldo_channel_config_t ldo4_cof = { .chan_id = 4, .voltage_mv = 3300, };
    err = esp_ldo_acquire_channel(&ldo4_cof, &ldo4);
    if (err != ESP_OK) init_fail_handler("ldo4", err);

    MAIN_INFO("Initializing I2C...");
    err = i2c_init();
    if (err != ESP_OK) init_fail_handler("I2C", err);

    MAIN_INFO("Initializing touch panel...");
    err = touch_init(); 
    if (err != ESP_OK) {
        MAIN_ERROR("El chip tactil no respondio, ignorando...");
    } else {
        touch_available = true;
    }


}

// ---------------------------------------------------------
// 5. FUNCIÓN PRINCIPAL
// ---------------------------------------------------------
void app_main(void)
{
    MAIN_INFO("Iniciando Arquitectura Unificada (Bridge PC-ESP32)...");
    MAIN_INFO("Motivo de reinicio: %d", (int)esp_reset_reason());
    system_init();
    bridge_init();
    bridge_set_button_sound_callback(button_audio_request, NULL);
    bridge_set_door_sound_callback(door_audio_request, NULL);
    bridge_set_hint_sound_callback(hint_audio_request, NULL);
    bridge_set_scene_sound_callback(scene_audio_request, NULL);
    bridge_set_hall_call_sound_callback(hall_call_audio_request, NULL);
    bridge_set_emergency_sound_callback(emergency_audio_request, NULL);
    bridge_set_cabin_motion_sound_callback(cabin_motion_audio_set, NULL);
    bridge_set_stairs_back_sound_callback(stairs_back_audio_request, NULL);
    bridge_set_options_sound_callbacks(options_open_audio_request, mute_click_audio_request, NULL);
    bridge_set_slider_sound_callback(slider_audio_request, NULL);
    esp_err_t err = map_graphics();
    if (err != ESP_OK) init_fail_handler("particion graphics (RGB565)", err);
    update_elevator_gpio_inputs();
    bridge_set_elevator_indicator_test_mode(false);
    bridge_set_exterior_calls_enabled(true);

    if (xTaskCreatePinnedToCore(animation_game_task, "Animacion_DSI",
                                ANIMATION_TASK_STACK_SIZE, NULL, 5, NULL, 1) != pdPASS) {
        init_fail_handler("tarea grafica", ESP_ERR_NO_MEM);
    }

    bool touch_was_pressed = false;
    uint16_t last_touch_x = 0, last_touch_y = 0;
    uint8_t touch_release_samples = 0;
    TickType_t input_last_wake = xTaskGetTickCount();
    uint32_t input_tick_remainder = 0;
    for (;;) {
        wait_for_input_poll(&input_last_wake, &input_tick_remainder);
        const int confirmed_floor = update_elevator_gpio_inputs();
        if (!atomic_load_explicit(&intro_complete, memory_order_acquire)) {
            continue;
        }

        uint16_t touch_x = 0xffff;
        uint16_t touch_y = 0xffff;
        bool touch_pressed = false;
        if (touch_available && touch_read() == ESP_OK) {
            get_coor(&touch_x, &touch_y, &touch_pressed);
            if (touch_pressed) {
                touch_release_samples = 0;
                last_touch_x = touch_x;
                last_touch_y = touch_y;
                if (!bridge_touch_options(touch_x, touch_y, true, H_size, V_size)) {
                    const bool stairs = bridge_touch_stairs(touch_x, touch_y, true, H_size, V_size);
                    bridge_touch_elevator_panel(touch_x, touch_y, true, H_size, V_size, confirmed_floor);
                    if (!touch_was_pressed && !stairs)
                        bridge_touch_elevator_door(touch_x, touch_y, H_size, V_size);
                }
                touch_was_pressed = true;
            } else if (touch_was_pressed && ++touch_release_samples >= 2) {
                // The GT911 can have no new report for one poll during a hold.
                bridge_touch_options(last_touch_x, last_touch_y, false, H_size, V_size);
                bridge_touch_stairs(last_touch_x, last_touch_y, false, H_size, V_size);
                bridge_touch_elevator_panel(0, 0, false, H_size, V_size, confirmed_floor);
                bridge_release_exterior_call();
                touch_was_pressed = false;
                touch_release_samples = 0;
            }
        } else {
            bridge_touch_elevator_panel(0, 0, false, H_size, V_size, confirmed_floor);
            bridge_release_exterior_call();
        }
        bridge_update_elevator_controls_at((uint64_t)esp_timer_get_time(), confirmed_floor);
        update_elevator_gpio_outputs();
    }
}
