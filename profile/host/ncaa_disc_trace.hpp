#pragma once
#include "psprecomp/runtime.hpp"
#include <ostream>

namespace pspweb {
// Observation only: the generated reader resumes with the original context.
inline void trace_ncaa_disc_reader(const psprecomp::GuestMemory &memory,
                                   const psprecomp::AllegrexContext &ctx, std::ostream &out) {
    const auto sp = ctx.gpr[29];
    out << "[ncaa-disc] fd=0x" << std::hex << memory.load32(sp + 32u)
        << " buffer=0x" << memory.load32(sp + 36u)
        << " size=0x" << memory.load32(sp + 52u)
        << " offset=0x" << memory.load32(sp + 48u)
        << " caller=0x" << memory.load32(sp + 104u) << " sp=0x" << sp << std::dec << '\n';
    if (memory.load32(sp + 36u) == 0u) {
        out << "[ncaa-disc-stack]" << std::hex;
        for (unsigned i = 0; i < 80u; ++i) out << " " << memory.load32(sp + i * 4u);
        out << std::dec << '\n';
    }
}
}
