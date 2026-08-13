// main.c
#include "main.h"
#include "lvgl.h"
#include "driver/gpio.h" // Librería obligatoria para el control físico de pines

// --- CONFIGURACIÓN DE PINES GPIO ---
#define GPIO_OUT_1 GPIO_NUM_50
#define GPIO_OUT_2 GPIO_NUM_51
#define GPIO_OUT_3 GPIO_NUM_52

#define GPIO_IN_1 GPIO_NUM_25
#define GPIO_IN_2 GPIO_NUM_27
#define GPIO_IN_3 GPIO_NUM_28

// Arreglos para iterar fácilmente en bucles
static const gpio_num_t output_gpios[3] = {GPIO_OUT_1, GPIO_OUT_2, GPIO_OUT_3};
static const gpio_num_t input_gpios[3] = {GPIO_IN_1, GPIO_IN_2, GPIO_IN_3};

static esp_ldo_channel_handle_t ldo3 = NULL;
static esp_ldo_channel_handle_t ldo4 = NULL;

// --- VARIABLES GLOBALES DE LA INTERFAZ ---
static bool out_state[3] = {false, false, false}; // Estado lógico de las salidas
static lv_obj_t * out_circles[3]; // Los 3 botones táctiles inferiores
static lv_obj_t * in_circles[3];  // Los 3 indicadores fijos superiores

// ---------------------------------------------------------
// 1. Inicialización del Hardware GPIO (Bare-Metal)
// ---------------------------------------------------------
static void hardware_gpio_init(void) {
    // Configurar pines de SALIDA
    for(int i = 0; i < 3; i++) {
        gpio_reset_pin(output_gpios[i]);
        gpio_set_direction(output_gpios[i], GPIO_MODE_OUTPUT);
        gpio_set_level(output_gpios[i], 0); // Iniciar apagados (0V)
    }

    // Configurar pines de ENTRADA con resistencia Pull-Down interna
    // (Asegura que lean 0V si no hay nada conectado, evitando ruido electromagnético)
    for(int i = 0; i < 3; i++) {
        gpio_reset_pin(input_gpios[i]);
        gpio_set_direction(input_gpios[i], GPIO_MODE_INPUT);
        gpio_set_pull_mode(input_gpios[i], GPIO_PULLDOWN_ONLY); 
    }
}

// ---------------------------------------------------------
// 2. Tarea de LVGL para escanear las entradas (Polling de Sensores)
// ---------------------------------------------------------
static void input_sensor_timer_cb(lv_timer_t * timer) {
    for(int i = 0; i < 3; i++) {
        // Leemos el voltaje físico del pin
        int pin_level = gpio_get_level(input_gpios[i]);
        
        if(pin_level == 1) {
            // Recibe 3.3V: Cambia a Amarillo
            lv_obj_set_style_bg_color(in_circles[i], lv_color_hex(0xFFFF00), LV_PART_MAIN);
        } else {
            // Recibe 0V: Vuelve a Negro
            lv_obj_set_style_bg_color(in_circles[i], lv_color_hex(0x000000), LV_PART_MAIN);
        }
    }
}

// ---------------------------------------------------------
// 3. Evento Táctil para controlar las salidas (Actuadores)
// ---------------------------------------------------------
static void output_touch_event_cb(lv_event_t * e) {
    // Recuperamos el índice del botón que fue presionado (0, 1 o 2)
    int idx = (int)(uintptr_t)lv_event_get_user_data(e);
    lv_obj_t * circle_obj = lv_event_get_target(e);

    // Invertimos el estado lógico (Toogle)
    out_state[idx] = !out_state[idx];

    if(out_state[idx]) {
        // Estado ON: Interfaz Verde, Hardware a 3.3V
        lv_obj_set_style_bg_color(circle_obj, lv_color_hex(0x00FF00), LV_PART_MAIN);
        gpio_set_level(output_gpios[idx], 1);
    } else {
        // Estado OFF: Interfaz Roja, Hardware a 0V
        lv_obj_set_style_bg_color(circle_obj, lv_color_hex(0xFF0000), LV_PART_MAIN);
        gpio_set_level(output_gpios[idx], 0);
    }
}

