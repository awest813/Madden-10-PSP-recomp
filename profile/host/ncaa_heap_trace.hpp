#pragma once
#include "psprecomp/runtime.hpp"
#include "psprecomp/common.hpp"
#include <ostream>

namespace pspweb {
// Opt-in instruction observations, without altering guest state. Unlike the
// dispatch hooks, this includes same-unit allocator returns and free entries.
inline void trace_ncaa_heap_instruction(const psprecomp::Runtime &rt,
        const psprecomp::AllegrexContext &c, std::uint32_t pc,
        std::uint32_t vcount, std::ostream &out) {
    const auto &m=rt.memory();
    if (pc==0x08CD3224u && c.gpr[16]==0u) {
        const auto sp=c.gpr[29], continuation=m.load32(sp+512u);
        out << "[ncaa-heap-ledger] event=alloc size=" << psprecomp::hex32(c.gpr[17])
            << " pointer=" << psprecomp::hex32(c.gpr[20])
            << " alignment=" << psprecomp::hex32(c.gpr[22])
            << " flags=" << psprecomp::hex32(c.gpr[18])
            << " continuation=" << psprecomp::hex32(continuation)
            << " caller=" << psprecomp::hex32(
                continuation==0x08CD326Cu || continuation==0x08CD3BB0u ? m.load32(sp+556u) : 0u)
            << " vcount=" << vcount << " thread=" << psprecomp::runtime_thread_name() << '\n';
    }
    if (pc==0x08C8225Cu && c.gpr[4]>=16u && m.contains(c.gpr[4]-16u,16u)) {
        const auto header=c.gpr[4]-16u;
        if (((m.load32(header)>>16u)&63u)!=0u) return;
        out << "[ncaa-heap-ledger] event=free-entry pointer=" << psprecomp::hex32(c.gpr[4])
            << " requested-size=" << psprecomp::hex32(m.load32(header+4u))
            << " caller=" << psprecomp::hex32(c.gpr[31])
            << " owner-caller=" << psprecomp::hex32(
                c.gpr[31]==0x08CD3B54u ? m.load32(c.gpr[29]+4u) : 0u)
            << " vcount=" << vcount << " thread=" << psprecomp::runtime_thread_name() << '\n';
    }
}
}
