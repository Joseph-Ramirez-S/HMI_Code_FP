#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum { BRIDGE_AUDIO_RATE = 48000, BRIDGE_AUDIO_CHANNELS = 2 };
enum { ELEVATOR_HINT_VOLUME_PERCENT = 65 };

typedef struct {
    const int16_t *samples;
    size_t frames;
} BridgeAudioClip;

typedef struct {
    BridgeAudioClip button, door, arrival, hint, scene, hall_call, emergency, cabin_motion, stairs_back;
    BridgeAudioClip options_open;
    BridgeAudioClip slider;
    size_t slider_position, slider_end;
    size_t button_position, door_position, door_duration, arrival_position, hint_position, scene_position;
    size_t hall_call_position, emergency_position;
    size_t cabin_motion_position, stairs_back_position;
    size_t options_open_position, mute_click_position;
    unsigned mute_click_volume;
    int cabin_motion_gain, cabin_motion_target;
    int music_duck;
    uint64_t door_hop_step;
    bool opening;
} BridgeAudioMixer;

// Small, crossfaded jumps preserve the pitch of the original door sound
// while fitting the different lengths of the lobby and cabin animations.
enum { DOOR_AUDIO_HOP = 960, DOOR_AUDIO_BLEND = 384 };
enum { CABIN_MOTION_GAIN_UNIT = 256, CABIN_MOTION_GAIN_STEP = 8 };
enum { MUSIC_DUCK_UNIT = 8192, MUSIC_DUCK_DEPTH = 3072 };

static inline void bridge_audio_cabin_motion(BridgeAudioMixer *mixer, unsigned volume) {
    mixer->cabin_motion_target = (int)volume * CABIN_MOTION_GAIN_UNIT;
}

static inline void bridge_audio_button(BridgeAudioMixer *mixer) {
    mixer->button_position = 0;
}

static inline void bridge_audio_emergency(BridgeAudioMixer *mixer) {
    if (mixer->emergency.frames)
        mixer->emergency_position = 0;
    else
        bridge_audio_button(mixer);
}

static inline void bridge_audio_stairs_back(BridgeAudioMixer *mixer) {
    if (mixer->stairs_back.frames)
        mixer->stairs_back_position = 0;
    else
        bridge_audio_button(mixer);
}

static inline void bridge_audio_mute_click(BridgeAudioMixer *mixer, unsigned volume) {
    mixer->mute_click_position = 0;
    mixer->mute_click_volume = volume;
}

static inline void bridge_audio_slider_step(BridgeAudioMixer *mixer, bool increasing) {
    const size_t frames = mixer->slider.frames / 2;
    mixer->slider_position = increasing ? 0 : frames;
    mixer->slider_end = mixer->slider_position + frames;
}

static inline void bridge_audio_door(BridgeAudioMixer *mixer, bool opening, unsigned ticks) {
    if (mixer->door.frames < 2 || !ticks) return;
    mixer->opening = opening;
    mixer->door_position = 0;
    mixer->door_duration = (size_t)ticks * (BRIDGE_AUDIO_RATE / 60);
    const size_t last_hop = (mixer->door_duration - 1) / DOOR_AUDIO_HOP;
    const size_t last_frames = mixer->door_duration - last_hop * DOOR_AUDIO_HOP;
    mixer->door_hop_step = last_hop && mixer->door.frames > last_frames
        ? ((uint64_t)(mixer->door.frames - last_frames) << 16) / last_hop : 0;
}

static inline bool bridge_audio_active(const BridgeAudioMixer *mixer) {
    return mixer->button_position < mixer->button.frames || mixer->door_position < mixer->door_duration ||
        mixer->arrival_position < mixer->arrival.frames || mixer->hint_position < mixer->hint.frames ||
        mixer->scene_position < mixer->scene.frames || mixer->hall_call_position < mixer->hall_call.frames ||
        mixer->emergency_position < mixer->emergency.frames || mixer->stairs_back_position < mixer->stairs_back.frames ||
        mixer->options_open_position < mixer->options_open.frames ||
        mixer->slider_position < mixer->slider_end ||
        (mixer->mute_click_volume && mixer->mute_click_position < mixer->button.frames) ||
        (mixer->cabin_motion.frames && (mixer->cabin_motion_target || mixer->cabin_motion_gain));
}

static inline int16_t bridge_audio_limit(int sample) {
    if (sample > INT16_MAX) return INT16_MAX;
    if (sample < INT16_MIN) return INT16_MIN;
    return (int16_t)sample;
}

