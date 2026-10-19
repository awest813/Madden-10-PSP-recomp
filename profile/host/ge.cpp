#include "ge.hpp"
#include "ge_math.hpp"

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif
#include "ge_gl.hpp"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <sstream>

namespace pspweb {
namespace {

// GE command opcodes used below.
enum : std::uint32_t {
    kNop = 0x00, kVaddr = 0x01, kIaddr = 0x02, kPrim = 0x04, kJump = 0x08, kCall = 0x0A, kRet = 0x0B,
    kEnd = 0x0C, kFinish = 0x0F, kBase = 0x10, kVertexType = 0x12, kOffsetAddr = 0x13, kOrigin = 0x14,
    kLightingEnable = 0x17, kLightEnable0 = 0x18, kCullEnable = 0x1D, kTextureEnable = 0x1E, kFogEnable = 0x1F,
    kBlendEnable = 0x21, kAlphaTestEnable = 0x22, kZTestEnable = 0x23,
    kBoneMatrixNumber = 0x2A, kBoneMatrixData = 0x2B, kMorphWeight0 = 0x2C,
    kWorldMatrixNumber = 0x3A, kWorldMatrixData = 0x3B, kViewMatrixNumber = 0x3C, kViewMatrixData = 0x3D,
    kProjMatrixNumber = 0x3E, kProjMatrixData = 0x3F, kTgenMatrixNumber = 0x40, kTgenMatrixData = 0x41,
    kViewportXScale = 0x42, kViewportYScale = 0x43, kViewportZScale = 0x44,
    kViewportXCenter = 0x45, kViewportYCenter = 0x46, kViewportZCenter = 0x47,
    kTexScaleU = 0x48, kTexScaleV = 0x49, kTexOffsetU = 0x4A, kTexOffsetV = 0x4B,
    kOffsetX = 0x4C, kOffsetY = 0x4D, kShadeMode = 0x50, kReverseNormals = 0x51, kMaterialUpdate = 0x53,
    kMaterialEmissive = 0x54, kMaterialAmbient = 0x55, kMaterialDiffuse = 0x56, kMaterialSpecular = 0x57, kMaterialAlpha = 0x58,
    kSpecularExponent = 0x5B, kAmbientColor = 0x5C, kAmbientAlpha = 0x5D, kLightMode = 0x5E,
    kLightType0 = 0x5F, kLightPos0 = 0x63, kLightDir0 = 0x6F,
    kLightAtt0 = 0x7B, kLightExponent0 = 0x87, kLightCutoff0 = 0x8B, kLightColor0 = 0x8F, kCull = 0x9B,
    kFrameBufPtr = 0x9C, kFrameBufWidth = 0x9D, kZBufPtr = 0x9E, kZBufWidth = 0x9F,
    kTexAddr0 = 0xA0, kTexBufWidth0 = 0xA8, kClutAddr = 0xB0, kClutAddrUpper = 0xB1,
    kTransferSrc = 0xB2, kTransferSrcW = 0xB3, kTransferDst = 0xB4, kTransferDstW = 0xB5,
    kTexSize0 = 0xB8, kTexMapMode = 0xC0, kTexShade = 0xC1, kTexMode = 0xC2, kTexFormat = 0xC3, kLoadClut = 0xC4,
    kClutFormat = 0xC5, kTexWrap = 0xC7, kTexFunc = 0xC9, kTexEnvColor = 0xCA,
    kFogEnd = 0xCD, kFogSlope = 0xCE, kFogColor = 0xCF,
    kFrameBufPixFormat = 0xD2, kClearMode = 0xD3, kScissor1 = 0xD4, kScissor2 = 0xD5,
    kAlphaTest = 0xDB, kZTest = 0xDE, kBlendMode = 0xDF, kBlendFixA = 0xE0, kBlendFixB = 0xE1,
    kZWriteDisable = 0xE7, kMaskRgb = 0xE8, kMaskAlpha = 0xE9, kTransferStart = 0xEA,
    kTransferSrcPos = 0xEB, kTransferDstPos = 0xEC, kTransferSize = 0xEE,
};

constexpr std::uint32_t kVramBase = 0x04000000u;

inline std::uint8_t expand5(std::uint32_t v) { return static_cast<std::uint8_t>((v << 3) | (v >> 2)); }
inline std::uint8_t expand6(std::uint32_t v) { return static_cast<std::uint8_t>((v << 2) | (v >> 4)); }
inline std::uint8_t expand4(std::uint32_t v) { return static_cast<std::uint8_t>(v * 17u); }

inline std::uint32_t pack(std::uint32_t r, std::uint32_t g, std::uint32_t b, std::uint32_t a) {
    return r | (g << 8) | (b << 16) | (a << 24);
}

// Converts a 16-bit PSP colour (format 0 5650, 1 5551, 2 4444) to RGBA8.
inline std::uint32_t decode16(std::uint32_t v, std::uint32_t format) {
    switch (format) {
    case 0: return pack(expand5(v & 31u), expand6((v >> 5) & 63u), expand5((v >> 11) & 31u), 255u);
    case 1: return pack(expand5(v & 31u), expand5((v >> 5) & 31u), expand5((v >> 10) & 31u), (v >> 15) ? 255u : 0u);
    default: return pack(expand4(v & 15u), expand4((v >> 4) & 15u), expand4((v >> 8) & 15u), expand4((v >> 12) & 15u));
    }
}

inline float clamp255(float v) { return v < 0.0f ? 0.0f : (v > 255.0f ? 255.0f : v); }

// GE texture rows are 16-byte aligned; low stride bits are ignored, and
// an encoded zero/short row selects the minimum row for that texel format.
std::uint32_t texture_row_width(std::uint32_t format, std::uint32_t encoded) {
    constexpr unsigned bits[] = {16,16,16,32,4,8,16,32,4,8,8};
    if (format >= std::size(bits)) return std::max(encoded & 0x7FFu,1u);
    const unsigned alignment = 128u / bits[format];
    const unsigned width = (encoded & 0x7FFu) & ~(alignment - 1u);
    return width == 0u ? alignment : width;
}

float interpolated_byte(float value) {
    return std::floor(clamp255(value));
}

float fog_factor(float value) {
    // Sign also decides nonfinite values: negative NaN/inf fog completely,
    // positive NaN/inf keep the source. Finite factors truncate to 8 bits.
    if (std::signbit(value)) return 0.0f;
    if (!(value < 255.0f / 256.0f)) return 255.0f;
    return std::floor(value * 256.0f);
}

float normalize3(float v[3]) {
    const float length = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (length > 0.0f) for (int k = 0; k < 3; ++k) v[k] /= length;
    return length;
}

// GE lighting uses 8-bit color factors with a half-step offset. Each light
// contribution is quantized before accumulation, including attenuation/spot.
int light_product(float a, float b) {
    return ((2 * static_cast<int>(clamp255(a)) + 1) * (2 * static_cast<int>(clamp255(b)) + 1)) >> 10;
}
int light_scale(int color, float factor) {
    if (!(factor > 0.0f)) return 0;
    if (factor >= 1.0f) return color;
    const int scale = static_cast<int>(factor * 256.0f);
    return ((2 * color + 1) * (2 * scale + 1)) >> 10;
}
float light_exponent(float value) {
    if (std::isnan(value)) return std::signbit(value) ? 0.0f : 496.0f;
    if (value <= 0.0f) return 0.0f;
    if (value >= 512.0f) return 496.0f;
    return std::bit_cast<float>(std::bit_cast<std::uint32_t>(value) & 0xFFF80000u);
}
float light_power(float value, float exponent) {
    if (exponent == 0.0f) return 1.0f;
    if (!(value > 0.0f)) return value;
    // Piecewise-linear log2/exp2 between powers of two, rather than std::pow.
    // The GE truncates the log product to 19 fractional bits.
    constexpr double one = 0x3F800000u;
    const double log = static_cast<double>(std::bit_cast<std::uint32_t>(value)) - one;
    const double powered = one + 16.0 * std::trunc(log * exponent / 16.0);
    const auto bits = static_cast<std::uint32_t>(std::clamp(powered, 0.0, static_cast<double>(0x7F7FFFFFu)));
    return std::bit_cast<float>(bits);
}

std::uint64_t hash_bytes(const std::uint8_t *data, std::size_t length) {
    std::uint64_t h = 0x9E3779B97F4A7C15ull ^ length;
    std::size_t i = 0;
    for (; i + 8u <= length; i += 8u) {
        std::uint64_t word;
        std::memcpy(&word, data + i, 8u);
        h = (h ^ word) * 0xFF51AFD7ED558CCDull;
        h ^= h >> 32u;
    }
    for (; i < length; ++i) h = (h ^ data[i]) * 0x100000001B3ull;
    return h;
}

constexpr std::uint64_t kTextureCacheTexelLimit = 16ull * 1024ull * 1024ull;

// PSPWEB_GE_PROFILE diagnostics.
std::map<std::uint64_t, std::uint64_t> g_target_pixels;
const bool g_profile_targets = std::getenv("PSPWEB_GE_PROFILE") != nullptr;
double g_section_ms[6] = {}; // decode, emit, texture, transfer, gl state, transfer GL sync
const char *const g_section_names[6] = {"decode", "emit", "texture", "transfer", "state", "xfer-gl"};
double g_transfer_bytes = 0.0;
// Milliseconds from a monotonic clock; in the browser performance.now() directly,
// which is far cheaper than std::chrono's path there.
double now_ms() {
#ifdef __EMSCRIPTEN__
    return emscripten_get_now();
#else
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
#endif
}

// Reads the clock only while profiling: in the browser every read is a call into JS.
struct SectionTimer {
    int index;
    double start = g_profile_targets ? now_ms() : 0.0;
    ~SectionTimer() {
        if (g_profile_targets) g_section_ms[index] += now_ms() - start;
    }
};
// Debugging aids: PSPWEB_DUMP_FRAME=n logs every primitive of frame n with its
// state and writes the textures it decodes to PSPWEB_DUMP_DIR (default
// build/dump); PSPWEB_SKIP_PRIMS=a-b[,c-d...] drops those primitives of every frame.
std::uint32_t env_number(const char *name, std::uint32_t fallback) {
    const char *v = std::getenv(name);
    return v != nullptr ? static_cast<std::uint32_t>(std::strtoul(v, nullptr, 10)) : fallback;
}
const std::uint32_t g_dump_frame = env_number("PSPWEB_DUMP_FRAME", 0u);
const std::string g_dump_dir = std::getenv("PSPWEB_DUMP_DIR") != nullptr ? std::getenv("PSPWEB_DUMP_DIR") : "build/dump";
const std::vector<std::pair<std::uint32_t, std::uint32_t>> g_skip_prims = [] {
    std::vector<std::pair<std::uint32_t, std::uint32_t>> ranges;
    const char *v = std::getenv("PSPWEB_SKIP_PRIMS");
    while (v != nullptr && *v != '\0') {
        char *end = nullptr;
        const auto lo = static_cast<std::uint32_t>(std::strtoul(v, &end, 10));
        const auto hi = *end == '-' ? static_cast<std::uint32_t>(std::strtoul(end + 1, &end, 10)) : lo;
        ranges.emplace_back(lo, hi);
        v = *end == ',' ? end + 1 : nullptr;
    }
    return ranges;
}();
bool skipped_prim(std::uint32_t index) {
    for (const auto &[lo, hi] : g_skip_prims)
        if (index >= lo && index <= hi) return true;
    return false;
}

#ifdef __EMSCRIPTEN__
// ?profile in the page sets PSPWEB_FRAME_PROFILE: list timing only, cheap enough
// not to distort the numbers it shows.
const bool g_list_timing = g_profile_targets || std::getenv("PSPWEB_FRAME_PROFILE") != nullptr;
#else
const bool g_list_timing = true;
#endif

void transform43(const std::array<float, 12> &m, const float in[3], float out[3]) {
    out[0] = ge_math::dot({in[0], in[1], in[2], 1}, {m[0], m[3], m[6], m[9]});
    out[1] = ge_math::dot({in[0], in[1], in[2], 1}, {m[1], m[4], m[7], m[10]});
    out[2] = ge_math::dot({in[0], in[1], in[2], 1}, {m[2], m[5], m[8], m[11]});
}

void rotate43(const std::array<float, 12> &m, const float in[3], float out[3]) {
    out[0] = in[0] * m[0] + in[1] * m[3] + in[2] * m[6];
    out[1] = in[0] * m[1] + in[1] * m[4] + in[2] * m[7];
    out[2] = in[0] * m[2] + in[1] * m[5] + in[2] * m[8];
}

void color24(std::uint32_t data, float out[3]) {
    out[0] = static_cast<float>(data & 0xFFu);
    out[1] = static_cast<float>((data >> 8) & 0xFFu);
    out[2] = static_cast<float>((data >> 16) & 0xFFu);
}

bool compare(std::uint32_t func, std::uint32_t value, std::uint32_t reference) {
    switch (func & 7u) {
    case 0: return false;
    case 1: return true;
    case 2: return value == reference;
    case 3: return value != reference;
    case 4: return value < reference;
    case 5: return value <= reference;
    case 6: return value > reference;
    default: return value >= reference;
    }
}

} // namespace

Ge::Ge(psprecomp::GuestMemory &memory) : memory_(memory) {
    world_ = {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0};
    view_ = world_;
    tgen_ = world_;
    for (auto &bone : bones_) bone = world_;
    proj_ = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    morph_weights_[0] = 1.0f;
    regs_[kTexScaleU] = regs_[kTexScaleV] = std::bit_cast<std::uint32_t>(1.0f) >> 8u;
    call_stack_.reserve(64);
}

float Ge::cmd_float(std::uint32_t data) { return std::bit_cast<float>(data << 8u); }

std::uint32_t Ge::relative_address(std::uint32_t data) const {
    return ((((regs_[kBase] & 0x000F0000u) << 8u) | data) + offset_) & 0x0FFFFFFFu;
}

bool Ge::run_list(std::uint32_t start, std::uint32_t stall) {
    const double started = g_list_timing ? now_ms() : 0.0;
    struct Timer {
        Stats &stats;
        double start;
        ~Timer() {
            if (g_list_timing) stats.nanoseconds += static_cast<std::uint64_t>((now_ms() - start) * 1e6);
        }
    } timer{stats_, started};
    ++stats_.lists;
    pc_ = start & 0x0FFFFFFFu;
    stall &= 0x0FFFFFFFu;
    call_stack_.clear();
    // Commands are read through a raw pointer to the current 4 KiB page.
    const std::uint8_t *page = nullptr;
    std::uint32_t page_start = 1u;
    for (std::uint32_t guard = 0; guard < 8'000'000u; ++guard) {
        if (stall != 0u && pc_ == stall) return false;
        if ((pc_ & ~0xFFFu) != page_start) {
            page_start = pc_ & ~0xFFFu;
            page = memory_.raw_pointer(page_start, 0x1000u);
        }
        std::uint32_t op;
        if (page != nullptr) std::memcpy(&op, page + (pc_ - page_start), 4u);
        else op = memory_.load32(pc_);
        pc_ += 4u;
        if ((op >> 24u) == kEnd) return true;
        execute(op);
    }
    return true;
}

void Ge::execute(std::uint32_t op) {
    const std::uint32_t cmd = op >> 24u;
    const std::uint32_t data = op & 0x00FFFFFFu;
    switch (cmd) {
    case kNop: return;
    case kVaddr: vertex_address_ = relative_address(data); break;
    case kIaddr: index_address_ = relative_address(data); break;
    case kPrim: draw_primitive((data >> 16u) & 7u, data & 0xFFFFu); break;
    case kJump: pc_ = relative_address(data & 0xFFFFFCu); break;
    case kCall:
        call_stack_.push_back(pc_);
        call_stack_.push_back(offset_);
        pc_ = relative_address(data & 0xFFFFFCu);
        break;
    case kRet:
        if (call_stack_.size() >= 2u) {
            offset_ = call_stack_.back();
            call_stack_.pop_back();
            pc_ = call_stack_.back();
            call_stack_.pop_back();
        }
        break;
    case kOffsetAddr: offset_ = data << 8u; break;
    case kOrigin: offset_ = pc_ - 4u; break;
    case kWorldMatrixNumber: world_index_ = data & 0xFu; break;
    case kWorldMatrixData: if (world_index_ < 12u) world_[world_index_++] = cmd_float(data); break;
    case kViewMatrixNumber: view_index_ = data & 0xFu; break;
    case kViewMatrixData: if (view_index_ < 12u) view_[view_index_++] = cmd_float(data); break;
    case kProjMatrixNumber: proj_index_ = data & 0xFu; break;
    case kProjMatrixData: if (proj_index_ < 16u) proj_[proj_index_++] = cmd_float(data); break;
    case kTgenMatrixNumber: tgen_index_ = data & 0xFu; break;
    case kTgenMatrixData: if (tgen_index_ < 12u) tgen_[tgen_index_++] = cmd_float(data); break;
    case kBoneMatrixNumber: bone_index_ = data & 0x7Fu; break;
    case kBoneMatrixData:
        if (bone_index_ < 96u) bones_[bone_index_ / 12u][bone_index_ % 12u] = cmd_float(data);
        ++bone_index_;
        break;
    default:
        if (cmd >= kMorphWeight0 && cmd < kMorphWeight0 + 8u) morph_weights_[cmd - kMorphWeight0] = cmd_float(data);
        break;
    }
    regs_[cmd] = data;
    switch (cmd) {
    case kNop: case kVaddr: case kIaddr: case kPrim: case kJump: case kCall: case kRet: case kEnd: case kFinish:
    case kBase: case kOffsetAddr: case kOrigin: case kVertexType: case kBoneMatrixNumber: case kBoneMatrixData:
    case kWorldMatrixNumber: case kViewMatrixNumber: case kProjMatrixNumber: case kTgenMatrixNumber: case kTgenMatrixData:
        break;
    case kWorldMatrixData:
        matrix_dirty_ = true;
        fog_dirty_ = true;
        break;
    case kViewMatrixData:
        matrix_dirty_ = true;
        light_dirty_ = true; // the specular viewer direction follows the view matrix
        tex_dirty_ = true; // shade mapping's specular light vectors use it too
        fog_dirty_ = true;
        break;
    case kProjMatrixData:
        matrix_dirty_ = true;
        break;
    default:
        if (cmd < kMorphWeight0 || cmd >= kMorphWeight0 + 8u) state_dirty_ = true;
        if (cmd == kFogEnd || cmd == kFogSlope) fog_dirty_ = true;
        // Light colors occupy 0x8F..0x9A, after the position/attenuation registers.
        // A color-only change must refresh the cached per-vertex lighting too.
        if ((cmd >= kLightEnable0 && cmd <= kLightEnable0 + 3u) ||
            (cmd >= kMaterialUpdate && cmd <= kLightColor0 + 11u)) light_dirty_ = true;
        if ((cmd >= kTexScaleU && cmd <= kTexOffsetV) || cmd == kTexSize0 ||
            cmd == kTexMapMode || cmd == kTexShade ||
            (cmd >= kLightType0 && cmd < kLightPos0 + 12u)) tex_dirty_ = true;
        break;
    }
    if (cmd == kLoadClut) load_clut();
    else if (cmd == kTransferStart) block_transfer();
}

// ---------------------------------------------------------------------------
// Vertices

void Ge::compute_layout() {
    const std::uint32_t vt = regs_[kVertexType];
    Layout l;
    l.tc = vt & 3u;
    l.col = (vt >> 2u) & 7u;
    l.nrm = (vt >> 5u) & 3u;
    l.pos = (vt >> 7u) & 3u;
    l.weight = (vt >> 9u) & 3u;
    l.idx = (vt >> 11u) & 3u;
    l.weights = ((vt >> 14u) & 7u) + 1u;
    l.morphs = ((vt >> 18u) & 7u) + 1u;
    l.through = (vt & 0x800000u) != 0u;
    static constexpr std::uint32_t comp[4] = {0, 1, 2, 4};
    static constexpr std::uint32_t col_size[8] = {0, 0, 0, 0, 2, 2, 2, 4};
    std::uint32_t off = 0, biggest = 1;
    auto place = [&](std::uint32_t align, std::uint32_t bytes) {
        if (align == 0u) return off;
        off = (off + align - 1u) & ~(align - 1u);
        const std::uint32_t at = off;
        off += bytes;
        biggest = std::max(biggest, align);
        return at;
    };
    l.weight_off = place(comp[l.weight], comp[l.weight] * l.weights);
    l.tc_off = place(comp[l.tc], comp[l.tc] * 2u);
    l.col_off = place(col_size[l.col], col_size[l.col]);
    l.nrm_off = place(comp[l.nrm], comp[l.nrm] * 3u);
    l.pos_off = place(comp[l.pos], comp[l.pos] * 3u);
    l.size = (off + biggest - 1u) & ~(biggest - 1u);
    layout_ = l;
}

void Ge::decode_vertex(std::uint32_t address, ClipVertex &out) const {
    out.sr = out.sg = out.sb = 0.0f;
    out.q = 1.0f;
    out.fog = 255.0f;
    const Layout &l = layout_;
    float pos[3] = {0, 0, 0}, nrm[3] = {0, 0, 1}, uv[2] = {0, 0}, col[4] = {0, 0, 0, 0};
    float weights[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    bool has_color = l.col >= 4u;

    // Common case (no morphing, vertex data directly addressable): one switch
    // per attribute instead of one per component.
    const std::uint32_t passes = vertex_raw_ != nullptr && l.morphs == 1u ? 0u : l.morphs;
    if (passes == 0u) {
        const std::uint8_t *p = vertex_raw_ + (address - vertex_raw_base_);
        auto load = [](const std::uint8_t *q, std::uint32_t type, bool sign, float scale, float *dst, std::uint32_t n) {
            switch (type) {
            case 1:
                for (std::uint32_t i = 0; i < n; ++i)
                    dst[i] = (sign ? static_cast<float>(static_cast<std::int8_t>(q[i])) : static_cast<float>(q[i])) * scale;
                break;
            case 2:
                for (std::uint32_t i = 0; i < n; ++i) {
                    std::uint16_t v;
                    std::memcpy(&v, q + i * 2u, 2u);
                    dst[i] = (sign ? static_cast<float>(static_cast<std::int16_t>(v)) : static_cast<float>(v)) * scale;
                }
                break;
            case 3: std::memcpy(dst, q, n * 4u); break;
            default: break;
            }
        };
        if (l.weight != 0u) load(p + l.weight_off, l.weight, false, l.weight == 1u ? 1.0f / 128.0f : 1.0f / 32768.0f, weights, l.weights);
        if (l.tc != 0u) load(p + l.tc_off, l.tc, false, l.through ? 1.0f : (l.tc == 1u ? 1.0f / 128.0f : 1.0f / 32768.0f), uv, 2u);
        if (has_color) {
            std::uint32_t rgba;
            if (l.col == 7u) {
                std::memcpy(&rgba, p + l.col_off, 4u);
            } else {
                std::uint16_t packed;
                std::memcpy(&packed, p + l.col_off, 2u);
                rgba = decode16(packed, l.col - 4u);
            }
            col[0] = static_cast<float>(rgba & 0xFFu);
            col[1] = static_cast<float>((rgba >> 8u) & 0xFFu);
            col[2] = static_cast<float>((rgba >> 16u) & 0xFFu);
            col[3] = static_cast<float>(rgba >> 24u);
        }
        if (l.nrm != 0u) load(p + l.nrm_off, l.nrm, true, l.nrm == 1u ? 1.0f / 128.0f : 1.0f / 32768.0f, nrm, 3u);
        if (l.pos != 0u) {
            if (!l.through) {
                load(p + l.pos_off, l.pos, true, l.pos == 1u ? 1.0f / 128.0f : 1.0f / 32768.0f, pos, 3u);
            } else {
                load(p + l.pos_off, l.pos, true, 1.0f, pos, 2u);
                load(p + l.pos_off + 2u * (l.pos == 3u ? 4u : l.pos), l.pos, l.pos != 2u, 1.0f, pos + 2, 1u);
            }
        }
    }

    for (std::uint32_t m = 0; m < passes; ++m) {
        const std::uint32_t base = address + m * l.size;
        const float mw = l.morphs > 1u ? morph_weights_[m] : 1.0f;
        auto read = [&](std::uint32_t at, std::uint32_t type, bool sign, float scale) -> float {
            switch (type) {
            case 1: {
                const std::uint8_t v8 = vertex_raw_ != nullptr ? vertex_raw_[at - vertex_raw_base_] : memory_.load8(at);
                return sign ? static_cast<float>(static_cast<std::int8_t>(v8)) * scale : static_cast<float>(v8) * scale;
            }
            case 2: {
                std::uint16_t v16;
                if (vertex_raw_ != nullptr) std::memcpy(&v16, vertex_raw_ + (at - vertex_raw_base_), 2u);
                else v16 = memory_.load16(at);
                return sign ? static_cast<float>(static_cast<std::int16_t>(v16)) * scale : static_cast<float>(v16) * scale;
            }
            case 3: {
                std::uint32_t v32;
                if (vertex_raw_ != nullptr) std::memcpy(&v32, vertex_raw_ + (at - vertex_raw_base_), 4u);
                else v32 = memory_.load32(at);
                return std::bit_cast<float>(v32);
            }
            default: return 0.0f;
            }
        };
        static constexpr std::uint32_t comp[4] = {0, 1, 2, 4};
        if (l.weight != 0u) {
            const float scale = l.weight == 1u ? 1.0f / 128.0f : 1.0f / 32768.0f;
            for (std::uint32_t i = 0; i < l.weights; ++i)
                weights[i] += mw * read(base + l.weight_off + i * comp[l.weight], l.weight, false, scale);
        }
        if (l.tc != 0u) {
            const float scale = l.through ? 1.0f : (l.tc == 1u ? 1.0f / 128.0f : 1.0f / 32768.0f);
            for (std::uint32_t i = 0; i < 2u; ++i)
                uv[i] += mw * read(base + l.tc_off + i * comp[l.tc], l.tc, false, scale);
        }
        if (has_color) {
            std::uint32_t rgba;
            const std::uint32_t at = base + l.col_off;
            if (l.col == 7u) {
                if (vertex_raw_ != nullptr) std::memcpy(&rgba, vertex_raw_ + (at - vertex_raw_base_), 4u);
                else rgba = memory_.load32(at);
            } else {
                std::uint16_t packed;
                if (vertex_raw_ != nullptr) std::memcpy(&packed, vertex_raw_ + (at - vertex_raw_base_), 2u);
                else packed = memory_.load16(at);
                rgba = decode16(packed, l.col - 4u);
            }
            col[0] += mw * static_cast<float>(rgba & 0xFFu);
            col[1] += mw * static_cast<float>((rgba >> 8u) & 0xFFu);
            col[2] += mw * static_cast<float>((rgba >> 16u) & 0xFFu);
            col[3] += mw * static_cast<float>(rgba >> 24u);
        }
        if (l.nrm != 0u) {
            const float scale = l.nrm == 1u ? 1.0f / 128.0f : 1.0f / 32768.0f;
            for (std::uint32_t i = 0; i < 3u; ++i) {
                const float value = read(base + l.nrm_off + i * comp[l.nrm], l.nrm, true, scale);
                nrm[i] = (m == 0u ? 0.0f : nrm[i]) + mw * value;
            }
        }
        if (l.pos != 0u) {
            for (std::uint32_t i = 0; i < 3u; ++i) {
                float value;
                if (l.through) {
                    const bool z_unsigned = i == 2u && l.pos == 2u;
                    value = read(base + l.pos_off + i * comp[l.pos], l.pos, !z_unsigned, 1.0f);
                } else {
                    const float scale = l.pos == 1u ? 1.0f / 128.0f : 1.0f / 32768.0f;
                    value = read(base + l.pos_off + i * comp[l.pos], l.pos, true, scale);
                }
                pos[i] += mw * value;
            }
        }
    }

    if (!has_color) {
        float material[3];
        color24(regs_[kMaterialAmbient], material);
        col[0] = material[0];
        col[1] = material[1];
        col[2] = material[2];
        col[3] = static_cast<float>(regs_[kMaterialAlpha] & 0xFFu);
    }

    if (l.through) {
        out = ClipVertex{pos[0], pos[1], pos[2], 1.0f, uv[0], uv[1], col[0], col[1], col[2], col[3]};
        return;
    }

    if ((regs_[kReverseNormals] & 1u) != 0u) for (float &component : nrm) component = -component;
    float model[3] = {pos[0], pos[1], pos[2]};
    float normal[3] = {nrm[0], nrm[1], nrm[2]};
    if (l.weight != 0u) {
        float skinned[3] = {0, 0, 0}, skinned_n[3] = {0, 0, 0};
        for (std::uint32_t i = 0; i < l.weights; ++i) {
            if (weights[i] == 0.0f) continue;
            float p[3], n[3];
            transform43(bones_[i], pos, p);
            rotate43(bones_[i], nrm, n);
            for (int k = 0; k < 3; ++k) {
                skinned[k] += weights[i] * p[k];
                skinned_n[k] += weights[i] * n[k];
            }
        }
        std::memcpy(model, skinned, sizeof model);
        std::memcpy(normal, skinned_n, sizeof normal);
    }

    if (ps_.fog) {
        const float eye_z = model[0] * fog_row_[0] + model[1] * fog_row_[1] + model[2] * fog_row_[2] + fog_row_[3];
        out.fog = fog_factor((eye_z + fog_end_) * fog_slope_);
    }

    if (use_wvp_) {
        out.cx = ge_math::dot({model[0], model[1], model[2], 1}, {wvp_[0], wvp_[4], wvp_[8], wvp_[12]});
        out.cy = ge_math::dot({model[0], model[1], model[2], 1}, {wvp_[1], wvp_[5], wvp_[9], wvp_[13]});
        out.cz = ge_math::dot({model[0], model[1], model[2], 1}, {wvp_[2], wvp_[6], wvp_[10], wvp_[14]});
        out.cw = ge_math::dot({model[0], model[1], model[2], 1}, {wvp_[3], wvp_[7], wvp_[11], wvp_[15]});
        out.r = col[0];
        out.g = col[1];
        out.b = col[2];
        out.a = col[3];
        out.u = ge_math::scale_offset(uv[0], tex_.su, tex_.ou) * tex_.tw;
        out.v = ge_math::scale_offset(uv[1], tex_.sv, tex_.ov) * tex_.th;
        return;
    }

    // World-space position is needed for lighting. Clip coordinates always use
    // the model position and the GE-composed matrix, including lit/skinned draws.
    const bool lighting = (regs_[kLightingEnable] & 1u) != 0u;
    const std::uint32_t mapping = regs_[kTexMapMode] & 3u;
    float world[3], world_n[3] = {0, 0, 1};
    transform43(world_, model, world);
    if (lighting || mapping == 2u) rotate43(world_, normal, world_n);
    out.cx = ge_math::dot({model[0], model[1], model[2], 1}, {wvp_[0], wvp_[4], wvp_[8], wvp_[12]});
    out.cy = ge_math::dot({model[0], model[1], model[2], 1}, {wvp_[1], wvp_[5], wvp_[9], wvp_[13]});
    out.cz = ge_math::dot({model[0], model[1], model[2], 1}, {wvp_[2], wvp_[6], wvp_[10], wvp_[14]});
    out.cw = ge_math::dot({model[0], model[1], model[2], 1}, {wvp_[3], wvp_[7], wvp_[11], wvp_[15]});

    if (lighting) {
        float secondary[3]{};
        light_vertex(world, world_n, col, secondary);
        out.sr = secondary[0]; out.sg = secondary[1]; out.sb = secondary[2];
    }
    out.r = col[0];
    out.g = col[1];
    out.b = col[2];
    out.a = col[3];

    // Texture coordinates in texels.
    const float tex_w = tex_.tw, tex_h = tex_.th;
    const std::uint32_t map_mode = regs_[kTexMapMode] & 3u;
    float s = uv[0], t = uv[1];
    if (map_mode == 0u || map_mode == 3u) {
        s = ge_math::scale_offset(s, tex_.su, tex_.ou);
        t = ge_math::scale_offset(t, tex_.sv, tex_.ov);
    } else if (map_mode == 1u) {
        const std::uint32_t source = (regs_[kTexMapMode] >> 8u) & 3u;
        float in[3] = {model[0], model[1], model[2]};
        if (source == 1u) { in[0] = uv[0]; in[1] = uv[1]; in[2] = 0.0f; }
        else if (source >= 2u) {
            in[0] = normal[0]; in[1] = normal[1]; in[2] = normal[2];
            if (source == 2u) normalize3(in);
        }
        float projected[3];
        transform43(tgen_, in, projected);
        // Keep homogeneous texture coordinates through clipping and
        // perspective interpolation; s/q and t/q are per-fragment operations.
        s = projected[0];
        t = projected[1];
        out.q = projected[2];
    } else {
        normalize3(world_n);
        auto shade = [&](const TexSetup::ShadeLight &light) {
            float direction[3];
            for (unsigned k = 0; k < 3; ++k)
                direction[k] = light.pos[k] - (light.directional ? 0.0f : world[k]);
            normalize3(direction);
            if (light.specular) {
                for (unsigned k = 0; k < 3; ++k) direction[k] += tex_.view_dir[k];
                normalize3(direction);
            }
            const float dot = direction[0] * world_n[0] + direction[1] * world_n[1] + direction[2] * world_n[2];
            return (dot + 1.0f) * 0.5f;
        };
        s = shade(tex_.shade[0]);
        t = shade(tex_.shade[1]);
    }
    out.u = s * tex_w;
    out.v = t * tex_h;
}

void Ge::build_light_setup() {
    light_dirty_ = false;
    LightSetup &l = light_;
    l.update = regs_[kMaterialUpdate];
    color24(regs_[kMaterialAmbient], l.mat_ambient);
    color24(regs_[kMaterialDiffuse], l.mat_diffuse);
    color24(regs_[kMaterialSpecular], l.mat_specular);
    color24(regs_[kMaterialEmissive], l.emissive);
    color24(regs_[kAmbientColor], l.global);
    l.alpha = static_cast<float>(regs_[kMaterialAlpha] & 0xFFu);
    l.global_alpha = static_cast<float>(regs_[kAmbientAlpha] & 0xFFu);
    l.exponent = light_exponent(cmd_float(regs_[kSpecularExponent]));
    l.separate = (regs_[kLightMode] & 1u) != 0u;
    l.view_dir[0] = view_[2]; l.view_dir[1] = view_[5]; l.view_dir[2] = view_[8];
    if (normalize3(l.view_dir) == 0.0f) l.view_dir[2] = 1.0f;
    l.count = 0;
    for (std::uint32_t i = 0; i < 4u; ++i) {
        if ((regs_[kLightEnable0 + i] & 1u) == 0u) continue;
        LightSetup::Light &light = l.lights[l.count++];
        light.type = (regs_[kLightType0 + i] >> 8u) & 3u;
        light.components = regs_[kLightType0 + i] & 3u;
        for (std::uint32_t k = 0; k < 3u; ++k) {
            light.pos[k] = cmd_float(regs_[kLightPos0 + i * 3u + k]);
            light.att[k] = cmd_float(regs_[kLightAtt0 + i * 3u + k]);
            light.direction[k] = cmd_float(regs_[kLightDir0 + i * 3u + k]);
        }
        if (light.type == 0u) { // directional: normalise once
            normalize3(light.pos);
        }
        normalize3(light.direction);
        light.cutoff = cmd_float(regs_[kLightCutoff0 + i]);
        light.exponent = light_exponent(cmd_float(regs_[kLightExponent0 + i]));
        color24(regs_[kLightColor0 + i * 3u], light.ambient);
        color24(regs_[kLightColor0 + i * 3u + 1u], light.diffuse);
        color24(regs_[kLightColor0 + i * 3u + 2u], light.specular);
    }
}

void Ge::build_tex_setup() {
    tex_dirty_ = false;
    tex_.su = cmd_float(regs_[kTexScaleU]);
    tex_.sv = cmd_float(regs_[kTexScaleV]);
    tex_.ou = cmd_float(regs_[kTexOffsetU]);
    tex_.ov = cmd_float(regs_[kTexOffsetV]);
    tex_.tw = static_cast<float>(1u << std::min(regs_[kTexSize0] & 0xFu, 9u));
    tex_.th = static_cast<float>(1u << std::min((regs_[kTexSize0] >> 8u) & 0xFu, 9u));
    if ((regs_[kTexMapMode] & 3u) == 2u) {
        tex_.view_dir[0] = view_[2]; tex_.view_dir[1] = view_[5]; tex_.view_dir[2] = view_[8];
        if (normalize3(tex_.view_dir) == 0.0f) tex_.view_dir[2] = 1.0f;
        for (unsigned axis = 0; axis < 2; ++axis) {
            const unsigned light = (regs_[kTexShade] >> (axis * 8)) & 3u;
            auto &shade = tex_.shade[axis];
            shade.directional = ((regs_[kLightType0 + light] >> 8) & 3u) == 0u;
            shade.specular = (regs_[kLightType0 + light] & 3u) == 1u;
            for (unsigned k = 0; k < 3; ++k) shade.pos[k] = cmd_float(regs_[kLightPos0 + light * 3u + k]);
        }
    }
}

void Ge::light_vertex(const float pos[3], const float normal[3], float color[4], float secondary[3]) const {
    const LightSetup &setup = light_;
    const auto update = layout_.col != 0u ? setup.update : 0u;
    const float *mat_ambient = (update & 1u) != 0u ? color : setup.mat_ambient;
    const float *mat_diffuse = (update & 2u) != 0u ? color : setup.mat_diffuse;
    const float *mat_specular = (update & 4u) != 0u ? color : setup.mat_specular;
    const float alpha = (update & 1u) != 0u ? color[3] : setup.alpha;

    float n[3] = {normal[0], normal[1], normal[2]};
    normalize3(n);
    float out[3];
    for (int k = 0; k < 3; ++k) {
        out[k] = setup.emissive[k] + light_product(setup.global[k], mat_ambient[k]);
        secondary[k] = 0.0f;
    }
    for (int i = 0; i < setup.count; ++i) {
        const LightSetup::Light &light = setup.lights[i];
        float l[3];
        float attenuation = 1.0f;
        if (light.type == 0u) {
            l[0] = light.pos[0]; l[1] = light.pos[1]; l[2] = light.pos[2];
        } else {
            for (int k = 0; k < 3; ++k) l[k] = light.pos[k] - pos[k];
            const float d2 = l[0] * l[0] + l[1] * l[1] + l[2] * l[2];
            const float d = normalize3(l);
            const float denom = light.att[0] + light.att[1] * d + light.att[2] * d2;
            attenuation = denom > 0.0f ? std::min(1.0f / denom, 1.0f) : 0.0f;
            if (d == 0.0f) l[2] = 1.0f;
        }
        float spot = 1.0f;
        if (light.type >= 2u) {
            const float alignment = light.direction[0] * l[0] + light.direction[1] * l[1] + light.direction[2] * l[2];
            spot = alignment >= light.cutoff ? light_power(alignment, light.exponent) : 0.0f;
        }
        auto attenuate = [&](int channel) { return light_scale(light_scale(channel, attenuation), spot); };
        float diffuse = n[0] * l[0] + n[1] * l[1] + n[2] * l[2];
        if (light.components == 2u) diffuse = light_power(diffuse, setup.exponent);
        float specular = 0.0f;
        if (light.components == 1u && diffuse >= 0.0f) {
            float half[3] = {l[0] + setup.view_dir[0], l[1] + setup.view_dir[1], l[2] + setup.view_dir[2]};
            if (normalize3(half) == 0.0f) half[2] = 1.0f;
            specular = light_power(n[0] * half[0] + n[1] * half[1] + n[2] * half[2], setup.exponent);
        }
        for (int k = 0; k < 3; ++k) {
            out[k] += attenuate(light_product(light.ambient[k], mat_ambient[k]));
            out[k] += attenuate(light_scale(light_product(light.diffuse[k], mat_diffuse[k]), diffuse));
            secondary[k] += attenuate(light_scale(light_product(light.specular[k], mat_specular[k]), specular));
        }
    }
    for (int k = 0; k < 3; ++k) {
        color[k] = clamp255(out[k] + (setup.separate ? 0.0f : secondary[k]));
        secondary[k] = setup.separate ? clamp255(secondary[k]) : 0.0f;
    }
    // Like the colour, the alpha is global ambient x material ambient (emissive adds no alpha).
    color[3] = static_cast<float>(light_product(alpha, setup.global_alpha));
}

Ge::Vertex Ge::to_screen(const ClipVertex &cv) const {
    Vertex v;
    if (layout_.through) {
        v.x = cv.cx;
        v.y = cv.cy;
        v.z = cv.cz;
        v.w = 1.0f;
    } else {
        const float inv_w = ge_math::reciprocal(cv.cw);
        const float screen_x = ge_math::viewport(cv.cx, cv.cw, viewport_[0], viewport_[3], inv_w);
        const float screen_y = ge_math::viewport(cv.cy, cv.cw, viewport_[1], viewport_[4], inv_w);
        // The raw screen center participates in the GE adder. Only then does
        // the integer drawing offset move the 12.4 coordinates to the target.
        v.x = (std::floor(screen_x * 16.0f) - float(regs_[kOffsetX] & 0xFFFFu)) / 16.0f;
        v.y = (std::floor(screen_y * 16.0f) - float(regs_[kOffsetY] & 0xFFFFu)) / 16.0f;
        v.z = std::floor(ge_math::viewport(cv.cz, cv.cw, viewport_[2], viewport_[5], inv_w));
        v.w = cv.cw;
    }
    v.u = cv.u;
    v.v = cv.v;
    v.q = cv.q;
    v.fog = std::floor(cv.fog); // intersections are quantized before rasterization too
    v.r = cv.r;
    v.g = cv.g;
    v.b = cv.b;
    v.a = cv.a;
    v.sr = cv.sr; v.sg = cv.sg; v.sb = cv.sb;
    return v;
}

Ge::Vertex Ge::to_raster(const ClipVertex &cv) const {
    Vertex v = to_screen(cv);
    // Transformed coordinates already passed the viewport grid. Through
    // coordinates truncate toward zero directly, preserving their bypass.
    v.x = std::trunc(v.x * 16.0f) / 16.0f;
    v.y = std::trunc(v.y * 16.0f) / 16.0f;
    return v;
}

// ---------------------------------------------------------------------------
// Primitives

void Ge::draw_primitive(std::uint32_t type, std::uint32_t count) {
    ++stats_.prims;
    compute_layout();
    viewport_[0] = cmd_float(regs_[kViewportXScale]);
    viewport_[1] = cmd_float(regs_[kViewportYScale]);
    viewport_[2] = cmd_float(regs_[kViewportZScale]);
    viewport_[3] = cmd_float(regs_[kViewportXCenter]);
    viewport_[4] = cmd_float(regs_[kViewportYCenter]);
    viewport_[5] = cmd_float(regs_[kViewportZCenter]);
    if (tex_dirty_) build_tex_setup();
    if (light_dirty_ && (regs_[kLightingEnable] & 1u) != 0u) build_light_setup();
    use_wvp_ = !layout_.through && layout_.weight == 0u && (regs_[kLightingEnable] & 1u) == 0u &&
               (regs_[kTexMapMode] & 3u) == 0u;
    // Column-major 4x4: W and V are 4x3 with an implicit (0, 0, 0, 1) row.
    auto expand = [](const std::array<float, 12> &m) {
        return std::array<float, 16>{m[0], m[1], m[2], 0, m[3], m[4], m[5], 0, m[6], m[7], m[8], 0, m[9], m[10], m[11], 1};
    };
    auto multiply = [](const std::array<float, 16> &a, const std::array<float, 16> &b) {
        std::array<float, 16> r{};
        for (int c = 0; c < 4; ++c)
            for (int row = 0; row < 4; ++row)
                r[c * 4 + row] = ge_math::dot({a[row], a[4 + row], a[8 + row], a[12 + row]},
                    {b[c * 4], b[c * 4 + 1], b[c * 4 + 2], b[c * 4 + 3]});
        return r;
    };
    if (!layout_.through && matrix_dirty_) {
        matrix_dirty_ = false;
        // GE combines world/view first; float24 truncation makes grouping visible.
        wvp_ = multiply(proj_, multiply(expand(view_), expand(world_)));
    }
    if (gl_ == nullptr) {
        prepare_pixel_state();
        bind_texture();
    } else if (state_dirty_ || layout_.through != submitted_through_) {
        // Through mode decides fog, so switching it needs fresh state too.
        prepare_pixel_state();
        submit_gl_state();
        state_dirty_ = false;
        submitted_through_ = layout_.through;
    }
    if (ps_.fog && fog_dirty_) {
        // Eye-space z = row 2 of view * world. Cache it independently of the
        // projection, and apply it after bone skinning like the other transforms.
        for (unsigned c = 0; c < 3; ++c)
            fog_row_[c] = view_[2] * world_[c * 3] + view_[5] * world_[c * 3 + 1] + view_[8] * world_[c * 3 + 2];
        fog_row_[3] = view_[2] * world_[9] + view_[5] * world_[10] + view_[8] * world_[11] + view_[11];
        fog_end_ = cmd_float(regs_[kFogEnd]);
        fog_slope_ = cmd_float(regs_[kFogSlope]);
        fog_dirty_ = false;
    }
    // Software drawing writes VRAM, which may hold textures; GL drawing does not.
    struct GenerationBump {
        std::uint64_t &generation;
        bool active;
        ~GenerationBump() { if (active) ++generation; }
    } bump{vram_generation_, gl_ == nullptr};
    const Layout &l = layout_;
    if (scratch_vertices_.size() < count) scratch_vertices_.resize(count);
    std::vector<ClipVertex> &vertices = scratch_vertices_;
    const std::uint32_t index_size = l.idx == 1u ? 1u : (l.idx == 2u ? 2u : 0u);
    SectionTimer decode_timer{0};
    const std::uint32_t stride = l.size * l.morphs;
    if (scratch_indices_.size() < count) scratch_indices_.resize(count);
    std::vector<std::uint32_t> &indices = scratch_indices_;
    const std::uint8_t *index_raw = index_size != 0u ? memory_.raw_pointer(index_address_, count * index_size) : nullptr;
    std::uint32_t max_index = count == 0u ? 0u : count - 1u, min_index = 0u;
    if (index_size != 0u) {
        max_index = 0u;
        min_index = ~0u;
        for (std::uint32_t i = 0; i < count; ++i) {
            std::uint32_t index;
            if (index_size == 1u) index = index_raw != nullptr ? index_raw[i] : memory_.load8(index_address_ + i);
            else if (index_raw != nullptr) {
                std::uint16_t v16;
                std::memcpy(&v16, index_raw + i * 2u, 2u);
                index = v16;
            } else {
                index = memory_.load16(index_address_ + i * 2u);
            }
            indices[i] = index;
            max_index = std::max(max_index, index);
            min_index = std::min(min_index, index);
        }
    } else {
        for (std::uint32_t i = 0; i < count; ++i) indices[i] = i;
    }
    // Indexed draws share vertices: decode each one in the used range once and
    // address it through `indices` (unless the range is much larger than the draw).
    const bool shared = index_size != 0u && count != 0u && max_index - min_index < count;
    const std::uint32_t first = shared ? min_index : 0u;
    const std::uint32_t decoded = shared ? max_index - min_index + 1u : count;
    if (scratch_vertices_.size() < decoded) scratch_vertices_.resize(decoded);
    const std::uint32_t decode_base = vertex_address_ + first * stride;
    vertex_raw_base_ = decode_base;
    vertex_raw_ = count != 0u ? memory_.raw_pointer(decode_base, (max_index + 1u - first) * stride) : nullptr;
    if (shared) {
        for (std::uint32_t i = 0; i < count; ++i) indices[i] -= min_index;
        for (std::uint32_t i = 0; i < decoded; ++i) decode_vertex(decode_base + i * stride, vertices[i]);
    } else {
        // Plain or sparsely indexed: one decode per position.
        for (std::uint32_t i = 0; i < count; ++i) {
            decode_vertex(decode_base + indices[i] * stride, vertices[i]);
            indices[i] = i;
        }
    }
    vertex_raw_ = nullptr;
    if (index_size != 0u) index_address_ += count * index_size;
    else vertex_address_ += count * l.size * l.morphs;
    g_section_ms[0] += 0.0; // decode timer ends with scope below
    const std::uint32_t prim_index = prim_in_frame_++;
    if (frame_ == g_dump_frame) log_primitive(prim_index, type, count);
    if (!g_skip_prims.empty() && skipped_prim(prim_index)) return;
    SectionTimer emit_timer{1};

    const std::uint64_t pixels_before = stats_.pixels;
    struct HeavyDrawReport {
        Ge &ge;
        std::uint64_t before;
        std::uint32_t type, count;
        const std::vector<ClipVertex> &vertices;
        ~HeavyDrawReport() {
            if (!g_profile_targets || ge.frame_ < 900u) return;
            const std::uint64_t drawn = ge.stats_.pixels - before;
            if (drawn < 100000u) return;
            std::cerr << "[ge-heavy] frame=" << ge.frame_ << " type=" << type << " count=" << count << " pixels=" << drawn
                      << (ge.layout_.through ? " through" : " 3d") << " vtype=0x" << std::hex << ge.regs_[kVertexType]
                      << std::dec;
            for (std::size_t i = 0; i < std::min<std::size_t>(vertices.size(), 4u); ++i) {
                const Vertex sv = ge.to_screen(vertices[i]);
                std::cerr << " v" << i << "=(" << sv.x << "," << sv.y << "," << sv.z << " w=" << vertices[i].cw << ")";
            }
            std::cerr << "\n";
        }
    } heavy{*this, pixels_before, type, count, vertices};

    if (gl_ != nullptr && type >= 3u && type <= 5u) {
        // Convert each vertex to drawing space once; triangles fully in front of
        // the near plane are appended directly, the rest go through clipping.
        if (scratch_screen_.size() < decoded) {
            scratch_screen_.resize(decoded);
            scratch_inside_.resize(decoded);
        }
        constexpr float kEpsilon = 1e-4f;
        for (std::uint32_t i = 0; i < decoded; ++i) {
            const ClipVertex &cv = vertices[i];
            const bool inside = layout_.through || (cv.cz + cv.cw >= 0.0f && cv.cw >= kEpsilon);
            scratch_inside_[i] = inside ? 1u : 0u;
            if (!inside) continue;
            const Vertex v = to_raster(cv);
            scratch_screen_[i] = GlVertex{v.x, v.y, v.z, v.w, v.u, v.v,
                                          static_cast<std::uint8_t>(clamp255(v.r)), static_cast<std::uint8_t>(clamp255(v.g)),
                                          static_cast<std::uint8_t>(clamp255(v.b)), static_cast<std::uint8_t>(clamp255(v.a)),
                                          static_cast<std::uint8_t>(clamp255(v.sr)), static_cast<std::uint8_t>(clamp255(v.sg)),
                                          static_cast<std::uint8_t>(clamp255(v.sb)), 0, v.q, v.fog};
        }
        const bool flat = (regs_[kShadeMode] & 1u) == 0u;
        // Backface culling (3D only), decided on the screen-space winding.  A
        // zero-area triangle (common in strips) covers no pixels either way.
        // In drawing space (y down) a positive area means clockwise; CULL bit 0 clear
        // removes clockwise triangles (checked against frames: the other way empties the scene).
        const bool cull = !layout_.through && (regs_[kCullEnable] & 1u) != 0u;
        const bool cull_clockwise = (regs_[kCull] & 1u) == 0u;
        // Each decoded vertex enters the GL batch once, the first time a kept triangle uses it.
        if (scratch_batch_index_.size() < decoded) scratch_batch_index_.resize(decoded);
        std::fill(scratch_batch_index_.begin(), scratch_batch_index_.begin() + decoded, ~0u);
        auto batch_index = [&](std::uint32_t i) {
            std::uint32_t &slot = scratch_batch_index_[i];
            if (slot == ~0u) slot = gl_->vertex(scratch_screen_[i]);
            return slot;
        };
        const std::uint32_t *at = indices.data();
        auto triangle = [&](std::uint32_t pa, std::uint32_t pb, std::uint32_t pc) {
            const std::uint32_t a = at[pa], b = at[pb], c = at[pc];
            if (scratch_inside_[a] == 0u || scratch_inside_[b] == 0u || scratch_inside_[c] == 0u) {
                draw_triangle(vertices[a], vertices[b], vertices[c]);
                return;
            }
            {
                const GlVertex &p = scratch_screen_[a], &q = scratch_screen_[b], &r = scratch_screen_[c];
                const float area = (q.x - p.x) * (r.y - p.y) - (r.x - p.x) * (q.y - p.y);
                if (area == 0.0f || (cull && (area > 0.0f) == cull_clockwise)) {
                    ++stats_.culled;
                    return;
                }
            }
            ++stats_.triangles;
            if (!flat) {
                gl_->indexed_triangle(batch_index(a), batch_index(b), batch_index(c));
                return;
            }
            GlVertex va = scratch_screen_[a], vb = scratch_screen_[b];
            const GlVertex &vc = scratch_screen_[c];
            va.r = vb.r = vc.r;
            va.g = vb.g = vc.g;
            va.b = vb.b = vc.b;
            va.a = vb.a = vc.a;
            va.sr = vb.sr = vc.sr;
            va.sg = vb.sg = vc.sg;
            va.sb = vb.sb = vc.sb;
            gl_->triangle(va, vb, vc);
        };
        if (type == 3u) {
            for (std::uint32_t i = 0; i + 2u < count; i += 3u) triangle(i, i + 1u, i + 2u);
        } else if (type == 4u) {
            for (std::uint32_t i = 2u; i < count;) {
                if ((count & 1u)==0u && (i & 1u)==0u && i+1u<count &&
                    try_rectangle(vertices[at[i-2u]],vertices[at[i-1u]],vertices[at[i]],vertices[at[i+1u]],true)) {
                    i+=2u;
                    continue;
                }
                if ((i & 1u) == 0u) triangle(i - 2u, i - 1u, i);
                else triangle(i - 1u, i - 2u, i);
                ++i;
            }
        } else {
            if (count!=4u || !try_rectangle(vertices[at[0]],vertices[at[1]],vertices[at[2]],vertices[at[3]],false))
                for (std::uint32_t i = 2u; i < count; ++i) triangle(0u, i - 1u, i);
        }
        return;
    }

    auto vert = [&](std::uint32_t i) -> const ClipVertex & { return vertices[indices[i]]; };
    switch (type) {
    case 1: // independent line pairs; an unmatched final vertex is ignored
        for (std::uint32_t i = 0; i + 1u < count; i += 2u) draw_line(vert(i), vert(i + 1u));
        break;
    case 2: // connected line strip
        for (std::uint32_t i = 1u; i < count; ++i) draw_line(vert(i - 1u), vert(i));
        break;
    case 3: // triangles
        for (std::uint32_t i = 0; i + 2u < count; i += 3u) draw_triangle(vert(i), vert(i + 1u), vert(i + 2u));
        break;
    case 4: // strip
        for (std::uint32_t i = 2u; i < count;) {
            if ((count & 1u)==0u && (i & 1u)==0u && i+1u<count &&
                try_rectangle(vert(i-2u),vert(i-1u),vert(i),vert(i+1u),true)) {
                i+=2u;
                continue;
            }
            if ((i & 1u) == 0u) draw_triangle(vert(i - 2u), vert(i - 1u), vert(i));
            else draw_triangle(vert(i - 1u), vert(i - 2u), vert(i));
            ++i;
        }
        break;
    case 5: // fan
        if (count!=4u || !try_rectangle(vert(0),vert(1),vert(2),vert(3),false))
            for (std::uint32_t i = 2u; i < count; ++i) draw_triangle(vert(0), vert(i - 1u), vert(i));
        break;
    case 6: // sprites
        for (std::uint32_t i = 0; i + 1u < count; i += 2u) emit_sprite(to_screen(vert(i)), to_screen(vert(i + 1u)));
        break;
    case 0: // points
        for (std::uint32_t i = 0; i < count; ++i) {
            Vertex v = to_screen(vert(i)), w = v;
            w.x += 1.0f;
            w.y += 1.0f;
            emit_sprite(v, w);
        }
        break;
    default:
        break;
    }
}

bool Ge::try_rectangle(const ClipVertex &a, const ClipVertex &b, const ClipVertex &c,
                       const ClipVertex &d, bool strip) {
    if ((regs_[kCullEnable] & 1u) != 0u || ps_.clear) return false;
    const std::array<const ClipVertex *,4> input{&a,&b,&c,&d};
    std::array<Vertex,4> v{};
    for (unsigned i = 0; i < 4; ++i) {
        const ClipVertex &cv = *input[i];
        if (!layout_.through && (!(cv.cw >= 1e-4f) || cv.cz < -cv.cw)) return false;
        v[i] = to_raster(cv);
        const Vertex &p = v[0], &q = v[i];
        if (!std::isfinite(q.x) || !std::isfinite(q.y) || !std::isfinite(q.z)) return false;
        if (p.r != q.r || p.g != q.g || p.b != q.b || p.a != q.a ||
            p.sr != q.sr || p.sg != q.sg || p.sb != q.sb) return false;
        if ((ps_.z_write || ps_.z_test) && p.z != q.z) return false;
        if (ps_.texture && (p.w != q.w || p.q != q.q || p.q != 1.0f)) return false;
        if (ps_.fog && p.fog != q.fog) return false;
    }
    float x0 = v[0].x, x1 = x0, y0 = v[0].y, y1 = y0;
    for (const Vertex &p : v) {
        x0 = std::min(x0,p.x); x1 = std::max(x1,p.x);
        y0 = std::min(y0,p.y); y1 = std::max(y1,p.y);
    }
    if (x0 == x1 || y0 == y1) return false;
    std::array<int,4> corners{-1,-1,-1,-1};
    std::array<unsigned,4> locations{};
    for (unsigned i = 0; i < 4; ++i) {
        if ((v[i].x != x0 && v[i].x != x1) || (v[i].y != y0 && v[i].y != y1)) return false;
        const unsigned corner = (v[i].x == x1 ? 1u : 0u) + (v[i].y == y1 ? 2u : 0u);
        if (corners[corner] != -1) return false;
        corners[corner] = static_cast<int>(i); locations[i] = corner;
    }
    // The shared edge must be a diagonal: a bow-tie strip or fan is not a
    // rectangle even when its four positions are the four unique corners.
    if ((strip ? locations[1] ^ locations[2] : locations[0] ^ locations[2]) != 3u) return false;
    const Vertex &tl = v[corners[0]], &tr = v[corners[1]], &bl = v[corners[2]], &br = v[corners[3]];
    if (ps_.texture) {
        if (tl.u != bl.u || tr.u != br.u || tl.v != tr.v || bl.v != br.v) return false;
        if (!strip && (!(tl.u < br.u) || !(tl.v < br.v))) return false;
    }
    // Triangles have distinct UV planes. Keep scaled, filtered and transformed
    // textures as triangles even when their corner attributes form a rectangle.
    if (ps_.texture && (!layout_.through || (regs_[0xC6] & 0x101u) != 0u ||
        std::abs(br.u-tl.u) != x1-x0 || std::abs(br.v-tl.v) != y1-y0)) return false;
    if (x0-std::floor(x0) == 9.0f/16.0f) return false;
    emit_sprite(tl,br);
    return true;
}

void Ge::draw_triangle(const ClipVertex &a, const ClipVertex &b, const ClipVertex &c) {
    if (layout_.through) {
        emit_triangle(to_raster(a), to_raster(b), to_raster(c));
        return;
    }
    // Clip against the near plane (z >= -w) and w > epsilon.
    std::array<ClipVertex, 8> in{a, b, c}, out{};
    std::size_t in_count = 3, out_count = 0;
    constexpr float kEpsilon = 1e-4f;
    auto distance = [](const ClipVertex &v) { return std::min(v.cz + v.cw, v.cw - kEpsilon); };
    if (distance(a) >= 0.0f && distance(b) >= 0.0f && distance(c) >= 0.0f) {
        emit_triangle(to_raster(a), to_raster(b), to_raster(c));
        return;
    }
    if ((regs_[kShadeMode] & 1u) == 0u) {
        // The provoking vertex is the original final vertex, even when the
        // near plane removes it. Give every generated vertex that same color.
        for (std::size_t i = 0; i < in_count; ++i) {
            in[i].r = c.r; in[i].g = c.g; in[i].b = c.b; in[i].a = c.a;
            in[i].sr = c.sr; in[i].sg = c.sg; in[i].sb = c.sb;
        }
    }
    for (std::size_t i = 0; i < in_count; ++i) {
        const ClipVertex &p = in[i], &q = in[(i + 1u) % in_count];
        const float dp = distance(p), dq = distance(q);
        if (dp >= 0.0f) out[out_count++] = p;
        if ((dp >= 0.0f) != (dq >= 0.0f)) {
            const float t = dp / (dp - dq);
            auto lerp = [t](float x, float y) { return x + (y - x) * t; };
            out[out_count++] = ClipVertex{lerp(p.cx, q.cx), lerp(p.cy, q.cy), lerp(p.cz, q.cz), lerp(p.cw, q.cw),
                                          lerp(p.u, q.u), lerp(p.v, q.v), lerp(p.r, q.r), lerp(p.g, q.g),
                                          lerp(p.b, q.b), lerp(p.a, q.a),
                                          lerp(p.sr, q.sr), lerp(p.sg, q.sg), lerp(p.sb, q.sb), lerp(p.q, q.q),
                                          lerp(p.fog, q.fog)};
        }
    }
    if (out_count < 3u) return;
    const Vertex first = to_raster(out[0]);
    for (std::size_t i = 1; i + 1u < out_count; ++i) emit_triangle(first, to_raster(out[i]), to_raster(out[i + 1u]));
}

void Ge::draw_line(const ClipVertex &first, const ClipVertex &last) {
    ClipVertex a = first, b = last;
    if (!layout_.through) {
        const bool depth_clamp = (regs_[0x1C] & 1u) != 0u;
        if ((!depth_clamp && (a.cz < -a.cw || a.cz > a.cw || b.cz < -b.cw || b.cz > b.cw)) ||
            (a.cz < -a.cw && b.cz < -b.cw) || (a.cz > a.cw && b.cz > b.cw)) return;
        for (unsigned plane = 0; plane < 2; ++plane) {
            const auto distance = [plane](const ClipVertex &v) { return plane == 0 ? v.cz + v.cw : v.cw - 1e-4f; };
            const float da = distance(a), db = distance(b);
            if (da < 0 && db < 0) return;
            if ((da < 0) != (db < 0)) {
                const float t = da / (da - db);
                const auto mix = [t](float x, float y) { return x + (y - x) * t; };
                const ClipVertex v{mix(a.cx,b.cx),mix(a.cy,b.cy),mix(a.cz,b.cz),mix(a.cw,b.cw),
                    mix(a.u,b.u),mix(a.v,b.v),mix(a.r,b.r),mix(a.g,b.g),mix(a.b,b.b),mix(a.a,b.a),
                    mix(a.sr,b.sr),mix(a.sg,b.sg),mix(a.sb,b.sb),mix(a.q,b.q),mix(a.fog,b.fog)};
                if (da < 0) a = v; else b = v;
            }
        }
    }
    const Vertex p = to_screen(a), q = to_screen(b);
    for (const Vertex *v : {&p, &q}) {
        if (!std::isfinite(v->x) || !std::isfinite(v->y) || !std::isfinite(v->z) ||
            std::abs(v->x) > 134217727.0f || std::abs(v->y) > 134217727.0f) return;
        if (!layout_.through) {
            const float sx = v->x + (regs_[kOffsetX] & 0xFFFFu) / 16.0f;
            const float sy = v->y + (regs_[kOffsetY] & 0xFFFFu) / 16.0f;
            if (sx < 0 || sy < 0 || sx >= 4095.96875f || sy >= 4095.96875f) return;
        }
    }
    // Line coverage is stepped on the GE's 12.4 screen grid. The final endpoint
    // is excluded, so a strip junction is drawn once rather than blended twice.
    const double ax = std::trunc(p.x * 16.0), ay = std::trunc(p.y * 16.0);
    const double bx = std::trunc(q.x * 16.0), by = std::trunc(q.y * 16.0);
    double dx = bx - ax, dy = by - ay;
    const auto steps = static_cast<std::int64_t>(std::max(std::abs(dx), std::abs(dy)) / 16.0);
    if (steps == 0) return;
    if (dx < 0 && dx >= -16) dx += 1;
    if (dy < 0 && dy >= -16) dy += 1;
    const double ix = dx / steps, iy = dy / steps;
    const double x0 = ax - (ax > bx ? 1 : 0), y0 = ay - (ay > by ? 1 : 0);
    double lo = 0, hi = static_cast<double>(steps - 1);
    const auto clip = [&](double start, double inc, int low, int high) {
        if (low > high) return false;
        const double lower = low * 16.0, upper = (high + 1.0) * 16.0 - 1.0;
        if (inc == 0) return start >= lower && start <= upper;
        const double s = (lower - start) / inc, t = (upper - start) / inc;
        lo = std::max(lo, std::ceil(std::min(s,t))); hi = std::min(hi, std::floor(std::max(s,t)));
        return lo <= hi;
    };
    if (!clip(x0,ix,std::max(ps_.sx1,0),std::min(ps_.sx2,1023)) ||
        !clip(y0,iy,std::max(ps_.sy1,0),std::min(ps_.sy2,1023))) return;
    const auto begin = static_cast<std::int64_t>(lo), end = static_cast<std::int64_t>(hi);
    const bool smooth = (regs_[kShadeMode] & 1u) != 0u;
    const auto uv_at = [&](float t) {
        const float s = 1.0f - t;
        if (layout_.through) return std::array<float,2>{s*p.u+t*q.u,s*p.v+t*q.v};
        if (p.u == q.u && p.v == q.v && p.q == q.q && p.q > 0)
            return std::array<float,2>{p.u/p.q,p.v/p.q};
        const float weight = s*p.q/p.w + t*q.q/q.w;
        if (!(weight > 0)) return std::array<float,2>{tex_.tw-.5f,tex_.th-.5f};
        return std::array<float,2>{(s*p.u/p.w+t*q.u/q.w)/weight,(s*p.v/p.w+t*q.v/q.w)/weight};
    };
    GlVertex run{};
    float run_right=0, run_bottom=0;
    bool have_run=false;
    const auto flush_run = [&]() {
        if (!have_run) return;
        GlVertex right=run, bottom=run, corner=run;
        right.x=corner.x=run_right; bottom.y=corner.y=run_bottom;
        gl_->triangle(run,right,bottom); gl_->triangle(right,corner,bottom);
        have_run=false;
    };
    const auto same_sample = [](const GlVertex &a, const GlVertex &b) {
        return a.z==b.z && a.u==b.u && a.v==b.v && a.r==b.r && a.g==b.g && a.b==b.b && a.a==b.a &&
            a.sr==b.sr && a.sg==b.sg && a.sb==b.sb && a.fog==b.fog && a.lod_footprint==b.lod_footprint;
    };
    const auto texture_address=(regs_[kTexAddr0]&0xFFFFF0u)|((regs_[kTexBufWidth0]&0x0F0000u)<<8u);
    // Retain individual cells for possible direct VRAM surfaces, where merging
    // could change source-feedback snapshots. Indexed formats always use a
    // decoded texture, including palettes/textures stored in VRAM.
    const bool merge_cells=!ps_.texture || (regs_[kTexFormat]&0xFu)>3u ||
        (texture_address&0x3F000000u)!=0x04000000u;
    double x = x0 + begin * ix, y = y0 + begin * iy;
    for (std::int64_t i = begin; i <= end; ++i, x += ix, y += iy) {
        const float t = static_cast<float>(i) / static_cast<float>(steps);
        const auto channel = [&](float av, float bv) {
            av = std::floor(clamp255(av)); bv = std::floor(clamp255(bv));
            return smooth ? static_cast<float>(std::floor((static_cast<double>(av)*(steps-i)+static_cast<double>(bv)*i)/steps)) : bv;
        };
        const float r=channel(p.r,q.r), g=channel(p.g,q.g), blue=channel(p.b,q.b), alpha=channel(p.a,q.a);
        const float sr=channel(p.sr,q.sr), sg=channel(p.sg,q.sg), sb=channel(p.sb,q.sb);
        const float depth = static_cast<float>(std::floor((1.0-t)*std::floor(std::clamp(p.z,0.0f,65535.0f)) +
                                                          t*std::floor(std::clamp(q.z,0.0f,65535.0f))));
        const float fog = std::floor(std::clamp((1.0f-t)*p.fog+t*q.fog,0.0f,255.0f));
        const auto uv = uv_at(t), next = uv_at(static_cast<float>(i+1)/static_cast<float>(steps));
        const float ds = ix == 0 ? 0.0f : static_cast<float>((next[0]-uv[0])*16.0/ix);
        const float dt = iy == 0 ? 0.0f : static_cast<float>((next[1]-uv[1])*16.0/iy);
        const int px = static_cast<int>(x) / 16, py = static_cast<int>(y) / 16;
        if (gl_ == nullptr) shade_pixel(px,py,depth,uv[0],uv[1],r,g,blue,alpha,sr,sg,sb,fog);
        else {
            // A logical pixel cell keeps line coverage and interpolation
            // independent of host GL line-width/diamond-exit rules.
            float footprint=std::max(std::abs(ds),std::abs(dt));
            if ((regs_[0xC8]&3u)==2u) footprint=2.0f*((1.0f-t)*p.w+t*q.w)*cmd_float(regs_[0xD0]);
            else if ((regs_[0xC8]&3u)!=0u) footprint=0;
            // Only exponent/high mantissa bits affect GE LOD. Canonicalizing
            // that key allows equal samples to share one rectangular run.
            if (!(footprint>0)) footprint=0;
            footprint=std::bit_cast<float>(std::bit_cast<std::uint32_t>(footprint)&0xFFF80000u);
            GlVertex v{static_cast<float>(px),static_cast<float>(py),ps_.z_test||ps_.clear?depth:0.0f,1.0f,uv[0],uv[1],
                static_cast<std::uint8_t>(r),static_cast<std::uint8_t>(g),static_cast<std::uint8_t>(blue),
                static_cast<std::uint8_t>(alpha),static_cast<std::uint8_t>(sr),static_cast<std::uint8_t>(sg),
                static_cast<std::uint8_t>(sb),0,1.0f,ps_.fog?fog:255.0f,footprint};
            bool joined=false;
            if (merge_cells && have_run && same_sample(run,v)) {
                if (v.y==run.y && run_bottom==run.y+1) {
                    if (v.x==run_right) { run_right+=1; joined=true; }
                    else if (v.x+1==run.x) { run.x=v.x; joined=true; }
                }
                if (!joined && v.x==run.x && run_right==run.x+1) {
                    if (v.y==run_bottom) { run_bottom+=1; joined=true; }
                    else if (v.y+1==run.y) { run.y=v.y; joined=true; }
                }
            }
            if (!joined) { flush_run(); run=v; run_right=v.x+1; run_bottom=v.y+1; have_run=true; }
        }
    }
    if (gl_ != nullptr) flush_run();
}

void Ge::raster_triangle(const Vertex &a, const Vertex &b, const Vertex &c) {
    const float area = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
    if (area == 0.0f || !std::isfinite(area)) return;
    ++stats_.triangles;

    const int sx1 = ps_.sx1, sy1 = ps_.sy1, sx2 = ps_.sx2, sy2 = ps_.sy2;
    int min_x = static_cast<int>(std::floor(std::min({a.x, b.x, c.x})));
    int max_x = static_cast<int>(std::ceil(std::max({a.x, b.x, c.x})));
    int min_y = static_cast<int>(std::floor(std::min({a.y, b.y, c.y})));
    int max_y = static_cast<int>(std::ceil(std::max({a.y, b.y, c.y})));
    min_x = std::max(min_x, sx1);
    min_y = std::max(min_y, sy1);
    max_x = std::min(max_x, sx2);
    max_y = std::min(max_y, sy2);
    if (min_x > max_x || min_y > max_y) return;

    const float inv_area = 1.0f / area;
    const bool flat = (regs_[kShadeMode] & 1u) == 0u;
    const bool perspective = !layout_.through;
    const bool constant_fog = a.fog == b.fog && a.fog == c.fog;
    const bool constant_color = a.r == b.r && a.r == c.r && a.g == b.g && a.g == c.g &&
                                a.b == b.b && a.b == c.b && a.a == b.a && a.a == c.a &&
                                a.sr == b.sr && a.sr == c.sr && a.sg == b.sg && a.sg == c.sg &&
                                a.sb == b.sb && a.sb == c.sb;
    const float iwa = 1.0f / a.w, iwb = 1.0f / b.w, iwc = 1.0f / c.w;

    for (int y = min_y; y <= max_y; ++y) {
        const float py = static_cast<float>(y) + 0.5f;
        for (int x = min_x; x <= max_x; ++x) {
            const float px = static_cast<float>(x) + 0.5f;
            float w0 = ((b.x - px) * (c.y - py) - (b.y - py) * (c.x - px)) * inv_area;
            float w1 = ((c.x - px) * (a.y - py) - (c.y - py) * (a.x - px)) * inv_area;
            float w2 = 1.0f - w0 - w1;
            if (w0 < 0.0f || w1 < 0.0f || w2 < 0.0f) continue;
            const float z = w0 * a.z + w1 * b.z + w2 * c.z;
            float u, v;
            if (perspective) {
                const float q = w0 * a.q * iwa + w1 * b.q * iwb + w2 * c.q * iwc;
                if (q > 0.0f) {
                    u = (w0 * a.u * iwa + w1 * b.u * iwb + w2 * c.u * iwc) / q;
                    v = (w0 * a.v * iwa + w1 * b.v * iwb + w2 * c.v * iwc) / q;
                } else {
                    // Nonpositive texture q saturates both axes at the last
                    // texel, in wrap as well as clamp mode.
                    u = tex_.tw * (1.0f - 1.0f / 65536.0f);
                    v = tex_.th * (1.0f - 1.0f / 65536.0f);
                }
            } else {
                u = w0 * a.u + w1 * b.u + w2 * c.u;
                v = w0 * a.v + w1 * b.v + w2 * c.v;
            }
            // Keep a constant factor exact, especially 255: floating sums of
            // barycentric weights can otherwise truncate it to 254.
            const float fog = constant_fog ? a.fog : w0 * a.fog + w1 * b.fog + w2 * c.fog;
            if (flat || constant_color) shade_pixel(x, y, z, u, v, c.r, c.g, c.b, c.a, c.sr, c.sg, c.sb, fog);
            else
                shade_pixel(x, y, z, u, v, w0 * a.r + w1 * b.r + w2 * c.r, w0 * a.g + w1 * b.g + w2 * c.g,
                            w0 * a.b + w1 * b.b + w2 * c.b, w0 * a.a + w1 * b.a + w2 * c.a,
                            w0 * a.sr + w1 * b.sr + w2 * c.sr, w0 * a.sg + w1 * b.sg + w2 * c.sg,
                            w0 * a.sb + w1 * b.sb + w2 * c.sb, fog);
        }
    }
}

void Ge::draw_sprite(const Vertex &a, const Vertex &b) {
    ++stats_.sprites;
    const int sx1 = ps_.sx1, sy1 = ps_.sy1, sx2 = ps_.sx2, sy2 = ps_.sy2;
    const float x0 = std::min(a.x, b.x), x1 = std::max(a.x, b.x);
    const float y0 = std::min(a.y, b.y), y1 = std::max(a.y, b.y);
    const int ix0 = std::max(static_cast<int>(std::floor(x0)), sx1);
    const int iy0 = std::max(static_cast<int>(std::floor(y0)), sy1);
    const int ix1 = std::min(static_cast<int>(std::ceil(x1)) - 1, sx2);
    const int iy1 = std::min(static_cast<int>(std::ceil(y1)) - 1, sy2);
    const float dx = b.x - a.x, dy = b.y - a.y;
    for (int y = iy0; y <= iy1; ++y) {
        const float ty = dy != 0.0f ? (static_cast<float>(y) + 0.5f - a.y) / dy : 0.0f;
        const float v = a.v + (b.v - a.v) * ty;
        for (int x = ix0; x <= ix1; ++x) {
            const float tx = dx != 0.0f ? (static_cast<float>(x) + 0.5f - a.x) / dx : 0.0f;
            float u = a.u + (b.u - a.u) * tx, t = v;
            if (!layout_.through) {
                // A rectangle uses the final vertex's clip w for all four
                // corners; texture q varies along x, like s.
                const float q = a.q + (b.q - a.q) * tx;
                if (q > 0.0f) { u /= q; t /= q; }
                else {
                    u = tex_.tw * (1.0f - 1.0f / 65536.0f);
                    t = tex_.th * (1.0f - 1.0f / 65536.0f);
                }
            }
            shade_pixel(x, y, b.z, u, t, b.r, b.g, b.b, b.a, b.sr, b.sg, b.sb, b.fog);
        }
    }
}

// ---------------------------------------------------------------------------
// Per-pixel pipeline


void Ge::take_sections(double out[6]) {
    for (int i = 0; i < 6; ++i) {
        out[i] = g_section_ms[i];
        g_section_ms[i] = 0.0;
    }
}

void Ge::report_targets() {
    if (!g_profile_targets) return;
    std::cerr << "[ge-sections]";
    for (int i = 0; i < 6; ++i) {
        std::cerr << " " << g_section_names[i] << "=" << g_section_ms[i] / 60.0 << "ms";
        g_section_ms[i] = 0.0;
    }
    std::cerr << " transfer=" << g_transfer_bytes / 60.0 / 1024.0 << "KiB (per frame)\n";
    g_transfer_bytes = 0.0;
    for (const auto &[key, count] : g_target_pixels)
        std::cerr << "[ge-target] fb=0x" << std::hex << (key >> 32u) << " stride=" << std::dec << ((key >> 16u) & 0xFFFFu)
                  << (key & 1u ? " through" : " 3d") << ((key & 2u) ? " tex" : "") << ((key & 4u) ? " blend" : "")
                  << " pixels=" << count << "\n";
    g_target_pixels.clear();
}

void Ge::shade_pixel(int x, int y, float z, float u, float v, float r, float g, float b, float a,
                     float sr, float sg, float sb, float fog) {
    const PixelState &p = ps_;
    if (p.vram == nullptr || x < 0 || y < 0 || x >= 1024 || y >= 1024) return;
    ++stats_.pixels;
    if (g_profile_targets) {
        const std::uint64_t key = (static_cast<std::uint64_t>(regs_[kFrameBufPtr] & 0xFFFFFFu) << 32u) |
                                  (static_cast<std::uint64_t>(p.fb_stride) << 16u) | (layout_.through ? 1u : 0u) |
                                  (p.texture ? 2u : 0u) | (p.blend ? 4u : 0u);
        ++g_target_pixels[key];
    }
    const auto ux = static_cast<std::uint32_t>(x), uy = static_cast<std::uint32_t>(y);
    std::uint8_t *fb = p.vram + ((p.fb_offset + (uy * p.fb_stride + ux) * p.fb_bpp) & (0x1FFFFFu & ~(p.fb_bpp - 1u)));
    std::uint8_t *zb = p.vram + ((p.z_offset + (uy * p.z_stride + ux) * 2u) & 0x1FFFFEu);
    const std::uint32_t depth = static_cast<std::uint32_t>(std::clamp(z, 0.0f, 65535.0f));
    auto load_fb = [&]() -> std::uint32_t {
        if (p.fb_bpp == 4u) {
            std::uint32_t value;
            std::memcpy(&value, fb, 4u);
            return value;
        }
        return decode16(static_cast<std::uint32_t>(fb[0]) | (static_cast<std::uint32_t>(fb[1]) << 8u), p.fb_format);
    };
    auto store_fb = [&](std::uint32_t rgba, std::uint32_t old) {
        rgba = (rgba & ~p.write_mask) | (old & p.write_mask);
        if (p.fb_bpp == 4u) {
            std::memcpy(fb, &rgba, 4u);
            return;
        }
        const std::uint32_t cr = rgba & 0xFFu, cg = (rgba >> 8u) & 0xFFu, cb = (rgba >> 16u) & 0xFFu, ca = rgba >> 24u;
        std::uint32_t packed;
        if (p.fb_format == 0u) packed = (cr >> 3u) | ((cg >> 2u) << 5u) | ((cb >> 3u) << 11u);
        else if (p.fb_format == 1u) packed = (cr >> 3u) | ((cg >> 3u) << 5u) | ((cb >> 3u) << 10u) | ((ca >> 7u) << 15u);
        else packed = (cr >> 4u) | ((cg >> 4u) << 4u) | ((cb >> 4u) << 8u) | ((ca >> 4u) << 12u);
        fb[0] = static_cast<std::uint8_t>(packed);
        fb[1] = static_cast<std::uint8_t>(packed >> 8u);
    };

    const unsigned nibble = (p.dither_rows[static_cast<unsigned>(y) & 3u] >> ((static_cast<unsigned>(x) & 3u) * 4u)) & 15u;
    const int dither = p.dither ? static_cast<int>(nibble ^ 8u) - 8 : 0;
    if (p.clear) {
        const std::uint32_t old = load_fb();
        std::uint32_t value = old;
        const std::uint32_t src = pack(static_cast<std::uint32_t>(clamp255(r + dither)), static_cast<std::uint32_t>(clamp255(g + dither)),
                                       static_cast<std::uint32_t>(clamp255(b + dither)), static_cast<std::uint32_t>(clamp255(a)));
        if ((p.clear_flags & 0x100u) != 0u) value = (value & 0xFF000000u) | (src & 0x00FFFFFFu);
        if ((p.clear_flags & 0x200u) != 0u) value = (value & 0x00FFFFFFu) | (src & 0xFF000000u);
        store_fb(value, old);
        if ((p.clear_flags & 0x400u) != 0u && p.z_stride != 0u) {
            zb[0] = static_cast<std::uint8_t>(depth);
            zb[1] = static_cast<std::uint8_t>(depth >> 8u);
        }
        return;
    }

    if (p.z_test) {
        const std::uint32_t stored = static_cast<std::uint32_t>(zb[0]) | (static_cast<std::uint32_t>(zb[1]) << 8u);
        if (!compare(p.z_func, depth, stored)) return;
    }

    float color[4] = {interpolated_byte(r), interpolated_byte(g), interpolated_byte(b), interpolated_byte(a)};
    if (p.texture) {
        float tex[4];
        sample_texture(u, v, tex);
        // Texture functions consume byte colors. Doubling applies before the
        // fixed-point division; alpha is never doubled.
        const float rgb_scale = p.tex_double ? 2.0f : 1.0f;
        const float alpha = p.tex_alpha ? std::floor((color[3] + 1.0f) * tex[3] / 256.0f) : color[3];
        switch (p.tex_func) {
        case 0: // modulate
            for (int k = 0; k < 3; ++k) color[k] = std::floor((color[k] + 1.0f) * tex[k] * rgb_scale / 256.0f);
            color[3] = alpha;
            break;
        case 1: // decal
            if (p.tex_alpha) for (int k = 0; k < 3; ++k)
                color[k] = std::floor(((color[k] + 1.0f) * (255.0f - tex[3]) + (tex[k] + 1.0f) * tex[3]) * rgb_scale / 256.0f);
            else for (int k = 0; k < 3; ++k) color[k] = tex[k] * rgb_scale;
            break;
        case 2: // blend
            for (int k = 0; k < 3; ++k)
                color[k] = std::floor((color[k] * (255.0f - tex[k]) + p.env[k] * tex[k] + 255.0f) * rgb_scale / 256.0f);
            color[3] = alpha;
            break;
        case 3: // replace
            for (int k = 0; k < 3; ++k) color[k] = tex[k] * rgb_scale;
            if (p.tex_alpha) color[3] = tex[3];
            break;
        default: // add
            for (int k = 0; k < 3; ++k) color[k] = (color[k] + tex[k]) * rgb_scale;
            color[3] = alpha;
            break;
        }
    }
    const float secondary_scale = p.texture && p.tex_double ? 2.0f : 1.0f;
    color[0] += interpolated_byte(sr) * secondary_scale;
    color[1] += interpolated_byte(sg) * secondary_scale;
    color[2] += interpolated_byte(sb) * secondary_scale;
    for (float &channel : color) channel = clamp255(channel);

    if (p.alpha_test && !compare(p.alpha_func, static_cast<std::uint32_t>(color[3]) & p.alpha_mask, p.alpha_ref)) return;

    if (p.fog) {
        const auto factor = static_cast<unsigned>(std::clamp(std::floor(fog), 0.0f, 255.0f));
        for (unsigned k = 0; k < 3; ++k)
            color[k] = static_cast<float>((static_cast<unsigned>(color[k]) * factor +
                       static_cast<unsigned>(p.fog_color[k]) * (255 - factor) + 255) >> 8);
    }

    const std::uint32_t old = load_fb();
    if (p.blend) {
        for (float &channel : color) channel = std::floor(channel);
        const float dst[4] = {static_cast<float>(old & 0xFFu), static_cast<float>((old >> 8u) & 0xFFu),
                              static_cast<float>((old >> 16u) & 0xFFu), static_cast<float>(old >> 24u)};
        const auto sa = static_cast<unsigned>(color[3]);
        // 565 textures are opaque, but a 565 destination has no alpha/stencil
        // channel: destination-alpha blend factors read zero.
        const auto da = p.fb_format == 0u ? 0u : static_cast<unsigned>(dst[3]);
        auto factor = [&](std::uint32_t f, bool is_src, int k) -> unsigned {
            switch (f) {
            case 0: return static_cast<unsigned>(is_src ? dst[k] : color[k]);
            case 1: return 255u - static_cast<unsigned>(is_src ? dst[k] : color[k]);
            case 2: return sa;
            case 3: return 255u - sa;
            case 4: return da;
            case 5: return 255u - da;
            case 6: return 2u * sa;
            case 7: return 255u - std::min(2u * sa, 255u);
            case 8: return 2u * da;
            case 9: return 255u - std::min(2u * da, 255u);
            default: return static_cast<unsigned>(is_src ? p.fix_a[k] : p.fix_b[k]);
            }
        };
        for (int k = 0; k < 3; ++k) {
            const float s = static_cast<float>(((2u * static_cast<unsigned>(color[k]) + 1u) *
                                               (2u * factor(p.src_factor, true, k) + 1u)) >> 10u);
            const float d = static_cast<float>(((2u * static_cast<unsigned>(dst[k]) + 1u) *
                                               (2u * factor(p.dst_factor, false, k) + 1u)) >> 10u);
            float result;
            switch (p.equation) {
            case 0: result = s + d; break;
            case 1: result = s - d; break;
            case 2: result = d - s; break;
            case 3: result = std::min(color[k], dst[k]); break;
            case 4: result = std::max(color[k], dst[k]); break;
            case 5: result = std::fabs(color[k] - dst[k]); break;
            default: result = color[k]; break;
            }
            color[k] = result;
        }
    }

    // Dither the final RGB before saturation and packing; stencil/alpha is
    // independent. Keeping blend results unclamped above preserves this order.
    for (unsigned k = 0; k < 3; ++k) color[k] = clamp255(color[k] + dither);
    store_fb(pack(static_cast<std::uint32_t>(color[0]), static_cast<std::uint32_t>(color[1]),
                  static_cast<std::uint32_t>(color[2]), static_cast<std::uint32_t>(color[3])),
             old);
    if (p.z_write) {
        zb[0] = static_cast<std::uint8_t>(depth);
        zb[1] = static_cast<std::uint8_t>(depth >> 8u);
    }
}

std::uint32_t Ge::read_pixel(std::uint32_t address) const {
    const std::uint32_t format = regs_[kFrameBufPixFormat] & 3u;
    if (format == 3u) return memory_.load32(address);
    return decode16(memory_.load16(address), format);
}

void Ge::write_pixel(std::uint32_t address, std::uint32_t rgba, std::uint32_t old) {
    const std::uint32_t format = regs_[kFrameBufPixFormat] & 3u;
    const std::uint32_t mask = (regs_[kMaskRgb] & 0xFFFFFFu) | ((regs_[kMaskAlpha] & 0xFFu) << 24u);
    rgba = (rgba & ~mask) | (old & mask);
    const std::uint32_t r = rgba & 0xFFu, g = (rgba >> 8u) & 0xFFu, b = (rgba >> 16u) & 0xFFu, a = rgba >> 24u;
    switch (format) {
    case 3: memory_.store32(address, rgba); break;
    case 0: memory_.store16(address, static_cast<std::uint16_t>((r >> 3u) | ((g >> 2u) << 5u) | ((b >> 3u) << 11u))); break;
    case 1:
        memory_.store16(address, static_cast<std::uint16_t>((r >> 3u) | ((g >> 3u) << 5u) | ((b >> 3u) << 10u) |
                                                            ((a >> 7u) << 15u)));
        break;
    default:
        memory_.store16(address, static_cast<std::uint16_t>((r >> 4u) | ((g >> 4u) << 4u) | ((b >> 4u) << 8u) |
                                                            ((a >> 4u) << 12u)));
        break;
    }
}

// ---------------------------------------------------------------------------
// Textures

void Ge::load_clut() {
    const std::uint32_t address = ((regs_[kClutAddr] & 0xFFFFF0u) | ((regs_[kClutAddrUpper] & 0x0F0000u) << 8u));
    const std::uint32_t bytes = std::min<std::uint32_t>((regs_[kLoadClut] & 0x3Fu) * 32u, static_cast<std::uint32_t>(clut_.size()));
    const std::uint8_t *src = memory_.raw_pointer(address, bytes);
    if (src != nullptr) std::memcpy(clut_.data(), src, bytes);
    clut_hash_ = hash_bytes(clut_.data(), clut_.size());
}

std::uint32_t Ge::texture_bytes(std::uint32_t format, std::uint32_t buf_width, std::uint32_t height) const {
    if (format >= 8u) {
        const std::uint32_t blocks = ((buf_width + 3u) / 4u) * ((height + 3u) / 4u);
        return blocks * (format == 8u ? 8u : 16u);
    }
    static constexpr std::uint32_t bits_per_texel[8] = {16, 16, 16, 32, 4, 8, 16, 32};
    if ((regs_[kTexMode] & 1u) != 0u) {
        const std::uint32_t row_bytes = (buf_width * bits_per_texel[format & 7u] + 7u) / 8u;
        return ((row_bytes + 15u) / 16u) * ((height + 7u) / 8u) * 128u;
    }
    return (buf_width * height * bits_per_texel[format & 7u] + 7u) / 8u;
}

// Cache a regular mip chain as one unit. RAM is revalidated each frame;
// every VRAM level is also revalidated after writes to its covered pages.
void Ge::bind_texture() {
    SectionTimer timer{2};
    texture_ = nullptr;
    if ((regs_[kTextureEnable] & 1u) == 0u || (regs_[kClearMode] & 1u) != 0u) return;
    const std::uint32_t format = regs_[kTexFormat] & 0xFu;
    if (format > 10u) return;
    const std::uint32_t address = (regs_[kTexAddr0] & 0xFFFFF0u) | ((regs_[kTexBufWidth0] & 0x0F0000u) << 8u);
    const std::uint32_t width = 1u << std::min(regs_[kTexSize0] & 0xFu, 9u);
    const std::uint32_t height = 1u << std::min((regs_[kTexSize0] >> 8u) & 0xFu, 9u);
    const std::uint32_t buf_width = texture_row_width(format,regs_[kTexBufWidth0]);
    const bool clut = format >= 4u && format <= 7u;
    TextureKey key{address, format, width, height, buf_width, regs_[kTexMode] & 1u,
                   clut ? regs_[kClutFormat] : 0u, clut ? clut_hash_ : 0u};
    key.mip_clut = clut ? regs_[kTexMode] & 0x100u : 0u;
    texture_width_ = static_cast<int>(width);
    texture_height_ = static_cast<int>(height);
    const unsigned requested = gl_ != nullptr && (regs_[0xC6] & 4u) != 0u
        ? std::min((regs_[kTexMode] >> 16u) & 7u, std::bit_width(std::max(width, height)) - 1u) : 0u;
    std::array<std::uint32_t, 8> addresses{}, widths{}, heights{}, strides{}, lengths{};
    std::array<const std::uint8_t *, 8> sources{};
    bool in_vram = false;
    std::uint64_t stamp = 0, texel_count = 0;
    for (unsigned level = 0; level <= requested; ++level) {
        addresses[level] = (regs_[kTexAddr0 + level] & 0xFFFFF0u) | ((regs_[kTexBufWidth0 + level] & 0x0F0000u) << 8u);
        widths[level] = 1u << std::min(regs_[kTexSize0 + level] & 0xFu, 9u);
        heights[level] = 1u << std::min((regs_[kTexSize0 + level] >> 8u) & 0xFu, 9u);
        strides[level] = texture_row_width(format,regs_[kTexBufWidth0 + level]);
        if (level && (widths[level] != std::max(width >> level, 1u) ||
                      heights[level] != std::max(height >> level, 1u))) break;
        lengths[level] = texture_bytes(format, strides[level], heights[level]);
        sources[level] = memory_.raw_pointer(addresses[level], lengths[level]);
        if (sources[level] == nullptr) { if (level == 0) return; break; }
        if ((addresses[level] & 0x0F000000u) == 0x04000000u) {
            in_vram = true;
            if (gl_ != nullptr) gl_->prepare_vram_read(addresses[level], lengths[level]);
            stamp = std::max(stamp, vram_stamp(addresses[level], lengths[level]));
        }
        texel_count += static_cast<std::uint64_t>(widths[level]) * heights[level];
        if (level) {
            key.mip_count = level;
            key.mip_regs[(level - 1) * 3] = addresses[level];
            key.mip_regs[(level - 1) * 3 + 1] = strides[level];
            key.mip_regs[(level - 1) * 3 + 2] = regs_[kTexSize0 + level] & 0xF0Fu;
        }
    }
    auto found = textures_.find(key);
    if (found != textures_.end() && found->second.frame == frame_ &&
        (!in_vram || found->second.vram_generation == stamp)) {
        texture_ = &found->second;
        return;
    }
    std::uint64_t hash = hash_bytes(sources[0], lengths[0]);
    for (unsigned level = 1; level <= key.mip_count; ++level)
        hash = (hash ^ hash_bytes(sources[level], lengths[level])) * 0x100000001B3ull;
    if (found != textures_.end() && found->second.source_hash == hash) {
        found->second.frame = frame_;
        found->second.vram_generation = stamp;
        texture_ = &found->second;
        return;
    }
    if (found == textures_.end()) {
        if (texture_cache_texels_ + texel_count > kTextureCacheTexelLimit) evict_textures(texel_count);
        found = textures_.emplace(key, CachedTexture{}).first;
        texture_cache_texels_ += texel_count;
    }
    CachedTexture &cached = found->second;
    if (gl_ != nullptr) gl_->flush();
    for (unsigned level = 0; level <= key.mip_count; ++level) {
        auto &texels = level ? cached.mips[level - 1] : cached.texels;
        texels.resize(static_cast<std::size_t>(widths[level]) * heights[level]);
        for (std::uint32_t y = 0; y < heights[level]; ++y)
            for (std::uint32_t x = 0; x < widths[level]; ++x)
                texels[y * widths[level] + x] = fetch_texel(static_cast<int>(x), static_cast<int>(y), level);
    }
    cached.mip_count = key.mip_count;
    cached.texel_count = texel_count;
    cached.source_hash = hash;
    cached.frame = frame_;
    cached.vram_generation = stamp;
    texture_ = &cached;
}

void Ge::evict_textures(std::uint64_t need) {
    if (gl_ != nullptr) gl_->flush();
    std::vector<std::pair<std::uint32_t, TextureKey>> order;
    order.reserve(textures_.size());
    for (const auto &[key, texture] : textures_) order.emplace_back(texture.frame, key);
    std::sort(order.begin(), order.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
    const std::uint64_t goal = kTextureCacheTexelLimit * 3u / 4u;
    for (const auto &[frame, key] : order) {
        if (texture_cache_texels_ + need <= goal) break;
        const auto it = textures_.find(key);
        if (gl_ != nullptr) gl_->forget_texture(&it->second);
        texture_cache_texels_ -= it->second.texel_count;
        textures_.erase(it);
    }
}

void Ge::dump_texture(const CachedTexture &texture, std::uint32_t address, std::uint32_t format, std::uint32_t width,
                      std::uint32_t height) {
    std::ostringstream name;
    name << g_dump_dir << "/tex_" << std::hex << address << std::dec << "_f" << format << "_" << width << "x" << height;
    if ((format >= 4u && format <= 7u)) name << "_clut" << std::hex << regs_[kClutFormat] << std::dec;
    // RGB, then alpha as a grey image next to it.
    std::ofstream out(name.str() + ".ppm", std::ios::binary);
    out << "P6\n" << width * 2u << " " << height << "\n255\n";
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            const std::uint32_t t = texture.texels[y * width + x];
            const char rgb[3] = {static_cast<char>(t & 0xFFu), static_cast<char>((t >> 8u) & 0xFFu), static_cast<char>((t >> 16u) & 0xFFu)};
            out.write(rgb, 3);
        }
        for (std::uint32_t x = 0; x < width; ++x) {
            const char a = static_cast<char>(texture.texels[y * width + x] >> 24u);
            const char grey[3] = {a, a, a};
            out.write(grey, 3);
        }
    }
}

void Ge::log_primitive(std::uint32_t index, std::uint32_t type, std::uint32_t count) {
    const std::uint32_t tex_addr = (regs_[kTexAddr0] & 0xFFFFF0u) | ((regs_[kTexBufWidth0] & 0x0F0000u) << 8u);
    std::cerr << std::hex << "[prim] " << std::dec << index << " type=" << type << " n=" << count << std::hex
              << " vtype=" << regs_[kVertexType] << " fb=" << ps_.fb_offset << "/" << ps_.fb_stride << "/f" << ps_.fb_format;
    if (ps_.clear) std::cerr << " CLEAR=" << ps_.clear_flags;
    if ((regs_[kTextureEnable] & 1u) != 0u)
        std::cerr << " tex=" << tex_addr << "/f" << (regs_[kTexFormat] & 0xFu) << "/" << (regs_[kTexSize0] & 0xFFFu)
                  << "/bw" << (regs_[kTexBufWidth0] & 0x7FFu) << " clut=" << regs_[kClutFormat] << " tfunc=" << regs_[kTexFunc]
                  << " env=" << regs_[kTexEnvColor] << " map=" << regs_[kTexMapMode];
    if ((regs_[kBlendEnable] & 1u) != 0u)
        std::cerr << " blend=" << regs_[kBlendMode] << " fa=" << regs_[kBlendFixA] << " fb=" << regs_[kBlendFixB];
    if ((regs_[kAlphaTestEnable] & 1u) != 0u) std::cerr << " atest=" << regs_[kAlphaTest];
    if ((regs_[kZTestEnable] & 1u) != 0u) std::cerr << " z=" << regs_[kZTest] << (ps_.z_write ? "w" : "");
    if ((regs_[0x24] & 1u) != 0u) std::cerr << " STENCIL=" << regs_[0xDC] << "/" << regs_[0xDD];
    if ((regs_[0x1F] & 1u) != 0u) std::cerr << " FOG=" << regs_[0xCF];
    if ((regs_[0x27] & 1u) != 0u) std::cerr << " COLORTEST";
    if ((regs_[0x28] & 1u) != 0u) std::cerr << " LOGICOP=" << regs_[0xE6];
    if ((regs_[kLightingEnable] & 1u) != 0u) {
        std::cerr << " light upd=" << regs_[kMaterialUpdate] << " emis=" << regs_[kMaterialEmissive]
                  << " diff=" << regs_[kMaterialDiffuse] << " global=" << regs_[kAmbientColor];
        for (std::uint32_t i = 0; i < 4u; ++i)
            if ((regs_[kLightEnable0 + i] & 1u) != 0u)
                std::cerr << " L" << i << "=" << regs_[kLightType0 + i] << ":" << regs_[kLightColor0 + i * 3u] << "/"
                          << regs_[kLightColor0 + i * 3u + 1u] << "/" << regs_[kLightColor0 + i * 3u + 2u];
    }
    if ((regs_[kMaskRgb] & 0xFFFFFFu) != 0u || (regs_[kMaskAlpha] & 0xFFu) != 0u)
        std::cerr << " mask=" << regs_[kMaskRgb] << "/" << regs_[kMaskAlpha];
    std::cerr << " mat=" << regs_[kMaterialAmbient] << "/" << regs_[kMaterialAlpha] << std::dec << "\n";
    static std::set<std::uintptr_t> dumped;
    if ((regs_[kTextureEnable] & 1u) != 0u && texture_ != nullptr &&
        dumped.insert(reinterpret_cast<std::uintptr_t>(texture_)).second)
        dump_texture(*texture_, tex_addr, regs_[kTexFormat] & 0xFu, static_cast<std::uint32_t>(texture_width_),
                     static_cast<std::uint32_t>(texture_height_));
}

void Ge::prepare_pixel_state() {
    PixelState &p = ps_;
    p.vram = memory_.raw_pointer(kVramBase, 0x200000u);
    p.fb_format = regs_[kFrameBufPixFormat] & 3u;
    p.fb_bpp = p.fb_format == 3u ? 4u : 2u;
    p.fb_stride = regs_[kFrameBufWidth] & 0x7FCu;
    p.fb_offset = ((regs_[kFrameBufPtr] & 0xFFFFFFu) | ((regs_[kFrameBufWidth] & 0xFF0000u) << 8u)) & 0x1FFFF0u;
    p.z_stride = regs_[kZBufWidth] & 0x7FCu;
    p.z_offset = ((regs_[kZBufPtr] & 0xFFFFFFu) | ((regs_[kZBufWidth] & 0xFF0000u) << 8u)) & 0x1FFFF0u;
    p.z_test = (regs_[kZTestEnable] & 1u) != 0u && p.z_stride != 0u;
    p.z_func = regs_[kZTest] & 7u;
    p.z_write = p.z_test && (regs_[kZWriteDisable] & 1u) == 0u;
    p.clear = (regs_[kClearMode] & 1u) != 0u;
    p.clear_flags = regs_[kClearMode];
    p.dither = (regs_[0x20] & 1u) != 0u;
    for (unsigned i = 0; i < 4; ++i) p.dither_rows[i] = regs_[0xE2 + i] & 0xFFFFu;
    p.texture = (regs_[kTextureEnable] & 1u) != 0u;
    p.tex_func = regs_[kTexFunc] & 7u;
    p.tex_alpha = (regs_[kTexFunc] & 0x100u) != 0u;
    p.tex_double = (regs_[kTexFunc] & 0x10000u) != 0u;
    color24(regs_[kTexEnvColor], p.env);
    p.fog = !p.clear && !layout_.through && (regs_[kFogEnable] & 1u) != 0u;
    color24(regs_[kFogColor], p.fog_color);
    p.alpha_test = (regs_[kAlphaTestEnable] & 1u) != 0u;
    p.alpha_func = regs_[kAlphaTest] & 7u;
    p.alpha_mask = (regs_[kAlphaTest] >> 16u) & 0xFFu;
    p.alpha_ref = ((regs_[kAlphaTest] >> 8u) & 0xFFu) & p.alpha_mask;
    p.blend = (regs_[kBlendEnable] & 1u) != 0u;
    p.src_factor = regs_[kBlendMode] & 0xFu;
    p.dst_factor = (regs_[kBlendMode] >> 4u) & 0xFu;
    p.equation = (regs_[kBlendMode] >> 8u) & 7u;
    color24(regs_[kBlendFixA], p.fix_a);
    color24(regs_[kBlendFixB], p.fix_b);
    p.write_mask = (regs_[kMaskRgb] & 0xFFFFFFu) | ((regs_[kMaskAlpha] & 0xFFu) << 24u);
    p.sx1 = static_cast<int>(regs_[kScissor1] & 0x3FFu);
    p.sy1 = static_cast<int>((regs_[kScissor1] >> 10u) & 0x3FFu);
    p.sx2 = std::min(static_cast<int>(regs_[kScissor2] & 0x3FFu), static_cast<int>(p.fb_stride) - 1);
    p.sy2 = static_cast<int>((regs_[kScissor2] >> 10u) & 0x3FFu);
}

std::uint32_t Ge::clut_lookup(std::uint32_t index, unsigned level) const {
    const std::uint32_t mode = regs_[kClutFormat];
    const std::uint32_t format = mode & 3u, shift = (mode >> 2u) & 0x1Fu, mask = (mode >> 8u) & 0xFFu;
    const std::uint32_t start = ((mode >> 16u) & 0x1Fu) << 4u;
    const std::uint32_t i = (((index >> shift) & mask) | start) +
                            ((regs_[kTexMode] & 0x100u) != 0u ? level * 16u : 0u);
    if (format == 3u) {
        const std::uint32_t at = (i * 4u) & 0x7FCu;
        return static_cast<std::uint32_t>(clut_[at]) | (static_cast<std::uint32_t>(clut_[at + 1u]) << 8u) |
               (static_cast<std::uint32_t>(clut_[at + 2u]) << 16u) | (static_cast<std::uint32_t>(clut_[at + 3u]) << 24u);
    }
    const std::uint32_t at = (i * 2u) & 0x7FEu;
    return decode16(static_cast<std::uint32_t>(clut_[at]) | (static_cast<std::uint32_t>(clut_[at + 1u]) << 8u), format);
}

std::uint32_t Ge::fetch_texel(int x, int y, unsigned level) const {
    const std::uint32_t format = regs_[kTexFormat] & 0xFu;
    const std::uint32_t address = (regs_[kTexAddr0 + level] & 0xFFFFF0u) | ((regs_[kTexBufWidth0 + level] & 0x0F0000u) << 8u);
    const std::uint32_t buf_width = texture_row_width(format,regs_[kTexBufWidth0 + level]);
    const auto ux = static_cast<std::uint32_t>(x), uy = static_cast<std::uint32_t>(y);

    if (format >= 8u) { // DXT1/3/5: 4x4 blocks
        const std::uint32_t block_bytes = format == 8u ? 8u : 16u;
        const std::uint32_t block = (uy / 4u) * (buf_width / 4u) + ux / 4u;
        const std::uint32_t at = address + block * block_bytes;
        const std::uint32_t lx = ux & 3u, ly = uy & 3u;
        const std::uint32_t lines = memory_.load32(at);
        const std::uint32_t c1 = memory_.load16(at + 4u), c2 = memory_.load16(at + 6u);
        // DXT colours are RGB565 with red in the top bits, unlike PSP 16-bit pixels.
        auto dxt565 = [](std::uint32_t c) {
            const std::uint32_t r = (c >> 11u) & 31u, g = (c >> 5u) & 63u, b = c & 31u;
            return ((r << 3u) | (r >> 2u)) | (((g << 2u) | (g >> 4u)) << 8u) | (((b << 3u) | (b >> 2u)) << 16u);
        };
        const std::uint32_t rgb1 = dxt565(c1), rgb2 = dxt565(c2);
        const std::uint32_t sel = (lines >> (ly * 8u + lx * 2u)) & 3u;
        auto mix = [](std::uint32_t p, std::uint32_t q, std::uint32_t wp, std::uint32_t wq, std::uint32_t div) {
            std::uint32_t out = 0;
            for (std::uint32_t s = 0; s < 24u; s += 8u)
                out |= ((((p >> s) & 0xFFu) * wp + ((q >> s) & 0xFFu) * wq) / div) << s;
            return out;
        };
        std::uint32_t rgb;
        std::uint32_t alpha = 255u;
        const bool four_colors = format != 8u || c1 > c2;
        if (sel == 0u) rgb = rgb1 & 0xFFFFFFu;
        else if (sel == 1u) rgb = rgb2 & 0xFFFFFFu;
        else if (four_colors) rgb = sel == 2u ? mix(rgb1, rgb2, 2u, 1u, 3u) : mix(rgb1, rgb2, 1u, 2u, 3u);
        else if (sel == 2u) rgb = mix(rgb1, rgb2, 1u, 1u, 2u);
        else { rgb = 0u; alpha = 0u; }
        if (format == 9u) {
            const std::uint32_t row = memory_.load16(at + 8u + ly * 2u);
            alpha = ((row >> (lx * 4u)) & 0xFu) * 17u;
        } else if (format == 10u) {
            // 48 bits of 3-bit indices: the low 32 at +8, the high 16 at +12.
            const std::uint64_t bits = static_cast<std::uint64_t>(memory_.load32(at + 8u)) |
                                       (static_cast<std::uint64_t>(memory_.load16(at + 12u)) << 32u);
            const std::uint32_t a1 = memory_.load8(at + 14u), a2 = memory_.load8(at + 15u);
            const std::uint32_t idx = static_cast<std::uint32_t>((bits >> ((ly * 4u + lx) * 3u)) & 7u);
            if (idx == 0u) alpha = a1;
            else if (idx == 1u) alpha = a2;
            else if (a1 > a2) alpha = (a1 * (8u - idx) + a2 * (idx - 1u)) / 7u;
            else if (idx < 6u) alpha = (a1 * (6u - idx) + a2 * (idx - 1u)) / 5u;
            else alpha = idx == 6u ? 0u : 255u;
        }
        return (rgb & 0xFFFFFFu) | (alpha << 24u);
    }

    static constexpr std::uint32_t bits_per_texel[8] = {16, 16, 16, 32, 4, 8, 16, 32};
    const std::uint32_t bpp = bits_per_texel[format & 7u];
    std::uint32_t byte_offset;
    const std::uint32_t x_bits = ux * bpp;
    if ((regs_[kTexMode] & 1u) != 0u) {
        const std::uint32_t row_bytes = buf_width * bpp / 8u;
        const std::uint32_t x_bytes = x_bits / 8u;
        const std::uint32_t block = (uy / 8u) * (row_bytes / 16u) + x_bytes / 16u;
        byte_offset = block * 128u + (uy % 8u) * 16u + x_bytes % 16u;
    } else {
        byte_offset = (uy * buf_width * bpp + x_bits) / 8u;
    }
    const std::uint32_t at = address + byte_offset;
    switch (format) {
    case 0: case 1: case 2: return decode16(memory_.load16(at), format);
    case 3: return memory_.load32(at);
    case 4: return clut_lookup((memory_.load8(at) >> ((ux & 1u) * 4u)) & 0xFu, level);
    case 5: return clut_lookup(memory_.load8(at), level);
    case 6: return clut_lookup(memory_.load16(at), level);
    default: return clut_lookup(memory_.load32(at), level);
    }
}

void Ge::sample_texture(float u, float v, float rgba[4]) const {
    const int width = 1 << std::min(regs_[kTexSize0] & 0xFu, 9u);
    const int height = 1 << std::min((regs_[kTexSize0] >> 8u) & 0xFu, 9u);
    int x = static_cast<int>(std::floor(u));
    int y = static_cast<int>(std::floor(v));
    if ((regs_[kTexWrap] & 1u) != 0u) x = std::clamp(x, 0, width - 1);
    else x &= width - 1;
    if ((regs_[kTexWrap] & 0x100u) != 0u) y = std::clamp(y, 0, height - 1);
    else y &= height - 1;
    const std::uint32_t texel = texture_ != nullptr
        ? texture_->texels[static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x)]
        : fetch_texel(x, y);
    rgba[0] = static_cast<float>(texel & 0xFFu);
    rgba[1] = static_cast<float>((texel >> 8u) & 0xFFu);
    rgba[2] = static_cast<float>((texel >> 16u) & 0xFFu);
    rgba[3] = static_cast<float>(texel >> 24u);
}

