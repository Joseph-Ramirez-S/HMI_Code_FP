#include <SDL.h>
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <iostream>
#include <memory>
#include <vector>

#include "../Bridge/engine_bridge.h"
#include "../Bridge/audio_effects.h"
#include "../Bridge/music_stream.h"

const int SCREEN_WIDTH = BRIDGE_PANEL_WIDTH;
const int SCREEN_HEIGHT = BRIDGE_PANEL_HEIGHT;
const int FRAME_SPEED_TRANSITION = 8;
struct SDLShutdown {
    ~SDLShutdown() {
        SDL_Quit();
    }
};

struct GameAudio {
    enum { block_frames = 256, music_capacity = 8192 };
    SDL_AudioDeviceID device = 0;
    std::vector<int16_t> button, door, arrival, hint, scene, hall_call, emergency, cabin_motion, stairs_back;
    std::vector<int16_t> options_open, slider;
    BridgeAudioMixer mixer{};
    SDL_Thread* music_thread = nullptr;
    SDL_atomic_t music_running{};
    std::string music_path;
    int16_t music_buffer[music_capacity * BRIDGE_AUDIO_CHANNELS];
    size_t music_head = 0, music_count = 0;

    static void load_clip(const std::string& path, unsigned start_ms, unsigned end_ms,
                          std::vector<int16_t>& output) {
        SDL_AudioSpec format = {};
        Uint8* data = nullptr;
        Uint32 size = 0;
        if (!SDL_LoadWAV(path.c_str(), &format, &data, &size)) return;
        const std::unique_ptr<Uint8, decltype(&SDL_FreeWAV)> wav(data, SDL_FreeWAV);
        if (format.freq != BRIDGE_AUDIO_RATE || format.format != AUDIO_S16SYS ||
            (format.channels != 1 && format.channels != BRIDGE_AUDIO_CHANNELS)) return;
        const auto* samples = reinterpret_cast<const int16_t*>(data);
        const size_t available = size / (sizeof(*samples) * format.channels);
        const size_t first = start_ms * (BRIDGE_AUDIO_RATE / 1000);
        const size_t end = end_ms ? end_ms * (BRIDGE_AUDIO_RATE / 1000) : available;
        if (end > available || end <= first) return;
        output.reserve((end - first) * BRIDGE_AUDIO_CHANNELS);
        for (size_t frame = first; frame < end; ++frame) {
            const auto* input = samples + frame * format.channels;
            output.push_back(input[0]);
            output.push_back(input[format.channels - 1]);
        }
    }

    bool load(const std::string& folder) {
        load_clip(folder + "BUTTONS.WAV", 0, 0, button);
        load_clip(folder + "INTRO.WAV", BRIDGE_INTRO_OPEN_MS, BRIDGE_INTRO_END_MS, door);
        load_clip(folder + "ARRIVAL.WAV", 0, 0, arrival);
        load_clip(folder + "S1_BF.WAV", 0, 0, hint);
        load_clip(folder + "SCENE.WAV", 0, 0, scene);
        load_clip(folder + "CALL.WAV", 0, 0, hall_call);
        load_clip(folder + "ALERT.WAV", 0, 0, emergency);
        load_clip(folder + "MOTION.WAV", 0, 0, cabin_motion);
        load_clip(folder + "BACK.WAV", 0, 0, stairs_back);
        load_clip(folder + "OPTIONS.WAV", 0, 0, options_open);
        load_clip(folder + "SLIDER.WAV", 0, 0, slider);
        if (button.empty() && door.empty() && arrival.empty() && hint.empty() && scene.empty() &&
            hall_call.empty() && emergency.empty() && cabin_motion.empty() && stairs_back.empty() && options_open.empty())
            return false;
        mixer.button = {button.data(), button.size() / BRIDGE_AUDIO_CHANNELS};
        mixer.door = {door.data(), door.size() / BRIDGE_AUDIO_CHANNELS};
        mixer.button_position = mixer.button.frames;
        mixer.arrival = {arrival.data(), arrival.size() / BRIDGE_AUDIO_CHANNELS};
        mixer.arrival_position = mixer.arrival.frames;
        mixer.hint = {hint.data(), hint.size() / BRIDGE_AUDIO_CHANNELS};
        mixer.hint_position = mixer.hint.frames;
        mixer.scene = {scene.data(), scene.size() / BRIDGE_AUDIO_CHANNELS};
        mixer.scene_position = mixer.scene.frames;
        mixer.hall_call = {hall_call.data(), hall_call.size() / BRIDGE_AUDIO_CHANNELS};
        mixer.hall_call_position = mixer.hall_call.frames;
        mixer.emergency = {emergency.data(), emergency.size() / BRIDGE_AUDIO_CHANNELS};
        mixer.emergency_position = mixer.emergency.frames;
        mixer.cabin_motion = {cabin_motion.data(), cabin_motion.size() / BRIDGE_AUDIO_CHANNELS};
        mixer.stairs_back = {stairs_back.data(), stairs_back.size() / BRIDGE_AUDIO_CHANNELS};
        mixer.stairs_back_position = mixer.stairs_back.frames;
        mixer.options_open = {options_open.data(), options_open.size() / BRIDGE_AUDIO_CHANNELS};
        mixer.options_open_position = mixer.options_open.frames;
        mixer.slider = {slider.data(), slider.size() / BRIDGE_AUDIO_CHANNELS};
        SDL_AudioSpec format = {};
        format.freq = BRIDGE_AUDIO_RATE;
        format.channels = BRIDGE_AUDIO_CHANNELS;
        format.format = AUDIO_S16SYS;
        format.samples = block_frames;
        format.userdata = this;
        format.callback = render;
        device = SDL_OpenAudioDevice(nullptr, 0, &format, nullptr, 0);
        if (!device) return false;
        SDL_PauseAudioDevice(device, 0);
        return true;
    }

