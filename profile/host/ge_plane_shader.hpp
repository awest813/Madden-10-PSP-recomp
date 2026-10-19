#pragma once
namespace pspweb::ge_plane {
// Shader implementations use the same measured data as the CPU, with 32-bit
// modular evaluation of covered 15-bit values and a full signed edge path.
inline constexpr char shader_math[] = R"glsl(
uniform highp usampler2D u_planes;
uniform highp usampler2D u_reciprocal;
uvec4 plane_word(int index) { return texelFetch(u_planes, ivec2(index & 63, index >> 6), 0); }
float plane_value(uvec4 p, ivec2 at) {
    uint sum = p.x + p.y * uint(at.x) + p.z * uint(at.y);
    int value = int(sum) >> 14;
    if (value == 0)
        return 0.0;
    uint bits = floatBitsToUint(float(value));
    int exponent = int((bits >> 23u) & 255u) + int(p.w);
    if (exponent <= 0 || exponent >= 255)
        return float(value) * exp2(float(int(p.w)));
    return uintBitsToFloat(((bits & 0x807fffffu) | (uint(exponent) << 23u)) & 0xffffff00u);
}
float plane_recip(float value) {
    uint bits = floatBitsToUint(value);
    int exponent = int((bits >> 23u) & 255u);
    if (exponent == 0 || exponent == 255)
        return 1.0 / value;
    uvec4 seg = texelFetch(u_reciprocal, ivec2(int((bits >> 16u) & 127u), 0), 0);
    uint q = uint(64 * int(seg.x) + 63 + int(seg.y) * int((bits >> 8u) & 255u)) >> 7u;
    uint qb = floatBitsToUint(float(q));
    int target = int((qb >> 23u) & 255u) + 111 - exponent;
    uint sign = bits & 0x80000000u;
    if (target <= 0) {
        uint mag = (qb & 0x7fffffu) | 0x800000u;
        uint shift = uint(1 - target), result = shift >= 32u ? 0u : mag >> shift;
        if (shift < 32u) {
            uint tail = mag & ((1u << shift) - 1u), midpoint = 1u << (shift - 1u);
            if (tail > midpoint || (tail == midpoint && (result & 1u) != 0u))
                ++result;
        }
        return uintBitsToFloat(sign | result);
    }
    return uintBitsToFloat(sign | (uint(target) << 23u) | (qb & 0x7fffffu));
}
float plane_product(float a, float b) {
    uint ab = floatBitsToUint(a), bb = floatBitsToUint(b);
    int ae = int((ab >> 23u) & 255u), be = int((bb >> 23u) & 255u);
    if (ae == 0 || be == 0 || ae == 255 || be == 255)
        return a * b;
    uint am = ((ab & 0x7fffffu) | 0x800000u) >> 8u, bm = ((bb & 0x7fffffu) | 0x800000u) >> 8u;
    uint product = am * bm, carry = product >= 0x80000000u ? 1u : 0u;
    int exponent = ae + be - 127 + int(carry);
    if (exponent <= 0 || exponent >= 255)
        return a * b;
    uint significand = product >> (7u + carry);
    return uintBitsToFloat(((ab ^ bb) & 0x80000000u) | (uint(exponent) << 23u) | (significand & 0x7fffffu));
}
#ifndef PSPWEB_FAST_COVERAGE
uvec2 plane_multiply(int a, int b) {
    uint ua = uint(a), ub = uint(b), al = ua & 65535u, ah = ua >> 16u, bl = ub & 65535u, bh = ub >> 16u;
    uint p0 = al * bl, p1 = ah * bl, p2 = al * bh;
    uint carry = (p0 >> 16u) + (p1 & 65535u) + (p2 & 65535u);
    uint low = (p0 & 65535u) | (carry << 16u), high = ah * bh + (p1 >> 16u) + (p2 >> 16u) + (carry >> 16u);
    if (a < 0)
        high -= ub;
    if (b < 0)
        high -= ua;
    return uvec2(low, high);
}
#endif
bool plane_edge_inside(ivec2 a, ivec2 b, ivec2 p) {
    ivec2 d = b - a, r = p - a;
#ifdef PSPWEB_FAST_COVERAGE
    int value = int(uint(d.x) * uint(r.y) - uint(d.y) * uint(r.x));
    if (value < 0)
        return false;
    if (value == 0)
        return d.y < 0 || (d.y == 0 && d.x > 0);
#else
    uvec2 a_product = plane_multiply(d.x, r.y), b_product = plane_multiply(d.y, r.x);
    uint low = a_product.x - b_product.x,
         high = a_product.y - b_product.y - (a_product.x < b_product.x ? 1u : 0u);
    if (int(high) < 0)
        return false;
    if (high == 0u && low == 0u)
        return d.y < 0 || (d.y == 0 && d.x > 0);
#endif
    return true;
}
bool plane_covers(ivec2 p, ivec2 a, ivec2 b, ivec2 c, bool rectangle) {
    if (rectangle) {
        ivec2 lo = min(a, min(b, c)), hi = max(a, max(b, c));
        return all(greaterThanEqual(p, lo)) && all(lessThan(p, hi));
    }
    return plane_edge_inside(a, b, p) && plane_edge_inside(b, c, p) && plane_edge_inside(c, a, p);
}
float plane_lod_q(int record, uvec4 qp, ivec2 pixel_at, uint flags) {
    int span = (pixel_at.x / 16) & ~3;
    int row = (pixel_at.y / 16) * 16 + 8;
    bool rtl = (flags & 8u) != 0u, rectangle = (flags & 4u) != 0u;
    uvec4 ab = plane_word(record + 5), cc = plane_word(record + 6);
    ivec2 a = ivec2(ab.xy), b = ivec2(ab.zw), c = ivec2(cc.xy);
    int pick = span + (rtl ? 2 : 1);
    if (!plane_covers(ivec2(pick * 16 + 8, row), a, b, c, rectangle)) {
        for (int i = 0; i < 4; ++i) {
            int x = span + (rtl ? 3 - i : i);
            if (plane_covers(ivec2(x * 16 + 8, row), a, b, c, rectangle)) {
                pick = x;
                break;
            }
        }
    }
    return plane_value(qp, ivec2(pick * 16 + 8, row));
}
float plane_log16(float value) { return float(int((floatBitsToUint(value) >> 19u) & 4095u) - 2032); }
)glsl";
} // namespace pspweb::ge_plane
