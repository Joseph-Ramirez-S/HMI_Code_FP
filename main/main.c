// main.c
#include "main.h"
#include "lvgl.h"              // Para reconocer la estructura lv_img_dsc_t
#include "esp_lcd_panel_ops.h" // Obligatorio para manipular el panel por DSI
#include "esp_heap_caps.h"     // Obligatorio para asignar memoria DMA
#include <string.h>            // Para memcpy

#define LCD_NUM_FBS 2 

// --- VARIABLES EXTERNAS ---
extern const lv_img_dsc_t test_png_2;
extern const lv_img_dsc_t test_png;   // ¡Segundo objeto!
extern esp_lcd_panel_handle_t panel_handle;

static esp_ldo_channel_handle_t ldo3 = NULL;
static esp_ldo_channel_handle_t ldo4 = NULL;

// ---------------------------------------------------------
// 1. INICIALIZACIÓN ABSOLUTA DEL FONDO (Blindaje VSYNC)
// ---------------------------------------------------------
void fill_celeste_background_dsi(void) {
    if(panel_handle == NULL) return;
    
    MAIN_INFO("Esperando VSYNC del hardware...");
    vTaskDelay(pdMS_TO_TICKS(200)); 

    uint16_t celeste_rgb565 = 0x867D;
    int chunk_h = 60;
    uint16_t *bg_buf = heap_caps_malloc(1024 * chunk_h * sizeof(uint16_t), MALLOC_CAP_DMA);

    if(bg_buf) {
        for(int i = 0; i < 1024 * chunk_h; i++) bg_buf[i] = celeste_rgb565;
        
        // Pintamos la pantalla 2 VECES para llenar ambos lienzos físicos (Double Buffering)
        for(int frame = 0; frame < 2; frame++) {
            for(int y = 0; y < 600; y += chunk_h) {
                while (esp_lcd_panel_draw_bitmap(panel_handle, 0, y, 1024, y + chunk_h, bg_buf) != ESP_OK) {
                    vTaskDelay(pdMS_TO_TICKS(1));
                }
            }
            vTaskDelay(pdMS_TO_TICKS(35)); 
        }
        
        free(bg_buf);
        MAIN_INFO("Fondo celeste inicial completado con exito.");
    }
}

