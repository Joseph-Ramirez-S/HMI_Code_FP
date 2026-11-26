#include "intro_audio.h"
#include "audio_sd.h"
#include "audio_effects.h"
#include "engine_bridge.h"
#include "music_stream.h"

#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    SAMPLE_RATE = BRIDGE_AUDIO_RATE,
    FRAME_BYTES = 4, // 16-bit PCM, stereo
    MAX_PCM_BYTES = SAMPLE_RATE * FRAME_BYTES * 5,
    DMA_FRAMES = 256,
    DMA_COUNT = 4,
    MUSIC_BUFFER_BYTES = 32 * 1024,
    MUSIC_DECODE_FRAMES = 1152,
    AUDIO_EVENT_QUEUE_SIZE = 8,
    PANEL_MUSIC_GAIN_PERCENT = 60,
    PANEL_MIX_PEAK = INT16_MAX * 55 / 100,
    PANEL_MIX_GAIN_UNIT = 256,
    AMP_ENABLE = GPIO_NUM_30
};

static const char TAG[] = "intro_audio";
typedef struct {
    uint8_t *pcm;
    size_t size;
} AudioClip;

static AudioClip intro, buttons, arrival;
static StaticSemaphore_t start_signal_storage;
static SemaphoreHandle_t start_signal;
typedef enum { AUDIO_BUTTON, AUDIO_DOOR_OPEN, AUDIO_DOOR_CLOSE, AUDIO_ARRIVAL,
               AUDIO_HINT, AUDIO_LOAD_HINT, AUDIO_SCENE, AUDIO_LOAD_SCENE,
               AUDIO_HALL_CALL, AUDIO_LOAD_HALL_CALL, AUDIO_EMERGENCY, AUDIO_LOAD_EMERGENCY,
               AUDIO_WAKE_MOTION, AUDIO_LOAD_MOTION, AUDIO_STAIRS_BACK, AUDIO_LOAD_STAIRS_BACK,
               AUDIO_OPTIONS_OPEN, AUDIO_LOAD_OPTIONS, AUDIO_MUTE_CLICK, AUDIO_WAKE_MUSIC,
               AUDIO_SLIDER, AUDIO_LOAD_SLIDER } AudioEventType;
typedef struct { AudioEventType type; unsigned ticks; AudioClip clip; } AudioEvent;
static QueueHandle_t audio_events;
static atomic_uint motion_volume = ATOMIC_VAR_INIT(0);
enum { AUDIO_STARTING, AUDIO_RUNNING, AUDIO_STOPPED };
static atomic_int audio_state = ATOMIC_VAR_INIT(AUDIO_STARTING);
static atomic_bool music_playing;
static atomic_uint music_refills;
static EXT_RAM_BSS_ATTR uint8_t music_data[MUSIC_BUFFER_BYTES + 1];
static StaticStreamBuffer_t music_buffer_control;
static StreamBufferHandle_t music_buffer;
static i2s_chan_handle_t speaker;
static size_t preloaded_bytes;

static uint16_t le16(const uint8_t *bytes) {
    return (uint16_t)bytes[0] | (uint16_t)bytes[1] << 8;
}

static uint32_t le32(const uint8_t *bytes) {
    return (uint32_t)le16(bytes) | (uint32_t)le16(bytes + 2) << 16;
}

