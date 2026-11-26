#include "panel_graphics.h"
#include "engine_bridge.h"
#include "esp_async_fbcpy.h"
#include "driver/ppa.h"
#include "esp_cache.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>

enum { W = BRIDGE_PANEL_WIDTH, H = BRIDGE_PANEL_HEIGHT, FRAME_BYTES = W * H * 2, ZOOM_W = W / 2, ZOOM_H = H / 2, BLACK_PIXELS = 65536 };
static const unsigned dma_caps = MALLOC_CAP_DMA | MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
static esp_async_fbcpy_handle_t dma;
static ppa_client_handle_t scaler;
static ppa_client_handle_t black_filler;
static StaticSemaphore_t done_storage;
static SemaphoreHandle_t done;
static StaticSemaphore_t scale_done_storage;
static SemaphoreHandle_t scale_done;
static bool scale_pending;
static BridgePanelGraphics graphics;

// Each cached rectangle contains only the stationary artwork visible during
// this zoom step. Actors, doors, floor numbers and button states come from the
// live snapshot. The two endpoints use the original background and pure black.
typedef struct {
    uint16_t *pixels;
    BridgeRect door;
    uint16_t *door_poses;
    bool closed_door;
} zoom_base_t;
static zoom_base_t backgrounds[BRIDGE_LOBBY_COUNT][BRIDGE_ZOOM_STEPS + 1];
static uint16_t source_x[W], source_y[H], red_green[2048], blue[32];
static uint16_t *black;
static uint16_t *cabin_zoom;
static int map_step = -1;

static bool IRAM_ATTR copied(esp_async_fbcpy_handle_t handle, esp_async_fbcpy_event_data_t *event, void *ctx) {
    BaseType_t wake = pdFALSE;
    xSemaphoreGiveFromISR(done, &wake);
    return wake == pdTRUE;
}

static bool blit(uint16_t *dst, int dx, int dy, const uint16_t *src, int sw, int sh,
                 int sx, int sy, int width, int height) {
    if (width <= 0 || height <= 0) return true;
    esp_async_fbcpy_trans_desc_t t = {
        .src_buffer = src, .dst_buffer = dst, .src_buffer_size_x = sw, .src_buffer_size_y = sh,
        .dst_buffer_size_x = W, .dst_buffer_size_y = H, .src_offset_x = sx, .src_offset_y = sy,
        .dst_offset_x = dx, .dst_offset_y = dy, .copy_size_x = width, .copy_size_y = height,
        .pixel_format_unique_id = {.color_type_id = COLOR_TYPE_ID(COLOR_SPACE_RGB, COLOR_PIXEL_RGB565)},
    };
    // Source and destination can also have CPU-drawn layers. Publish and
    // invalidate whole cache lines before releasing this rectangle to DMA.
    if (esp_cache_msync((void *)(src + sy * sw), (size_t)sw * height * 2,
                        ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED) != ESP_OK ||
        esp_cache_msync(dst + dy * W, (size_t)W * height * 2,
                        ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_INVALIDATE) != ESP_OK) return false;
    if (esp_async_fbcpy(dma, &t, copied, NULL) != ESP_OK) return false;
    return xSemaphoreTake(done, portMAX_DELAY) == pdTRUE;
}

static bool copy_columns(uint16_t *dst, int dx, const uint16_t *src, int sx, int width) {
    return blit(dst, dx, 0, src, W, H, sx, 0, width, H);
}

static bool fill_black(uint16_t *dst, int x, int y, int w, int h) {
    if (w <= 0 || h <= 0) return true;
    const int src_rows = BLACK_PIXELS / w;
    while (h > 0) {
        const int rows = h < src_rows ? h : src_rows;
        if (!blit(dst, x, y, black, w, src_rows, 0, 0, w, rows)) return false;
        y += rows;
        h -= rows;
    }
    return true;
}

