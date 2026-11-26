#ifndef ENGINE_BRIDGE_H
#define ENGINE_BRIDGE_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Identificadores para saber qué imagen le estamos inyectando al motor
typedef enum {
    SPRITE_PLAYER,
    SPRITE_BACKGROUND,
    SPRITE_RECEPTIONIST,
    SPRITE_NEKO,
    SPRITE_ELEVATOR_DOOR,
    SPRITE_LEVEL_INDICATOR,
    SPRITE_ELEVATOR_INSIDE_LEVEL_INDICATOR,
    SPRITE_AGENT_SHADOW,
    SPRITE_ELEVATOR_INSIDE_BACKGROUND,
    SPRITE_ELEVATOR_INSIDE_DOOR,
    SPRITE_ELEVATOR_PANEL,
    SPRITE_ELEVATOR_BUTTONS,
    SPRITE_ELEVATOR_LIGHTS,
    SPRITE_ELEVATOR_DIRECTION_INDICATOR
} SpriteID;

// Inicializa el motor gráfico.
void bridge_init(void);

// Configura cuantas llamadas de renderizado transcurren entre cuadros de la recepcionista.
void bridge_set_receptionist_frame_delay(int render_frames);
void bridge_set_neko_frame_delay(int render_frames);
void bridge_set_elevator_door_frame_delay(int render_frames);
void bridge_set_agent_shadow_frame_delay(int render_frames);
// Opens the lobby elevator door or returns through the open inside elevator door.
bool bridge_touch_elevator_door(int x, int y, int screen_width, int screen_height);
// Enable hardware hall calls before starting the input and animation loops.
void bridge_set_exterior_calls_enabled(bool enabled);
void bridge_release_exterior_call(void);
int bridge_get_elevator_output_button(void);
enum { BRIDGE_ELEVATOR_EMERGENCY_BUTTON = 5 };
// Handles panel presses using the same confirmed floor as the current input poll.
bool bridge_touch_elevator_panel(int x, int y, bool pressed,
                                 int screen_width, int screen_height, int confirmed_floor);
// Handles the exterior stairs and consumes taps while the floor menu is open.
bool bridge_touch_stairs(int x, int y, bool pressed, int screen_width, int screen_height);
bool bridge_touch_options(int x, int y, bool pressed, int screen_width, int screen_height);
// Call with a monotonic clock and a confirmed floor, 0 between floors, or -1 for unstable/ambiguous inputs.
void bridge_update_elevator_controls_at(uint64_t now_us, int confirmed_floor);
unsigned bridge_get_sfx_volume(void);
unsigned bridge_get_music_volume(void);
// Register before processing input. Invoked once when a supported control accepts a press.
void bridge_set_button_sound_callback(void (*callback)(void*), void* context);
void bridge_set_hint_sound_callback(void (*callback)(void*), void* context);
void bridge_set_scene_sound_callback(void (*callback)(void*), void* context);
void bridge_set_hall_call_sound_callback(void (*callback)(void*), void* context);
void bridge_set_emergency_sound_callback(void (*callback)(void*), void* context);
void bridge_set_stairs_back_sound_callback(void (*callback)(void*), void* context);
void bridge_set_options_sound_callbacks(void (*open)(void*),
                                       void (*mute_click)(unsigned, void*), void* context);
void bridge_set_slider_sound_callback(void (*step)(bool increasing, void*), void* context);
// Volume of the cabin motion loop as a percentage, including braking.
void bridge_set_cabin_motion_sound_callback(void (*callback)(unsigned, void*), void* context);
// Reports the start (or reversal) of visible door movement.
void bridge_set_door_sound_callback(void (*callback)(bool opening, unsigned ticks, void*), void* context);
// Reports additional input, such as a keyboard press in the desktop port.
void bridge_note_interaction(void);
int bridge_get_pressed_elevator_button(void);
// Returns whether the confirmed floor change should play the arrival sound.
bool bridge_set_elevator_level(int level);
void bridge_set_elevator_indicator_test_mode(bool enabled);
// Ajusta el intervalo entre cuadros en llamadas a bridge_render_frame.
void bridge_set_level_indicator_frame_delay(int render_frames);
// Define el tamaño en coordenadas de la imagen de fondo.
void bridge_set_level_indicator_size(int width, int height);

// La PC (SDL2) o el ESP32 inyectan la matriz de colores RGB565.
void bridge_set_sprite(SpriteID id, const uint16_t* pixel_data, int width, int height);

// The aligned RGB565 pack must stay readable until bridge_cleanup().
size_t bridge_graphics_pack_size(void);
bool bridge_bind_graphics(const void* pack, size_t bytes);

// Draw the intro directly into a panel-sized RGB565 buffer.
enum {
    BRIDGE_PANEL_WIDTH = 1024, BRIDGE_PANEL_HEIGHT = 600,
    BRIDGE_INTRO_OPEN_MS = 2015, BRIDGE_INTRO_END_MS = BRIDGE_INTRO_OPEN_MS + 1800
};
enum { BRIDGE_ZOOM_STEPS = 15, BRIDGE_LOBBY_COUNT = 3, BRIDGE_CABIN = 3 };
typedef struct { int x, y, w, h; } BridgeRect;
bool bridge_prepare_zoom_scene(int scene);
bool bridge_prepare_closed_lobby_zoom(int scene, BridgeRect* door);
void bridge_zoom_values(int step, int* zoom, int* brightness);

// Optional native panel backend. All surfaces have panel dimensions.
typedef struct {
    uint16_t *lobbies[BRIDGE_LOBBY_COUNT], *cabin, *snapshot, *intro_doors;
    bool (*copy_columns)(uint16_t* dst, int dx, const uint16_t* src, int sx, int width);
    bool (*clear_screen)(uint16_t* dst);
    void (*start_zoom)(uint16_t* dst, int step, int scene, bool closed_door);
    bool (*zoom_fade)(uint16_t* dst, const uint16_t* src, int step, int scene,
                      const BridgeRect* layers, int layer_count, bool closed_door);
} BridgePanelGraphics;
bool bridge_prepare_panel_graphics(const BridgePanelGraphics* graphics);
bool bridge_prepare_software_cabin(uint16_t* scratch, int width, int height);
int bridge_intro_frame_at(uint32_t elapsed_ms);
bool bridge_render_intro(uint16_t* framebuffer, uint32_t elapsed_ms);

// Dibuja la imagen centrada y ajustada a la pantalla sin cambiar su proporción.
bool bridge_render_frame(uint16_t* framebuffer_rgb565, int screen_width, int screen_height);
// Advance the reference 60 Hz timeline from a monotonic clock.
bool bridge_render_frame_at(uint16_t* framebuffer, int width, int height, uint64_t now_us);
// Indica si la última llamada a bridge_render_frame recompuso el framebuffer.
bool bridge_frame_was_rendered(void);
uint32_t bridge_frame_revision(void);
// Dirty scanlines of the last composed frame, [top, bottom).
void bridge_frame_rows(int* top, int* bottom);

// Limpieza (útil al cerrar el simulador en PC).
void bridge_cleanup(void);

#ifdef __cplusplus
}
#endif

#endif // ENGINE_BRIDGE_H