    static void SDLCALL render(void* context, Uint8* output, int bytes) {
        constexpr int frame_bytes = sizeof(int16_t) * BRIDGE_AUDIO_CHANNELS;
        auto& sound = *static_cast<GameAudio*>(context);
        auto* samples = reinterpret_cast<int16_t*>(output);
        size_t remaining = bytes / frame_bytes;
        const auto sfx = bridge_get_sfx_volume(), volume = bridge_get_music_volume();
        while (remaining) {
            int16_t music[block_frames * BRIDGE_AUDIO_CHANNELS];
            const size_t frames = std::min(remaining, size_t(block_frames));
            const size_t music_frames = std::min(frames, sound.music_count);
            const size_t first = std::min(music_frames, music_capacity - sound.music_head);
            std::memcpy(music, sound.music_buffer + sound.music_head * BRIDGE_AUDIO_CHANNELS, first * frame_bytes);
            std::memcpy(music + first * BRIDGE_AUDIO_CHANNELS, sound.music_buffer, (music_frames - first) * frame_bytes);
            sound.music_head = (sound.music_head + music_frames) % music_capacity;
            sound.music_count -= music_frames;
            bridge_audio_mix_with_music(&sound.mixer, samples, frames, sfx, music, music_frames, volume);
            samples += frames * BRIDGE_AUDIO_CHANNELS;
            remaining -= frames;
        }
    }

    static int SDLCALL stream_music(void* context) {
        auto& sound = *static_cast<GameAudio*>(context);
        std::unique_ptr<BridgeMusicStream, decltype(&bridge_music_close)>
            stream(bridge_music_open(sound.music_path.c_str()), bridge_music_close);
        if (!stream) {
            std::cerr << "[WARN] No se pudo abrir el MP3 " << sound.music_path << '\n';
            return 1;
        }

        constexpr size_t frame_bytes = sizeof(int16_t) * BRIDGE_AUDIO_CHANNELS;
        int16_t pcm[block_frames * BRIDGE_AUDIO_CHANNELS];
        while (SDL_AtomicGet(&sound.music_running)) {
            const size_t decoded = bridge_music_read(stream.get(), pcm, block_frames);
            size_t sent = 0;
            while (sent < decoded && SDL_AtomicGet(&sound.music_running)) {
                SDL_LockAudioDevice(sound.device);
                const size_t tail = (sound.music_head + sound.music_count) % music_capacity;
                const size_t count = std::min({decoded - sent, music_capacity - sound.music_count, music_capacity - tail});
                std::memcpy(sound.music_buffer + tail * BRIDGE_AUDIO_CHANNELS,
                            pcm + sent * BRIDGE_AUDIO_CHANNELS, count * frame_bytes);
                sound.music_count += count;
                SDL_UnlockAudioDevice(sound.device);
                sent += count;
                if (!count) SDL_Delay(4);
            }
            if (decoded < block_frames) break;
        }
        return 0;
    }

