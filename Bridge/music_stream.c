#include "music_stream.h"
#include "audio_effects.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#include <fcntl.h>
#include <unistd.h>
#endif

// dr_mp3 from mackron/dr_libs, commit 7f032b984ae5dd3ab8a58721375aca9200f91965.
#define DRMP3_DATA_CHUNK_SIZE (32 * 1024)
#define DR_MP3_NO_STDIO
#define DR_MP3_IMPLEMENTATION
#include "dr_mp3.h"

enum { MUSIC_DECODE_FRAMES = 1152 };
#ifdef ESP_PLATFORM
enum { SD_PAGE_BYTES = 8 * 1024, SD_PAGE_ALIGNMENT = 512 };
#endif

struct BridgeMusicStream {
    drmp3 mp3;
#ifndef ESP_PLATFORM
    FILE *file;
#endif
    void *compressed;
    int16_t source[MUSIC_DECODE_FRAMES * BRIDGE_AUDIO_CHANNELS];
    size_t position, available;
    int16_t current[2], next[2];
    unsigned phase;
    bool input_buffer_full;
#ifdef ESP_PLATFORM
    int sd_fd;
    uint8_t *sd_page;
    long sd_page_offset;
    size_t sd_pos, sd_size;
    bool sd_failed;
#endif
};

static void *reserve_input_buffer(void *buffer, size_t bytes, void *context) {
    BridgeMusicStream *stream = (BridgeMusicStream *)context;
    if (bytes <= DRMP3_DATA_CHUNK_SIZE && (!buffer || buffer == stream->compressed))
        return stream->compressed;
    stream->input_buffer_full = true;
    return NULL;
}

static void release_input_buffer(void *buffer, void *context) {
    // The stream owns its fixed input buffer even when the decoder is reset.
    (void)buffer;
    (void)context;
}

#ifdef ESP_PLATFORM
static bool refill_sd_page(BridgeMusicStream *stream) {
    if (stream->sd_failed || stream->sd_size > (size_t)(LONG_MAX - stream->sd_page_offset)) return false;
    stream->sd_page_offset += (long)stream->sd_size;
    stream->sd_pos = 0;
    int count = read(stream->sd_fd, stream->sd_page, SD_PAGE_BYTES);
    if (count < 0) {
        // Retry the same aligned page once, in the decoder task.
        if (lseek(stream->sd_fd, stream->sd_page_offset, SEEK_SET) >= 0)
            count = read(stream->sd_fd, stream->sd_page, SD_PAGE_BYTES);
        stream->sd_failed = count < 0;
    }
    stream->sd_size = count > 0 ? (size_t)count : 0;
    return stream->sd_size > 0;
}
#endif

static size_t read_bytes(void *context, void *buffer, size_t bytes) {
    BridgeMusicStream *stream = (BridgeMusicStream *)context;
#ifdef ESP_PLATFORM
    // Avoid long SDMMC bursts competing with LCD reads from PSRAM.
    if (bytes > 2 * SD_PAGE_BYTES) bytes = 2 * SD_PAGE_BYTES;
    size_t read = 0;
    while (read < bytes) {
        if (stream->sd_failed || (stream->sd_pos == stream->sd_size && !refill_sd_page(stream))) break;
        const size_t available = stream->sd_size - stream->sd_pos;
        const size_t count = bytes - read < available ? bytes - read : available;
        memcpy((uint8_t *)buffer + read, stream->sd_page + stream->sd_pos, count);
        read += count;
        stream->sd_pos += count;
    }
    return read;
#else
    return fread(buffer, 1, bytes, stream->file);
#endif
}

static drmp3_bool32 seek_bytes(void *context, int offset, drmp3_seek_origin origin) {
    BridgeMusicStream *stream = (BridgeMusicStream *)context;
#ifdef ESP_PLATFORM
    if (stream->sd_failed || origin == DRMP3_SEEK_END) return DRMP3_FALSE;
    const int64_t target = origin == DRMP3_SEEK_CUR
        ? (int64_t)stream->sd_page_offset + (int64_t)stream->sd_pos + offset : offset;
    if (target < 0 || target > LONG_MAX) return DRMP3_FALSE;
    if (target >= stream->sd_page_offset && target - stream->sd_page_offset <= (int64_t)stream->sd_size) {
        stream->sd_pos = (size_t)(target - stream->sd_page_offset);
        return DRMP3_TRUE;
    }
    const long page = (long)target - (long)target % SD_PAGE_BYTES;
    if (lseek(stream->sd_fd, page, SEEK_SET) < 0) return DRMP3_FALSE;
    stream->sd_page_offset = page;
    stream->sd_pos = stream->sd_size = 0;
    const size_t within_page = (size_t)(target - page);
    if (within_page && (!refill_sd_page(stream) || within_page > stream->sd_size)) return DRMP3_FALSE;
    stream->sd_pos = within_page;
    return DRMP3_TRUE;
#else
    const int whence = origin == DRMP3_SEEK_SET ? SEEK_SET : origin == DRMP3_SEEK_CUR ? SEEK_CUR : SEEK_END;
    return !ferror(stream->file) && fseek(stream->file, offset, whence) == 0;
#endif
}

