#pragma once
#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <cmath>
#include "ge_reciprocal_data.hpp"

namespace pspweb::ge_math {

// Four finite float24 products keep a shared, unnormalized weight. Align each
// magnitude before cancellation; a float32 dot followed by a bit mask differs.
// Nonfinite arithmetic retains an IEEE fallback and is not hardware certified.
// Independent integer formulation of the measured GE row-sum rules described
// at https://www.ppsspp.org/docs/psp-hardware/gpu/arithmetic/ .
inline float narrow(float value) {
    auto bits = std::bit_cast<std::uint32_t>(value);
    if ((bits & 0x7F800000u) == 0u) bits &= 0x80000000u;
    else bits &= 0xFFFFFF00u;
    return std::bit_cast<float>(bits);
}

inline float dot(const std::array<float, 4> &left, const std::array<float, 4> &right) {
    std::array<std::uint32_t, 4> magnitude{};
    std::array<int, 4> weight{};
    std::array<bool, 4> negative{};
    int common = -10000;
    for (unsigned i = 0; i < 4; ++i) {
        const auto a = std::bit_cast<std::uint32_t>(left[i]);
        const auto b = std::bit_cast<std::uint32_t>(right[i]);
        const int ae = (a >> 23) & 255, be = (b >> 23) & 255;
        if (ae == 255 || be == 255) {
            // Preserve the previous IEEE path for unmeasured nonfinite inputs.
            return narrow(left[0] * right[0] + left[1] * right[1] + left[2] * right[2] + left[3] * right[3]);
        }
        if (ae == 0 || be == 0) continue;
        const std::uint32_t am = ((a & 0x7FFFFFu) | 0x800000u) >> 8;
        const std::uint32_t bm = ((b & 0x7FFFFFu) | 0x800000u) >> 8;
        magnitude[i] = (am * bm) >> 15;
        weight[i] = ae + be - 269;
        negative[i] = ((a ^ b) >> 31) != 0;
        common = std::max(common, weight[i]);
    }
    // Four 17-bit products fit in a signed 20-bit accumulator.
    int sum = 0;
    for (unsigned i = 0; i < 4; ++i) if (magnitude[i]) {
        const int shift = common - weight[i];
        const std::uint32_t aligned = shift >= 32 ? 0 : magnitude[i] >> shift;
        sum += negative[i] ? -static_cast<int>(aligned) : static_cast<int>(aligned);
    }
    if (sum == 0) return 0;
    const std::uint32_t sign = sum < 0 ? 0x80000000u : 0u;
    const std::uint32_t mag = sum < 0 ? static_cast<std::uint32_t>(-sum) : static_cast<std::uint32_t>(sum);
    const int leading = std::bit_width(mag) - 1, exponent = leading + common + 127;
    if (exponent <= 0) {
        // The represented output may be subnormal; the next GE input stage
        // treats it as zero. Model host float32 conversion before the low-bit cut.
        const int shift = common + 149;
        std::uint32_t sub = 0;
        if (shift >= 0) sub = mag << shift;
        else if (-shift < 32) {
            const unsigned n = static_cast<unsigned>(-shift);
            const std::uint32_t tail = mag & ((1u << n) - 1u), half = 1u << (n - 1u);
            sub = mag >> n;
            if (tail > half || (tail == half && (sub & 1u))) ++sub;
        }
        return std::bit_cast<float>(sign | (sub & 0xFFFFFF00u));
    }
    if (exponent >= 255) return std::bit_cast<float>(sign | 0x7F800000u);
    const std::uint32_t mantissa = leading > 15 ? mag >> (leading - 15) : mag << (15 - leading);
    return std::bit_cast<float>(sign | (static_cast<std::uint32_t>(exponent) << 23) | ((mantissa & 0x7FFFu) << 8));
}

inline float scale_offset(float value, float scale, float offset) {
    // UV scale products normalize to float24 before the separate GE add.
    const float product = dot({value, 0, 0, 0}, {scale, 0, 0, 0});
    return dot({product, offset, 0, 0}, {1, 1, 0, 0});
}
inline float reciprocal(float value) {
    const auto bits = std::bit_cast<std::uint32_t>(value);
    const int exponent = (bits >> 23) & 255;
    if (exponent == 0 || exponent == 255) return 1.0f / value;
    const auto &seg = reciprocal_segments[(bits >> 16) & 127];
    const unsigned q = (64 * seg.start + 63 + seg.slope * int((bits >> 8) & 255)) / 128;
    // q is an exact integer. A power-of-two scale adjusts only its exponent.
    const auto qb = std::bit_cast<std::uint32_t>(float(q));
    const int target = int((qb >> 23) & 255) + 111 - exponent;
    if (target <= 0 || target >= 255) return std::copysign(std::ldexp(float(q), 111 - exponent), value);
    return std::bit_cast<float>((bits & 0x80000000u) | (std::uint32_t(target) << 23) | (qb & 0x7FFFFFu));
}

// Keep the carry bit: viewport addition is not a float24 row result.
inline float aligned_add(float a, float b) {
    const auto ab = std::bit_cast<std::uint32_t>(a), bb = std::bit_cast<std::uint32_t>(b);
    const int ae = (ab >> 23) & 255, be = (bb >> 23) & 255;
    if (ae == 255 || be == 255) return a + b;
    const int e = std::max(ae, be);
    auto term = [&](std::uint32_t bits, int exponent) {
        if (exponent == 0 || e - exponent >= 16) return 0;
        const int magnitude = int((((bits & 0x7FFFFFu) | 0x800000u) >> 8) >> (e - exponent));
        return (bits >> 31) ? -magnitude : magnitude;
    };
    const int sum = term(ab, ae) + term(bb, be);
    if (sum == 0) return 0;
    const std::uint32_t magnitude = unsigned(sum < 0 ? -sum : sum), sign = sum < 0 ? 0x80000000u : 0u;
    const int leading = std::bit_width(magnitude) - 1, target = leading + e - 15;
    if (target <= 0 || target >= 255) return std::ldexp(float(sum), e - 142);
    return std::bit_cast<float>(sign | (unsigned(target) << 23) | ((magnitude << (23 - leading)) & 0x7FFFFFu));
}

// Unlike row products, this stage normalizes immediately to a 16-bit significand.
inline float product24(float a, float b) {
    const auto ab = std::bit_cast<std::uint32_t>(a), bb = std::bit_cast<std::uint32_t>(b);
    const int ae = (ab >> 23) & 255, be = (bb >> 23) & 255;
    if (ae == 0 || be == 0 || ae == 255 || be == 255) {
        const double product = double(a) * double(b);
        if (!std::isfinite(product) || product == 0) return float(product);
        int exponent;
        const double fraction = std::frexp(product, &exponent);
        return float(std::ldexp(std::trunc(fraction * 65536.0), exponent - 16));
    }
    const std::uint32_t am = ((ab & 0x7FFFFFu) | 0x800000u) >> 8, bm = ((bb & 0x7FFFFFu) | 0x800000u) >> 8;
    const std::uint32_t product = am * bm;
    const int carry = product >= 0x80000000u ? 1 : 0;
    const std::uint32_t significand = product >> (15 + carry), sign = (ab ^ bb) & 0x80000000u;
    const int exponent = ae + be - 127 + carry;
    if (exponent <= 0 || exponent >= 255)
        return std::copysign(std::ldexp(float(significand), exponent - 142), std::bit_cast<float>(sign));
    return std::bit_cast<float>(sign | (std::uint32_t(exponent) << 23) | ((significand & 0x7FFFu) << 8));
}

// inv_w is shared by all three coordinates of this vertex.
inline float viewport(float clip, float w, float scale, float center, float inv_w) {
    if (!std::isfinite(clip) || !std::isfinite(w) || std::abs(w) < 0x1p-126f)
        return clip * scale / w + center;
    const float clip24 = std::bit_cast<float>(std::bit_cast<std::uint32_t>(clip) & 0xFFFFFF00u);
    const float ndc = product24(clip24, inv_w);
    const float scaled = product24(ndc, scale);
    return aligned_add(scaled, center);
}
} // namespace pspweb::ge_math
