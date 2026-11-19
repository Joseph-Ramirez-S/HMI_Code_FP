#include "panel_graphics.h"
#include "engine_bridge.h"
#include "esp_async_fbcpy.h"
#include "esp_cache.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>

enum { W = BRIDGE_PANEL_WIDTH, H = BRIDGE_PANEL_HEIGHT, FRAME_BYTES = W * H * 2, BLACK_PIXELS = 65536 };
static const unsigned dma_caps = MALLOC_CAP_DMA | MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
static esp_async_fbcpy_handle_t dma;
static StaticSemaphore_t done_storage;
static SemaphoreHandle_t done;
static BridgePanelGraphics graphics;

// Each cached rectangle contains only the stationary artwork visible during
// this zoom step. Actors, doors, floor numbers and button states come from the
// live snapshot. The two endpoints use the original background and pure black.
typedef struct {
    BridgeRect area;
    uint16_t *pixels;
} zoom_base_t;
static zoom_base_t backgrounds[2][BRIDGE_ZOOM_STEPS + 1];
static uint16_t source_x[W], source_y[H], red_green[2048], blue[32];
static uint16_t *black;
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

static void shade_rect(uint16_t *dst, int stride, BridgeRect area, const uint16_t *frame) {
    for (int y = 0; y < area.h; ++y) {
        const uint16_t *src = frame + source_y[area.y + y] * W;
        for (int x = 0; x < area.w; ++x) {
            const uint16_t c = src[source_x[area.x + x]];
            dst[x] = red_green[c >> 5] | blue[c & 31];
        }
        dst += stride;
    }
}

static BridgeRect visible_bounds(const uint16_t *frame) {
    int left = W, top = H, right = 0, bottom = 0;
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            if (frame[y * W + x]) {
                if (x < left) left = x;
                if (x >= right) right = x + 1;
                if (y < top) top = y;
                bottom = y + 1;
            }
        }
    }
    return right ? (BridgeRect){left, top, right - left, bottom - top} : (BridgeRect){0};
}

static bool prepare_zoom_bases(void) {
    for (int i = 0; i < 2; ++i) {
        if (!bridge_prepare_zoom_scene(i != 0)) return false;
        BridgeRect bounds = visible_bounds(graphics.snapshot);
        for (int step = 1; step < BRIDGE_ZOOM_STEPS; ++step) {
            prepare_maps(step);
            if (!red_green[2047] && !blue[31]) continue;
            zoom_base_t *base = &backgrounds[i][step];
            base->area = project(bounds);
            if (!base->area.w || !base->area.h) continue;
            base->pixels = heap_caps_malloc(base->area.w * base->area.h * 2, dma_caps);
            if (!base->pixels) return false;
            shade_rect(base->pixels, base->area.w, base->area, graphics.snapshot);
        }
    }
    return true;
}

static bool zoom_fade(uint16_t *dst, const uint16_t *snapshot, int step, bool inside,
                      const BridgeRect *layers, int count) {
    // The endpoints are the ordinary scene (composed by Bridge) and black.
    prepare_maps(step);
    const zoom_base_t *base = &backgrounds[inside][step];
    const BridgeRect r = base->area;
    if (!base->pixels) return fill_black(dst, 0, 0, W, H);
    // Copy disjoint bands so each destination pixel is written just once.
    if (!fill_black(dst, 0, 0, W, r.y) ||
        !fill_black(dst, 0, r.y, r.x, r.h) ||
        !blit(dst, r.x, r.y, base->pixels, r.w, r.h, 0, 0, r.w, r.h) ||
        !fill_black(dst, r.x+r.w, r.y, W-r.x-r.w, r.h) ||
        !fill_black(dst, 0, r.y+r.h, W, H-r.y-r.h)) return false;
    for (int i = 0; i < count; ++i) {
        const BridgeRect area = project(layers[i]);
        if (area.w && area.h) shade_rect(dst + area.y * W + area.x, W, area, snapshot);
    }
    return true;
}

esp_err_t panel_graphics_init(void) {
    done = xSemaphoreCreateBinaryStatic(&done_storage);
    const esp_async_fbcpy_config_t config = {};
    esp_err_t err = esp_async_fbcpy_install(&config, &dma);
    if (err != ESP_OK) return err;
    graphics.copy_columns = copy_columns;
    graphics.zoom_fade = zoom_fade;
    graphics.lobby = heap_caps_malloc(FRAME_BYTES, dma_caps);
    graphics.cabin = heap_caps_malloc(FRAME_BYTES, dma_caps);
    graphics.snapshot = heap_caps_malloc(FRAME_BYTES, dma_caps);
    graphics.intro_doors = heap_caps_malloc(FRAME_BYTES, dma_caps);
    black = heap_caps_calloc(BLACK_PIXELS, sizeof(*black), dma_caps);
    if (black && bridge_prepare_panel_graphics(&graphics) && prepare_zoom_bases()) return ESP_OK;
    bridge_prepare_panel_graphics(NULL);
    for (int scene = 0; scene < 2; ++scene)
        for (int step = 1; step < BRIDGE_ZOOM_STEPS; ++step)
            heap_caps_free(backgrounds[scene][step].pixels);
    heap_caps_free(black);
    panel_graphics_finish_intro();
    heap_caps_free(graphics.snapshot);
    heap_caps_free(graphics.cabin);
    heap_caps_free(graphics.lobby);
    esp_async_fbcpy_uninstall(dma);
    return ESP_ERR_NO_MEM;
}


void panel_graphics_finish_intro(void) {
    heap_caps_free(graphics.intro_doors);
    graphics.intro_doors = NULL;
}
