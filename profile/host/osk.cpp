// PSP utility keyboard ABI from PSPSDK's BSD-licensed psputility_osk.h.
#include "kernel.hpp"
#include <algorithm>
#include <cstdlib>
#include <iostream>
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

namespace pspweb {
#ifdef __EMSCRIPTEN__
namespace {
std::string json_utf16(const std::u16string &text) {
    constexpr char digits[] = "0123456789abcdef";
    std::string result = "\"";
    for (const auto ch : text) {
        result += "\\u";
        for (int shift = 12; shift >= 0; shift -= 4) result += digits[(ch >> shift) & 15u];
    }
    return result + "\"";
}
}
#endif
void Kernel::sceUtilityOskInitStart(Ctx &ctx) {
    if (osk_dialog_.status != 0u) { finish(ctx, 0x80110001u); return; }
    auto &m = rt_.memory();
    const auto params = ctx.gpr[4];
    if (!m.raw_pointer(params, 64u)) { finish(ctx, 0x80110004u); return; }
    const auto count = m.load32(params + 48u), data = m.load32(params + 52u);
    if (count == 0u || count > 16u || !m.raw_pointer(data, count * 52u)) {
        finish(ctx, 0x80110004u); return;
    }
    std::vector<OskField> fields;
    for (std::uint32_t i = 0; i < count; ++i) {
        const auto at = data + i * 52u;
        OskField field;
        field.address = at;
        field.capacity = m.load32(at + 36u);
        field.output = m.load32(at + 40u);
        if (field.capacity == 0u || field.capacity > 65536u || !m.raw_pointer(field.output, field.capacity * 2u)) {
            finish(ctx, 0x80110004u); return;
        }
        const auto limit = m.load32(at + 48u);
        field.limit = std::min(field.capacity - 1u, limit != 0u ? limit : field.capacity - 1u);
        const auto input = m.load32(at + 32u);
        for (std::uint32_t j = 0; input != 0u && j < field.limit; ++j) {
            if (!m.raw_pointer(input + j * 2u, 2u)) break;
            const auto ch = m.load16(input + j * 2u);
            if (ch == 0u) break;
            field.initial.push_back(static_cast<char16_t>(ch));
        }
        field.text = field.initial;
        fields.push_back(std::move(field));
    }
    osk_fields_ = std::move(fields);
    osk_dialog_ = UtilityDialog{1u, params};
    osk_done_ = false;
    m.store32(params + 28u, 0u);
    m.store32(params + 56u, 1u);
    std::cerr << "[utility] keyboard: " << count << " field(s)\n";
#ifdef __EMSCRIPTEN__
    // PSP virtual pointers are not WebAssembly heap pointers. Resolve all
    // guest strings here and send plain field data to the page.
    std::string json = "[";
    for (std::size_t i = 0; i < osk_fields_.size(); ++i) {
        const auto &field = osk_fields_[i];
        const auto description = m.load32(field.address + 28u);
        std::u16string label;
        for (std::uint32_t j = 0; description != 0u && j < 1024u; ++j) {
            if (!m.raw_pointer(description + j * 2u, 2u)) break;
            const auto ch = m.load16(description + j * 2u);
            if (ch == 0u) break;
            label.push_back(static_cast<char16_t>(ch));
        }
        if (i) json += ',';
        json += "{\"label\":" + json_utf16(label) + ",\"initial\":" + json_utf16(field.initial) +
                ",\"limit\":" + std::to_string(field.limit) + "}";
    }
    json += ']';
    if (EM_ASM_INT({ return Module.pspOskDialog ? (Module.pspOskDialog(JSON.parse(UTF8ToString($0))), 1) : 0; }, json.c_str())) {
        finish(ctx, 0u); return;
    }
#else
    if (std::getenv("PSPWEB_OSK_WAIT")) { finish(ctx, 0u); return; }
    // Headless tests can give a unique ASCII save name. Interactive browsers
    // use the text fields below; a native run otherwise retains initial text.
    if (const auto *text = std::getenv("PSPWEB_OSK_TEXT")) {
        std::u16string value;
        for (const auto *p = text; *p; ++p) value.push_back(static_cast<unsigned char>(*p));
        osk_set_field(0u, value);
    }
#endif
    osk_complete(false);
    finish(ctx, 0u);
}

void Kernel::osk_set_field(std::uint32_t index, const std::u16string &text) {
    if (osk_dialog_.status == 0u || osk_done_ || index >= osk_fields_.size()) return;
    auto &field = osk_fields_[index];
    field.text = text.substr(0u, field.limit);
    // Never return half a UTF-16 surrogate pair at the length limit.
    if (!field.text.empty() && field.text.back() >= 0xD800u && field.text.back() <= 0xDBFFu)
        field.text.pop_back();
}

void Kernel::osk_complete(bool canceled) {
    if (osk_dialog_.status == 0u || osk_done_) return;
    auto &m = rt_.memory();
    for (const auto &field : osk_fields_) {
        const auto &text = canceled ? field.initial : field.text;
        for (std::size_t i = 0; i < text.size(); ++i)
            m.store16(field.output + static_cast<std::uint32_t>(i) * 2u, text[i]);
        m.store16(field.output + static_cast<std::uint32_t>(text.size()) * 2u, 0u);
        m.store32(field.address + 44u, canceled ? 1u : text == field.initial ? 0u : 2u);
    }
    osk_done_ = true;
    m.store32(osk_dialog_.params + 56u, 4u);
}

void Kernel::sceUtilityOskGetStatus(Ctx &ctx) {
    const auto status = osk_dialog_.status;
    if (status == 1u) osk_dialog_.status = 2u;
    else if (status == 2u && osk_done_) osk_dialog_.status = 3u;
    else if (status == 4u) {
        osk_dialog_.status = 0u;
        rt_.memory().store32(osk_dialog_.params + 56u, 5u);
    }
    finish(ctx, status);
}

void Kernel::sceUtilityOskShutdownStart(Ctx &ctx) {
    if (osk_dialog_.status != 3u) { finish(ctx, 0x80110001u); return; }
    osk_dialog_.status = 4u;
    finish(ctx, 0u);
}
} // namespace pspweb
