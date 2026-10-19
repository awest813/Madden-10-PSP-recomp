#pragma once
#include "psprecomp/runtime.hpp"

namespace psprecomp {
struct NcaaSoundProgram {
    const std::uint32_t *words;
    std::uint32_t word_count;
    void (*entry)(Runtime &, AllegrexContext &, std::uint16_t, GuestMemory::AotFastView &, std::uint32_t);
};
extern const NcaaSoundProgram ncaa_sound_programs[];
extern const std::uint32_t ncaa_sound_program_count;
void register_ncaa_sound_programs(Runtime &runtime);
void reset_ncaa_sound_programs();
}