// Locate the PCM independently of optional RIFF metadata chunks.
static esp_err_t find_pcm(FILE *file, size_t *pcm_bytes, unsigned *channels) {
    uint8_t riff[12], chunk[8], format[16];
    if (fread(riff, 1, sizeof(riff), file) != sizeof(riff) ||
        memcmp(riff, "RIFF", 4) || memcmp(riff + 8, "WAVE", 4)) return ESP_ERR_INVALID_ARG;
    uint32_t remaining = le32(riff + 4);
    if (remaining < 4 || remaining > MAX_PCM_BYTES + 65536U) return ESP_ERR_INVALID_SIZE;
    remaining -= 4;
    bool has_format = false;
    while (remaining >= sizeof(chunk) && fread(chunk, 1, sizeof(chunk), file) == sizeof(chunk)) {
        remaining -= sizeof(chunk);
        const uint32_t size = le32(chunk + 4);
        if (size > remaining || (size == remaining && (size & 1))) return ESP_ERR_INVALID_SIZE;
        uint32_t skip = size + (size & 1);
        remaining -= skip;
        if (!memcmp(chunk, "fmt ", 4)) {
            if (size < sizeof(format) || fread(format, 1, sizeof(format), file) != sizeof(format))
                return ESP_ERR_INVALID_ARG;
            const unsigned count = le16(format + 2);
            const unsigned frame_bytes = count * sizeof(int16_t);
            if (le16(format) != 1 || (count != 1 && count != 2) || le32(format + 4) != SAMPLE_RATE ||
                le32(format + 8) != SAMPLE_RATE * frame_bytes ||
                le16(format + 12) != frame_bytes || le16(format + 14) != 16) return ESP_ERR_NOT_SUPPORTED;
            *channels = count;
            skip -= sizeof(format);
            has_format = true;
        } else if (!memcmp(chunk, "data", 4)) {
            if (!has_format || !size || size > MAX_PCM_BYTES * *channels / 2 ||
                size % (*channels * sizeof(int16_t))) return ESP_ERR_INVALID_SIZE;
            *pcm_bytes = size;
            return ESP_OK;
        }
        if (fseek(file, (long)skip, SEEK_CUR)) return ESP_FAIL;
    }
    return ESP_ERR_NOT_FOUND;
}

static esp_err_t load_pcm(const char *path, const char *backup, AudioClip *clip) {
    FILE *file = fopen(path, "rb");
    if (!file && errno == ENOENT && backup) file = fopen(backup, "rb");
    if (!file) {
        const int open_error = errno;
        ESP_LOGW(TAG, "microSD montada; no se pudo abrir %s: %s", path, strerror(open_error));
        return open_error == ENOENT ? ESP_ERR_NOT_FOUND : ESP_FAIL;
    }
    size_t bytes = 0;
    unsigned channels = 0;
    esp_err_t err = find_pcm(file, &bytes, &channels);
    if (err == ESP_OK) {
        const size_t stereo_bytes = bytes * 2 / channels;
        uint8_t *pcm = heap_caps_malloc(stereo_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!pcm) err = ESP_ERR_NO_MEM;
        else if (fread(pcm, 1, bytes, file) != bytes) {
            free(pcm);
            err = ESP_FAIL;
        } else {
            if (channels == 1) {
                int16_t *samples = (int16_t *)pcm;
                for (size_t i = bytes / sizeof(*samples); i > 0; --i) {
                    const int16_t value = samples[i - 1];
                    samples[(i - 1) * 2] = value;
                    samples[(i - 1) * 2 + 1] = value;
                }
            }
            *clip = (AudioClip){pcm, stereo_bytes};
            ESP_LOGI(TAG, "%s: %u bytes PCM listos desde la microSD", path, (unsigned)bytes);
        }
    }
    fclose(file);
    if (err != ESP_OK) ESP_LOGW(TAG, "%s requiere PCM mono o estereo de 48 kHz y 16 bits: %s",
                                path, esp_err_to_name(err));
    return err;
}

static esp_err_t prepare_speaker(void) {
    esp_err_t err = gpio_set_level(AMP_ENABLE, 1); // Active-low amplifier mute.
    if (err == ESP_OK) err = gpio_set_direction(AMP_ENABLE, GPIO_MODE_OUTPUT);
    if (err != ESP_OK) return err;
    i2s_chan_config_t channel = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
    channel.dma_desc_num = DMA_COUNT;
    channel.dma_frame_num = DMA_FRAMES;
    channel.auto_clear_after_cb = true;
    err = i2s_new_channel(&channel, &speaker, NULL);
    if (err != ESP_OK) return err;
    const i2s_std_config_t config = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {.mclk = I2S_GPIO_UNUSED, .bclk = GPIO_NUM_22, .ws = GPIO_NUM_21,
                     .dout = GPIO_NUM_23, .din = I2S_GPIO_UNUSED}
    };
    err = i2s_channel_init_std_mode(speaker, &config);
    if (err == ESP_OK && intro.size)
        err = i2s_channel_preload_data(speaker, intro.pcm, intro.size, &preloaded_bytes);
    return err;
}

