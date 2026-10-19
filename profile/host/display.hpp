#pragma once
#include <cstdint>

namespace pspweb {
struct DisplayBuffer {
    std::uint32_t framebuffer{};
    std::uint32_t stride{512};
    std::uint32_t format{3};
};

struct DisplayState : DisplayBuffer {
    std::uint32_t vcount{};
    std::uint64_t flips{}; // valid sceDisplaySetFrameBuf requests
    std::uint64_t frame_serial{}; // committed display frames, including same-address draws
    std::uint32_t flip_vcount{~0u};
    DisplayBuffer latched{};
    bool latch_pending{};

    std::uint32_t set_buffer(std::uint32_t address, std::uint32_t line_size,
                             std::uint32_t pixel_format, std::uint32_t sync,
                             std::uint32_t ram_size) {
        if (sync > 1u) return 0x80000107u;
        const auto physical = address & 0x1FFFFFFFu;
        const bool valid_address = (physical >= 0x04000000u && physical < 0x04200000u) ||
                                   (physical >= 0x08000000u && physical - 0x08000000u < ram_size);
        if (address != 0u && (!valid_address || (address & 15u) != 0u)) return 0x80000103u;
        if ((line_size & 63u) != 0u || (address != 0u && line_size == 0u)) return 0x80000104u;
        if (pixel_format > 3u) return 0x80000108u;
        if (sync == 0u && (latched.stride != line_size || latched.format != pixel_format))
            return 0x80000107u;
        ++flips;
        latched = {address, line_size, pixel_format};
        // Format and stride change immediately even for NEXTFRAME requests.
        stride = line_size;
        format = pixel_format;
        if (sync == 0u) {
            framebuffer = address;
            latch_pending = false;
            ++frame_serial;
        } else {
            latch_pending = true;
        }
        return 0u;
    }

    void begin_vblank() {
        ++vcount;
        if (!latch_pending) return;
        static_cast<DisplayBuffer &>(*this) = latched;
        latch_pending = false;
        ++frame_serial;
    }

    const DisplayBuffer &get_buffer(std::uint32_t sync) const {
        return sync == 1u ? latched : static_cast<const DisplayBuffer &>(*this);
    }
};
}