// ---------------------------------------------------------------------------
// Block transfer (TRXKICK)

void Ge::block_transfer() {
    SectionTimer timer{3};
    ++stats_.transfers;
    const std::uint32_t src = (regs_[kTransferSrc] & 0xFFFFF0u) | ((regs_[kTransferSrcW] & 0xFF0000u) << 8u);
    const std::uint32_t dst = (regs_[kTransferDst] & 0xFFFFF0u) | ((regs_[kTransferDstW] & 0xFF0000u) << 8u);
    const std::uint32_t src_stride = regs_[kTransferSrcW] & 0x7F8u;
    const std::uint32_t dst_stride = regs_[kTransferDstW] & 0x7F8u;
    const std::uint32_t sx = regs_[kTransferSrcPos] & 0x3FFu, sy = (regs_[kTransferSrcPos] >> 10u) & 0x3FFu;
    const std::uint32_t dx = regs_[kTransferDstPos] & 0x3FFu, dy = (regs_[kTransferDstPos] >> 10u) & 0x3FFu;
    const std::uint32_t width = (regs_[kTransferSize] & 0x3FFu) + 1u;
    const std::uint32_t height = ((regs_[kTransferSize] >> 10u) & 0x3FFu) + 1u;
    const std::uint32_t bpp = (regs_[kTransferStart] & 1u) != 0u ? 4u : 2u;
    const std::uint32_t src_first = src + (sy * src_stride + sx) * bpp, dst_first = dst + (dy * dst_stride + dx) * bpp;
    const std::uint32_t src_span = ((height - 1u) * src_stride + width) * bpp, dst_span = ((height - 1u) * dst_stride + width) * bpp;
    static const std::uint32_t log_frame = [] {
        const char *v = std::getenv("PSPWEB_XFER_FRAME");
        return v != nullptr ? static_cast<std::uint32_t>(std::strtoul(v, nullptr, 10)) : 900u;
    }();
    if (g_profile_targets && frame_ >= log_frame && frame_ < log_frame + 1u)
        std::cerr << "[ge-transfer] frame=" << frame_ << " src=0x" << std::hex << src_first << " stride=" << std::dec
                  << src_stride << " dst=0x" << std::hex << dst_first << " stride=" << std::dec << dst_stride << " size="
                  << width << "x" << height << " bpp=" << bpp << "\n";
    (void)src_first;
    (void)dst_first;
    (void)src_span;
    (void)dst_span;
    const TransferRect src_rect{src, src_stride, sx, sy, width, height, bpp};
    const TransferRect dst_rect{dst, dst_stride, dx, dy, width, height, bpp};
    touch_vram(dst_first, dst_span);
    state_dirty_ = true;
    g_transfer_bytes += static_cast<double>(width) * height * bpp;
    if (gl_ != nullptr) {
        SectionTimer gl_timer{5};
        gl_->before_transfer(src_rect, dst_rect);
    }
    struct AfterTransfer {
        GeGl *gl;
        TransferRect rect;
        ~AfterTransfer() {
            if (gl == nullptr) return;
            SectionTimer gl_timer{5};
            gl->after_transfer(rect);
        }
    } after{gl_, dst_rect};
    for (std::uint32_t row = 0; row < height; ++row) {
        const std::uint32_t from = src + ((sy + row) * src_stride + sx) * bpp;
        const std::uint32_t to = dst + ((dy + row) * dst_stride + dx) * bpp;
        const std::uint8_t *s = memory_.raw_pointer(from, width * bpp);
        std::uint8_t *d = memory_.raw_pointer(to, width * bpp);
        if (s != nullptr && d != nullptr) std::memmove(d, s, width * bpp);
    }
}

