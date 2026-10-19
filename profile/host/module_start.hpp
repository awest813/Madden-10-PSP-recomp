#pragma once
#include "psprecomp/guest_memory.hpp"

namespace pspweb {
struct ModuleStartParameters {
    std::uint32_t priority = 0x20u;
    std::uint32_t stack = 0x40000u;
    std::uint32_t attributes = 0x80000000u;
};

// sce_module_start_thread_parameter is an exported variable, after the function
// NIDs in the library's two parallel NID/address arrays.
inline ModuleStartParameters module_start_parameters(psprecomp::GuestMemory &memory,
                                                     std::uint32_t begin, std::uint32_t end) {
    ModuleStartParameters result;
    for (auto address = begin; address <= end && end - address >= 16u;) {
        if (!memory.raw_pointer(address, 16u)) break;
        const auto size = static_cast<std::uint32_t>(memory.load8(address + 8u)) * 4u;
        if (size < 16u || size > end - address) break;
        const auto variables = memory.load8(address + 9u);
        const auto functions = memory.load16(address + 10u);
        const auto count = static_cast<std::uint32_t>(variables) + functions;
        const auto table = memory.load32(address + 12u);
        if (count <= 4096u && memory.raw_pointer(table, count * 8u)) {
            for (std::uint32_t i = functions; i < count; ++i) {
                if (memory.load32(table + i * 4u) != 0x0F7C276Cu) continue;
                const auto parameters = memory.load32(table + (count + i) * 4u);
                if (!memory.raw_pointer(parameters, 16u) || memory.load32(parameters) < 3u) continue;
                const auto priority = memory.load32(parameters + 4u);
                const auto stack = memory.load32(parameters + 8u);
                if (priority != 0u) result.priority = priority;
                if (stack != 0u) result.stack = stack;
                result.attributes = memory.load32(parameters + 12u);
            }
        }
        address += size;
    }
    return result;
}
}
