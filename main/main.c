// main.c
#include "main.h"
#include "esp_lcd_panel_ops.h" 
#include "esp_heap_caps.h"     
#include "esp_spiffs.h"
#include <stdbool.h>
#include <stdint.h>

// --- PROTECCIÓN PARA LA MEMORIA RAM (STB_IMAGE EN PSRAM) ---
#define STBI_NO_SIMD
#define STBI_NO_THREAD_LOCALS
#define STBI_MALLOC(sz)           heap_caps_malloc(sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#define STBI_REALLOC(p,newsz)     heap_caps_realloc(p, newsz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#define STBI_FREE(p)              heap_caps_free(p)
#define STB_IMAGE_IMPLEMENTATION
#define FRAME_SPEED_TRANSITION 4
#define ANIMATION_FRAME_RATE_HZ 60U
#define FRAME_STATS_INTERVAL_US 5000000LL
#define RECEPTIONIST_FRAME_COLUMNS 12
#define RECEPTIONIST_FRAME_ROWS 2
#define ANIMATION_TASK_STACK_SIZE 16384U
#include "stb_image.h"

// 1. Incluimos nuestro intermediario
#include "engine_bridge.h" 

// --- VARIABLES EXTERNAS ---
extern esp_lcd_panel_handle_t panel_handle;

static esp_ldo_channel_handle_t ldo3 = NULL;
static esp_ldo_channel_handle_t ldo4 = NULL;

// Variable global para mantener los píxeles en memoria
static uint16_t* background_pixels = NULL;
static uint16_t* receptionist_pixels = NULL;
static uint16_t* neko_pixels = NULL;
static uint16_t* elevator_door_pixels = NULL;
static uint16_t* level_indicator_pixels = NULL;
static uint16_t* agent_shadow_pixels = NULL;
static uint16_t* elevator_inside_pixels = NULL;
static uint16_t* elevator_inside_door_pixels = NULL;
static uint16_t* elevator_panel_pixels = NULL;
static uint16_t* elevator_buttons_pixels = NULL;
static uint16_t* elevator_lights_pixels = NULL;
static bool touch_available = false;

// ---------------------------------------------------------
// 1. SISTEMA DE ARCHIVOS (LittleFS / SPIFFS)
// ---------------------------------------------------------
esp_err_t init_spiffs(void) {
    esp_vfs_spiffs_conf_t conf = {
      .base_path = "/storage",
      .partition_label = "storage",
      .max_files = 5,
      .format_if_mount_failed = true
    };
    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    if (ret != ESP_OK) {
        MAIN_ERROR("Error inicializando SPIFFS");
    } else {
        MAIN_INFO("SPIFFS montado correctamente.");
    }
    return ret;
}

