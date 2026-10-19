#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace pspweb {
inline constexpr double kPspVblankMs = 1001.0 / 60.0;
inline constexpr double kPspVblankHz = 60000.0 / 1001.0;
inline constexpr std::uint64_t psp_vblank_time_us(std::uint64_t count) {
    return count / 60u * 1001000u + (count % 60u) * 1001000u / 60u;
}

// Keep PSP simulation time independent of the host display refresh. A resumed
// or stalled tab starts a fresh deadline instead of fast-forwarding old debt.
class FramePacer {
public:
    void reset() { initialized_ = false; }
    unsigned consume(double now_ms) {
        if (!std::isfinite(now_ms)) return 0u;
        if (!initialized_ || now_ms < previous_ms_ || now_ms - next_ms_ > 100.0) {
            next_ms_ = now_ms;
            initialized_ = true;
        }
        previous_ms_ = now_ms;
        const auto debt = now_ms - next_ms_;
        if (debt < -0.000001) return 0u;
        const auto steps = static_cast<unsigned>(std::clamp(std::floor((debt + 0.000001) / kPspVblankMs) + 1.0, 1.0, 2.0));
        next_ms_ += steps * kPspVblankMs;
        return steps;
    }
private:
    bool initialized_{};
    double previous_ms_{}, next_ms_{};
};
}
