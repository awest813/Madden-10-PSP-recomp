"""Compile and execute synthetic NCAA regressions; no game assets required."""
import argparse
from pathlib import Path
import struct
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--compiler', default='clang++')
parser.add_argument('--tools', type=Path, default=ROOT / 'build/tools')
args = parser.parse_args()
codegen = args.tools / ('psp_recomp.exe' if (args.tools / 'psp_recomp.exe').exists() else 'psp_recomp')
library = next(args.tools.glob('*psprecomp_core.a'), None) or args.tools / 'psprecomp_core.lib'
with tempfile.TemporaryDirectory(prefix='ncaa-regression-', dir=ROOT / 'build') as temporary:
    directory = Path(temporary)
    programs = {
        'pack': [0xD03F8081, 0x03E00008, 0],  # vi2s.q C010,C000; jr ra; nop
        'call': [0x27BDFFF0, 0xAFBF000C, 0x0E201008, 0x24020001,
                 0x8FBF000C, 0x03E00008, 0x27BD0010, 0, 0x03E00008, 0x24420002],
    }
    for name, words in programs.items():
        raw = directory / f'{name}.bin'
        raw.write_bytes(struct.pack(f'<{len(words)}I', *words))
        subprocess.run([str(codegen), '--blob', str(raw), str(directory / f'{name}.inc'), name], check=True)
    (directory / 'disc.manifest').write_bytes(b'D PSP_GAME\r\r\nF 8 PSP_GAME/TEST.BIN\r\r\nF 4096 __umd.iso\r\n')
    source = r'''
#include "psprecomp/runtime.hpp"
#include "module_start.hpp"
#include "webfs.hpp"
#include "display.hpp"
#include "frame_pacer.hpp"
#include "ncaa_wait.hpp"
#include <bit>
#include <cassert>
#include <cmath>
#include <limits>
#include <iostream>
namespace psprecomp {
#include "pack.inc"
#include "call.inc"
}
int main(int argc, char **argv) {
    using namespace psprecomp;
    Runtime rt;
    auto &memory = rt.memory();
    auto view = memory.aot_fast_view();
    // Preserve PSP display latching and reject malformed requests atomically.
    pspweb::DisplayState display;
    assert(display.set_buffer(0x04000000u, 512u, 0u, 1u, rt.memory().size()) == 0u);
    assert(display.get_buffer(0).framebuffer == 0u);
    assert(display.get_buffer(1).framebuffer == 0x04000000u);
    assert(display.format == 0u && display.frame_serial == 0u);
    display.begin_vblank();
    assert(display.framebuffer == 0x04000000u && display.frame_serial == 1u);
    assert(display.set_buffer(0x04000000u, 512u, 0u, 0u, rt.memory().size()) == 0u);
    assert(display.frame_serial == 2u); // same-address redraw is still a frame
    assert(display.set_buffer(0x04040000u, 512u, 0u, 1u, rt.memory().size()) == 0u);
    assert(display.set_buffer(0x44080000u, 512u, 0u, 1u, rt.memory().size()) == 0u);
    display.begin_vblank();
    assert(display.framebuffer == 0x44080000u); // newest pending buffer wins
    const auto flips = display.flips;
    assert(display.set_buffer(0x04000001u, 512u, 0u, 1u, rt.memory().size()) == 0x80000103u);
    assert(display.set_buffer(0x04000000u, 500u, 0u, 1u, rt.memory().size()) == 0x80000104u);
    assert(display.set_buffer(0x04000000u, 512u, 4u, 1u, rt.memory().size()) == 0x80000108u);
    assert(display.set_buffer(0x04000000u, 512u, 3u, 0u, rt.memory().size()) == 0x80000107u);
    assert(display.set_buffer(0x04000000u, 512u, 0u, 2u, rt.memory().size()) == 0x80000107u);
    assert(display.flips == flips && display.framebuffer == 0x44080000u);
    assert(display.set_buffer(0u, 512u, 0u, 0u, rt.memory().size()) == 0u && display.framebuffer == 0u);
    // Simulation remains 59.94 Hz on common monitor refresh rates.
    for (const auto hz : {30.0, 60.0, 120.0, 144.0}) {
        pspweb::FramePacer pacer;
        unsigned ticks = 0;
        for (unsigned i = 0; i < static_cast<unsigned>(hz * 120); ++i) {
            const auto due = pacer.consume(i * 1000.0 / hz);
            assert(due <= 2u);
            ticks += due;
        }
        assert(std::abs(static_cast<int>(ticks) - 7193) <= 2);
    }
    pspweb::FramePacer pacer;
    assert(pacer.consume(0.0) == 1u && pacer.consume(1.0) == 0u);
    assert(pacer.consume(10000.0) == 1u); // resume without old time debt
    assert(pacer.consume(2.0) == 1u); // backwards host clock
    assert(pacer.consume(std::numeric_limits<double>::quiet_NaN()) == 0u);
    pacer.reset();
    assert(pacer.consume(3.0) == 1u);
    assert(pspweb::psp_vblank_time_us(60000u) == 1001000000u);
    AllegrexContext idle{};
    idle.gpr[16] = 0x08900000u;
    idle.gpr[18] = 0x08E30000u;
    idle.gpr[31] = 0x08C9BBC4u;
    memory.store32(idle.gpr[16] + 620u, 2u);
    memory.store32(0x08E34E70u, 1u);
    assert(pspweb::ncaa_waits_for_vblank(memory, idle, true));
    assert(!pspweb::ncaa_waits_for_vblank(memory, idle, false));
    idle.gpr[4] = 1u;
    assert(!pspweb::ncaa_waits_for_vblank(memory, idle, true));
    idle.gpr[4] = 0u; idle.gpr[31] += 4u;
    assert(!pspweb::ncaa_waits_for_vblank(memory, idle, true));
    idle.gpr[31] -= 4u;
    memory.store32(0x08E34E70u, 2u);
    assert(!pspweb::ncaa_waits_for_vblank(memory, idle, true));
    idle.gpr[16] = 0xFFFFFFFFu;
    assert(!pspweb::ncaa_waits_for_vblank(memory, idle, true));
    for (const auto base : {0x08900000u, 0x09123400u}) {
        AllegrexContext ctx{};
        ctx.eat_vfpu_prefixes();
        const float source[] = {std::bit_cast<float>(0x12345678u), std::bit_cast<float>(0x89ABCDEFu),
                                std::bit_cast<float>(0x7FFF0000u), std::bit_cast<float>(0x80000000u)};
        ctx.write_vfpu_vector(source, 0u, 4u);
        ctx.pc = base;
        ctx.gpr[31] = 0x0BAD0000u;
        pack_entry(rt, ctx, 0u, view, base);
        float output[4]{};
        ctx.read_vfpu_vector(output, 1u, 2u);
        assert(std::bit_cast<std::uint32_t>(output[0]) == 0x89AB1234u);
        assert(std::bit_cast<std::uint32_t>(output[1]) == 0x80007FFFu);
        assert(ctx.pc == 0x0BAD0000u);
        // Destination mask preserves the second packed lane and prefixes reset.
        const float preserved[] = {0.0f, std::bit_cast<float>(0xDEADBEEFu)};
        ctx.write_vfpu_vector(preserved, 1u, 2u);
        ctx.vfpu_ctrl[2] = 1u << 9u;
        ctx.pc = base;
        pack_entry(rt, ctx, 0u, view, base);
        ctx.read_vfpu_vector(output, 1u, 2u);
        assert(std::bit_cast<std::uint32_t>(output[0]) == 0x89AB1234u);
        assert(std::bit_cast<std::uint32_t>(output[1]) == 0xDEADBEEFu);
        assert(ctx.vfpu_ctrl[2] == 0u);
        // The guest relocates JAL after loading. Test its delay slot and return.
        memory.store32(base + 8u, 0x0C000000u | ((base + 32u) >> 2u));
        ctx = AllegrexContext{};
        ctx.pc = base;
        ctx.gpr[29] = 0x09F00000u;
        ctx.gpr[31] = 0x0BAD0000u;
        for (unsigned step = 0; step < 10 && ctx.pc != 0x0BAD0000u; ++step)
            call_entry(rt, ctx, 0u, view, base);
        assert(ctx.pc == 0x0BAD0000u && ctx.gpr[2] == 3u);
        assert(ctx.gpr[29] == 0x09F00000u);
    }
    // One function export precedes the start-thread variable in both arrays.
    constexpr auto record = 0x08810000u, table = record + 32u, parameters = record + 64u;
    memory.store8(record + 8u, 4u);
    memory.store8(record + 9u, 1u);
    memory.store16(record + 10u, 1u);
    memory.store32(record + 12u, table);
    memory.store32(table, 0xD632ACDBu);
    memory.store32(table + 4u, 0x0F7C276Cu);
    memory.store32(table + 8u, 0x08804000u);
    memory.store32(table + 12u, parameters);
    memory.store32(parameters, 3u);
    memory.store32(parameters + 4u, 0x20u);
    memory.store32(parameters + 8u, 0x400u);
    memory.store32(parameters + 12u, 0x80000000u);
    auto boot = pspweb::module_start_parameters(memory, record, record + 16u);
    assert(boot.stack == 0x400u && boot.priority == 0x20u && boot.attributes == 0x80000000u);
    memory.store32(parameters + 8u, 0u);
    assert(pspweb::module_start_parameters(memory, record, record + 16u).stack == 0x40000u);
    memory.store32(table + 12u, 0xFFFFFFFFu);
    assert(pspweb::module_start_parameters(memory, record, record + 16u).stack == 0x40000u);
    assert(pspweb::module_start_parameters(memory, record, record + 8u).stack == 0x40000u);
    pspweb::WebFs fs;
    assert(fs.load_manifest(argv[1], "."));
    assert(fs.find("PSP_GAME/TEST.BIN") && fs.find("__umd.iso"));
    assert(fs.find("__umd.iso")->size == 4096u);
    std::cout << "PASS: display latch/errors, 30/60/120/144 Hz pacing, guarded NCAA wait, vi2s packing/masks, relocated AOT JAL/delay/return, module stack exports, CRLF manifest\n";
}
'''
    harness = directory / 'test.cpp'
    harness.write_text(source)
    executable = directory / 'test.exe'
    subprocess.run([args.compiler, '-std=c++20', '-O1', '-pthread',
                    f'-I{ROOT / "PSPRecomp/include"}', f'-I{ROOT / "profile/host"}',
                    str(harness), str(ROOT / 'profile/host/webfs.cpp'), str(library), '-o', str(executable)], check=True)
    subprocess.run([str(executable), str(directory / 'disc.manifest')], check=True)