    void start_music(const std::string& path) {
        if (!device) return;
        music_path = path;
        SDL_AtomicSet(&music_running, 1);
        music_thread = SDL_CreateThread(stream_music, "MusicDecode", this);
        if (!music_thread)
            std::cerr << "[WARN] No se pudo iniciar la música: " << SDL_GetError() << '\n';
    }

    static void play_button(void* context) {
        auto& sound = *static_cast<GameAudio*>(context);
        SDL_LockAudioDevice(sound.device);
        bridge_audio_button(&sound.mixer);
        SDL_UnlockAudioDevice(sound.device);
    }

    static void play_door(bool opening, unsigned ticks, void* context) {
        auto& sound = *static_cast<GameAudio*>(context);
        SDL_LockAudioDevice(sound.device);
        bridge_audio_door(&sound.mixer, opening, ticks);
        SDL_UnlockAudioDevice(sound.device);
    }

    static void play_hint(void* context) {
        auto& sound = *static_cast<GameAudio*>(context);
        SDL_LockAudioDevice(sound.device);
        sound.mixer.hint_position = 0;
        SDL_UnlockAudioDevice(sound.device);
    }

    static void play_scene(void* context) {
        auto& sound = *static_cast<GameAudio*>(context);
        SDL_LockAudioDevice(sound.device);
        sound.mixer.scene_position = 0;
        SDL_UnlockAudioDevice(sound.device);
    }

    static void play_hall_call(void* context) {
        auto& sound = *static_cast<GameAudio*>(context);
        SDL_LockAudioDevice(sound.device);
        sound.mixer.hall_call_position = 0;
        SDL_UnlockAudioDevice(sound.device);
    }

    static void play_emergency(void* context) {
        auto& sound = *static_cast<GameAudio*>(context);
        SDL_LockAudioDevice(sound.device);
        bridge_audio_emergency(&sound.mixer);
        SDL_UnlockAudioDevice(sound.device);
    }

    static void set_cabin_motion(unsigned volume, void* context) {
        auto& sound = *static_cast<GameAudio*>(context);
        SDL_LockAudioDevice(sound.device);
        bridge_audio_cabin_motion(&sound.mixer, volume);
        SDL_UnlockAudioDevice(sound.device);
    }

    static void play_stairs_back(void* context) {
        auto& sound = *static_cast<GameAudio*>(context);
        SDL_LockAudioDevice(sound.device);
        bridge_audio_stairs_back(&sound.mixer);
        SDL_UnlockAudioDevice(sound.device);
    }

    static void play_options_open(void* context) {
        auto& sound = *static_cast<GameAudio*>(context);
        SDL_LockAudioDevice(sound.device);
        sound.mixer.options_open_position = 0;
        SDL_UnlockAudioDevice(sound.device);
    }

    static void play_mute_click(unsigned volume, void* context) {
        auto& sound = *static_cast<GameAudio*>(context);
        SDL_LockAudioDevice(sound.device);
        bridge_audio_mute_click(&sound.mixer, volume);
        SDL_UnlockAudioDevice(sound.device);
    }

    static void play_slider_step(bool increasing, void* context) {
        auto& sound = *static_cast<GameAudio*>(context);
        SDL_LockAudioDevice(sound.device);
        bridge_audio_slider_step(&sound.mixer, increasing);
        SDL_UnlockAudioDevice(sound.device);
    }

    void play_arrival() {
        if (!device) return;
        SDL_LockAudioDevice(device);
        mixer.arrival_position = 0;
        SDL_UnlockAudioDevice(device);
    }

    ~GameAudio() {
        if (music_thread) {
            SDL_AtomicSet(&music_running, 0);
            SDL_WaitThread(music_thread, nullptr);
        }
        if (device) SDL_CloseAudioDevice(device);
    }
};