static bool clear_screen(uint16_t *dst) {
    if (black_filler) {
        const ppa_fill_oper_config_t fill = {
            .out = {.buffer = dst, .buffer_size = FRAME_BYTES, .pic_w = W, .pic_h = H,
                    .fill_cm = PPA_FILL_COLOR_MODE_RGB565},
            .fill_block_w = W, .fill_block_h = H,
            .fill_color_val = 0, .mode = PPA_TRANS_MODE_BLOCKING,
        };
        if (ppa_do_fill(black_filler, &fill) == ESP_OK) return true;
    }
    return fill_black(dst, 0, 0, W, H);
}

static void prepare_maps(int step) {
    if (step == map_step) return;
    int zoom, brightness;
    bridge_zoom_values(step, &zoom, &brightness);
    for (int i = 0; i < W; ++i) source_x[i] = W / 2 + (i - W / 2) * 1000 / zoom;
    for (int i = 0; i < H; ++i) source_y[i] = H / 2 + (i - H / 2) * 1000 / zoom;
    for (int r = 0; r < 32; ++r) {
        blue[r] = r * brightness / 255;
        for (int g = 0; g < 64; ++g)
            red_green[r * 64 + g] = (blue[r] << 11) | ((g * brightness / 255) << 5);
    }
    map_step = step;
}

static int lower_bound_coord(const uint16_t *coords, int size, int value) {
    int lo = 0, hi = size;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (coords[mid] < value) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}

static BridgeRect project(BridgeRect r) {
    const int x = lower_bound_coord(source_x, W, r.x);
    const int y = lower_bound_coord(source_y, H, r.y);
    return (BridgeRect){x, y, lower_bound_coord(source_x, W, r.x + r.w) - x,
                              lower_bound_coord(source_y, H, r.y + r.h) - y};
}

static void sample_zoom(uint16_t *dst, const uint16_t *snapshot) {
    for (int y = 0; y < ZOOM_H; ++y) {
        const uint16_t *row = snapshot + source_y[y * 2] * W;
        for (int x = 0; x < ZOOM_W; ++x) {
            const uint16_t color = row[source_x[x * 2]];
            *dst++ = red_green[color >> 5] | blue[color & 31];
        }
    }
}

static bool IRAM_ATTR scaled(ppa_client_handle_t handle, ppa_event_data_t *event, void *ctx) {
    if (!ctx) return false;
    BaseType_t wake = pdFALSE;
    xSemaphoreGiveFromISR((SemaphoreHandle_t)ctx, &wake);
    return wake == pdTRUE;
}

static bool submit_scale(uint16_t *dst, const uint16_t *src, bool async) {
    if (!scaler) return false;
    const ppa_srm_oper_config_t op = {
        .in = {.buffer = src, .pic_w = ZOOM_W, .pic_h = ZOOM_H, .block_w = ZOOM_W,
               .block_h = ZOOM_H, .srm_cm = PPA_SRM_COLOR_MODE_RGB565},
        .out = {.buffer = dst, .buffer_size = FRAME_BYTES, .pic_w = W, .pic_h = H,
                .srm_cm = PPA_SRM_COLOR_MODE_RGB565},
        .scale_x = 2.0f, .scale_y = 2.0f,
        .mode = async ? PPA_TRANS_MODE_NON_BLOCKING : PPA_TRANS_MODE_BLOCKING,
        .user_data = async ? scale_done : NULL,
    };
    return ppa_do_scale_rotate_mirror(scaler, &op) == ESP_OK;
}

static bool select_zoom_door(zoom_base_t *base, bool closed) {
    if (!base->door_poses || base->closed_door == closed) return true;
    const BridgeRect r = base->door;
    const uint16_t *src = base->door_poses + (closed ? r.w * r.h : 0);
    uint16_t *dst = base->pixels + r.y * ZOOM_W;
    for (int y = 0; y < r.h; ++y)
        memcpy(dst + y * ZOOM_W + r.x, src + y * r.w, r.w * sizeof(*src));
    if (esp_cache_msync(dst, (size_t)r.h * ZOOM_W * sizeof(*dst),
                        ESP_CACHE_MSYNC_FLAG_DIR_C2M) != ESP_OK) return false;
    base->closed_door = closed;
    return true;
}

