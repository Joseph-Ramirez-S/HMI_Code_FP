#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BRIDGE_MUSIC_FILENAME "Background_Sound.mp3"

typedef struct BridgeMusicStream BridgeMusicStream;

BridgeMusicStream *bridge_music_open(const char *path);
size_t bridge_music_read(BridgeMusicStream *stream, int16_t *stereo, size_t frames);
void bridge_music_close(BridgeMusicStream *stream);

#ifdef __cplusplus
}
#endif
