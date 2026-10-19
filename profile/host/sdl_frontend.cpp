// SDL2 windowed frontend: software-present player for desktop builds.
// See sdl_frontend.hpp for the roadmap (GL acceleration comes later).

#include "sdl_frontend.hpp"

#ifdef PSPWEB_USE_SDL

#include "kernel.hpp" // pad::k* button bits

#include <SDL.h>

#include <cstdio>

namespace pspweb::sdl {
namespace {

SDL_Window *g_window = nullptr;
SDL_Renderer *g_renderer = nullptr;
SDL_Texture *g_texture = nullptr;
SDL_AudioDeviceID g_audio = 0;
SDL_GameController *g_pad = nullptr;
int g_scale = 2;
bool g_quit = false;

void open_first_controller() {
    for (int i = 0; i < SDL_NumJoysticks(); ++i) {
        if (SDL_IsGameController(i)) {
            g_pad = SDL_GameControllerOpen(i);
            if (g_pad) break;
        }
    }
}

} // namespace

bool init(int scale) {
    g_scale = scale < 1 ? 2 : (scale > 4 ? 4 : scale);
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) != 0) {
        std::fprintf(stderr, "[sdl] SDL_Init failed: %s\n", SDL_GetError());
        return false;
    }
    g_window = SDL_CreateWindow("pspweb", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                480 * g_scale, 272 * g_scale, SDL_WINDOW_RESIZABLE);
    if (!g_window) {
        std::fprintf(stderr, "[sdl] CreateWindow failed: %s\n", SDL_GetError());
        return false;
    }
    g_renderer = SDL_CreateRenderer(g_window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!g_renderer) g_renderer = SDL_CreateRenderer(g_window, -1, SDL_RENDERER_SOFTWARE);
    if (!g_renderer) {
        std::fprintf(stderr, "[sdl] CreateRenderer failed: %s\n", SDL_GetError());
        return false;
    }
    SDL_RenderSetLogicalSize(g_renderer, 480, 272);
    // RGBA32 is SDL's endian-correct alias for a byte-order RGBA buffer.
    g_texture = SDL_CreateTexture(g_renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STREAMING, 480, 272);
    if (!g_texture) {
        std::fprintf(stderr, "[sdl] CreateTexture failed: %s\n", SDL_GetError());
        return false;
    }
    SDL_AudioSpec want{};
    want.freq = 44100;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 2048;
    g_audio = SDL_OpenAudioDevice(nullptr, 0, &want, nullptr, 0);
    if (g_audio != 0) SDL_PauseAudioDevice(g_audio, 0);
    open_first_controller();
    std::fprintf(stderr, "[sdl] window %dx%d%s\n", 480 * g_scale, 272 * g_scale,
                 g_pad ? ", game controller connected" : ", keyboard only");
    std::fprintf(stderr, "[sdl] keys: arrows=dpad ENTER=start SHIFT=select Z/X/C/V=shapes Q/E=L/R\n");
    return true;
}