static void start_zoom(uint16_t *dst, int step, int scene, bool closed_door) {
    zoom_base_t *base = &backgrounds[scene][step];
    if (base->pixels && select_zoom_door(base, closed_door))
        scale_pending = submit_scale(dst, base->pixels, true);
}

static void expand_zoom(uint16_t *dst, const uint16_t *src) {
    if (scale_pending) {
        xSemaphoreTake(scale_done, portMAX_DELAY);
        scale_pending = false;
        return;
    }
    if (submit_scale(dst, src, false)) return;
    for (int y = 0; y < ZOOM_H; ++y) {
        uint32_t *top = (uint32_t *)(dst + y * 2 * W);
        uint32_t *bottom = top + ZOOM_W;
        for (int x = 0; x < ZOOM_W; ++x) {
            const uint32_t pixel = *src++;
            const uint32_t pair = pixel | (pixel << 16);
            top[x] = pair;
            bottom[x] = pair;
        }
    }
}

static void shade_lobby_layers(uint16_t *dst, BridgeRect r, const uint16_t *snapshot,
                               const uint16_t *background) {
    for (int y = r.y; y < r.y + r.h; ++y) {
        const int offset = source_y[y] * W;
        for (int x = r.x; x < r.x + r.w; ++x) {
            const int sample = offset + source_x[x];
            const uint16_t color = snapshot[sample];
            if (color != background[sample]) dst[y * W + x] = red_green[color >> 5] | blue[color & 31];
        }
    }
}

static bool prepare_zoom_doors(int scene) {
    BridgeRect door;
    if (!bridge_prepare_closed_lobby_zoom(scene, &door)) return false;
    for (int step = 1; step < BRIDGE_ZOOM_STEPS; ++step) {
        zoom_base_t *base = &backgrounds[scene][step];
        if (!base->pixels) continue;
        prepare_maps(step);
        const BridgeRect area = project(door);
        const int x = (area.x + 1) / 2, y = (area.y + 1) / 2;
        const BridgeRect r = {x, y, (area.x + area.w + 1) / 2 - x,
                                  (area.y + area.h + 1) / 2 - y};
        if (!r.w || !r.h) continue;
        const size_t cells = (size_t)r.w * r.h;
        base->door = r;
        base->door_poses = heap_caps_malloc(cells * 2 * sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!base->door_poses) return false;
        for (int row = 0; row < r.h; ++row) {
            const size_t offset = (size_t)row * r.w;
            memcpy(base->door_poses + offset, base->pixels + (r.y+row)*ZOOM_W+r.x, r.w * sizeof(uint16_t));
            const uint16_t *src = graphics.snapshot + source_y[(r.y+row)*2] * W;
            uint16_t *closed = base->door_poses + cells + offset;
            for (int col = 0; col < r.w; ++col) {
                const uint16_t color = src[source_x[(r.x+col)*2]];
                closed[col] = red_green[color >> 5] | blue[color & 31];
            }
        }
    }
    return true;
}

static bool prepare_zoom_bases(void) {
    for (int scene = 0; scene < BRIDGE_LOBBY_COUNT; ++scene) {
        if (!bridge_prepare_zoom_scene(scene)) return false;
        for (int step = 1; step < BRIDGE_ZOOM_STEPS; ++step) {
            prepare_maps(step);
            if (!red_green[2047] && !blue[31]) continue;
            zoom_base_t *base = &backgrounds[scene][step];
            base->pixels = heap_caps_malloc(ZOOM_W * ZOOM_H * sizeof(*base->pixels), dma_caps);
            if (!base->pixels) return false;
            sample_zoom(base->pixels, graphics.snapshot);
        }
        if (!prepare_zoom_doors(scene)) return false;
    }
    return true;
}

