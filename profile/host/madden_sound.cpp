#include "madden_sound.hpp"
#include <cstring>
#include <iostream>
#include <map>

namespace psprecomp {
namespace {
struct Binding { const MaddenSoundProgram *program; std::uint32_t base; };
std::map<std::uint32_t, Binding> bindings;
void execute_sound_program(Runtime &runtime, AllegrexContext &ctx) {
    const auto binding = bindings.at(ctx.pc);
    auto memory = runtime.memory().aot_fast_view();
    binding.program->entry(runtime, ctx, 0u, memory, binding.base);
}
std::uint32_t unlinked(std::uint32_t word) {
    const auto opcode = word >> 26u;
    return opcode == 2u || opcode == 3u ? word & 0xFC000000u : word;
}
}

void reset_madden_sound_programs() { bindings.clear(); }

void register_madden_sound_programs(Runtime &runtime) {
    constexpr std::uint32_t begin = 0x08F3C000u;
    const auto end = GuestMemory::kPhysicalBase + runtime.memory().size();
    const auto *ram = runtime.memory().raw_pointer(begin, end - begin);
    if (!ram) return;
    std::uint32_t added = 0;
    for (std::uint32_t offset = 0; offset + 16u <= end - begin; offset += 4u) {
        std::uint32_t first;
        std::memcpy(&first, ram + offset, 4u);
        if (first != 0x27BDFFF8u) continue;
        const auto base = begin + offset;
        for (std::uint32_t index = 0; index < madden_sound_program_count; ++index) {
            const auto &program = madden_sound_programs[index];
            if (offset + program.word_count * 4u > end - begin) continue;
            bool matches = true;
            for (std::uint32_t word = 0; word < program.word_count; ++word) {
                std::uint32_t actual;
                std::memcpy(&actual, ram + offset + word * 4u, 4u);
                if (unlinked(actual) != unlinked(program.words[word])) { matches = false; break; }
            }
            if (!matches) continue;
            for (std::uint32_t word = 0; word < program.word_count; ++word) {
                const auto pc = base + word * 4u;
                const auto old = bindings.find(pc);
                if (old == bindings.end() || old->second.program != &program || old->second.base != base) {
                    bindings[pc] = Binding{&program, base};
                    runtime.register_function(pc, &execute_sound_program, "madden10_audio_asset_aot");
                    ++added;
                }
            }
            break;
        }
    }
    if (added) std::cerr << "[madden10] linked " << added << " AOT audio instruction entries\n";
}
}
