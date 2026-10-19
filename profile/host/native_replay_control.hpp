#pragma once
#include <cstdint>
#include <fstream>
#include <sstream>
#include <string>

namespace pspweb {
// Local diagnostic controller only. Each new serial queues one PSP input pulse.
// File format: serial buttons_hex lx ly frames run|pause|quit.
// Invalid/incomplete writes are ignored, and input releases after `frames`.
struct NativeReplayControl {
    std::uint64_t serial = 0;
    std::uint32_t buttons = 0;
    int lx = 128, ly = 128, remaining = 0;
    std::string command = "run";
    void poll(const std::string &path) {
        std::ifstream in(path);
        std::uint64_t next_serial;
        std::string mask, next_command;
        int x, y, frames;
        if (!(in >> next_serial >> mask >> x >> y >> frames >> next_command) ||
            next_serial <= serial || x < 0 || x > 255 || y < 0 || y > 255 ||
            frames < 0 || frames > 3600 ||
            (next_command != "run" && next_command != "pause" && next_command != "quit")) return;
        try {
            std::size_t consumed;
            const auto value = std::stoul(mask, &consumed, 16);
            if (consumed != mask.size() || value > 0xFFFFFFFFull) return;
            serial = next_serial; buttons = static_cast<std::uint32_t>(value);
            lx = x; ly = y; remaining = frames; command = next_command;
        } catch (...) { return; }
    }
    void advance() {
        if (remaining > 0 && --remaining == 0) {
            buttons = 0;
            lx = ly = 128;
        }
    }
};
}