// ---------------------------------------------------------------------------
// GL backend glue

void Ge::set_gl(GeGl *gl) {
    gl_ = gl;
    if (gl_ != nullptr) gl_->set_vram_write_hook([this](std::uint32_t address, std::uint32_t bytes) { touch_vram(address, bytes); });
}

void Ge::touch_vram(std::uint32_t address, std::uint32_t bytes) {
    const std::uint32_t c = address & 0x1FFFFFFFu;
    if (c < kVramBase || c >= kVramBase + 0x800000u || bytes == 0u) return;
    const std::uint32_t offset = (c - kVramBase) & 0x1FFFFFu;
    const std::uint32_t first = offset >> 12u, last = std::min((offset + bytes - 1u) >> 12u, 511u);
    const std::uint64_t stamp = ++vram_write_counter_;
    for (std::uint32_t page = first; page <= last; ++page) vram_pages_[page] = stamp;
}

std::uint64_t Ge::vram_stamp(std::uint32_t address, std::uint32_t bytes) const {
    const std::uint32_t c = address & 0x1FFFFFFFu;
    const std::uint32_t offset = (c - kVramBase) & 0x1FFFFFu;
    const std::uint32_t first = offset >> 12u, last = std::min((offset + std::max(bytes, 1u) - 1u) >> 12u, 511u);
    std::uint64_t newest = vram_generation_; // software drawing invalidates all of VRAM
    for (std::uint32_t page = first; page <= last; ++page) newest = std::max(newest, vram_pages_[page]);
    return newest;
}

