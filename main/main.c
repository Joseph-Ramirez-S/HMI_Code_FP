// main.c
#include "main.h"
#include "lvgl.h"
#include "driver/gpio.h"

// --- CONFIGURACIÓN DE PINES GPIO ---
// Salidas a relés/actuadores (Botones 1, 2, 3)
#define GPIO_OUT_1 GPIO_NUM_50
#define GPIO_OUT_2 GPIO_NUM_51
#define GPIO_OUT_3 GPIO_NUM_52

// Salida a la Bocina
#define GPIO_OUT_HORN GPIO_NUM_5

// Entradas de sensores/voltaje (Indicadores 1, 2, 3)
#define GPIO_IN_1 GPIO_NUM_25
#define GPIO_IN_2 GPIO_NUM_27
#define GPIO_IN_3 GPIO_NUM_28

// Arreglos para iterar fácilmente
static const gpio_num_t output_gpios[3] = {GPIO_OUT_1, GPIO_OUT_2, GPIO_OUT_3};
static const gpio_num_t input_gpios[3] = {GPIO_IN_1, GPIO_IN_2, GPIO_IN_3};

static esp_ldo_channel_handle_t ldo3 = NULL;
static esp_ldo_channel_handle_t ldo4 = NULL;

// --- VARIABLES GLOBALES DE LA INTERFAZ ---
static lv_obj_t * out_circles[3]; // Botones táctiles principales
static lv_obj_t * in_circles[3];  // Pequeños indicadores fijos

// ---------------------------------------------------------
// 1. Inicialización del Hardware GPIO (Bare-Metal)
// ---------------------------------------------------------
static void hardware_gpio_init(void) {
    // Configurar pines de SALIDA (Botones 1, 2, 3)
    for(int i = 0; i < 3; i++) {
        gpio_reset_pin(output_gpios[i]);
        gpio_set_direction(output_gpios[i], GPIO_MODE_OUTPUT);
        gpio_set_level(output_gpios[i], 0); // Iniciar apagados (0V)
    }

    // Configurar pin de SALIDA (Bocina)
    gpio_reset_pin(GPIO_OUT_HORN);
    gpio_set_direction(GPIO_OUT_HORN, GPIO_MODE_OUTPUT);
    gpio_set_level(GPIO_OUT_HORN, 0);

    // Configurar pines de ENTRADA con Pull-Down
    for(int i = 0; i < 3; i++) {
        gpio_reset_pin(input_gpios[i]);
        gpio_set_direction(input_gpios[i], GPIO_MODE_INPUT);
        gpio_set_pull_mode(input_gpios[i], GPIO_PULLDOWN_ONLY); 
    }
}

// ---------------------------------------------------------
// 2. Tarea de LVGL para escanear las entradas (Polling)
// ---------------------------------------------------------
static void input_sensor_timer_cb(lv_timer_t * timer) {
    for(int i = 0; i < 3; i++) {
        int pin_level = gpio_get_level(input_gpios[i]);
        
        if(pin_level == 1) {
            // Recibe voltaje: Círculo pequeño cambia a Amarillo
            lv_obj_set_style_bg_color(in_circles[i], lv_color_hex(0xFFFF00), LV_PART_MAIN);
        } else {
            // Sin voltaje: Vuelve a Negro
            lv_obj_set_style_bg_color(in_circles[i], lv_color_hex(0x000000), LV_PART_MAIN);
        }
    }
}

// ---------------------------------------------------------
// 3. Eventos NO RETENSIVOS (Pulsadores momentáneos)
// ---------------------------------------------------------

// Evento para los botones 1, 2 y 3
static void momentary_touch_event_cb(lv_event_t * e) {
    lv_event_code_t code = lv_event_get_code(e);
    int idx = (int)(uintptr_t)lv_event_get_user_data(e);
    lv_obj_t * circle_obj = lv_event_get_target(e);

    // Si el dedo está presionando la pantalla
    if(code == LV_EVENT_PRESSED) {
        lv_obj_set_style_bg_color(circle_obj, lv_color_hex(0x00FF00), LV_PART_MAIN); // Verde
        gpio_set_level(output_gpios[idx], 1); // Disparo a 3.3V
    } 
    // Si el dedo se levanta de la pantalla o desliza fuera del botón
    else if(code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        lv_obj_set_style_bg_color(circle_obj, lv_color_hex(0xFF0000), LV_PART_MAIN); // Vuelve a Rojo
        gpio_set_level(output_gpios[idx], 0); // Corte a 0V
    }
}

