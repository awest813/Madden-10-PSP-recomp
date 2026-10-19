#include "kernel.hpp"
#include <cassert>
#include <cstdlib>
#include <iostream>

using psprecomp::Runtime;
namespace {
constexpr std::uint32_t init = 0xF6269B82u, status = 0xF3F76017u, shutdown = 0x3DFAEBA9u;
constexpr std::uint32_t params = 0x08900000u, data = params + 0x100u;
constexpr std::uint32_t initial = params + 0x200u, output = params + 0x300u;
std::uint32_t call(Runtime &rt, std::uint32_t nid, std::uint32_t arg = 0u) {
    psprecomp::AllegrexContext ctx{}; ctx.gpr[4] = arg;
    rt.invoke_import("sceUtility", nid, ctx); return ctx.gpr[2];
}
void setup(Runtime &rt) {
    auto &m = rt.memory();
    for (unsigned i = 0; i < 0x400u; i += 4u) m.store32(params + i, 0u);
    m.store32(params, 64u);
    m.store32(params + 48u, 1u); m.store32(params + 52u, data);
    m.store32(data + 32u, initial); m.store32(data + 36u, 6u);
    m.store32(data + 40u, output); m.store32(data + 48u, 5u);
    m.store16(initial, u'A'); m.store16(initial + 2u, u'B');
    m.store16(output + 12u, 0xBEEFu); // guard immediately after output capacity
}
void close(Runtime &rt) {
    assert(call(rt, status) == 2u);
    assert(call(rt, status) == 3u);
    assert(call(rt, shutdown) == 0u);
    assert(call(rt, status) == 4u);
    assert(call(rt, status) == 0u);
}
}
int main() {
#ifdef _WIN32
    _putenv_s("PSPWEB_OSK_WAIT", "1");
#else
    setenv("PSPWEB_OSK_WAIT", "1", 1);
#endif
    Runtime rt;
    pspweb::Kernel kernel(rt); kernel.install({});
    assert(call(rt, status) == 0u); // pre-init query used by NCAA save screen
    setup(rt);
    assert(call(rt, init, params) == 0u);
    assert(call(rt, init, params) != 0u); // don't replace an open dialog
    assert(call(rt, status) == 1u && call(rt, status) == 2u);
    assert(call(rt, shutdown) != 0u); // still waiting for text
    kernel.osk_set_field(0u, u"NCAATEST"); kernel.osk_complete(false);
    close(rt);
    auto &m = rt.memory();
    assert(m.load32(data + 44u) == 2u);
    for (unsigned i = 0; i < 5u; ++i) assert(m.load16(output + i * 2u) == u"NCAAT"[i]);
    assert(m.load16(output + 10u) == 0u && m.load16(output + 12u) == 0xBEEFu);
    setup(rt); assert(call(rt, init, params) == 0u); assert(call(rt, status) == 1u);
    kernel.osk_set_field(0u, u"X"); kernel.osk_complete(true); close(rt);
    assert(m.load32(data + 44u) == 1u && m.load16(output) == u'A' && m.load16(output + 2u) == u'B');
    setup(rt); assert(call(rt, init, params) == 0u); assert(call(rt, status) == 1u);
    kernel.osk_complete(false); close(rt); assert(m.load32(data + 44u) == 0u);
    setup(rt); m.store32(data + 48u, 2u);
    assert(call(rt, init, params) == 0u); assert(call(rt, status) == 1u);
    kernel.osk_set_field(0u, u"A\U0001F3C8"); kernel.osk_complete(false); close(rt);
    assert(m.load16(output) == u'A' && m.load16(output + 2u) == 0u); // no split surrogate
    setup(rt); m.store32(data + 36u, 0u);
    assert(call(rt, init, params) != 0u && call(rt, status) == 0u);
    std::cout << "Utility keyboard: idle/init/visible/quit/shutdown, UTF-16 bounds, changed/unchanged/canceled results passed\n";
}