int main(int, char**) {
    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        std::cerr << "[ERROR] SDL_Init: " << SDL_GetError() << '\n';
        return 1;
    }
    const SDLShutdown sdl_shutdown;

    using WindowPtr = std::unique_ptr<SDL_Window, decltype(&SDL_DestroyWindow)>;
    using RendererPtr = std::unique_ptr<SDL_Renderer, decltype(&SDL_DestroyRenderer)>;
    using TexturePtr = std::unique_ptr<SDL_Texture, decltype(&SDL_DestroyTexture)>;
    WindowPtr window(SDL_CreateWindow(
        "Simulador DMA - ESP32-P4",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        SCREEN_WIDTH, SCREEN_HEIGHT, SDL_WINDOW_SHOWN), SDL_DestroyWindow);
    if (!window) {
        std::cerr << "[ERROR] SDL_CreateWindow: " << SDL_GetError() << '\n';
        return 1;
    }

    RendererPtr renderer(
        SDL_CreateRenderer(window.get(), -1,
                           SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC),
        SDL_DestroyRenderer);
    if (!renderer) {
        std::cerr << "[WARN] Renderizador acelerado con VSync no disponible: "
                  << SDL_GetError() << '\n';
        renderer.reset(SDL_CreateRenderer(window.get(), -1, SDL_RENDERER_ACCELERATED));
    }
    if (!renderer) {
        std::cerr << "[WARN] Renderizador acelerado no disponible: "
                  << SDL_GetError() << '\n';
        renderer.reset(SDL_CreateRenderer(window.get(), -1, 0));
    }
    if (!renderer) {
        std::cerr << "[ERROR] SDL_CreateRenderer: " << SDL_GetError() << '\n';
        return 1;
    }

    SDL_RendererInfo renderer_info = {};
    if (SDL_GetRendererInfo(renderer.get(), &renderer_info) == 0) {
        std::cout << "[INFO] Renderizador SDL2: " << renderer_info.name
                  << ((renderer_info.flags & SDL_RENDERER_ACCELERATED)
                          ? " (acelerado)" : " (software)")
                  << ((renderer_info.flags & SDL_RENDERER_PRESENTVSYNC)
                          ? ", VSync activo\n" : ", VSync no disponible\n");
    }

    SDL_Texture* raw_texture = SDL_CreateTexture(
        renderer.get(), SDL_PIXELFORMAT_RGB565, SDL_TEXTUREACCESS_STREAMING,
        SCREEN_WIDTH, SCREEN_HEIGHT);
    TexturePtr texture(raw_texture, SDL_DestroyTexture);
    if (!texture) {
        std::cerr << "[ERROR] SDL_CreateTexture: " << SDL_GetError() << '\n';
        return 1;
    }
    if (SDL_SetTextureBlendMode(texture.get(), SDL_BLENDMODE_NONE) < 0) {
        std::cerr << "[ERROR] SDL_SetTextureBlendMode: " << SDL_GetError() << '\n';
        return 1;
    }

    std::unique_ptr<char, decltype(&SDL_free)> base_path(SDL_GetBasePath(), SDL_free);
    const std::string pack_path = std::string(base_path ? base_path.get() : "") +
        "graphics.bin";
    size_t pack_size = 0;
    std::unique_ptr<void, decltype(&SDL_free)> graphics(
        SDL_LoadFile(pack_path.c_str(), &pack_size), SDL_free);
    bridge_init();
    if (!graphics || !bridge_bind_graphics(graphics.get(), pack_size)) {
        std::cerr << "[ERROR] Paquete grafico ausente o incompatible: " << pack_path << '\n';
        return 1;
    }

    const size_t framebuffer_size =
        static_cast<size_t>(SCREEN_WIDTH) * SCREEN_HEIGHT * sizeof(uint16_t);
    std::unique_ptr<uint16_t, decltype(&std::free)> virtual_psram(
        static_cast<uint16_t*>(std::malloc(framebuffer_size)), std::free);
    if (!virtual_psram) {
        std::cerr << "[ERROR] No hay memoria para el framebuffer.\n";
        return 1;
    }
    if (!bridge_prepare_software_cabin(virtual_psram.get(), SCREEN_WIDTH, SCREEN_HEIGHT)) {
        std::cerr << "[ERROR] No se pudo preparar el fondo de la cabina.\n";
        bridge_cleanup();
        return 1;
    }
    GameAudio audio;
    const std::string audio_path = std::string(base_path ? base_path.get() : "") + "sdcard/";
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) == 0 && audio.load(audio_path)) {
        bridge_set_button_sound_callback(GameAudio::play_button, &audio);
        bridge_set_door_sound_callback(GameAudio::play_door, &audio);
        bridge_set_hint_sound_callback(GameAudio::play_hint, &audio);
        bridge_set_scene_sound_callback(GameAudio::play_scene, &audio);
        bridge_set_hall_call_sound_callback(GameAudio::play_hall_call, &audio);
        bridge_set_emergency_sound_callback(GameAudio::play_emergency, &audio);
        bridge_set_cabin_motion_sound_callback(GameAudio::set_cabin_motion, &audio);
        bridge_set_stairs_back_sound_callback(GameAudio::play_stairs_back, &audio);
        bridge_set_options_sound_callbacks(GameAudio::play_options_open, GameAudio::play_mute_click, &audio);
        bridge_set_slider_sound_callback(GameAudio::play_slider_step, &audio);
    } else {
        std::cerr << "[WARN] Audio del ascensor no disponible: " << SDL_GetError() << '\n';
    }
    int simulated_level = 1;
    int simulated_destination = 0;
    uint64_t simulated_arrival_ms = 0;
    bool direction_demo_started = false;
    bridge_set_elevator_level(simulated_level);
    bridge_set_exterior_calls_enabled(true);
    bridge_set_receptionist_frame_delay(FRAME_SPEED_TRANSITION + 2);
    bridge_set_neko_frame_delay(FRAME_SPEED_TRANSITION);
    bridge_set_elevator_door_frame_delay(FRAME_SPEED_TRANSITION + 3);
    bridge_set_level_indicator_frame_delay(FRAME_SPEED_TRANSITION + 7);
    bridge_set_agent_shadow_frame_delay(FRAME_SPEED_TRANSITION + 2);

    bool running = true;
    int exit_code = 0;
    SDL_Event event;
    const uint64_t frame_duration = SDL_GetPerformanceFrequency() / 60;
    const uint64_t intro_start = SDL_GetPerformanceCounter();
    while (running) {
        const uint64_t frame_start = SDL_GetPerformanceCounter();
        const uint64_t elapsed_us = (frame_start - intro_start) * 1000000 / SDL_GetPerformanceFrequency();
        const uint64_t elapsed_intro_ms = elapsed_us / 1000;
        if (simulated_destination && elapsed_intro_ms >= simulated_arrival_ms) {
            simulated_level = simulated_destination;
            simulated_destination = 0;
            if (bridge_set_elevator_level(simulated_level)) audio.play_arrival();
        }
        const auto start_simulated_trip = [&](int level) {
            if (level > 0 && level <= BRIDGE_LOBBY_COUNT &&
                level != simulated_level && level != simulated_destination) {
                simulated_destination = level;
                simulated_arrival_ms = elapsed_intro_ms + 800;
                simulated_level = 0;
                bridge_set_elevator_level(0);
            }
        };
        const auto apply_elevator_output = [&]() {
            const int button = bridge_get_elevator_output_button();
            if (button == BRIDGE_ELEVATOR_EMERGENCY_BUTTON) simulated_destination = 0;
            else start_simulated_trip(button + 1);
        };
        const auto press_cabin_panel = [&](int x, int y) {
            bridge_touch_elevator_panel(x, y, true, SCREEN_WIDTH, SCREEN_HEIGHT, simulated_level);
            apply_elevator_output();
        };
        while (SDL_PollEvent(&event)) {
            if ((event.type == SDL_KEYDOWN && !event.key.repeat) ||
                (event.type == SDL_MOUSEBUTTONDOWN && event.button.button != SDL_BUTTON_LEFT)) {
                bridge_note_interaction();
            }
            if (event.type == SDL_QUIT) {
                running = false;
            } else if (event.type == SDL_KEYDOWN && !event.key.repeat &&
                       event.key.keysym.sym >= SDLK_0 && event.key.keysym.sym <= SDLK_3) {
                simulated_level = event.key.keysym.sym - SDLK_0;
                if (simulated_level == simulated_destination) simulated_destination = 0;
                if (bridge_set_elevator_level(simulated_level)) audio.play_arrival();
            } else if (event.type == SDL_MOUSEBUTTONDOWN &&
                       event.button.button == SDL_BUTTON_LEFT &&
                       elapsed_intro_ms >= BRIDGE_INTRO_END_MS) {
                if (!bridge_touch_options(event.button.x, event.button.y, true, SCREEN_WIDTH, SCREEN_HEIGHT)) {
                    const bool stairs = bridge_touch_stairs(
                        event.button.x, event.button.y, true, SCREEN_WIDTH, SCREEN_HEIGHT);
                    press_cabin_panel(event.button.x, event.button.y);
                    if (!stairs && bridge_touch_elevator_door(event.button.x, event.button.y, SCREEN_WIDTH, SCREEN_HEIGHT))
                        apply_elevator_output();
                }
            } else if (event.type == SDL_MOUSEBUTTONUP &&
                       event.button.button == SDL_BUTTON_LEFT &&
                       elapsed_intro_ms >= BRIDGE_INTRO_END_MS) {
                bridge_touch_options(event.button.x, event.button.y, false, SCREEN_WIDTH, SCREEN_HEIGHT);
                bridge_touch_stairs(event.button.x, event.button.y, false, SCREEN_WIDTH, SCREEN_HEIGHT);
                bridge_release_exterior_call();
                bridge_touch_elevator_panel(
                    event.button.x, event.button.y, false,
                    SCREEN_WIDTH, SCREEN_HEIGHT, simulated_level);
            } else if (event.type == SDL_MOUSEMOTION &&
                       (event.motion.state & SDL_BUTTON_LMASK) != 0 &&
                       elapsed_intro_ms >= BRIDGE_INTRO_END_MS) {
                if (!bridge_touch_options(event.motion.x, event.motion.y, true, SCREEN_WIDTH, SCREEN_HEIGHT)) {
                    bridge_touch_stairs(event.motion.x, event.motion.y, true, SCREEN_WIDTH, SCREEN_HEIGHT);
                    press_cabin_panel(event.motion.x, event.motion.y);
                }
            } else if (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
                bridge_release_exterior_call();
                bridge_touch_options(-1, -1, false, SCREEN_WIDTH, SCREEN_HEIGHT);
                bridge_touch_stairs(-1, -1, false, SCREEN_WIDTH, SCREEN_HEIGHT);
                bridge_touch_elevator_panel(-1, -1, false, SCREEN_WIDTH, SCREEN_HEIGHT, simulated_level);
            }
        }

        if (!running) {
            break;
        }
        if (elapsed_intro_ms >= BRIDGE_INTRO_END_MS) {
            bridge_update_elevator_controls_at(elapsed_us, simulated_level);
            apply_elevator_output();
        }
        bool frame_ready = true;
        if (elapsed_intro_ms < BRIDGE_INTRO_END_MS) {
            frame_ready = bridge_render_intro(virtual_psram.get(), static_cast<uint32_t>(elapsed_intro_ms));
            if (!frame_ready) {
                std::cerr << "[ERROR] No se pudo componer la introduccion.\n";
                exit_code = 1;
                break;
            }
        } else {
            if (!direction_demo_started) {
                bridge_set_elevator_indicator_test_mode(true);
                audio.start_music(audio_path + BRIDGE_MUSIC_FILENAME);
                direction_demo_started = true;
            }
            frame_ready = bridge_render_frame_at(
                virtual_psram.get(), SCREEN_WIDTH, SCREEN_HEIGHT, elapsed_us);
            if (!frame_ready) {
                std::cerr << "[ERROR] No se pudo componer el cuadro.\n";
                exit_code = 1;
                break;
            }
        }

        if ((elapsed_intro_ms < BRIDGE_INTRO_END_MS || bridge_frame_was_rendered()) &&
            SDL_UpdateTexture(texture.get(), nullptr, virtual_psram.get(),
                              SCREEN_WIDTH * static_cast<int>(sizeof(uint16_t))) < 0) {
            std::cerr << "[ERROR] SDL_UpdateTexture: " << SDL_GetError() << '\n';
            exit_code = 1;
            break;
        }
        if (SDL_RenderCopy(renderer.get(), texture.get(), nullptr, nullptr) < 0) {
            std::cerr << "[ERROR] SDL_RenderCopy: " << SDL_GetError() << '\n';
            exit_code = 1;
            break;
        }
        SDL_RenderPresent(renderer.get());

        const uint64_t elapsed = SDL_GetPerformanceCounter() - frame_start;
        if (elapsed < frame_duration) {
            const uint64_t remaining_ms =
                (frame_duration - elapsed) * 1000 / SDL_GetPerformanceFrequency();
            if (remaining_ms > 0) {
                SDL_Delay(static_cast<uint32_t>(remaining_ms));
            }
        }
    }

    bridge_cleanup();
    return exit_code;
}