static bool zoom_fade(uint16_t *dst, const uint16_t *snapshot, int step, int scene,
                      const BridgeRect *layers, int count, bool closed_door) {
    // The endpoints are the ordinary scene (composed by Bridge) and black.
    prepare_maps(step);
    if (scene == BRIDGE_CABIN) {
        if (!red_green[2047] && !blue[31]) return clear_screen(dst);
        if (step <= 0) return copy_columns(dst, 0, snapshot, 0, W);
        // Shade a quarter of the pixels on the CPU, then use the same
        // hardware scale as the lobby. Both axes share a uniform transform.
        sample_zoom(cabin_zoom, snapshot);
        if (esp_cache_msync(cabin_zoom, ZOOM_W * ZOOM_H * sizeof(*cabin_zoom),
                            ESP_CACHE_MSYNC_FLAG_DIR_C2M) != ESP_OK) return false;
        expand_zoom(dst, cabin_zoom);
        return true;
    }
    zoom_base_t *base = &backgrounds[scene][step];
    if (!base->pixels) return clear_screen(dst);
    if (!select_zoom_door(base, closed_door)) return false;
    expand_zoom(dst, base->pixels);
    for (int i = 0; i < count; ++i)
        shade_lobby_layers(dst, project(layers[i]), snapshot, graphics.lobbies[scene]);
    return true;
}

esp_err_t panel_graphics_init(void) {
    done = xSemaphoreCreateBinaryStatic(&done_storage);
    const esp_async_fbcpy_config_t config = {};
    esp_err_t err = esp_async_fbcpy_install(&config, &dma);
    if (err != ESP_OK) return err;
    const ppa_client_config_t scale_config = {.oper_type = PPA_OPERATION_SRM};
    if (ppa_register_client(&scale_config, &scaler) == ESP_OK) {
        scale_done = xSemaphoreCreateBinaryStatic(&scale_done_storage);
        const ppa_event_callbacks_t callbacks = {.on_trans_done = scaled};
        if (ppa_client_register_event_callbacks(scaler, &callbacks) != ESP_OK) {
            ppa_unregister_client(scaler);
            scaler = NULL;
        }
    }
    if (!scaler) ESP_LOGW("panel_graphics", "No se pudo iniciar el escalador PPA");
    const ppa_client_config_t fill_config = {.oper_type = PPA_OPERATION_FILL};
    if (ppa_register_client(&fill_config, &black_filler) != ESP_OK) black_filler = NULL;
    graphics.copy_columns = copy_columns;
    graphics.clear_screen = clear_screen;
    graphics.start_zoom = start_zoom;
    graphics.zoom_fade = zoom_fade;
    for (int i = 0; i < BRIDGE_LOBBY_COUNT; ++i)
        graphics.lobbies[i] = heap_caps_malloc(FRAME_BYTES, dma_caps);
    graphics.cabin = heap_caps_malloc(FRAME_BYTES, dma_caps);
    graphics.snapshot = heap_caps_malloc(FRAME_BYTES, dma_caps);
    graphics.intro_doors = heap_caps_malloc(FRAME_BYTES, dma_caps);
    black = heap_caps_calloc(BLACK_PIXELS, sizeof(*black), dma_caps);
    cabin_zoom = heap_caps_malloc(ZOOM_W * ZOOM_H * sizeof(*cabin_zoom), dma_caps);
    if (black && cabin_zoom && bridge_prepare_panel_graphics(&graphics) && prepare_zoom_bases()) return ESP_OK;
    bridge_prepare_panel_graphics(NULL);
    for (int scene = 0; scene < BRIDGE_LOBBY_COUNT; ++scene) {
        for (int step = 1; step < BRIDGE_ZOOM_STEPS; ++step) {
            heap_caps_free(backgrounds[scene][step].pixels);
            heap_caps_free(backgrounds[scene][step].door_poses);
            backgrounds[scene][step] = (zoom_base_t){0};
        }
    }
    heap_caps_free(black);
    heap_caps_free(cabin_zoom);
    panel_graphics_finish_intro();
    heap_caps_free(graphics.snapshot);
    heap_caps_free(graphics.cabin);
    for (int i = 0; i < BRIDGE_LOBBY_COUNT; ++i) heap_caps_free(graphics.lobbies[i]);
    if (scaler) ppa_unregister_client(scaler);
    if (black_filler) ppa_unregister_client(black_filler);
    esp_async_fbcpy_uninstall(dma);
    return ESP_ERR_NO_MEM;
}


void panel_graphics_finish_intro(void) {
    heap_caps_free(graphics.intro_doors);
    graphics.intro_doors = NULL;
}
