#pragma once
#include "psprecomp/runtime.hpp"

namespace psprecomp {
struct MaddenSoundProgram {
    const std::uint32_t *words;
    std::uint32_t word_count;
    void (*entry)(Runtime &, AllegrexContext &, std::uint16_t, GuestMemory::AotFastView &, std::uint32_t);
};
extern const MaddenSoundProgram madden_sound_programs[];
extern const std::uint32_t madden_sound_program_count;
void register_madden_sound_programs(Runtime &runtime);
void reset_madden_sound_programs();
}
