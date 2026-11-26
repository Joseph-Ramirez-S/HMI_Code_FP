#pragma once

#include "esp_err.h"
#include <stdbool.h>

esp_err_t intro_audio_prepare(void);
void intro_audio_start(void);
void button_audio_request(void *context);
void door_audio_request(bool opening, unsigned ticks, void *context);
void arrival_audio_request(void);
void animation_audio_prepare(void);
void hint_audio_request(void *context);
void scene_audio_request(void *context);
void hall_call_audio_request(void *context);
void emergency_audio_request(void *context);
void cabin_motion_audio_set(unsigned volume, void *context);
void stairs_back_audio_request(void *context);
void options_open_audio_request(void *context);
void mute_click_audio_request(unsigned volume, void *context);
void slider_audio_request(bool increasing, void *context);