static size_t read_loop(BridgeMusicStream *stream, int16_t *output, size_t frames) {
    if (stream->input_buffer_full) return 0;
    size_t count = (size_t)drmp3_read_pcm_frames_s16(&stream->mp3, frames, output);
    if (!count && !stream->input_buffer_full && drmp3_seek_to_pcm_frame(&stream->mp3, 0))
        count = (size_t)drmp3_read_pcm_frames_s16(&stream->mp3, frames, output);
    return count;
}

static bool next_frame(BridgeMusicStream *stream, int16_t *stereo) {
    if (stream->position == stream->available) {
        stream->available = read_loop(stream, stream->source, MUSIC_DECODE_FRAMES);
        stream->position = 0;
        if (!stream->available) return false;
    }
    const unsigned channels = stream->mp3.channels;
    const int16_t *frame = stream->source + stream->position++ * channels;
    stereo[0] = frame[0];
    stereo[1] = frame[channels - 1];
    return true;
}

static bool needs_conversion(const BridgeMusicStream *stream) {
    return stream->mp3.sampleRate != BRIDGE_AUDIO_RATE || stream->mp3.channels != BRIDGE_AUDIO_CHANNELS;
}

BridgeMusicStream *bridge_music_open(const char *path) {
#ifdef ESP_PLATFORM
    // The decoder's frequently accessed state fits in SRAM.
    BridgeMusicStream *stream = heap_caps_calloc(1, sizeof(*stream), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!stream) stream = calloc(1, sizeof(*stream));
#else
    BridgeMusicStream *stream = (BridgeMusicStream *)calloc(1, sizeof(*stream));
#endif
    if (!stream) return NULL;
#ifdef ESP_PLATFORM
    int flags = O_RDONLY;
#ifdef O_BINARY
    flags |= O_BINARY;
#endif
    stream->sd_fd = open(path, flags);
    // A sector-aligned page avoids the SDMMC driver's per-read bounce allocation
    // when dr_mp3 asks for data at an unaligned offset in its compressed buffer.
    if (stream->sd_fd >= 0)
        stream->sd_page = heap_caps_aligned_alloc(SD_PAGE_ALIGNMENT, SD_PAGE_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    if (!stream->sd_page) {
        bridge_music_close(stream);
        return NULL;
    }
#else
    stream->file = fopen(path, "rb");
    if (!stream->file) {
        bridge_music_close(stream);
        return NULL;
    }
#endif
    stream->compressed = malloc(DRMP3_DATA_CHUNK_SIZE);
    const drmp3_allocation_callbacks allocation = {stream, NULL, reserve_input_buffer, release_input_buffer};
    // Skip trailing tag inspection so a long FAT32 file can start reading immediately.
    if (!stream->compressed ||
        !drmp3_init(&stream->mp3, read_bytes, seek_bytes, NULL, NULL, stream, &allocation)) {
        bridge_music_close(stream);
        return NULL;
    }
    if ((stream->mp3.channels != 1 && stream->mp3.channels != 2) ||
        !stream->mp3.sampleRate || stream->mp3.sampleRate > BRIDGE_AUDIO_RATE ||
        (needs_conversion(stream) && (!next_frame(stream, stream->current) || !next_frame(stream, stream->next)))) {
        bridge_music_close(stream);
        return NULL;
    }
    return stream;
}

size_t bridge_music_read(BridgeMusicStream *stream, int16_t *stereo, size_t frames) {
    if (!needs_conversion(stream)) {
        size_t written = 0;
        while (written < frames) {
            const size_t count = read_loop(stream, stereo + written * BRIDGE_AUDIO_CHANNELS, frames - written);
            if (!count) break;
            written += count;
        }
        return written;
    }

    // Carry the fractional source position through each block and across the loop point.
    for (size_t i = 0; i < frames; ++i) {
        const int phase = (int)stream->phase;
        for (unsigned ch = 0; ch < BRIDGE_AUDIO_CHANNELS; ++ch)
            *stereo++ = (int16_t)((stream->current[ch] * (BRIDGE_AUDIO_RATE - phase) +
                                   stream->next[ch] * phase) / BRIDGE_AUDIO_RATE);
        stream->phase += stream->mp3.sampleRate;
        if (stream->phase >= BRIDGE_AUDIO_RATE) {
            stream->phase -= BRIDGE_AUDIO_RATE;
            stream->current[0] = stream->next[0];
            stream->current[1] = stream->next[1];
            if (!next_frame(stream, stream->next)) return i + 1;
        }
    }
    return frames;
}

void bridge_music_close(BridgeMusicStream *stream) {
    if (!stream) return;
    if (stream->mp3.onRead) drmp3_uninit(&stream->mp3);
#ifdef ESP_PLATFORM
    if (stream->sd_fd >= 0) close(stream->sd_fd);
    free(stream->sd_page);
#else
    if (stream->file) fclose(stream->file);
#endif
    free(stream->compressed);
    free(stream);
}