static inline size_t bridge_audio_door_offset(const BridgeAudioMixer *mixer, size_t offset) {
    return offset < mixer->door.frames ? offset : mixer->door.frames - 1;
}

static inline int bridge_audio_music_room(int effect, int music) {
    if (music > 0 && effect + music > INT16_MAX) return (INT16_MAX - effect) * 256 / music;
    if (music < 0 && effect + music < INT16_MIN) return (effect - INT16_MIN) * 256 / -music;
    return 256;
}

static inline void bridge_audio_mix_tracks(BridgeAudioMixer *mixer, int16_t *output,
                                           size_t frames, unsigned sfx_volume,
                                           const int16_t *music, size_t music_frames, int music_gain_percent) {
    const bool effects = bridge_audio_active(mixer);
    if (!music_frames || !music_gain_percent) mixer->music_duck = 0;
    if (!effects && !mixer->music_duck) {
        const size_t music_samples = music_gain_percent ? music_frames * BRIDGE_AUDIO_CHANNELS : 0;
        for (size_t i = 0; i < music_samples; ++i)
            output[i] = (int16_t)(music[i] * music_gain_percent / 100);
        for (size_t i = music_samples; i < frames * BRIDGE_AUDIO_CHANNELS; ++i) output[i] = 0;
        return;
    }
    const bool mute_click = mixer->mute_click_volume && mixer->mute_click_position < mixer->button.frames;
    const bool audible_effects = effects && (sfx_volume || mute_click);
    const int duck_target = audible_effects && music_gain_percent ? MUSIC_DUCK_DEPTH : 0;
    for (size_t frame = 0; frame < frames; ++frame) {
        int left = 0, right = 0;
        if (mixer->door_position < mixer->door_duration) {
            // Reversing the output index also reverses the crossfades exactly.
            const size_t t = mixer->opening ? mixer->door_position
                : mixer->door_duration - 1 - mixer->door_position;
            const size_t hop = t / DOOR_AUDIO_HOP;
            const size_t age = t % DOOR_AUDIO_HOP;
            size_t sample = ((mixer->door_hop_step * hop + 32768) >> 16) + age;
            if (mixer->door_duration <= DOOR_AUDIO_HOP)
                sample = (uint64_t)t * (mixer->door.frames - 1) / (mixer->door_duration - 1);
            const int16_t *current = mixer->door.samples +
                BRIDGE_AUDIO_CHANNELS * bridge_audio_door_offset(mixer, sample);
            left = current[0];
            right = current[1];
            size_t blend = mixer->door_duration - hop * DOOR_AUDIO_HOP;
            if (blend > DOOR_AUDIO_BLEND) blend = DOOR_AUDIO_BLEND;
            if (hop && age < blend) {
                const size_t old_sample = ((mixer->door_hop_step * (hop - 1) + 32768) >> 16) +
                    DOOR_AUDIO_HOP + age;
                const int16_t *old = mixer->door.samples +
                    BRIDGE_AUDIO_CHANNELS * bridge_audio_door_offset(mixer, old_sample);
                const int now = (int)age, previous = (int)blend - now;
                left = (old[0] * previous + left * now) / (int)blend;
                right = (old[1] * previous + right * now) / (int)blend;
            }
            ++mixer->door_position;
        }
        if (mixer->button_position < mixer->button.frames) {
            const int16_t *sample = mixer->button.samples + BRIDGE_AUDIO_CHANNELS * mixer->button_position++;
            left += sample[0];
            right += sample[1];
        }
        if (mixer->arrival_position < mixer->arrival.frames) {
            const int16_t *sample = mixer->arrival.samples + BRIDGE_AUDIO_CHANNELS * mixer->arrival_position++;
            left += sample[0];
            right += sample[1];
        }
        if (mixer->hint_position < mixer->hint.frames) {
            const int16_t *sample = mixer->hint.samples + BRIDGE_AUDIO_CHANNELS * mixer->hint_position++;
            left += sample[0] * ELEVATOR_HINT_VOLUME_PERCENT / 100;
            right += sample[1] * ELEVATOR_HINT_VOLUME_PERCENT / 100;
        }
        if (mixer->scene_position < mixer->scene.frames) {
            const int16_t *sample = mixer->scene.samples + BRIDGE_AUDIO_CHANNELS * mixer->scene_position++;
            left += sample[0];
            right += sample[1];
        }
        if (mixer->hall_call_position < mixer->hall_call.frames) {
            const int16_t *sample = mixer->hall_call.samples + BRIDGE_AUDIO_CHANNELS * mixer->hall_call_position++;
            left += sample[0];
            right += sample[1];
        }
        if (mixer->emergency_position < mixer->emergency.frames) {
            const int16_t *sample = mixer->emergency.samples + BRIDGE_AUDIO_CHANNELS * mixer->emergency_position++;
            left += sample[0];
            right += sample[1];
        }
        if (mixer->stairs_back_position < mixer->stairs_back.frames) {
            const int16_t *sample = mixer->stairs_back.samples +
                BRIDGE_AUDIO_CHANNELS * mixer->stairs_back_position++;
            left += sample[0];
            right += sample[1];
        }
        if (mixer->options_open_position < mixer->options_open.frames) {
            const int16_t *sample = mixer->options_open.samples +
                BRIDGE_AUDIO_CHANNELS * mixer->options_open_position++;
            left += sample[0];
            right += sample[1];
        }
        if (mixer->slider_position < mixer->slider_end) {
            const int16_t *sample = mixer->slider.samples + BRIDGE_AUDIO_CHANNELS * mixer->slider_position++;
            left += sample[0];
            right += sample[1];
        }
        if (mixer->cabin_motion.frames && (mixer->cabin_motion_target || mixer->cabin_motion_gain)) {
            int step = mixer->cabin_motion_target - mixer->cabin_motion_gain;
            if (step > CABIN_MOTION_GAIN_STEP) step = CABIN_MOTION_GAIN_STEP;
            if (step < -CABIN_MOTION_GAIN_STEP) step = -CABIN_MOTION_GAIN_STEP;
            mixer->cabin_motion_gain += step;
            const int16_t *sample = mixer->cabin_motion.samples +
                BRIDGE_AUDIO_CHANNELS * mixer->cabin_motion_position++;
            if (mixer->cabin_motion_position == mixer->cabin_motion.frames)
                mixer->cabin_motion_position = 0;
            left += sample[0] * mixer->cabin_motion_gain / (100 * CABIN_MOTION_GAIN_UNIT);
            right += sample[1] * mixer->cabin_motion_gain / (100 * CABIN_MOTION_GAIN_UNIT);
        }
        left = left * (int)sfx_volume / 10;
        right = right * (int)sfx_volume / 10;
        // The mute confirmation uses the level just before mute (or restored
        // on unmute), so the tap is audible without other effects leaking.
        if (mixer->mute_click_volume && mixer->mute_click_position < mixer->button.frames) {
            const int16_t *sample = mixer->button.samples + BRIDGE_AUDIO_CHANNELS * mixer->mute_click_position++;
            left += sample[0] * (int)mixer->mute_click_volume / 10;
            right += sample[1] * (int)mixer->mute_click_volume / 10;
        }
        if (frame < music_frames && music_gain_percent) {
            int duck = mixer->music_duck;
            if (duck < duck_target) {
                duck += 4;
                if (duck > duck_target) duck = duck_target;
            } else if (duck > duck_target) {
                --duck;
            }
            mixer->music_duck = duck;
            const int gain = music_gain_percent * (MUSIC_DUCK_UNIT - duck) / (MUSIC_DUCK_UNIT / 256);
            const int ml = music[frame * BRIDGE_AUDIO_CHANNELS] * gain / 25600;
            const int mr = music[frame * BRIDGE_AUDIO_CHANNELS + 1] * gain / 25600;
            left = bridge_audio_limit(left);
            right = bridge_audio_limit(right);
            const int left_room = bridge_audio_music_room(left, ml);
            const int right_room = bridge_audio_music_room(right, mr);
            const int room = left_room < right_room ? left_room : right_room;
            left += ml * room / 256;
            right += mr * room / 256;
        }
        *output++ = bridge_audio_limit(left);
        *output++ = bridge_audio_limit(right);
    }
}

static inline void bridge_audio_mix_with_music(BridgeAudioMixer *mixer, int16_t *output,
                                               size_t frames, unsigned sfx_volume,
                                               const int16_t *music, size_t music_frames, unsigned music_volume) {
    bridge_audio_mix_tracks(mixer, output, frames, sfx_volume, music, music_frames, (int)music_volume * 10);
}

static inline void bridge_audio_mix(BridgeAudioMixer *mixer, int16_t *output, size_t frames, unsigned sfx_volume) {
    bridge_audio_mix_with_music(mixer, output, frames, sfx_volume, NULL, 0, 0);
}
