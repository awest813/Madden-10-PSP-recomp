#pragma once

// SDL2 windowed frontend for desktop builds (Windows / Linux).
//
// The accelerated GL path (ge_gl.cpp, OpenGL ES 3) needs an EGL/ANGLE setup
// on Windows that is not wired up yet, so this frontend presents the guest
// framebuffer via the software rasterizer: each frame is uploaded as an SDL
// texture and scaled with nearest/linear filtering.  That is enough to boot
// a title, walk menus and measure guest CPU progress while the GL bring-up
// continues.  Compiled out unless PSPWEB_USE_SDL is defined (see
// profile/CMakeLists.txt and scripts/build_sdl.*).

#include <cstdint>

namespace pspweb::sdl {

struct Pad {
    std::uint32_t buttons{};
    std::uint8_t lx{128u};
    std::uint8_t ly{128u};
    bool quit{false};
};

// Opens a window scaled `scale` times the PSP's 480x272 and starts audio.
// Returns false when SDL is unavailable; the caller falls back to headless.
bool init(int scale);
// Polls keyboard + game controllers, returns the current pad state.
Pad poll();
// Shows the latest guest framebuffer (RGBA, top row first).
void present(const std::uint8_t *rgba, int width, int height);
// Queues interleaved stereo 16-bit PCM at 44.1 kHz for playback.
void push_audio(const std::int16_t *samples, std::uint32_t frames);
void shutdown();

} // namespace pspweb::sdl
