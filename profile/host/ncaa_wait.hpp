#pragma once
#include "psprecomp/runtime.hpp"

namespace pspweb {
// ULUS10440: EA's display wait calls EAThreadYield(0) until the registered
// Vblank handler increments 08E34E70 to the frame object's requested count.
// Only this caller may sleep: other EAThreadYield uses still execute as AOT.
inline bool ncaa_waits_for_vblank(psprecomp::GuestMemory &memory,
                                  const psprecomp::AllegrexContext &ctx,
                                  bool handler_enabled) {
    if (!handler_enabled || ctx.gpr[4] != 0u || ctx.gpr[31] != 0x08C9BBC4u ||
        ctx.gpr[18] != 0x08E30000u || !memory.raw_pointer(ctx.gpr[16], 624u)) return false;
    return memory.load32(0x08E34E70u) < memory.load32(ctx.gpr[16] + 620u);
}
}