// ---------------------------------------------------------
// 2. FUNCIÓN DE CARGA SEGURA PARA EL ESP32
// ---------------------------------------------------------
bool load_png_to_rgb565_esp32(const char* filepath, SpriteID sprite_id) {
    int w, h, channels;
    unsigned char* raw = stbi_load(filepath, &w, &h, &channels, 4);
    if (raw == NULL) {
        MAIN_ERROR("Fallo al decodificar %s: %s", filepath, stbi_failure_reason());
        return false;
    }

    if (w <= 0 || h <= 0 ||
        (size_t)w > SIZE_MAX / (size_t)h / 4) {
        MAIN_ERROR("Dimensiones de imagen invalidas: %s", filepath);
        stbi_image_free(raw);
        return false;
    }
    if (sprite_id == SPRITE_RECEPTIONIST &&
        (w % RECEPTIONIST_FRAME_COLUMNS != 0 ||
         h % RECEPTIONIST_FRAME_ROWS != 0)) {
        MAIN_ERROR("La hoja de la recepcionista debe tener 12 columnas y 2 filas: %s",
                   filepath);
        stbi_image_free(raw);
        return false;
    }
    if (sprite_id == SPRITE_AGENT_SHADOW && (w % 8 != 0 || h <= 0)) {
        MAIN_ERROR("La hoja del agente sombra debe tener 8 columnas: %s", filepath);
        stbi_image_free(raw);
        return false;
    }
    if (sprite_id == SPRITE_ELEVATOR_INSIDE_DOOR && (w < 8 || h <= 0)) {
        MAIN_ERROR("La hoja de la puerta interior no contiene 8 cuadros: %s", filepath);
        stbi_image_free(raw);
        return false;
    }

    const size_t pixel_count = (size_t)w * (size_t)h;
    uint16_t* pixels = (uint16_t*)raw;
    for (size_t i = 0; i < pixel_count; ++i) {
        const uint8_t r = raw[i * 4 + 0] >> 3;
        const uint8_t g = raw[i * 4 + 1] >> 2;
        const uint8_t b = raw[i * 4 + 2] >> 3;
        const uint8_t a = raw[i * 4 + 3];
        pixels[i] = a == 0 ? 0xF81F : (uint16_t)((r << 11) | (g << 5) | b);
    }

    uint16_t* compact_pixels = (uint16_t*)heap_caps_realloc(
        pixels, pixel_count * sizeof(*pixels), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (compact_pixels == NULL) {
        MAIN_ERROR("No se pudo compactar la imagen %s; se conserva el buffer original",
                   filepath);
    } else {
        pixels = compact_pixels;
    }

    MAIN_INFO("Imagen decodificada con exito: %s (%dx%d)", filepath, w, h);
    if (sprite_id == SPRITE_BACKGROUND) {
        background_pixels = pixels;
    } else if (sprite_id == SPRITE_RECEPTIONIST) {
        receptionist_pixels = pixels;
    } else if (sprite_id == SPRITE_NEKO) {
        neko_pixels = pixels;
    } else if (sprite_id == SPRITE_ELEVATOR_DOOR) {
        elevator_door_pixels = pixels;
    } else if (sprite_id == SPRITE_LEVEL_INDICATOR) {
        level_indicator_pixels = pixels;
    } else if (sprite_id == SPRITE_AGENT_SHADOW) {
        agent_shadow_pixels = pixels;
    } else if (sprite_id == SPRITE_ELEVATOR_INSIDE_BACKGROUND) {
        elevator_inside_pixels = pixels;
    } else if (sprite_id == SPRITE_ELEVATOR_INSIDE_DOOR) {
        elevator_inside_door_pixels = pixels;
    } else if (sprite_id == SPRITE_ELEVATOR_PANEL) {
        elevator_panel_pixels = pixels;
    } else if (sprite_id == SPRITE_ELEVATOR_BUTTONS) {
        elevator_buttons_pixels = pixels;
    } else if (sprite_id == SPRITE_ELEVATOR_LIGHTS) {
        elevator_lights_pixels = pixels;
    }
    bridge_set_sprite(sprite_id, pixels, w, h);
    return true;
}
// ---------------------------------------------------------
// 3. MOTOR GRÁFICO UNIVERSAL (Enlazado al Bridge)
// ---------------------------------------------------------
static void animation_game_task(void *pvParameters) {
    MAIN_INFO("Tarea de video iniciada. Decodificando recursos...");
    
    if (!load_png_to_rgb565_esp32("/storage/roomde.png", SPRITE_BACKGROUND)) {
        vTaskDelete(NULL);
        return;
    }
    if (!load_png_to_rgb565_esp32(
            "/storage/la_que_atiende_strip4.png", SPRITE_RECEPTIONIST)) {
        MAIN_ERROR("No se pudo cargar la animacion de la recepcionista");
        vTaskDelete(NULL);
        return;
    }
    if (!load_png_to_rgb565_esp32("/storage/neko_strip6.png", SPRITE_NEKO)) {
        MAIN_ERROR("No se pudo cargar la animacion de Neko");
        vTaskDelete(NULL);
        return;
    }
    if (!load_png_to_rgb565_esp32(
            "/storage/elevator_door_strip8.png", SPRITE_ELEVATOR_DOOR)) {
        MAIN_ERROR("No se pudo cargar la animacion de las puertas del ascensor");
        vTaskDelete(NULL);
        return;
    }
    if (!load_png_to_rgb565_esp32(
            "/storage/Level_indicator.png", SPRITE_LEVEL_INDICATOR)) {
        MAIN_ERROR("No se pudo cargar la animacion del indicador de nivel");
        vTaskDelete(NULL);
        return;
    }
    if (!load_png_to_rgb565_esp32(
            "/storage/agente.png", SPRITE_AGENT_SHADOW)) {
        MAIN_ERROR("No se pudo cargar la animacion del agente sombra");
        vTaskDelete(NULL);
        return;
    }
    if (!load_png_to_rgb565_esp32(
            "/storage/elevator_inside.png", SPRITE_ELEVATOR_INSIDE_BACKGROUND)) {
        MAIN_ERROR("No se pudo cargar el interior del ascensor");
        vTaskDelete(NULL);
        return;
    }
    if (!load_png_to_rgb565_esp32(
            "/storage/Panel_pixelado.png", SPRITE_ELEVATOR_PANEL)) {
        MAIN_ERROR("No se pudo cargar el panel del ascensor");
        vTaskDelete(NULL);
        return;
    }
    if (!load_png_to_rgb565_esp32(
            "/storage/botones_ascensor.png", SPRITE_ELEVATOR_BUTTONS)) {
        MAIN_ERROR("No se pudieron cargar los botones del ascensor");
        vTaskDelete(NULL);
        return;
    }
    if (!load_png_to_rgb565_esp32(
            "/storage/luces_ascensor.png", SPRITE_ELEVATOR_LIGHTS)) {
        MAIN_ERROR("No se pudieron cargar las luces del ascensor");
        vTaskDelete(NULL);
        return;
    }
    if (!load_png_to_rgb565_esp32(
            "/storage/inside_door.png", SPRITE_ELEVATOR_INSIDE_DOOR)) {
        MAIN_ERROR("No se pudo cargar la puerta del interior del ascensor");
        vTaskDelete(NULL);
        return;
    }

    void *framebuffers[2] = { NULL, NULL };
    esp_err_t err = esp_lcd_dpi_panel_get_frame_buffer(
        panel_handle, 2, &framebuffers[0], &framebuffers[1]);
    if (err != ESP_OK || framebuffers[0] == NULL || framebuffers[1] == NULL) {
        MAIN_ERROR("No se pudieron obtener los framebuffers del LCD: %s",
                   esp_err_to_name(err == ESP_OK ? ESP_ERR_NO_MEM : err));
        vTaskDelete(NULL);
        return;
    }

    uint8_t back_buffer_index = 1;
    TickType_t last_wake = xTaskGetTickCount();
    uint32_t frame_tick_remainder = 0;
    uint32_t frames_in_window = 0;
    uint32_t frames_rendered_in_window = 0;
    uint32_t frame_deadline_misses = 0;
    uint64_t frame_work_time_us = 0;
    uint32_t max_frame_work_us = 0;
    int64_t window_start_us = esp_timer_get_time();

    for (;;) {
        const int64_t frame_start_us = esp_timer_get_time();
        if (panel_handle == NULL) {
            MAIN_ERROR("El panel LCD no esta disponible para transmitir cuadros");
            vTaskDelete(NULL);
            return;
        }
        uint16_t *framebuffer = (uint16_t *)framebuffers[back_buffer_index];
        if (!bridge_render_frame(framebuffer, H_size, V_size)) {
            MAIN_ERROR("No se pudo componer el cuadro de animacion");
            vTaskDelete(NULL);
            return;
        }

        if (bridge_frame_was_rendered()) {
            ++frames_rendered_in_window;
            err = esp_lcd_panel_draw_bitmap(
                panel_handle, 0, 0, H_size, V_size, framebuffer);
            if (err != ESP_OK) {
                MAIN_ERROR("Error transmitiendo cuadro al LCD: %s",
                           esp_err_to_name(err));
                vTaskDelete(NULL);
                return;
            }
            back_buffer_index ^= 1;
        }

        const int64_t frame_end_us = esp_timer_get_time();
        const uint32_t frame_work_us = (uint32_t)(frame_end_us - frame_start_us);
        frame_work_time_us += frame_work_us;
        if (frame_work_us > max_frame_work_us) {
            max_frame_work_us = frame_work_us;
        }
        const uint32_t frame_budget_us = 1000000 / ANIMATION_FRAME_RATE_HZ;
        const uint32_t tick_period_us = 1000000 / configTICK_RATE_HZ;
        const bool frame_missed_deadline =
            frame_work_us >= frame_budget_us - tick_period_us;
        if (frame_missed_deadline) {
            ++frame_deadline_misses;
        }
        ++frames_in_window;
        if (frame_end_us - window_start_us >= FRAME_STATS_INTERVAL_US) {
            const uint64_t elapsed_us = frame_end_us - window_start_us;
            const uint32_t fps_tenths =
                (uint32_t)((uint64_t)frames_in_window * 10000000ULL / elapsed_us);
            const uint32_t average_work_us =
                (uint32_t)(frame_work_time_us / frames_in_window);
            MAIN_INFO("Video: %u.%u FPS, composiciones: %u, "
                      "trabajo promedio/maximo: %u/%u us, "
                      "plazos perdidos: %u",
                      fps_tenths / 10, fps_tenths % 10,
                      frames_rendered_in_window, average_work_us,
                      max_frame_work_us, frame_deadline_misses);
            frames_in_window = 0;
            frames_rendered_in_window = 0;
            frame_deadline_misses = 0;
            frame_work_time_us = 0;
            max_frame_work_us = 0;
            window_start_us = frame_end_us;
        }

        frame_tick_remainder += configTICK_RATE_HZ % ANIMATION_FRAME_RATE_HZ;
        TickType_t frame_ticks = configTICK_RATE_HZ / ANIMATION_FRAME_RATE_HZ;
        if (frame_tick_remainder >= ANIMATION_FRAME_RATE_HZ) {
            ++frame_ticks;
            frame_tick_remainder -= ANIMATION_FRAME_RATE_HZ;
        }
        vTaskDelayUntil(&last_wake, frame_ticks);
        if (frame_work_us >= frame_budget_us) {
            last_wake = xTaskGetTickCount();
            frame_tick_remainder = 0;
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

    MAIN_INFO("Initializing LCD MIPI-DSI...");
    err = display_init();
    if (err != ESP_OK) init_fail_handler("LCD", err);

    err = set_lcd_blight(100);
    if (err != ESP_OK) init_fail_handler("LCD Backlight", err);
}

// ---------------------------------------------------------
// 5. FUNCIÓN PRINCIPAL
// ---------------------------------------------------------
void app_main(void)
{
    MAIN_INFO("Iniciando Arquitectura Unificada (Bridge PC-ESP32)...");
    system_init();
    esp_err_t err = init_spiffs();
    if (err != ESP_OK) init_fail_handler("SPIFFS", err);
    bridge_init();
    bridge_set_receptionist_frame_delay(FRAME_SPEED_TRANSITION + 2);
    bridge_set_neko_frame_delay(FRAME_SPEED_TRANSITION);
    bridge_set_elevator_door_frame_delay(FRAME_SPEED_TRANSITION - 1);
    bridge_set_level_indicator_frame_delay(FRAME_SPEED_TRANSITION + 7);
    bridge_set_agent_shadow_frame_delay(FRAME_SPEED_TRANSITION);

    // Lanzamos directamente la tarea gráfica, ella hará la carga de memoria pesada
    if (xTaskCreatePinnedToCore(animation_game_task, "Animacion_DSI",
                                ANIMATION_TASK_STACK_SIZE, NULL, 5, NULL, 1) != pdPASS) {
        init_fail_handler("tarea grafica", ESP_ERR_NO_MEM);
    }

    bool touch_was_pressed = false;
    uint8_t touch_release_samples = 0;
    for (;;) {
        if (touch_available) {
            if (touch_read() != ESP_OK) {
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }

            uint16_t touch_x = 0xffff;
            uint16_t touch_y = 0xffff;
            bool touch_pressed = false;
            get_coor(&touch_x, &touch_y, &touch_pressed);
            if (touch_pressed) {
                touch_release_samples = 0;
                bridge_touch_elevator_panel(
                    touch_x, touch_y, true, H_size, V_size);
                if (!touch_was_pressed) {
                    bridge_touch_elevator_door(
                        touch_x, touch_y, H_size, V_size);
                }
                touch_was_pressed = true;
            } else if (touch_was_pressed) {
                if (++touch_release_samples >= 2) {
                    bridge_touch_elevator_panel(
                        touch_x, touch_y, false, H_size, V_size);
                    touch_was_pressed = false;
                    touch_release_samples = 0;
                }
            }
            vTaskDelay(pdMS_TO_TICKS(10));
        } else {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }
}