void Ge::submit_gl_state() {
    SectionTimer timer{4};
    const PixelState &p = ps_;
    GlDrawState st{};
    st.fb_offset = p.fb_offset;
    st.fb_stride = p.fb_stride;
    st.fb_format = p.fb_format;
    st.sx1 = p.sx1;
    st.sy1 = p.sy1;
    st.sx2 = p.sx2;
    st.sy2 = p.sy2;
    st.clear = p.clear;
    st.clear_flags = p.clear ? (p.clear_flags & 0x700u) : 0u;
    st.dither = p.dither;
    st.dither_rows = p.dither_rows;
    if (!p.clear) {
        st.z_test = p.z_test;
        st.z_write = p.z_write;
        st.z_func = p.z_test ? p.z_func : 0u;
        st.blend = p.blend;
        if (p.blend) {
            st.src_factor = p.src_factor;
            st.dst_factor = p.dst_factor;
            st.equation = p.equation;
            st.fix_a = regs_[kBlendFixA] & 0xFFFFFFu;
            st.fix_b = regs_[kBlendFixB] & 0xFFFFFFu;
        }
        st.alpha_test = p.alpha_test;
        if (p.alpha_test) {
            st.alpha_func = p.alpha_func;
            st.alpha_ref = p.alpha_ref;
            st.alpha_mask = p.alpha_mask;
        }
        st.stencil = (regs_[0x24] & 1u) != 0u;
        if (st.stencil) {
            st.stencil_func = regs_[0xDC] & 0xFFFFFFu;
            st.stencil_ops = regs_[0xDD] & 0xFFFFFFu;
        }
        st.mask_rgb = regs_[kMaskRgb] & 0xFFFFFFu;
        st.mask_alpha = regs_[kMaskAlpha] & 0xFFu;
        st.texture = p.texture;
        st.through = layout_.through;
        st.fog = p.fog;
        st.fog_color = regs_[kFogColor] & 0xFFFFFFu;
    }
    if (st.texture) {
        st.tex_func = p.tex_func;
        st.tex_alpha = p.tex_alpha;
        st.tex_double = p.tex_double;
        st.env = regs_[kTexEnvColor] & 0xFFFFFFu;
        st.clamp_u = (regs_[kTexWrap] & 1u) != 0u;
        st.clamp_v = (regs_[kTexWrap] & 0x100u) != 0u;
        st.min_linear = (regs_[0xC6] & 1u) != 0u;
        st.mag_linear = (regs_[0xC6] & 0x100u) != 0u;
        // LOD mode/bias also selects min/mag filtering with only level zero,
        // including textures sampled directly from a GPU surface.
        st.lod_mode = regs_[0xC8] & 3u;
        st.lod_bias = static_cast<float>(static_cast<std::int8_t>(regs_[0xC8] >> 16u)) / 16.0f;
        st.lod_slope = cmd_float(regs_[0xD0]);
        const std::uint32_t address = (regs_[kTexAddr0] & 0xFFFFF0u) | ((regs_[kTexBufWidth0] & 0x0F0000u) << 8u);
        const std::uint32_t format = regs_[kTexFormat] & 0xFu;
        const std::uint32_t buf_width = texture_row_width(format,regs_[kTexBufWidth0]);
        if (format <= 3u && ((regs_[0xC6] & 4u) == 0u || ((regs_[kTexMode] >> 16u) & 7u) == 0u) &&
            gl_->is_gpu_target(address, format, buf_width)) {
            st.tex_from_target = true;
            st.tex_address = address;
            st.tex_format = format;
            st.tex_buf_width = buf_width;
            st.tex_width = 1u << std::min(regs_[kTexSize0] & 0xFu, 9u);
            st.tex_height = 1u << std::min((regs_[kTexSize0] >> 8u) & 0xFu, 9u);
        } else {
            bind_texture();
            if (texture_ != nullptr) {
                st.texels = texture_->texels.data();
                st.texture_identity = texture_;
                st.texture_hash = texture_->source_hash;
                st.mip_count = texture_->mip_count;
                st.mip_linear = (regs_[0xC6] & 2u) != 0u;
                for (unsigned level = 1; level <= st.mip_count; ++level)
                    st.mip_texels[level - 1] = texture_->mips[level - 1].data();
                st.tex_width = static_cast<std::uint32_t>(texture_width_);
                st.tex_height = static_cast<std::uint32_t>(texture_height_);
            } else {
                st.texture = false;
            }
        }
    }
    gl_->set_state(st);
}