static void release_audio(void) {
    if (speaker) {
        i2s_del_channel(speaker);
        speaker = NULL;
    }
    free(intro.pcm);
    free(buttons.pcm);
    free(arrival.pcm);
    intro = buttons = arrival = (AudioClip){0};
}

static void retain_door_sound(void) {
    const size_t opening_start = BRIDGE_INTRO_OPEN_MS * (SAMPLE_RATE / 1000) * FRAME_BYTES;
    const size_t opening_end = BRIDGE_INTRO_END_MS * (SAMPLE_RATE / 1000) * FRAME_BYTES;
    if (intro.size < opening_end) {
        free(intro.pcm);
        intro = (AudioClip){0};
        return;
    }
    intro.size = opening_end - opening_start;
    memmove(intro.pcm, intro.pcm + opening_start, intro.size);
    uint8_t *sound = heap_caps_realloc(intro.pcm, intro.size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (sound) intro.pcm = sound;
    ESP_LOGI(TAG, "Efecto puerta: %u cuadros PCM desde INTRO.WAV", (unsigned)(intro.size / FRAME_BYTES));
}

static void stream_music(void *unused) {
    (void)unused;
    const char path[] = AUDIO_SD_MOUNT_PATH "/" BRIDGE_MUSIC_FILENAME;
    BridgeMusicStream *music = bridge_music_open(path);
    if (!music) {
        ESP_LOGW(TAG, "No se pudo abrir el MP3 %s", path);
        vTaskDelete(NULL);
        return;
    }

    while (atomic_load_explicit(&audio_state, memory_order_acquire) == AUDIO_STARTING)
        vTaskDelay(pdMS_TO_TICKS(10));
    int16_t pcm[MUSIC_DECODE_FRAMES * BRIDGE_AUDIO_CHANNELS];
    bool started = false;
    int64_t next_report = esp_timer_get_time() + 60000000;
    while (atomic_load_explicit(&audio_state, memory_order_acquire) == AUDIO_RUNNING) {
        const size_t frames = bridge_music_read(music, pcm, MUSIC_DECODE_FRAMES);
        if (!frames) break;
        const size_t size = frames * FRAME_BYTES;
        size_t sent = 0;
        while (sent < size && atomic_load_explicit(&audio_state, memory_order_acquire) == AUDIO_RUNNING)
            sent += xStreamBufferSend(music_buffer, (const uint8_t *)pcm + sent, size - sent, pdMS_TO_TICKS(50));
        if (!started && xStreamBufferBytesAvailable(music_buffer) >= MUSIC_BUFFER_BYTES / 2) {
            started = true;
            atomic_store_explicit(&music_playing, true, memory_order_release);
            const AudioEvent event = {.type = AUDIO_WAKE_MUSIC};
            xQueueSend(audio_events, &event, 0);
            ESP_LOGI(TAG, "Música en bucle desde %s, PCM estéreo a %u Hz, MUSIC=%u/10",
                     path, (unsigned)SAMPLE_RATE, bridge_get_music_volume());
        }
        if (frames < MUSIC_DECODE_FRAMES) break;
        const int64_t now = esp_timer_get_time();
        if (now >= next_report) {
            ESP_LOGI(TAG, "MP3 continuo: MUSIC=%u/10, SRAM libre=%u, pila libre mínima=%u, recargas=%u",
                     bridge_get_music_volume(), (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                     (unsigned)uxTaskGetStackHighWaterMark(NULL), atomic_load(&music_refills));
            next_report = now + 60000000;
        }
    }
    bridge_music_close(music);
    atomic_store_explicit(&music_playing, false, memory_order_release);
    if (atomic_load_explicit(&audio_state, memory_order_acquire) == AUDIO_RUNNING)
        ESP_LOGW(TAG, "Se interrumpió la lectura del MP3 %s", path);
    vTaskDelete(NULL);
}

static int limit_music_mix(int16_t *pcm, size_t samples, int gain) {
    int peak = 0;
    for (size_t i = 0; i < samples; ++i) {
        const int magnitude = abs((int)pcm[i]);
        if (magnitude > peak) peak = magnitude;
    }
    const int limit = peak > PANEL_MIX_PEAK ? PANEL_MIX_PEAK * PANEL_MIX_GAIN_UNIT / peak : PANEL_MIX_GAIN_UNIT;
    // React to peaks immediately, then release the attenuation over several blocks.
    gain += 8;
    if (gain > limit) gain = limit;
    if (gain != PANEL_MIX_GAIN_UNIT)
        for (size_t i = 0; i < samples; ++i) pcm[i] = (int16_t)(pcm[i] * gain / PANEL_MIX_GAIN_UNIT);
    return gain;
}

static void play_audio(void *arg) {
    (void)arg;
    if (xSemaphoreTake(start_signal, pdMS_TO_TICKS(5000)) == pdTRUE) {
        esp_err_t err = i2s_channel_enable(speaker);
        if (err == ESP_OK) {
            err = gpio_set_level(AMP_ENABLE, 0);
            if (err == ESP_OK && preloaded_bytes < intro.size)
                err = i2s_channel_write(speaker, intro.pcm + preloaded_bytes,
                                        intro.size - preloaded_bytes, NULL, 200);
            retain_door_sound();
            if (!buttons.size && !intro.size && !arrival.size) {
                static const uint32_t silence[DMA_FRAMES] = {0};
                for (int i = 0; i < DMA_COUNT && err == ESP_OK; ++i)
                    err = i2s_channel_write(speaker, silence, sizeof(silence), NULL, 200);
            }
            BridgeAudioMixer mixer = {
                .button = {(const int16_t *)buttons.pcm, buttons.size / FRAME_BYTES},
                .door = {(const int16_t *)intro.pcm, intro.size / FRAME_BYTES},
                .arrival = {(const int16_t *)arrival.pcm, arrival.size / FRAME_BYTES},
                .button_position = buttons.size / FRAME_BYTES,
                .arrival_position = arrival.size / FRAME_BYTES
            };
            AudioClip hint = {0}, scene = {0}, hall_call = {0}, emergency = {0}, motion = {0}, stairs_back = {0};
            AudioClip options = {0}, slider = {0};
            int16_t block[DMA_FRAMES * BRIDGE_AUDIO_CHANNELS];
            int16_t music[DMA_FRAMES * BRIDGE_AUDIO_CHANNELS];
            bool music_buffered = false;
            int panel_mix_gain = PANEL_MIX_GAIN_UNIT;
            unsigned write_timeouts = 0;
            if (err == ESP_OK) atomic_store_explicit(&audio_state, AUDIO_RUNNING, memory_order_release);
            while (err == ESP_OK) {
                AudioEvent event;
                TickType_t wait = bridge_audio_active(&mixer) ||
                    atomic_load_explicit(&music_playing, memory_order_acquire) ? 0 : portMAX_DELAY;
                for (unsigned i = 0; i < AUDIO_EVENT_QUEUE_SIZE &&
                     xQueueReceive(audio_events, &event, wait) == pdTRUE; ++i) {
                    switch (event.type) {
                    case AUDIO_BUTTON: bridge_audio_button(&mixer); break;
                    case AUDIO_DOOR_OPEN:
                    case AUDIO_DOOR_CLOSE:
                        bridge_audio_door(&mixer, event.type == AUDIO_DOOR_OPEN, event.ticks);
                        break;
                    case AUDIO_ARRIVAL: mixer.arrival_position = 0; break;
                    case AUDIO_HINT:
                        if (mixer.hint.frames) {
                            mixer.hint_position = 0;
                            ESP_LOGI(TAG, "S1_BF: reproduciendo %u cuadros al %u%%",
                                     (unsigned)mixer.hint.frames, (unsigned)ELEVATOR_HINT_VOLUME_PERCENT);
                        }
                        break;
                    case AUDIO_LOAD_HINT:
                        free(hint.pcm);
                        hint = event.clip;
                        mixer.hint = (BridgeAudioClip){(const int16_t *)hint.pcm, hint.size / FRAME_BYTES};
                        mixer.hint_position = mixer.hint.frames;
                        break;
                    case AUDIO_SCENE: mixer.scene_position = 0; break;
                    case AUDIO_LOAD_SCENE:
                        free(scene.pcm);
                        scene = event.clip;
                        mixer.scene = (BridgeAudioClip){(const int16_t *)scene.pcm, scene.size / FRAME_BYTES};
                        mixer.scene_position = mixer.scene.frames;
                        break;
                    case AUDIO_HALL_CALL: mixer.hall_call_position = 0; break;
                    case AUDIO_LOAD_HALL_CALL:
                        free(hall_call.pcm);
                        hall_call = event.clip;
                        mixer.hall_call = (BridgeAudioClip){
                            (const int16_t *)hall_call.pcm, hall_call.size / FRAME_BYTES};
                        mixer.hall_call_position = mixer.hall_call.frames;
                        break;
                    case AUDIO_EMERGENCY: bridge_audio_emergency(&mixer); break;
                    case AUDIO_LOAD_EMERGENCY:
                        free(emergency.pcm);
                        emergency = event.clip;
                        mixer.emergency = (BridgeAudioClip){
                            (const int16_t *)emergency.pcm, emergency.size / FRAME_BYTES};
                        mixer.emergency_position = mixer.emergency.frames;
                        break;
                    case AUDIO_WAKE_MOTION: break;
                    case AUDIO_WAKE_MUSIC: break;
                    case AUDIO_LOAD_MOTION:
                        free(motion.pcm);
                        motion = event.clip;
                        mixer.cabin_motion = (BridgeAudioClip){
                            (const int16_t *)motion.pcm, motion.size / FRAME_BYTES};
                        mixer.cabin_motion_position = 0;
                        break;
                    case AUDIO_STAIRS_BACK: bridge_audio_stairs_back(&mixer); break;
                    case AUDIO_LOAD_STAIRS_BACK:
                        free(stairs_back.pcm);
                        stairs_back = event.clip;
                        mixer.stairs_back = (BridgeAudioClip){
                            (const int16_t *)stairs_back.pcm, stairs_back.size / FRAME_BYTES};
                        mixer.stairs_back_position = mixer.stairs_back.frames;
                        break;
                    case AUDIO_OPTIONS_OPEN: mixer.options_open_position = 0; break;
                    case AUDIO_MUTE_CLICK: bridge_audio_mute_click(&mixer, event.ticks); break;
                    case AUDIO_SLIDER: bridge_audio_slider_step(&mixer, event.ticks != 0); break;
                    case AUDIO_LOAD_OPTIONS:
                        free(options.pcm);
                        options = event.clip;
                        mixer.options_open = (BridgeAudioClip){
                            (const int16_t *)options.pcm, options.size / FRAME_BYTES};
                        mixer.options_open_position = mixer.options_open.frames;
                        break;
                    case AUDIO_LOAD_SLIDER:
                        free(slider.pcm);
                        slider = event.clip;
                        mixer.slider = (BridgeAudioClip){
                            (const int16_t *)slider.pcm, slider.size / FRAME_BYTES};
                        mixer.slider_position = mixer.slider_end = 0;
                        break;
                    }
                    wait = 0;
                }
                bridge_audio_cabin_motion(&mixer, atomic_load_explicit(&motion_volume, memory_order_acquire));
                const bool play_music = atomic_load_explicit(&music_playing, memory_order_acquire);
                if (!bridge_audio_active(&mixer) && !play_music) continue;
                // Refill the reservoir after an SD read delay before mixing music again.
                if (play_music) {
                    const size_t threshold = music_buffered ? sizeof(music) : MUSIC_BUFFER_BYTES / 4;
                    const bool ready = xStreamBufferBytesAvailable(music_buffer) >= threshold;
                    if (music_buffered && !ready) atomic_fetch_add(&music_refills, 1);
                    music_buffered = ready;
                } else {
                    music_buffered = false;
                }
                const size_t music_frames = music_buffered
                    ? xStreamBufferReceive(music_buffer, music, sizeof(music), 0) / FRAME_BYTES : 0;
                const bool hint_was_playing = mixer.hint_position < mixer.hint.frames;
                // Keep headroom on the panel speaker for music peaks plus sound effects.
                const int music_gain = (int)bridge_get_music_volume() * PANEL_MUSIC_GAIN_PERCENT / 10;
                bridge_audio_mix_tracks(&mixer, block, DMA_FRAMES, bridge_get_sfx_volume(),
                                        music, music_frames, music_gain);
                if (music_gain && play_music)
                    panel_mix_gain = limit_music_mix(block, sizeof(block) / sizeof(block[0]), panel_mix_gain);
                else
                    panel_mix_gain = PANEL_MIX_GAIN_UNIT;
                err = i2s_channel_write(speaker, block, sizeof(block), NULL, 200);
                if (err == ESP_ERR_TIMEOUT && write_timeouts < 2) {
                    if (!write_timeouts) ESP_LOGW(TAG, "Retraso de I2S; se reanuda el siguiente bloque");
                    ++write_timeouts;
                    err = ESP_OK;
                } else if (err == ESP_OK) {
                    write_timeouts = 0;
                }
                if (err == ESP_OK && hint_was_playing && mixer.hint_position == mixer.hint.frames)
                    ESP_LOGI(TAG, "S1_BF: reproduccion completa");
            }
            atomic_store_explicit(&audio_state, AUDIO_STOPPED, memory_order_release);
            free(hint.pcm);
            free(scene.pcm);
            free(hall_call.pcm);
            free(emergency.pcm);
            free(motion.pcm);
            free(stairs_back.pcm);
            free(options.pcm);
            free(slider.pcm);
            gpio_set_level(AMP_ENABLE, 1);
            i2s_channel_disable(speaker);
        }
        if (err != ESP_OK) ESP_LOGW(TAG, "Reproduccion interrumpida: %s", esp_err_to_name(err));
    }
    // The event queue remains valid if either caller posts after a failure.
    atomic_store_explicit(&audio_state, AUDIO_STOPPED, memory_order_release);
    release_audio();
    vTaskDelete(NULL);
}

esp_err_t intro_audio_prepare(void) {
    esp_err_t err = audio_sd_mount();
    if (err != ESP_OK) return err;
    load_pcm(AUDIO_SD_MOUNT_PATH "/INTRO.WAV", NULL, &intro);
    load_pcm(AUDIO_SD_BUTTONS, AUDIO_SD_BUTTONS_BACKUP, &buttons);
    load_pcm(AUDIO_SD_ARRIVAL, AUDIO_SD_ARRIVAL_BACKUP, &arrival);
    err = prepare_speaker();
    if (err != ESP_OK) {
        release_audio();
        return err;
    }
    start_signal = xSemaphoreCreateBinaryStatic(&start_signal_storage);
    audio_events = xQueueCreate(AUDIO_EVENT_QUEUE_SIZE, sizeof(AudioEvent));
    if (!audio_events ||
        xTaskCreatePinnedToCore(play_audio, "GameAudio", 6144, NULL, 4, NULL, 0) != pdPASS) {
        if (audio_events) vQueueDelete(audio_events);
        audio_events = NULL;
        start_signal = NULL;
        release_audio();
        return ESP_ERR_NO_MEM;
    }
    return err;
}

void intro_audio_start(void) {
    if (start_signal) xSemaphoreGive(start_signal);
}

void button_audio_request(void *context) {
    (void)context;
    const AudioEvent event = {.type = AUDIO_BUTTON};
    if (audio_events) xQueueSend(audio_events, &event, 0);
}

void door_audio_request(bool opening, unsigned ticks, void *context) {
    (void)context;
    const AudioEvent event = {.type = opening ? AUDIO_DOOR_OPEN : AUDIO_DOOR_CLOSE, .ticks = ticks};
    if (audio_events) xQueueSend(audio_events, &event, 0);
}

void arrival_audio_request(void) {
    const AudioEvent event = {.type = AUDIO_ARRIVAL};
    if (audio_events) xQueueSend(audio_events, &event, 0);
}

static void load_animation_sound(AudioEventType type, const char *path, const char *backup) {
    AudioEvent event = {.type = type};
    load_pcm(path, backup, &event.clip);
    if (event.clip.pcm && xQueueSend(audio_events, &event, pdMS_TO_TICKS(100)) != pdTRUE)
        free(event.clip.pcm);
}

void animation_audio_prepare(void) {
    if (!audio_events) return;
    if (audio_sd_mount() != ESP_OK) return;
    music_buffer = xStreamBufferCreateStatic(sizeof(music_data), 1, music_data, &music_buffer_control);
    if (!music_buffer || xTaskCreatePinnedToCore(stream_music, "MusicDecode", 24576, NULL, 3, NULL, 0) != pdPASS)
        ESP_LOGW(TAG, "No se pudo iniciar la tarea de música");
    load_animation_sound(AUDIO_LOAD_HINT, AUDIO_SD_HINT, AUDIO_SD_HINT_BACKUP);
    load_animation_sound(AUDIO_LOAD_SCENE, AUDIO_SD_SCENE, AUDIO_SD_SCENE_BACKUP);
    load_animation_sound(AUDIO_LOAD_HALL_CALL, AUDIO_SD_HALL_CALL, AUDIO_SD_HALL_CALL_BACKUP);
    load_animation_sound(AUDIO_LOAD_EMERGENCY, AUDIO_SD_EMERGENCY, AUDIO_SD_EMERGENCY_BACKUP);
    load_animation_sound(AUDIO_LOAD_MOTION, AUDIO_SD_CABIN_MOTION, AUDIO_SD_CABIN_MOTION_BACKUP);
    load_animation_sound(AUDIO_LOAD_STAIRS_BACK, AUDIO_SD_STAIRS_BACK, AUDIO_SD_STAIRS_BACK_BACKUP);
    load_animation_sound(AUDIO_LOAD_OPTIONS, AUDIO_SD_OPTIONS, AUDIO_SD_OPTIONS_BACKUP);
    load_animation_sound(AUDIO_LOAD_SLIDER, AUDIO_SD_SLIDER, AUDIO_SD_SLIDER_BACKUP);
}

void hint_audio_request(void *context) {
    (void)context;
    const AudioEvent event = {.type = AUDIO_HINT};
    if (audio_events) xQueueSend(audio_events, &event, 0);
}

void scene_audio_request(void *context) {
    (void)context;
    const AudioEvent event = {.type = AUDIO_SCENE};
    if (audio_events) xQueueSend(audio_events, &event, 0);
}

void hall_call_audio_request(void *context) {
    (void)context;
    const AudioEvent event = {.type = AUDIO_HALL_CALL};
    if (audio_events) xQueueSend(audio_events, &event, 0);
}

void emergency_audio_request(void *context) {
    (void)context;
    const AudioEvent event = {.type = AUDIO_EMERGENCY};
    if (audio_events) xQueueSend(audio_events, &event, 0);
}

void cabin_motion_audio_set(unsigned volume, void *context) {
    (void)context;
    // Keep the latest gain outside the queue so braking and stopping cannot
    // be lost if several sound effects arrive during the same audio block.
    if (atomic_exchange_explicit(&motion_volume, volume, memory_order_acq_rel) == 0 &&
        volume && audio_events) {
        const AudioEvent event = {.type = AUDIO_WAKE_MOTION};
        xQueueSend(audio_events, &event, 0);
    }
}

void stairs_back_audio_request(void *context) {
    (void)context;
    const AudioEvent event = {.type = AUDIO_STAIRS_BACK};
    if (audio_events) xQueueSend(audio_events, &event, 0);
}

void options_open_audio_request(void *context) {
    (void)context;
    const AudioEvent event = {.type = AUDIO_OPTIONS_OPEN};
    if (audio_events) xQueueSend(audio_events, &event, 0);
}

void mute_click_audio_request(unsigned volume, void *context) {
    (void)context;
    const AudioEvent event = {.type = AUDIO_MUTE_CLICK, .ticks = volume};
    if (audio_events) xQueueSend(audio_events, &event, 0);
}

void slider_audio_request(bool increasing, void *context) {
    (void)context;
    const AudioEvent event = {.type = AUDIO_SLIDER, .ticks = increasing};
    if (audio_events) xQueueSend(audio_events, &event, 0);
}