// ---------------------------------------------------------
// 4. Creación de la Interfaz HMI
// ---------------------------------------------------------
void create_hmi_dashboard(void) {
    lvgl_port_lock(0); 

    lv_obj_t * scr = lv_scr_act();
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x2C3E50), 0); // Fondo gris azulado industrial

    // Posiciones en el eje X para mantener todo simétrico en los 1024 píxeles
    int x_positions[3] = {-250, 0, 250}; 

    // A. CREAR LOS 3 INDICADORES DE ENTRADA (Fila Superior)
    for(int i = 0; i < 3; i++) {
        in_circles[i] = lv_obj_create(scr);
        lv_obj_set_size(in_circles[i], 120, 120);
        lv_obj_align(in_circles[i], LV_ALIGN_CENTER, x_positions[i], -120); // Desplazados hacia arriba
        
        lv_obj_set_style_radius(in_circles[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(in_circles[i], lv_color_hex(0x000000), 0); // Inicial: Negro
        lv_obj_set_style_border_width(in_circles[i], 4, 0);
        lv_obj_set_style_border_color(in_circles[i], lv_color_hex(0xFFFFFF), 0); // Borde blanco
        lv_obj_clear_flag(in_circles[i], LV_OBJ_FLAG_SCROLLABLE);
        
        // Estos círculos NO tienen el flag CLICKABLE, son estáticos
    }

    // B. CREAR LOS 3 BOTONES DE SALIDA (Fila Inferior)
    for(int i = 0; i < 3; i++) {
        out_circles[i] = lv_obj_create(scr);
        lv_obj_set_size(out_circles[i], 120, 120);
        lv_obj_align(out_circles[i], LV_ALIGN_CENTER, x_positions[i], 120); // Desplazados hacia abajo
        
        lv_obj_set_style_radius(out_circles[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(out_circles[i], lv_color_hex(0xFF0000), 0); // Inicial: Rojo (OFF)
        lv_obj_set_style_border_width(out_circles[i], 0, 0);
        lv_obj_clear_flag(out_circles[i], LV_OBJ_FLAG_SCROLLABLE);
        
        // Hacemos que reaccionen al toque
        lv_obj_add_flag(out_circles[i], LV_OBJ_FLAG_CLICKABLE);
        
        // Asignamos el evento y pasamos el índice (i) como "user_data" para saber cuál se tocó
        lv_obj_add_event_cb(out_circles[i], output_touch_event_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)i);
    }

    // C. CREAR EL TEMPORIZADOR DE ESCANEO
    // LVGL llamará a esta función cada 50 milisegundos para leer los pines físicos
    lv_timer_create(input_sensor_timer_cb, 50, NULL);

    lvgl_port_unlock();
}

/**
 * @brief Initialization failure handler
 */
static void init_fail_handler(const char *module_name, esp_err_t err) {
    while (1) {
        MAIN_ERROR("[%s] init failed: %s", module_name, esp_err_to_name(err));
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

/**
 * @brief System initialization
 */
static void system_init(void) {
    esp_err_t err = ESP_OK;

    // Inicializamos el hardware físico (Pines GPIO) antes de la pantalla
    hardware_gpio_init();
    MAIN_INFO("Hardware GPIO config success");

    esp_ldo_channel_config_t ldo3_cof = { .chan_id = 3, .voltage_mv = 2500, };
    err = esp_ldo_acquire_channel(&ldo3_cof, &ldo3);
    if (err != ESP_OK) init_fail_handler("ldo3", err);

    esp_ldo_channel_config_t ldo4_cof = { .chan_id = 4, .voltage_mv = 3300, };
    err = esp_ldo_acquire_channel(&ldo4_cof, &ldo4);
    if (err != ESP_OK) init_fail_handler("ldo4", err);
    MAIN_INFO("LDO3 and LDO4 init success");

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

    // Lanzamos la interfaz HMI
    create_hmi_dashboard();
    MAIN_INFO("Panel de control I/O generado exitosamente");
}

void app_main(void)
{
    MAIN_INFO("Iniciando Sistema de Control Mecatronico HMI...");
    system_init();

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}