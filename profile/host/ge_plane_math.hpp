#pragma once
#include "ge_math.hpp"
#include "ge_setup_reciprocal_data.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>

namespace pspweb::ge_plane {
struct Setup {
    std::int64_t q;
    int exponent;
};
inline Setup setup(std::uint64_t area) {
    if (area == 0)
        return {0, 0};
    const int e = std::bit_width(area) - 1;
    const auto normalized = e >= 16 ? area >> (e - 16) : area << (16 - e);
    const unsigned index = unsigned(normalized - 65536), segment = index >> 8;
    return {(2 * std::int64_t(setup_starts[segment]) + setup_slopes[segment] * std::int64_t(index & 255)) >> 8, e};
}

// A 14-fraction-bit plane. Base/evaluation arithmetic is explicitly modular:
// large cancelling intermediates must not invoke signed-overflow behavior.
struct Plane {
    std::int64_t base{}, dx{}, dy{};
    int exponent{};
    bool right{};
    std::int64_t at(std::int64_t x, std::int64_t y) const {
        const auto sum = std::uint64_t(base) + std::uint64_t(dx) * std::uint64_t(x) + std::uint64_t(dy) * std::uint64_t(y);
        return std::bit_cast<std::int64_t>(sum) >> 14;
    }
};

// Geometry and anchor selection are shared by every attribute of a triangle.
// Rectangles override the anchor with their first virtual corner.
class Geometry {
  public:
    Geometry(const std::array<std::int64_t, 3> &x, const std::array<std::int64_t, 3> &y, int forced_anchor = -1) : x_(x), y_(y) {
        const auto area = (x[1] - x[0]) * (y[2] - y[0]) - (x[2] - x[0]) * (y[1] - y[0]);
        sign_ = area < 0 ? -1 : 1;
        divisor_ = setup(area < 0 ? std::uint64_t(-area) : std::uint64_t(area));
        std::array<unsigned, 3> order{0, 1, 2};
        std::sort(order.begin(), order.end(), [&](unsigned a, unsigned b) { return y[a] < y[b] || (y[a] == y[b] && x[a] < x[b]); });
        const auto top = order[0], middle = order[1], bottom = order[2];
        const auto side = (x[bottom] - x[top]) * (y[middle] - y[top]) - (y[bottom] - y[top]) * (x[middle] - x[top]);
        right_ = side > 0 && y[top] < y[middle] && y[middle] < y[bottom];
        for (unsigned i = 1; i < 3; ++i)
            if ((right_ ? x[i] > x[anchor_] : x[i] < x[anchor_]) || (x[i] == x[anchor_] && y[i] < y[anchor_]))
                anchor_ = i;
        if (forced_anchor >= 0) {
            anchor_ = unsigned(forced_anchor);
            right_ = false;
        }
    }

    Plane fixed(const std::array<std::int64_t, 3> &v) const {
        if (divisor_.q == 0)
            return {v[0] * 16384};
        const auto nx = (v[1] - v[0]) * (y_[2] - y_[0]) - (v[2] - v[0]) * (y_[1] - y_[0]);
        const auto ny = (v[2] - v[0]) * (x_[1] - x_[0]) - (v[1] - v[0]) * (x_[2] - x_[0]);
        Plane p{};
        p.dx = (nx * sign_ * divisor_.q) >> (divisor_.exponent + 2);
        p.dy = (ny * sign_ * divisor_.q) >> (divisor_.exponent + 2);
        const auto base = std::uint64_t(v[anchor_] * 16384) - std::uint64_t(p.dx) * std::uint64_t(x_[anchor_]) -
                          std::uint64_t(p.dy) * std::uint64_t(y_[anchor_]);
        p.base = std::bit_cast<std::int64_t>(base);
        p.right = right_;
        return p;
    }

    Plane shared(const std::array<float, 3> &v) const {
        std::array<std::uint32_t, 3> bits{};
        unsigned largest_exponent = 0;
        for (unsigned i = 0; i < 3; ++i) {
            bits[i] = std::bit_cast<std::uint32_t>(v[i]);
            largest_exponent = std::max(largest_exponent, (bits[i] >> 23) & 255u);
        }
        int exponent = 0;
        std::array<std::int64_t, 3> integer{};
        if (largest_exponent == 0) {
            const float largest = std::max({std::abs(v[0]), std::abs(v[1]), std::abs(v[2])});
            exponent = largest == 0 ? 0 : std::ilogb(largest) - 14;
            for (unsigned i = 0; i < 3; ++i)
                integer[i] = std::int64_t(std::ldexp(double(v[i]), -exponent));
        } else {
            exponent = int(largest_exponent) - 127 - 14;
            for (unsigned i = 0; i < 3; ++i) {
                const unsigned e = (bits[i] >> 23) & 255u;
                if (e == 0) {
                    if ((bits[i] & 0x7FFFFFu) != 0)
                        integer[i] = std::int64_t(std::ldexp(double(v[i]), -exponent));
                } else {
                    const unsigned shift = 9 + largest_exponent - e;
                    const std::int64_t magnitude = shift >= 32 ? 0 : ((bits[i] & 0x7FFFFFu) | 0x800000u) >> shift;
                    integer[i] = (bits[i] >> 31) ? -magnitude : magnitude;
                }
            }
        }
        Plane p = fixed(integer);
        p.exponent = exponent;
        return p;
    }

  private:
    std::array<std::int64_t, 3> x_, y_;
    Setup divisor_{};
    int sign_{};
    unsigned anchor_{};
    bool right_{};
};

inline Plane fixed(const std::array<std::int64_t, 3> &x, const std::array<std::int64_t, 3> &y, const std::array<std::int64_t, 3> &v) {
    return Geometry(x, y).fixed(v);
}
inline Plane shared(const std::array<std::int64_t, 3> &x, const std::array<std::int64_t, 3> &y, const std::array<float, 3> &v) {
    return Geometry(x, y).shared(v);
}
inline float value(const Plane &p, std::int64_t x, std::int64_t y) {
    return ge_math::narrow(float(std::ldexp(double(p.at(x, y)), p.exponent)));
}
// Per-fragment UV products keep 24 significant bits, rather than float24's 16.
inline float uv_product(float a, float b) {
    const double d = double(a) * b;
    if (d == 0 || !std::isfinite(d))
        return float(d);
    int e;
    const double f = std::frexp(d, &e);
    return float(std::ldexp(std::trunc(f * 16777216.0), e - 24));
}
} // namespace pspweb::ge_plane