// ---------------------------------------------------------
// 2. MOTOR GRÁFICO MULTI-OBJETO CON TRANSPARENCIA
// ---------------------------------------------------------
static void animation_game_task(void *pvParameters) {
    // --- OBJETO 1 (La Pluma Original - Sin transparencia manual) ---
    int pos_x_1 = 0;           
    int pos_y_1 = 50;          
    int speed_1 = 4;           
    int width_1 = 236;
    int height_1 = 236;
    int direction_1 = 1; // Hacia la derecha

    // --- OBJETO 2 (test_png - Con fondo negro transparente) ---
    int width_2 = 43;
    int height_2 = 61;
    int pos_x_2 = 1024 - width_2; 
    int pos_y_2 = 350;            
    int speed_2 = 5;              
    int direction_2 = -1; // Hacia la izquierda

    uint16_t celeste_rgb565 = 0x867D;

    // --- ASIGNACIÓN DE BÚFERES INDEPENDIENTES ---
    int clear_width_1 = speed_1 * LCD_NUM_FBS; 
    int max_draw_w_1 = width_1 + clear_width_1;
    uint16_t *unified_buf_1 = heap_caps_malloc(max_draw_w_1 * height_1 * sizeof(uint16_t), MALLOC_CAP_DMA);

    int clear_width_2 = speed_2 * LCD_NUM_FBS; 
    int max_draw_w_2 = width_2 + clear_width_2;
    uint16_t *unified_buf_2 = heap_caps_malloc(max_draw_w_2 * height_2 * sizeof(uint16_t), MALLOC_CAP_DMA);

    TickType_t xLastWakeTime = xTaskGetTickCount();
    const TickType_t xFrequency = pdMS_TO_TICKS(16); // 60 FPS fijos

    for (;;) { 
        if(panel_handle != NULL && unified_buf_1 != NULL && unified_buf_2 != NULL) {
            
            // ==========================================
            // RENDERIZADO DEL OBJETO 1 (test_png_2)
            // ==========================================
            pos_x_1 += (speed_1 * direction_1);
            if (pos_x_1 + width_1 >= 1024) { pos_x_1 = 1024 - width_1; direction_1 = -1; } 
            else if (pos_x_1 <= 0) { pos_x_1 = 0; direction_1 = 1; }

            int start_x_1 = (direction_1 == 1) ? (pos_x_1 - clear_width_1) : pos_x_1;
            if (start_x_1 < 0) start_x_1 = 0;
            int end_x_1 = (direction_1 == 1) ? (pos_x_1 + width_1) : (pos_x_1 + width_1 + clear_width_1);
            if (end_x_1 > 1024) end_x_1 = 1024;

            int current_draw_w_1 = end_x_1 - start_x_1;
            int offset_x_1 = pos_x_1 - start_x_1; 

            // Capa Absoluta 1
            for(int i = 0; i < current_draw_w_1 * height_1; i++) unified_buf_1[i] = celeste_rgb565;
            
            // Capa Relativa 1 (Se copia directo por filas porque ya tiene su propio fondo cuadrado)
            uint16_t *img_data_1 = (uint16_t *)test_png_2.data;
            for(int y = 0; y < height_1; y++) {
                memcpy(&unified_buf_1[y * current_draw_w_1 + offset_x_1], &img_data_1[y * width_1], width_1 * sizeof(uint16_t)); 
            }

            while (esp_lcd_panel_draw_bitmap(panel_handle, start_x_1, pos_y_1, end_x_1, pos_y_1 + height_1, unified_buf_1) != ESP_OK) {
                taskYIELD(); 
            }

            // ==========================================
            // RENDERIZADO DEL OBJETO 2 (test_png con TRANSPARENCIA)
            // ==========================================
            pos_x_2 += (speed_2 * direction_2);
            if (pos_x_2 + width_2 >= 1024) { pos_x_2 = 1024 - width_2; direction_2 = -1; } 
            else if (pos_x_2 <= 0) { pos_x_2 = 0; direction_2 = 1; }

            int start_x_2 = (direction_2 == 1) ? (pos_x_2 - clear_width_2) : pos_x_2;
            if (start_x_2 < 0) start_x_2 = 0;
            int end_x_2 = (direction_2 == 1) ? (pos_x_2 + width_2) : (pos_x_2 + width_2 + clear_width_2);
            if (end_x_2 > 1024) end_x_2 = 1024;

            int current_draw_w_2 = end_x_2 - start_x_2;
            int offset_x_2 = pos_x_2 - start_x_2; 

            // Capa Absoluta 2 (Llenamos de celeste todo el cuadro)
            for(int i = 0; i < current_draw_w_2 * height_2; i++) unified_buf_2[i] = celeste_rgb565;
            
            // Capa Relativa 2 (Píxel por Píxel con Chroma Key)
            uint16_t *img_data_2 = (uint16_t *)test_png.data;
            for(int y = 0; y < height_2; y++) {
                for(int x = 0; x < width_2; x++) {
                    uint16_t pixel = img_data_2[y * width_2 + x];
                    
                    // LÓGICA DE TRANSPARENCIA: 
                    // 0x0000 es el color Negro absoluto en formato 16 bits (RGB565).
                    // Solo dibujamos el píxel de la imagen si NO es negro. 
                    // Si es negro, lo ignoramos, por lo que el celeste de la capa absoluta permanece visible.
                    if (pixel != 0x0000) {
                        unified_buf_2[y * current_draw_w_2 + offset_x_2 + x] = pixel;
                    }
                }
            }

            while (esp_lcd_panel_draw_bitmap(panel_handle, start_x_2, pos_y_2, end_x_2, pos_y_2 + height_2, unified_buf_2) != ESP_OK) {
                taskYIELD(); 
            }
        }
        
        vTaskDelayUntil(&xLastWakeTime, xFrequency);
    }
}

// ---------------------------------------------------------
// 3. INICIALIZACIÓN DEL SISTEMA
// ---------------------------------------------------------
static void init_fail_handler(const char *module_name, esp_err_t err) {
    for (;;) {
        MAIN_ERROR("[%s] init failed: %s", module_name, err);
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
    if (err != ESP_OK) init_fail_handler("Touch", err);

    err = display_init();
    if (err != ESP_OK) init_fail_handler("LCD", err);

    err = set_lcd_blight(100);
    if (err != ESP_OK) init_fail_handler("LCD Backlight", err);
}

// ---------------------------------------------------------
// 4. FUNCIÓN PRINCIPAL
// ---------------------------------------------------------
void app_main(void)
{
    MAIN_INFO("Iniciando Motor Multiobjeto (Transparencia Chroma Key)...");
    
    system_init();
    
    fill_celeste_background_dsi();

    xTaskCreatePinnedToCore(animation_game_task, "Animacion_DSI", 8192, NULL, 5, NULL, 1);

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}