// Evento exclusivo para la bocina
static void horn_touch_event_cb(lv_event_t * e) {
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t * horn_obj = lv_event_get_target(e);

    if(code == LV_EVENT_PRESSED) {
        lv_obj_set_style_bg_color(horn_obj, lv_color_hex(0x00A8FF), LV_PART_MAIN); // Azul brillante
        gpio_set_level(GPIO_OUT_HORN, 1);
    } 
    else if(code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        lv_obj_set_style_bg_color(horn_obj, lv_color_hex(0x34495E), LV_PART_MAIN); // Gris original
        gpio_set_level(GPIO_OUT_HORN, 0);
    }
}

// ---------------------------------------------------------
// 4. Creación de la Interfaz HMI (Layout)
// ---------------------------------------------------------
void create_hmi_dashboard(void) {
    lvgl_port_lock(0); 

    lv_obj_t * scr = lv_scr_act();
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x87CEEB), 0); // Celeste (Sky Blue)

    // Coordenadas Y para los botones 1, 2 y 3 (De abajo hacia arriba)
    int y_positions[3] = {160, 0, -160}; 
    const char* btn_labels[3] = {"1", "2", "3"};

    // A. CREAR BOTONES LATERALES DERECHOS E INDICADORES
    for(int i = 0; i < 3; i++) {
        // --- Botón Principal ---
        out_circles[i] = lv_obj_create(scr);
        lv_obj_set_size(out_circles[i], 120, 120);
        // Alineamos a la derecha central y ajustamos en Y
        lv_obj_align(out_circles[i], LV_ALIGN_RIGHT_MID, -120, y_positions[i]); 
        
        lv_obj_set_style_radius(out_circles[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(out_circles[i], lv_color_hex(0xFF0000), 0); // Rojo Inicial
        lv_obj_set_style_border_width(out_circles[i], 4, 0);
        lv_obj_set_style_border_color(out_circles[i], lv_color_hex(0x000000), 0);
        lv_obj_clear_flag(out_circles[i], LV_OBJ_FLAG_SCROLLABLE);
        
        // Asignamos la función no-retensiva (atrapando ALL_EVENTS)
        lv_obj_add_event_cb(out_circles[i], momentary_touch_event_cb, LV_EVENT_ALL, (void *)(uintptr_t)i);

        // --- Etiqueta del botón (Número) ---
        lv_obj_t * label = lv_label_create(out_circles[i]);
        lv_label_set_text(label, btn_labels[i]);
        lv_obj_set_style_text_font(label, &lv_font_montserrat_28, 0);
        lv_obj_set_style_text_color(label, lv_color_hex(0xFFFFFF), 0);
        lv_obj_center(label);

        // --- Indicador Pequeño Superior Derecho ---
        in_circles[i] = lv_obj_create(scr);
        lv_obj_set_size(in_circles[i], 25, 25);
        lv_obj_set_style_radius(in_circles[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(in_circles[i], lv_color_hex(0x000000), 0); // Negro Inicial
        lv_obj_set_style_border_width(in_circles[i], 3, 0);
        lv_obj_set_style_border_color(in_circles[i], lv_color_hex(0x000000), 0);
        lv_obj_clear_flag(in_circles[i], LV_OBJ_FLAG_SCROLLABLE);
        
        // Alineamos respecto al botón principal
        lv_obj_align_to(in_circles[i], out_circles[i], LV_ALIGN_OUT_TOP_RIGHT, 15, 10); 
    }

    // B. CREAR BOTÓN DE BOCINA (Esquina Inferior Izquierda)
    lv_obj_t * horn_btn = lv_obj_create(scr);
    lv_obj_set_size(horn_btn, 110, 110);
    lv_obj_align(horn_btn, LV_ALIGN_BOTTOM_LEFT, 60, -60);
    lv_obj_set_style_radius(horn_btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(horn_btn, lv_color_hex(0x34495E), 0); // Gris Azulado Inicial
    lv_obj_set_style_border_width(horn_btn, 4, 0);
    lv_obj_set_style_border_color(horn_btn, lv_color_hex(0x000000), 0);
    lv_obj_clear_flag(horn_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(horn_btn, horn_touch_event_cb, LV_EVENT_ALL, NULL);

    lv_obj_t * horn_label = lv_label_create(horn_btn);
    lv_label_set_text(horn_label, LV_SYMBOL_VOLUME_MAX); // Icono nativo de LVGL
    lv_obj_set_style_text_font(horn_label, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(horn_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(horn_label);

    // C. CREAR EL TEMPORIZADOR DE LECTURA (Entradas Físicas)
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

    create_hmi_dashboard();
    MAIN_INFO("Panel de control HMI generado exitosamente");
}

void app_main(void)
{
    MAIN_INFO("Iniciando Sistema de Control Mecatronico HMI...");
    system_init();

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}