Pad poll() {
    Pad pad;
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_QUIT) g_quit = true;
        if ((event.type == SDL_CONTROLLERDEVICEADDED) && !g_pad) open_first_controller();
        if (event.type == SDL_CONTROLLERDEVICEREMOVED && g_pad &&
            SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(g_pad)) == event.cdevice.which) {
            SDL_GameControllerClose(g_pad);
            g_pad = nullptr;
            open_first_controller();
        }
    }
    pad.quit = g_quit;
    Uint32 buttons = 0u;
    const Uint8 *st = SDL_GetKeyboardState(nullptr);
    if (st[SDL_SCANCODE_UP]) buttons |= pad::kUp;
    if (st[SDL_SCANCODE_DOWN]) buttons |= pad::kDown;
    if (st[SDL_SCANCODE_LEFT]) buttons |= pad::kLeft;
    if (st[SDL_SCANCODE_RIGHT]) buttons |= pad::kRight;
    if (st[SDL_SCANCODE_RETURN]) buttons |= pad::kStart;
    if (st[SDL_SCANCODE_LSHIFT] || st[SDL_SCANCODE_RSHIFT]) buttons |= pad::kSelect;
    if (st[SDL_SCANCODE_Z] || st[SDL_SCANCODE_J]) buttons |= pad::kCross;
    if (st[SDL_SCANCODE_X] || st[SDL_SCANCODE_K]) buttons |= pad::kCircle;
    if (st[SDL_SCANCODE_C] || st[SDL_SCANCODE_L]) buttons |= pad::kSquare;
    if (st[SDL_SCANCODE_V] || st[SDL_SCANCODE_SEMICOLON]) buttons |= pad::kTriangle;
    if (st[SDL_SCANCODE_Q] || st[SDL_SCANCODE_U]) buttons |= pad::kL;
    if (st[SDL_SCANCODE_E] || st[SDL_SCANCODE_O]) buttons |= pad::kR;
    int lx = 128, ly = 128;
    if (st[SDL_SCANCODE_W]) ly = 0;
    if (st[SDL_SCANCODE_S]) ly = 255;
    if (st[SDL_SCANCODE_A]) lx = 0;
    if (st[SDL_SCANCODE_D]) lx = 255;

    if (g_pad) {
        auto btn = [&](SDL_GameControllerButton b, Uint32 bit) {
            if (SDL_GameControllerGetButton(g_pad, b)) buttons |= bit;
        };
        btn(SDL_CONTROLLER_BUTTON_DPAD_UP, pad::kUp);
        btn(SDL_CONTROLLER_BUTTON_DPAD_DOWN, pad::kDown);
        btn(SDL_CONTROLLER_BUTTON_DPAD_LEFT, pad::kLeft);
        btn(SDL_CONTROLLER_BUTTON_DPAD_RIGHT, pad::kRight);
        btn(SDL_CONTROLLER_BUTTON_START, pad::kStart);
        btn(SDL_CONTROLLER_BUTTON_BACK, pad::kSelect);
        btn(SDL_CONTROLLER_BUTTON_A, pad::kCross);
        btn(SDL_CONTROLLER_BUTTON_B, pad::kCircle);
        btn(SDL_CONTROLLER_BUTTON_X, pad::kSquare);
        btn(SDL_CONTROLLER_BUTTON_Y, pad::kTriangle);
        btn(SDL_CONTROLLER_BUTTON_LEFTSHOULDER, pad::kL);
        btn(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, pad::kR);
        const int ax = SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_LEFTX);
        const int ay = SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_LEFTY);
        if (ax < -8000 || ax > 8000) lx = (ax + 32768) >> 8;
        if (ay < -8000 || ay > 8000) ly = (ay + 32768) >> 8;
    }

    pad.buttons = buttons;
    pad.lx = static_cast<std::uint8_t>(lx);
    pad.ly = static_cast<std::uint8_t>(ly);
    return pad;
}

void present(const std::uint8_t *rgba, int width, int height) {
    if (!g_texture) return;
    SDL_UpdateTexture(g_texture, nullptr, rgba, width * 4);
    SDL_RenderClear(g_renderer);
    SDL_RenderCopy(g_renderer, g_texture, nullptr, nullptr);
    SDL_RenderPresent(g_renderer);
    (void)height;
}

void push_audio(const std::int16_t *samples, std::uint32_t frames) {
    if (g_audio == 0 || !samples || !frames) return;
    // Drop backlog beyond ~250 ms so a slow guest cannot desync audio forever.
    const Uint32 max_queued = 44100u / 4u * 4u;
    if (SDL_GetQueuedAudioSize(g_audio) < max_queued)
        SDL_QueueAudio(g_audio, samples, frames * 4u);
}

void shutdown() {
    if (g_pad) SDL_GameControllerClose(g_pad);
    g_pad = nullptr;
    if (g_texture) SDL_DestroyTexture(g_texture);
    g_texture = nullptr;
    if (g_renderer) SDL_DestroyRenderer(g_renderer);
    g_renderer = nullptr;
    if (g_window) SDL_DestroyWindow(g_window);
    g_window = nullptr;
    if (g_audio != 0) SDL_CloseAudioDevice(g_audio);
    g_audio = 0;
    SDL_Quit();
}

} // namespace pspweb::sdl

#else // !PSPWEB_USE_SDL

namespace pspweb::sdl {

bool init(int) { return false; }
Pad poll() { return {}; }
void present(const std::uint8_t *, int, int) {}
void push_audio(const std::int16_t *, std::uint32_t) {}
void shutdown() {}

} // namespace pspweb::sdl

#endif