void Ge::emit_triangle(const Vertex &a, const Vertex &b, const Vertex &c) {
    if (gl_ == nullptr) {
        raster_triangle(a, b, c);
        return;
    }
    ++stats_.triangles;
    const bool flat = (regs_[kShadeMode] & 1u) == 0u;
    auto convert = [&](const Vertex &v) {
        const Vertex &col = flat ? c : v;
        return GlVertex{v.x, v.y, v.z, v.w, v.u, v.v,
                        static_cast<std::uint8_t>(clamp255(col.r)), static_cast<std::uint8_t>(clamp255(col.g)),
                        static_cast<std::uint8_t>(clamp255(col.b)), static_cast<std::uint8_t>(clamp255(col.a)),
                        static_cast<std::uint8_t>(clamp255(col.sr)), static_cast<std::uint8_t>(clamp255(col.sg)),
                        static_cast<std::uint8_t>(clamp255(col.sb)), 0, v.q, v.fog};
    };
    gl_->triangle(convert(a), convert(b), convert(c));
}

void Ge::emit_sprite(const Vertex &a, const Vertex &b) {
    if (ps_.fog && a.fog != b.fog) {
        // GE rectangles select the opposite endpoint's fog in each horizontal
        // half. Preserve the UV/Q split as well as full height for each half.
        Vertex left_a = a, left_b = b, right_a = a, right_b = b;
        left_b.x = right_a.x = (a.x + b.x) * .5f;
        left_b.u = right_a.u = (a.u + b.u) * .5f;
        left_b.q = right_a.q = (a.q + b.q) * .5f;
        left_a.fog = left_b.fog = b.fog;
        right_a.fog = right_b.fog = a.fog;
        emit_sprite(left_a, left_b);
        emit_sprite(right_a, right_b);
        return;
    }
    if (gl_ == nullptr) {
        draw_sprite(a, b);
        return;
    }
    ++stats_.sprites;
    const auto r = static_cast<std::uint8_t>(clamp255(b.r)), g = static_cast<std::uint8_t>(clamp255(b.g));
    const auto bl = static_cast<std::uint8_t>(clamp255(b.b)), al = static_cast<std::uint8_t>(clamp255(b.a));
    const auto sr = static_cast<std::uint8_t>(clamp255(b.sr)), sg = static_cast<std::uint8_t>(clamp255(b.sg));
    const auto sb = static_cast<std::uint8_t>(clamp255(b.sb));
    GlVertex tl{a.x, a.y, b.z, a.w, a.u, a.v, r, g, bl, al, sr, sg, sb, 0, a.q, b.fog};
    GlVertex tr{b.x, a.y, b.z, b.w, b.u, a.v, r, g, bl, al, sr, sg, sb, 0, b.q, b.fog};
    GlVertex bl_{a.x, b.y, b.z, a.w, a.u, b.v, r, g, bl, al, sr, sg, sb, 0, a.q, b.fog};
    GlVertex br{b.x, b.y, b.z, b.w, b.u, b.v, r, g, bl, al, sr, sg, sb, 0, b.q, b.fog};
    tl.sprite_part = tr.sprite_part = bl_.sprite_part = GlVertex::kSpriteFirst;
    gl_->triangle(tl, tr, bl_);
    tr.sprite_part = br.sprite_part = bl_.sprite_part = GlVertex::kSpriteSecond;
    gl_->triangle(tr, br, bl_);
}

} // namespace pspweb
