// Host entry for the web profile.  The same code builds natively (headless,
// dumps the framebuffer to a PPM for checking) and with Emscripten (presents
// every frame to a <canvas> from requestAnimationFrame).

#include "ge_gl.hpp"
#include "kernel.hpp"
#include "module_start.hpp"
#include "frame_pacer.hpp"
#include "frame_batch.hpp"
#ifdef PSPWEB_NCAA10
#include "ncaa_sound.hpp"
#include "ncaa_disc_trace.hpp"
#include "ncaa_pointer_trace.hpp"
#include "ncaa_heap_trace.hpp"
#include "../generated/ncaa10/generated_units.hpp"
#endif
#ifdef PSPWEB_MADDEN10
#include "madden_sound.hpp"
#include "../generated/madden10/generated_units.hpp"
#endif

#include "psprecomp/common.hpp"
#include "psprecomp/elf32.hpp"
#include "psprecomp/runtime.hpp"
#include "psprecomp/instruction_trace.hpp"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#if defined(__EMSCRIPTEN__)
#include <emscripten/emscripten.h>
#include <emscripten/html5.h>
#else
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <atomic>
#include <chrono>
#include <thread>

#include "sdl_frontend.hpp"
#include "native_replay_control.hpp"
#endif

namespace {

constexpr std::uint32_t kWidth = 480u;
constexpr std::uint32_t kHeight = 272u;

struct Session {
    std::unique_ptr<psprecomp::Runtime> runtime;
    std::unique_ptr<pspweb::Kernel> kernel;
    std::unique_ptr<pspweb::GeGl> gl;     // null: software rendering; lives on the GE thread
    GLuint out_fbo{}, out_texture{};       // native presentation target
    std::vector<std::uint8_t> rgba = std::vector<std::uint8_t>(kWidth * kHeight * 4u);
    std::atomic<int> scale{1};             // GL internal resolution multiple (set on the GE thread)
    int max_scale{1};
    bool use_gl{};                         // the GL renderer is wanted (it may still be starting)
    std::atomic<bool> gl_ready{false};     // set on the GE thread once the renderer is up
    bool running{};
    bool trace_loading_layout_seen{}; // opt-in NCAA allocation diagnostics only
    bool trace_heap_initial_captured{}, trace_heap_target_captured{}, trace_heap_next_query_captured{};
};

Session g_session;
#ifdef PSPWEB_NCAA10
pspweb::NcaaPointerTrace g_pointer_trace;
#endif

#ifdef PSPWEB_NCAA10
// Only called by the opt-in ULUS10440 diagnostics below. A resource's path
// pointer is at +4; its slot count and data/flags tables are at +20/+128/+132.
void trace_ncaa_resource(const psprecomp::GuestMemory &memory,
                         std::uint32_t resource, std::uint32_t index) {
    if (!memory.raw_pointer(resource, 136u)) return;
    const auto count = memory.load16(resource + 20u);
    std::cerr << " resource=0x" << std::hex << resource << " slot=0x" << index
              << " slots=0x" << count << " type=0x" << memory.load16(resource + 100u);
    auto path = memory.load32(resource + 4u);
    std::string name;
    for (unsigned i = 0; i < 256u && memory.raw_pointer(path + i, 1u); ++i) {
        const auto c = memory.load8(path + i);
        if (!c) break;
        if (c < 32u || c > 126u) { name.clear(); break; }
        name.push_back(static_cast<char>(c));
    }
    if (!name.empty()) std::cerr << " path=" << name;
    if (index < count) {
        const auto data = memory.load32(resource + 128u) + index * 4u;
        const auto flags = memory.load32(resource + 132u) + index;
        if (memory.raw_pointer(data, 4u)) std::cerr << " slot-buffer=0x" << memory.load32(data);
        if (memory.raw_pointer(flags, 1u)) std::cerr << " slot-flags=0x" << unsigned(memory.load8(flags));
    }
}
#endif

bool start_session(const std::string &module_path, const std::string &disc_root,
                   const std::string &memstick_root, const std::string &guest_path,
                   const std::string &manifest = {}, const std::string &data_url = {}) {
    auto &s = g_session;
    s.trace_loading_layout_seen = false;
    s.trace_heap_initial_captured = s.trace_heap_target_captured = false;
    s.trace_heap_next_query_captured = false;
#ifdef PSPWEB_NCAA10
    psprecomp::reset_ncaa_sound_programs();
    g_pointer_trace.reset();
#endif
#ifdef PSPWEB_MADDEN10
    psprecomp::reset_madden_sound_programs();
#endif
    // NCAA and Madden need the larger layout for cold stadium loads. Other titles
    // retain their original default; either layout can be selected explicitly.
    // Let the title size its own heaps from the backed SysMem partition.
#if defined(PSPWEB_NCAA10) || defined(PSPWEB_MADDEN10)
    constexpr std::uint32_t default_ram_mb = 64u;
#else
    constexpr std::uint32_t default_ram_mb = 32u;
#endif
    const char *ram_option = std::getenv("PSPWEB_RAM_MB");
    if (ram_option && std::strcmp(ram_option, "32") != 0 && std::strcmp(ram_option, "64") != 0) {
        std::cerr << "[pspweb] PSPWEB_RAM_MB must be 32 or 64\n";
#ifdef __EMSCRIPTEN__
        EM_ASM({ if (Module.pspStatus) Module.pspStatus('RAM must be 32 or 64 MiB'); });
#endif
        return false;
    }
    const std::uint32_t ram_mb = ram_option ? (std::strcmp(ram_option, "64") == 0 ? 64u : 32u) : default_ram_mb;
    s.runtime = std::make_unique<psprecomp::Runtime>(ram_mb << 20u);
    std::cout << "[pspweb] guest RAM=" << ram_mb << " MiB, user partition=" << ram_mb - 8u << " MiB\n";
    psprecomp::reset_aot_instruction_trace();
    psprecomp::set_aot_instruction_observer(nullptr);
    auto &rt = *s.runtime;
    const auto elf = psprecomp::Elf32Image::from_file(module_path);
    const std::uint32_t load_base = psprecomp::kDefaultPspUserLoadBase;
    const auto relocations = elf.load_and_relocate(rt.memory(), load_base);
    const auto module = elf.find_module_info(rt.memory(), load_base);
    if (!module) {
        std::cerr << "[pspweb] no module info in " << module_path << "\n";
        return false;
    }
    const auto imports = elf.scan_imports(rt.memory(), *module);

    std::uint32_t image_end = load_base;
    for (std::size_t i = 0; i < elf.segments().size(); ++i) {
        const auto &segment = elf.segments()[i];
        if (segment.type != 1u) continue;
        image_end = std::max(image_end, elf.segment_runtime_address(i, load_base) + segment.memory_size);
    }

    psprecomp::register_generated_functions(rt);
#ifdef PSPWEB_MADDEN10
    if (module->name == "MaddenNFL2006" && std::getenv("PSPWEB_PLAY_MOVIES") == nullptr) {
        rt.register_function(0x08CDDB64u, [](psprecomp::Runtime &runtime, psprecomp::AllegrexContext &ctx) {
            std::string movie;
            for (unsigned i = 0; i < 256u; ++i) {
                const auto c = runtime.memory().load8(ctx.gpr[4] + i);
                if (!c) break;
                movie.push_back(static_cast<char>(c));
            }
            std::cerr << "[madden10] skipping unsupported intro movie: " << movie << "\n";
            ctx.gpr[2] = 0u;
            ctx.pc = ctx.gpr[31];
        }, "madden10_skip_movie");
    }
#endif
#ifdef PSPWEB_NCAA10
    // ULUS10440's movie player assumes sceMpeg produces a decoded texture.
    // The host has no AVC decoder yet. Skip this optional movie routine at
    // its public entry, before it creates a streamer or dereferences a frame.
    // PSPWEB_PLAY_MOVIES allows decoder development without this title patch.
    if (module->name == "NCAA2007" && std::getenv("PSPWEB_PLAY_MOVIES") == nullptr) {
        rt.register_function(0x08CBC838u, [](psprecomp::Runtime &runtime, psprecomp::AllegrexContext &ctx) {
            std::string movie;
            for (unsigned i = 0; i < 256u; ++i) {
                const auto c = runtime.memory().load8(ctx.gpr[4] + i);
                if (!c) break;
                movie.push_back(static_cast<char>(c));
            }
            std::cerr << "[ncaa10] skipping unsupported intro movie: " << movie << "\n";
            ctx.gpr[2] = 0u;
            ctx.pc = ctx.gpr[31];
        }, "ncaa10_skip_movie");
    }
    if (module->name == "NCAA2007" && std::getenv("PSPWEB_TRACE_NCAA_IO") != nullptr) {
        psprecomp::set_aot_instruction_observer([](const psprecomp::Runtime &runtime,
                const psprecomp::AllegrexContext &ctx, std::uint32_t pc, std::uint32_t word, bool delay) {
            g_pointer_trace.observe(runtime, ctx, pc, word, delay,
                                    g_session.kernel->display().vcount, std::cerr);
            static const bool heap_ledger = std::getenv("PSPWEB_TRACE_NCAA_HEAP") != nullptr;
            if (heap_ledger) pspweb::trace_ncaa_heap_instruction(runtime, ctx, pc,
                                    g_session.kernel->display().vcount, std::cerr);
        });
        // Observe resumes after the low-level allocator, rather than treating
        // a yielded allocation entry as a result. These sites retain the size,
        // heap tag and alignment in s1/s0/s6; v0 is the returned buffer.
        // Local generated transfers may bypass an observer, so this is a
        // diagnostic sample, not an exhaustive allocation ledger.
        for (const auto pc : {0x08CD31A4u, 0x08CD31CCu, 0x08CD31E8u}) {
            rt.register_function(pc, [](psprecomp::Runtime &runtime, psprecomp::AllegrexContext &ctx) {
                if (ctx.gpr[17] >= 0x10000u || ctx.gpr[17] == 0x4450u ||
                    ctx.gpr[17] == 0x79C0u || ctx.gpr[17] == 0x9340u ||
                    ctx.gpr[17] == 0xE57Du || ctx.gpr[17] == 0xE68Au || ctx.gpr[2] == 0u) {
                    const auto &memory = runtime.memory();
                    const auto sp = ctx.gpr[29];
                    const auto continuation = memory.load32(sp + 512u);
                    std::cerr << "[ncaa-disc-alloc-result] pc=0x" << std::hex << ctx.pc
                              << " size=0x" << ctx.gpr[17] << " buffer=0x" << ctx.gpr[2]
                              << " heap=0x" << ctx.gpr[16] << " align=0x" << ctx.gpr[22]
                              << " flags=0x" << ctx.gpr[18]
                              << " continuation=0x" << continuation
                              << " vcount=" << std::dec << g_session.kernel->display().vcount
                              << " thread=" << psprecomp::runtime_thread_name() << std::hex;
                    if (continuation == 0x08CD3BB0u || continuation == 0x08CD326Cu)
                        std::cerr << " caller=0x" << memory.load32(sp + 556u);
                    std::cerr << std::dec << '\n';
                    const auto caller = (continuation == 0x08CD3BB0u || continuation == 0x08CD326Cu)
                                            ? memory.load32(sp + 556u) : 0u;
                    if (ctx.gpr[17] == 0x4450u && caller == 0x08CD85B0u && memory.raw_pointer(sp + 608u, 8u)) {
                        // CD83A4 saves the resource and slot in its 64-byte
                        // frame, above the allocator's 528+32-byte frames.
                        std::cerr << "[ncaa-disc-menu-alloc-owner] buffer=0x" << std::hex << ctx.gpr[2];
                        trace_ncaa_resource(memory, memory.load32(sp + 608u), memory.load32(sp + 612u));
                        std::cerr << std::dec << '\n';
                    }
                    const bool asset_path = continuation == 0x08CD3BB0u && caller == 0x08CC4728u;
                    const bool bank_path = continuation == 0x08CD326Cu && caller == 0x08B31A9Cu;
                    if ((asset_path || bank_path) && memory.raw_pointer(sp + 560u, asset_path ? 304u : 288u)) {
                        std::string path;
                        for (unsigned i = 0; i < 256u; ++i) {
                            const auto c = memory.load8(sp + 560u + i);
                            if (!c) break;
                            path.push_back(static_cast<char>(c));
                        }
                        std::cerr << "[ncaa-disc-alloc-asset] path=" << path << " caller=0x"
                                  << std::hex << memory.load32(sp + (asset_path ? 852u : 836u))
                                  << std::dec << '\n';
                    }
                }
                psprecomp::recomp_unit_0307(runtime, ctx);
            }, "ncaa10_trace_disc_alloc_result");
        }
        // The loading-loop sound banks occupy 0x79C0/0x9340 bytes. In a failed
        // daytime start one bank separated the two largest free regions.
        // Sample its free entry without changing the guest's lifetime/order.
        // A 0x4450 menu allocation also bounds the bank's selected free region;
        // track its release to distinguish placement from bank lifetime.
        rt.register_function(0x08C8225Cu, [](psprecomp::Runtime &runtime, psprecomp::AllegrexContext &ctx) {
            const auto &memory = runtime.memory();
            const auto buffer = ctx.gpr[4], caller = ctx.gpr[31];
            bool sample = false;
            bool menu = false;
            if (buffer >= 0x08800000u && memory.raw_pointer(buffer - 16u, 16u)) {
                const auto size = memory.load32(buffer - 12u);
                menu = size == 0x4450u;
                sample = (menu && g_session.trace_loading_layout_seen) ||
                         (buffer >= 0x09D00000u && (size == 0x79C0u || size == 0x9340u));
                if (sample) {
                    std::cerr << (menu ? "[ncaa-disc-menu-free-entry] buffer=0x" : "[ncaa-disc-bank-free-entry] buffer=0x") << std::hex << buffer
                              << " allocation-size=0x" << size << " caller=0x" << caller;
                    if (caller == 0x08CD3B54u && memory.raw_pointer(ctx.gpr[29], 16u)) {
                        const auto owner = memory.load32(ctx.gpr[29] + 4u);
                        std::cerr << " owner-caller=0x" << owner;
                        // The free wrapper temporarily replaces s0 with the
                        // buffer, saving the resource slot at sp+0. s2 is intact.
                        if (owner == 0x08CD8A50u)
                            trace_ncaa_resource(memory, ctx.gpr[18], memory.load32(ctx.gpr[29]));
                    }
                    std::cerr << std::dec << " vcount=" << g_session.kernel->display().vcount
                              << " thread=" << psprecomp::runtime_thread_name() << '\n';
                }
            }
            psprecomp::recomp_unit_0287(runtime, ctx);
            if (sample && ctx.pc == caller)
                std::cerr << (menu ? "[ncaa-disc-menu-free-return] buffer=0x" : "[ncaa-disc-bank-free-return] buffer=0x") << std::hex << buffer
                          << " result=0x" << ctx.gpr[2] << std::dec << '\n';
        }, "ncaa10_trace_loading_bank_free");
        // This heap query completes synchronously. Observing its entry also
        // catches returns that would otherwise stay inside a generated unit.
        rt.register_function(0x08D50C3Cu, [](psprecomp::Runtime &runtime, psprecomp::AllegrexContext &ctx) {
            const auto caller = ctx.gpr[31], size = ctx.gpr[17], heap = ctx.gpr[4];
            const auto sp = ctx.gpr[29];
            psprecomp::recomp_unit_0339(runtime, ctx);
#ifndef __EMSCRIPTEN__
            // Full, read-only snapshots at comparable guest boundaries. The
            // prefix is supplied explicitly for a local diagnostic run.
            if (caller == 0x08CD3080u && ctx.pc == caller && heap == 0u) {
                if (const char *prefix = std::getenv("PSPWEB_NCAA_HEAP_CAPTURE")) {
                    const bool initial = !g_session.trace_heap_initial_captured;
                    const bool target = size == 0x39480u && !g_session.trace_heap_target_captured;
                    const bool next = g_session.trace_heap_target_captured && !g_session.trace_heap_next_query_captured;
                    if (initial || target || next) {
                        const auto path = std::string(prefix) + (initial ? "-init.ram" : target ? "-target.ram" : "-next-query.ram");
                        std::ofstream capture(path, std::ios::binary);
                        const auto bytes = runtime.memory().size();
                        capture.write(reinterpret_cast<const char *>(runtime.memory().raw_pointer(0x08000000u, bytes)), bytes);
                        if (capture) {
                            if (initial) g_session.trace_heap_initial_captured = true;
                            else if (target) g_session.trace_heap_target_captured = true;
                            else g_session.trace_heap_next_query_captured = true;
                            std::cerr << "[ncaa-heap-capture] path=" << path << " size=" << psprecomp::hex32(size)
                                      << " largest=" << psprecomp::hex32(ctx.gpr[2]) << '\n';
                        }
                    }
                }
            }
#endif
            // These two compressed loading textures took different free regions
            // before otherwise identical DAY stadium assets were allocated.
            // Capture their input free lists and the subsequent loading bank's
            // list, rather than inferring an allocation order from read buffers.
            const bool loading_asset = size == 0xE57Du || size == 0xE68Au ||
                                       size == 0x79C0u || size == 0x9340u;
            if (caller == 0x08CD3080u && ctx.pc == caller &&
                (size >= 0x10000u || size == 0x4450u || loading_asset || ctx.gpr[2] < size)) {
                if (loading_asset) g_session.trace_loading_layout_seen = true;
                const auto &memory = runtime.memory();
                const auto continuation = memory.load32(sp + 512u);
                std::cerr << "[ncaa-disc-alloc-capacity] size=0x" << std::hex << size
                          << " available=0x" << ctx.gpr[2] << " heap=0x" << heap
                          << " continuation=0x" << continuation
                          << " vcount=" << std::dec << g_session.kernel->display().vcount
                          << " thread=" << psprecomp::runtime_thread_name() << std::hex;
                if (continuation == 0x08CD3BB0u || continuation == 0x08CD326Cu)
                    std::cerr << " caller=0x" << memory.load32(sp + 556u);
                std::cerr << std::dec << '\n';
                if (ctx.gpr[2] < size || size == 0x39480u || loading_asset) {
                    const auto heap_id = (heap ? heap : memory.load32(0x08E31228u)) & 63u;
                    const auto heap_address = memory.load32(0x08EC5AF0u + heap_id * 4u);
                    if (heap_address && memory.raw_pointer(heap_address, 64u)) {
                        std::cerr << "[ncaa-disc-heap] id=0x" << std::hex << heap_id
                                  << " address=0x" << heap_address;
                        for (unsigned i = 0; i < 16u; ++i)
                            std::cerr << " " << memory.load32(heap_address + i * 4u);
                        std::cerr << std::dec << '\n';
                        const auto sentinel = heap_address + 16u;
                        auto node = memory.load32(heap_address + 32u);
                        std::uint64_t total = 0;
                        std::uint32_t largest = 0;
                        unsigned count = 0;
                        while (node != sentinel && node && count < 256u && memory.raw_pointer(node, 24u)) {
                            const auto block_size = memory.load32(node + 4u);
                            if (block_size < 16u) break;
                            total += block_size - 16u;
                            largest = std::max(largest, block_size - 16u);
                            std::cerr << "[ncaa-disc-heap-free] address=0x" << std::hex << node;
                            for (unsigned i = 0; i < 6u; ++i)
                                std::cerr << " " << memory.load32(node + i * 4u);
                            std::cerr << std::dec << '\n';
                            ++count;
                            node = memory.load32(node + 16u);
                        }
                        std::cerr << "[ncaa-disc-heap-free-summary] blocks=" << count
                                  << " total=0x" << std::hex << total << " largest=0x" << largest
                                  << " end=0x" << node << " sentinel=0x" << sentinel << std::dec << '\n';
                    }
                }
            }
        }, "ncaa10_trace_disc_alloc_capacity");
        // The asset loader stores this allocation in its request object, then
        // passes it to 08CCC5F8 even when the allocation returned zero.
        rt.register_function(0x08CDB4A4u, [](psprecomp::Runtime &runtime, psprecomp::AllegrexContext &ctx) {
            const auto &memory = runtime.memory();
            const auto size = memory.load32(ctx.gpr[29] + 8u);
            if (size >= 0x10000u || ctx.gpr[2] == 0u)
                std::cerr << "[ncaa-disc-asset-alloc-result] request=0x" << std::hex << ctx.gpr[30]
                          << " size=0x" << size << " buffer=0x" << ctx.gpr[2] << std::dec << '\n';
            psprecomp::recomp_unit_0309(runtime, ctx);
        }, "ncaa10_trace_disc_asset_alloc_result");
        // Observe the public wrapper: its call to the queue helper is a local
        // generated jump, so a host override of the helper alone misses it.
        rt.register_function(0x08C7F660u, [](psprecomp::Runtime &runtime, psprecomp::AllegrexContext &ctx) {
            if (ctx.gpr[5] == 0u) {
                const auto &memory = runtime.memory();
                std::cerr << "[ncaa-disc-null-submit] fd=0x" << std::hex << ctx.gpr[4]
                          << " size=0x" << ctx.gpr[7] << " offset=0x" << ctx.gpr[6]
                          << " caller=0x" << ctx.gpr[31] << " sp=0x" << ctx.gpr[29] << '\n';
                std::cerr << "[ncaa-disc-submit-stack]";
                for (unsigned i = 0; i < 640u && memory.raw_pointer(ctx.gpr[29] + i * 4u, 4u); ++i)
                    std::cerr << " " << memory.load32(ctx.gpr[29] + i * 4u);
                std::cerr << std::dec << '\n';
                // This call chain retains the EA read descriptor in the saved
                // s0 slot of 08C7856C. Validate both return addresses before
                // interpreting that title-specific stack layout.
                const auto sp = ctx.gpr[29];
                if (memory.raw_pointer(sp, 184u) && memory.load32(sp + 100u) == 0x08C785F0u &&
                    (memory.load32(sp + 180u) == 0x08C79FA8u ||
                     memory.load32(sp + 180u) == 0x08C79F84u ||
                     memory.load32(sp + 180u) == 0x08C79F68u)) {
                    const auto descriptor = memory.load32(sp + 148u);
                    const auto words = [&](const char *label, std::uint32_t address, unsigned count) {
                        if (!address || !memory.raw_pointer(address, count * 4u)) return;
                        std::cerr << label << " address=0x" << std::hex << address;
                        for (unsigned i = 0; i < count; ++i)
                            std::cerr << " " << memory.load32(address + i * 4u);
                        std::cerr << std::dec << '\n';
                    };
                    words("[ncaa-disc-descriptor]", descriptor, 12u);
                    if (descriptor && memory.raw_pointer(descriptor, 40u)) {
                        words("[ncaa-disc-file]", memory.load32(descriptor + 36u), 12u);
                        if (memory.load32(descriptor + 40u) == 0x08C7416Cu)
                            words("[ncaa-disc-stream]", memory.load32(descriptor + 20u), 16u);
                    }
                    words("[ncaa-disc-memory-callbacks]", 0x08E310A8u, 4u);
                }
                g_session.kernel->dump_threads();
            }
            psprecomp::recomp_unit_0286(runtime, ctx);
        }, "ncaa10_trace_disc_submit");
        // The drive-status return resumes the game's disc reader. Log its saved
        // request before it copies the sector prefix or invokes sceIoRead.
        if (std::getenv("PSPWEB_NO_DISC_READER_TRACE") == nullptr)
        rt.register_function(0x08C7EDA4u, [](psprecomp::Runtime &runtime, psprecomp::AllegrexContext &ctx) {
            pspweb::trace_ncaa_disc_reader(runtime.memory(), ctx, std::cerr);
            psprecomp::recomp_unit_0286(runtime, ctx);
        }, "ncaa10_trace_disc_reader");
    }
#endif
    s.kernel = std::make_unique<pspweb::Kernel>(rt);
    s.kernel->reserve(load_base, image_end - load_base, "module:" + module->name);
    s.kernel->set_roots(disc_root, memstick_root);
    if (!manifest.empty() && !s.kernel->enable_webfs(manifest, data_url))
        std::cerr << "[pspweb] no disc manifest at " << manifest << ", using local files\n";
    s.kernel->install(imports);
#ifdef PSPWEB_MADDEN10
    if (module->name == "MaddenNFL2006" && std::getenv("PSPWEB_NO_IDLE_ACCEL") == nullptr) {
        rt.register_function(0x08C6725Cu, [](psprecomp::Runtime &runtime, psprecomp::AllegrexContext &ctx) {
            if (!g_session.kernel->try_madden_vblank_yield(ctx))
                psprecomp::recomp_unit_0280(runtime, ctx);
        }, "madden10_vblank_yield");
    }
#endif
#ifdef PSPWEB_NCAA10
    if (module->name == "NCAA2007" && std::getenv("PSPWEB_NO_IDLE_ACCEL") == nullptr) {
        rt.register_function(0x08C83E28u, [](psprecomp::Runtime &runtime, psprecomp::AllegrexContext &ctx) {
            if (!g_session.kernel->try_ncaa_vblank_yield(ctx))
                psprecomp::recomp_unit_0287(runtime, ctx);
        }, "ncaa10_vblank_yield");
    }
#ifndef __EMSCRIPTEN__
    if (module->name == "NCAA2007" && std::getenv("PSPWEB_TRACE_NCAA_INPUT") != nullptr) {
        rt.register_function(0x08CBC250u, [](psprecomp::Runtime &runtime, psprecomp::AllegrexContext &ctx) {
            const auto &memory = runtime.memory();
            if (memory.load16(0x08EC6A12u) != 0u) {
                std::cerr << "[ncaa-input-get] ra=0x" << std::hex << ctx.gpr[31]
                          << " input=0x" << memory.load32(0x08EC6A10u) << std::dec << '\n';
            }
            psprecomp::recomp_unit_0302(runtime, ctx);
        }, "ncaa10_trace_input_getter");
        rt.register_function(0x08CD580Cu, [](psprecomp::Runtime &runtime, psprecomp::AllegrexContext &ctx) {
            const auto &memory = runtime.memory();
            if (memory.load16(0x08EC6A12u) != 0u) {
                const auto object = ctx.gpr[4];
                std::cerr << "[ncaa-input-map] object=0x" << std::hex << object << " ra=0x" << ctx.gpr[31]
                          << " output=0x" << ctx.gpr[5] << std::dec
                          << " type=" << memory.load32(object + 12u)
                          << " bits=" << memory.load16(object + 20u)
                          << " count=" << memory.load32(0x08EC7B2Cu) << '\n';
            }
            psprecomp::recomp_unit_0308(runtime, ctx);
        }, "ncaa10_trace_input_mapper");
        rt.register_function(0x08CD682Cu, [](psprecomp::Runtime &runtime, psprecomp::AllegrexContext &ctx) {
            const auto held = runtime.memory().load16(0x08EC6A10u);
            const auto controller = ctx.gpr[4], binding = ctx.gpr[5], state = ctx.gpr[6], ra = ctx.gpr[31];
            psprecomp::recomp_unit_0308(runtime, ctx);
            if (held != 0u && ctx.pc == ra && ctx.fpr[0] != 0.0f) {
                std::cerr << "[ncaa-input-query] held=0x" << std::hex << held << " ra=0x" << ra
                          << " state=0x" << state << " pc=0x" << ctx.pc << std::dec
                          << " controller=" << controller << " binding=" << binding << " value=" << ctx.fpr[0] << '\n';
            }
        }, "ncaa10_trace_input_query");
    }
#endif
#endif
    // User modules may export sce_module_start_thread_parameter. Its three
    // values override the firmware defaults (priority, stack bytes, attributes).
    // EA's module_start only creates user_main and asks for a 1 KiB stack.
    const auto boot = pspweb::module_start_parameters(rt.memory(), module->ent_top, module->ent_end);
    s.kernel->boot(elf.runtime_entry(load_base), module->gp, guest_path, boot.priority, boot.stack, boot.attributes);
    s.running = true;

    std::cout << "[pspweb] module " << module->name << " loaded at 0x" << std::hex << load_base << "-0x"
              << image_end << std::dec << ", " << imports.size() << " imports, " << relocations.total
              << " relocations, " << rt.function_count() << " registered entries\n";
    return true;
}

// Switches the GE to the GL backend; a GL ES 3 context must be current.
bool enable_gl() {
    auto &s = g_session;
    auto gl = std::make_unique<pspweb::GeGl>(s.runtime->memory());
    if (!gl->init()) {
        std::cerr << "[pspweb] GL backend failed to initialize, using software rendering\n";
        return false;
    }
    s.gl = std::move(gl);
    s.kernel->ge().set_gl(s.gl.get());
    std::cout << "[pspweb] GL renderer: " << reinterpret_cast<const char *>(glGetString(GL_RENDERER)) << "\n";
    return true;
}

// Runs one guest frame; returns false when done.
bool step(double budget_ms) {
    auto &s = g_session;
    if (!s.running) return false;
    s.running = s.kernel->frame(budget_ms);
    if (s.use_gl) {
        s.kernel->ge_worker().post([] {
            if (auto *gl = g_session.gl.get()) gl->next_frame();
        });
    } else {
        s.kernel->read_framebuffer(s.rgba.data());
    }
    if (!s.running) std::cout << "[pspweb] guest stopped: " << s.kernel->halt_reason() << "\n";
    return s.running;
}

// Queues drawing the displayed framebuffer with GL into `fbo` (0 = the canvas),
// after the GE work queued so far.
void present_gl(GLuint fbo) {
    auto &s = g_session;
    const auto &d = s.kernel->display();
    auto &worker = s.kernel->ge_worker();
    worker.post_present([fb = d.framebuffer, stride = d.stride, format = d.format, fbo] {
        auto *gl = g_session.gl.get();
        if (gl == nullptr) return false;
        const int scale = gl->render_scale();
        gl->present(fb, stride, format, fbo, static_cast<int>(kWidth) * scale, static_cast<int>(kHeight) * scale);
        return true;
    });
}

// Runs on the thread with the GL context: sets up the GL renderer.
bool enable_gl_here() {
    if (!enable_gl()) return false;
    g_session.max_scale = g_session.gl->max_render_scale();
    g_session.gl_ready.store(true);
    return true;
}

void write_ppm(const std::string &path, const std::vector<std::uint8_t> &rgba, std::uint32_t width, std::uint32_t height) {
    std::ofstream out(path, std::ios::binary);
    out << "P6\n" << width << " " << height << "\n255\n";
    for (std::uint32_t i = 0; i < width * height; ++i) out.write(reinterpret_cast<const char *>(&rgba[i * 4u]), 3);
}

// Size of the presented picture: the PSP screen times the internal scale.
int out_width() { return static_cast<int>(kWidth) * g_session.scale.load(); }
int out_height() { return static_cast<int>(kHeight) * g_session.scale.load(); }

} // namespace

#if defined(__EMSCRIPTEN__)

namespace {
pspweb::FramePacer g_frame_pacer;
double g_previous_web_callback = 0.0;

// ?profile in the page URL sets PSPWEB_FRAME_PROFILE: a breakdown of each
// game frame is shown in the status bar every 60 frames.
struct WebProfile {
    bool enabled = std::getenv("PSPWEB_FRAME_PROFILE") != nullptr;
    int frames = 0, steps = 0;
    double step_ms = 0.0, ge_busy_from = 0.0, ge_wait_from = 0.0, gl_from = 0.0;
    double max_step_ms = 0.0, max_gap_ms = 0.0;
    pspweb::FrameBudgetStats budget_from{};
} g_web_profile;

void report_web_profile() {
    auto &p = g_web_profile;
    if (!p.enabled || ++p.frames < 60) return;
    auto &s = g_session;
    auto &worker = s.kernel->ge_worker();
    const double steps = std::max(p.steps, 1);
    const double ge_ms = (worker.busy_ms() - p.ge_busy_from) / steps;
    const double wait_ms = (s.kernel->ge_wait_ms() - p.ge_wait_from) / steps;
    const bool ready = s.gl_ready.load();
    const double gl_ms = ready ? s.gl->flush_ms() / steps : 0.0;
    // With a GE thread the guest's own time excludes waiting for it; without
    // one the GE runs inside the guest's time.
    const double game_ms = p.step_ms / steps - (worker.threaded() ? wait_ms : ge_ms);
    auto &ge = s.kernel->ge();
    const auto budget = s.kernel->frame_budget_stats();
    const auto presentation = worker.take_presentation_stats();
    char text[512];
    std::snprintf(text, sizeof text, "game %.1f | GE %.1f%s (GL %.1f) | waiting for GE %.1f | draws %.0f | tris %.0fk | readbacks %.1f | budget CPU %llu GE %llu idle %llu | max step %.1f callback gap %.1f | submits %llu interval avg %.1f max %.1f late %llu | queue avg %.1f max %.1f",
                  game_ms, ge_ms, worker.threaded() ? " on its own thread" : "", gl_ms, wait_ms,
                  ready ? s.gl->draw_calls() / steps : 0.0, static_cast<double>(ge.stats().triangles) / steps / 1000.0,
                  ready ? s.gl->downloads() / steps : 0.0,
                  static_cast<unsigned long long>(budget.cpu_hits - p.budget_from.cpu_hits),
                  static_cast<unsigned long long>(budget.ge_hits - p.budget_from.ge_hits),
                  static_cast<unsigned long long>(budget.idle_hits - p.budget_from.idle_hits),
                  p.max_step_ms, p.max_gap_ms,
                  static_cast<unsigned long long>(presentation.frames),
                  presentation.interval_ms / std::max<std::uint64_t>(presentation.intervals, 1u),
                  presentation.max_interval_ms,
                  static_cast<unsigned long long>(presentation.late_intervals),
                  presentation.queue_ms / std::max<std::uint64_t>(presentation.frames, 1u),
                  presentation.max_queue_ms);
    EM_ASM({ if (Module.pspProfile) Module.pspProfile(UTF8ToString($0), !!$1); },
           text, p.max_step_ms > 25.0 || p.max_gap_ms > 50.0 ||
                 presentation.max_interval_ms > 50.0 || presentation.max_queue_ms > 25.0);
    ge.reset_stats();
    if (ready) s.gl->reset_counters();
    p = WebProfile{};
    p.ge_busy_from = worker.busy_ms();
    p.ge_wait_from = s.kernel->ge_wait_ms();
    p.budget_from = budget;
}

void web_frame() {
    auto &s = g_session;
    auto &worker = s.kernel->ge_worker();
    // Advance the guest at the PSP's 59.94 Hz regardless of the display rate.
    const double now = emscripten_get_now();
    static double ge_busy_last = 0.0, ge_wait_last = 0.0;
    if (EM_ASM_INT({ return document.hidden ? 1 : 0; })) {
        g_frame_pacer.reset();
        g_previous_web_callback = 0.0;
        return;
    }
    if (g_previous_web_callback != 0.0)
        g_web_profile.max_gap_ms = std::max(g_web_profile.max_gap_ms, now - g_previous_web_callback);
    g_previous_web_callback = now;
    const auto due = g_frame_pacer.consume(now);
#if defined(PSPWEB_NCAA10) || defined(PSPWEB_MADDEN10)
    constexpr bool committed_only = true;
#else
    constexpr bool committed_only = false;
#endif
    const auto batch = pspweb::advance_frame_batch(due, s.running, committed_only, [&] {
        const auto previous_serial = s.kernel->display().frame_serial;
        const double step_start = emscripten_get_now();
        step(12.0);
        g_web_profile.max_step_ms = std::max(g_web_profile.max_step_ms, emscripten_get_now() - step_start);
        return s.kernel->display().frame_serial - previous_serial;
    }, [&] {
        if (s.use_gl) present_gl(0);
        else EM_ASM({ if (Module.pspPresent) Module.pspPresent(HEAPU8.subarray($0, $0 + $1 * $2 * 4), $1, $2); },
                    s.rgba.data(), kWidth, kHeight);
    });
    const auto frames = batch.steps;
    g_web_profile.steps += frames;
    if (frames == 0) return;
    const auto game_frames = batch.commits;
    const double stepped = emscripten_get_now();
    g_web_profile.step_ms += stepped - now;
    if (s.use_gl) {
        report_web_profile();
        // The frame's cost is whichever thread was busier: the guest (not
        // counting time spent waiting for the GE) or the GE.
        const double ge_busy = worker.busy_ms(), ge_wait = s.kernel->ge_wait_ms();
        double cost = stepped - now - (ge_wait - ge_wait_last);
        if (worker.threaded()) cost = std::max(cost, ge_busy - ge_busy_last);
        ge_busy_last = ge_busy;
        ge_wait_last = ge_wait;
        EM_ASM({ if (Module.pspFrame) Module.pspFrame($0, $1, $2); }, cost, frames, static_cast<unsigned>(game_frames));
    } else {
        EM_ASM({ if (Module.pspFrame) Module.pspFrame($0, $1, $2); }, stepped - now, frames, static_cast<unsigned>(game_frames));
    }
    if (!s.running) {
        EM_ASM({ if (Module.pspStatus) Module.pspStatus(UTF8ToString($0)); }, s.kernel->halt_reason().c_str());
    }
}
} // namespace

// requestAnimationFrame may stop entirely while hidden, so reset on the DOM
// visibility event rather than waiting for a hidden callback to arrive.
extern "C" EMSCRIPTEN_KEEPALIVE void pspweb_reset_frame_pacer() {
    g_frame_pacer.reset();
    g_previous_web_callback = 0.0;
    g_web_profile = WebProfile{};
    if (g_session.kernel) {
        g_session.kernel->ge_worker().reset_presentation_stats();
        g_web_profile.ge_busy_from = g_session.kernel->ge_worker().busy_ms();
        g_web_profile.ge_wait_from = g_session.kernel->ge_wait_ms();
        g_web_profile.budget_from = g_session.kernel->frame_budget_stats();
    }
}

// Sets the internal resolution (1..4 times 480x272) and resizes the canvas.
// Returns the scale that will be used, or 0 while the GL renderer is not up yet.
extern "C" EMSCRIPTEN_KEEPALIVE int pspweb_set_render_scale(int scale) {
    auto &s = g_session;
    if (!s.gl_ready.load()) return 0;
    scale = std::clamp(scale, 1, s.max_scale);
    s.kernel->ge_worker().post([scale] {
        auto *gl = g_session.gl.get();
        const int used = gl->set_render_scale(scale);
        emscripten_set_canvas_element_size("#canvas", static_cast<int>(kWidth) * used, static_cast<int>(kHeight) * used);
        g_session.scale.store(used);
    });
    return scale;
}

// Mixed audio goes to the page, which plays it through Web Audio.
void web_audio_sink(const std::int16_t *samples, std::uint32_t frames) {
    EM_ASM({ if (Module.pspAudio) Module.pspAudio(HEAP16.subarray($0 >> 1, ($0 >> 1) + $1 * 2)); }, samples, frames);
}

extern "C" EMSCRIPTEN_KEEPALIVE void pspweb_set_pad(std::uint32_t buttons, std::uint32_t lx, std::uint32_t ly) {
    if (g_session.kernel) g_session.kernel->set_pad(buttons, static_cast<std::uint8_t>(lx), static_cast<std::uint8_t>(ly));
}

extern "C" EMSCRIPTEN_KEEPALIVE void pspweb_save_choice(int index) {
    if (g_session.kernel) g_session.kernel->savedata_choose(index);
}

extern "C" EMSCRIPTEN_KEEPALIVE void pspweb_osk_field(std::uint32_t index, const char16_t *text, std::uint32_t length) {
    if (g_session.kernel && length <= 65536u) g_session.kernel->osk_set_field(index, std::u16string(text, length));
}
extern "C" EMSCRIPTEN_KEEPALIVE void pspweb_osk_complete(int canceled) {
    if (g_session.kernel) g_session.kernel->osk_complete(canceled != 0);
}

namespace {
// Creates the WebGL2 context on the calling thread (the GE thread when there
// is one: the canvas has been handed to it) and the GL renderer.
void init_web_gl(bool threaded) {
    EmscriptenWebGLContextAttributes attributes;
    emscripten_webgl_init_context_attributes(&attributes);
    attributes.majorVersion = 2;
    attributes.minorVersion = 0;
    attributes.alpha = false;
    attributes.depth = false;
    attributes.stencil = false;
    attributes.antialias = false;
    attributes.preserveDrawingBuffer = false;
    if (threaded) attributes.proxyContextToMainThread = EMSCRIPTEN_WEBGL_CONTEXT_PROXY_DISALLOW;
    const EMSCRIPTEN_WEBGL_CONTEXT_HANDLE context = emscripten_webgl_create_context("#canvas", &attributes);
    if (context <= 0) {
        std::cerr << "[pspweb] WebGL2 is not available, using software rendering\n";
        return;
    }
    emscripten_webgl_make_context_current(context);
    enable_gl_here();
}
} // namespace

int main() {
#ifdef PSPWEB_NCAA10
    EM_ASM({
        document.title = 'NCAA Football 10 — PSP Web Recomp';
        document.querySelector('h1').textContent = 'NCAA Football 10';
    });
#endif
#ifdef PSPWEB_MADDEN10
    EM_ASM({
        document.title = 'Madden NFL 10 — PSP Web Recomp';
        document.querySelector('h1').textContent = 'Madden NFL 10';
    });
#endif
    const bool local_iso = EM_ASM_INT({ return Module.pspReadDisc ? 1 : 0; });
    const char *manifest_path = local_iso ? "/game/local-disc.manifest" : "/game/disc.manifest";
    const bool streamed = std::ifstream(manifest_path).good();
    if (!start_session("/game/EBOOT.BIN", "/game/disc", "/game/ms0", PSPWEB_GUEST_PATH,
                       streamed ? manifest_path : "", "disc"))
        return 1;
    // The GE gets a thread of its own (with the canvas) unless ?threads=0 or the
    // browser cannot draw WebGL2 on an OffscreenCanvas.
    const bool threaded = EM_ASM_INT({
        if (/[?&]threads=0(&|$)/.test(location.search)) return 0;
        try {
            return typeof OffscreenCanvas != 'undefined' && !!new OffscreenCanvas(1, 1).getContext('webgl2') ? 1 : 0;
        } catch (e) {
            return 0;
        }
    }) != 0;
    g_session.use_gl = true;
    if (threaded) {
        g_session.kernel->ge_worker().start([] { init_web_gl(true); }, "#canvas");
    } else {
        init_web_gl(false);
    }
    std::cout << "[pspweb] GE " << (g_session.kernel->ge_worker().threaded() ? "on its own thread" : "on the main thread") << "\n";
    g_session.kernel->set_audio_sink(&web_audio_sink);
    EM_ASM({ if (Module.pspStatus) Module.pspStatus("running"); });
    emscripten_set_main_loop(web_frame, 0, false);
    return 0;
}

#else

namespace {
// Headless GL ES 3 context (Mesa surfaceless platform) for testing the GL path.
bool create_headless_gl() {
#if defined(_WIN32)
    EGLDisplay display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
#else
    auto get_display = reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(eglGetProcAddress("eglGetPlatformDisplayEXT"));
    EGLDisplay display = get_display != nullptr
        ? get_display(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr)
        : eglGetDisplay(EGL_DEFAULT_DISPLAY);
#endif
    EGLint major = 0, minor = 0;
    if (display == EGL_NO_DISPLAY || !eglInitialize(display, &major, &minor)) {
        std::cerr << "[pspweb] EGL initialisation failed\n";
        return false;
    }
    eglBindAPI(EGL_OPENGL_ES_API);
    const EGLint config_attributes[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_NONE};
    EGLConfig config = nullptr;
    EGLint count = 0;
    eglChooseConfig(display, config_attributes, &config, 1, &count);
    const EGLint context_attributes[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_NONE};
    EGLContext context = eglCreateContext(display, count > 0 ? config : EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, context_attributes);
    const EGLint surface_attributes[] = {EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE};
    EGLSurface surface = count > 0 ? eglCreatePbufferSurface(display, config, surface_attributes) : EGL_NO_SURFACE;
    if (context == EGL_NO_CONTEXT || !eglMakeCurrent(display, surface, surface, context)) {
        std::cerr << "[pspweb] EGL context creation failed (0x" << std::hex << eglGetError() << std::dec << ")\n";
        return false;
    }
    return true;
}

// Dumps are GL captures at the internal scale, or the guest framebuffer.
std::uint32_t capture_width() { return g_session.gl ? static_cast<std::uint32_t>(out_width()) : kWidth; }
std::uint32_t capture_height() { return g_session.gl ? static_cast<std::uint32_t>(out_height()) : kHeight; }

// --wav: everything the game plays, written out at the end.
std::vector<std::int16_t> g_wav;
void wav_sink(const std::int16_t *samples, std::uint32_t frames) { g_wav.insert(g_wav.end(), samples, samples + frames * 2u); }

void write_wav(const std::string &path) {
    std::ofstream out(path, std::ios::binary);
    auto u32 = [&](std::uint32_t v) { out.write(reinterpret_cast<const char *>(&v), 4); };
    auto u16 = [&](std::uint16_t v) { out.write(reinterpret_cast<const char *>(&v), 2); };
    const auto bytes = static_cast<std::uint32_t>(g_wav.size() * 2u);
    out.write("RIFF", 4); u32(36u + bytes); out.write("WAVEfmt ", 8);
    u32(16u); u16(1u); u16(2u); u32(44100u); u32(44100u * 4u); u16(4u); u16(16u);
    out.write("data", 4); u32(bytes);
    out.write(reinterpret_cast<const char *>(g_wav.data()), bytes);
}

// Presents with GL and reads the result back into session.rgba (top row first).
void capture_gl() {
    auto &s = g_session;
    if (!s.gl) return;
    present_gl(s.out_fbo);
    s.kernel->ge_worker().call([] {
        auto &session = g_session;
        const auto width = static_cast<std::uint32_t>(out_width()), height = static_cast<std::uint32_t>(out_height());
        std::vector<std::uint8_t> pixels(width * height * 4u);
        glBindFramebuffer(GL_FRAMEBUFFER, session.out_fbo);
        glReadPixels(0, 0, static_cast<GLsizei>(width), static_cast<GLsizei>(height), GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
        session.rgba.resize(width * height * 4u);
        for (std::uint32_t y = 0; y < height; ++y)
            std::memcpy(&session.rgba[y * width * 4u], &pixels[(height - 1u - y) * width * 4u], width * 4u);
    });
}

// Runs on the GE thread: headless GL context, renderer and capture target.
void init_native_gl() {
    auto &s = g_session;
    if (!create_headless_gl() || !enable_gl_here()) return;
    s.scale.store(s.gl->set_render_scale(s.scale.load()));
    glGenTextures(1, &s.out_texture);
    glBindTexture(GL_TEXTURE_2D, s.out_texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, out_width(), out_height(), 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glGenFramebuffers(1, &s.out_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, s.out_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s.out_texture, 0);
}
} // namespace

int main(int argc, char **argv) {
    if (argc < 2) {
        std::cerr << "usage: " << argv[0] << " <module.prx|EBOOT.ELF> [--disc dir] [--ms dir] [--frames n]"
                  << " [--dump out.ppm] [--sdl 0|1] [--scale n]\n";
        return 2;
    }
    std::string disc = ".", memstick = ".", dump = "frame.ppm", wav, control_file;
    int frames = 600, dump_every = 0;
    double guest_budget_ms = 50.0;
    std::string manifest;
    bool use_gl = false, threads = true, realtime = false;
#ifdef PSPWEB_USE_SDL
    bool use_sdl = true; // SDL player build defaults to a window; --sdl 0 keeps headless
#else
    bool use_sdl = false;
#endif
    struct Press { int frame, length; std::uint32_t buttons; };
    std::vector<Press> presses; // --press frame:buttons_hex:length
    struct Stick { int frame, length, x, y; };
    std::vector<Stick> sticks;  // --stick frame:x,y:length (0..255, 128 = centre)
    for (int i = 2; i + 1 < argc; i += 2) {
        const std::string flag = argv[i];
        if (flag == "--disc") disc = argv[i + 1];
        else if (flag == "--ms") memstick = argv[i + 1];
        else if (flag == "--frames") frames = std::stoi(argv[i + 1]);
        else if (flag == "--budget-ms") guest_budget_ms = std::clamp(std::stod(argv[i + 1]), 0.1, 100.0);
        else if (flag == "--dump") dump = argv[i + 1];
        else if (flag == "--dump-every") dump_every = std::stoi(argv[i + 1]);
        else if (flag == "--webfs") manifest = argv[i + 1];
        else if (flag == "--gl") use_gl = std::string(argv[i + 1]) != "0";
        else if (flag == "--sdl") use_sdl = std::string(argv[i + 1]) != "0";
        else if (flag == "--scale") g_session.scale.store(std::stoi(argv[i + 1]));
        else if (flag == "--threads") threads = std::string(argv[i + 1]) != "0";
        else if (flag == "--realtime") realtime = std::string(argv[i + 1]) != "0";
        else if (flag == "--control-file") control_file = argv[i + 1];
        else if (flag == "--wav") wav = argv[i + 1];
        else if (flag == "--press") {
            const std::string spec = argv[i + 1];
            const auto a = spec.find(':'), b = spec.rfind(':');
            presses.push_back({std::stoi(spec.substr(0, a)), std::stoi(spec.substr(b + 1u)),
                               static_cast<std::uint32_t>(std::stoul(spec.substr(a + 1u, b - a - 1u), nullptr, 16))});
        } else if (flag == "--stick") {
            const std::string spec = argv[i + 1];
            const auto a = spec.find(':'), comma = spec.find(','), b = spec.rfind(':');
            sticks.push_back({std::stoi(spec.substr(0, a)), std::stoi(spec.substr(b + 1u)),
                              std::stoi(spec.substr(a + 1u, comma - a - 1u)), std::stoi(spec.substr(comma + 1u, b - comma - 1u))});
        }
    }
    try {
        if (!start_session(argv[1], disc, memstick, PSPWEB_GUEST_PATH, manifest, disc)) return 1;
        if (!wav.empty()) g_session.kernel->set_audio_sink(&wav_sink);
#ifdef PSPWEB_USE_SDL
        // The SDL player presents the software framebuffer; the EGL/GL path
        // is not combined with it yet (needs ANGLE on Windows).
        if (use_sdl && use_gl) {
            std::cerr << "[pspweb] --sdl with --gl is not supported yet; using the SDL software window\n";
            use_gl = false;
        }
        bool sdl_open = false;
        if (use_sdl) {
            sdl_open = sdl::init(g_session.scale.load() <= 1 ? 2 : g_session.scale.load());
            if (sdl_open) {
                g_session.kernel->set_audio_sink(&sdl::push_audio);
                // Interactive play: run until the window closes unless the
                // user gave an explicit --frames limit (600 is the default).
                if (frames == 600) frames = std::numeric_limits<int>::max() - 600;
                std::cout << "[pspweb] SDL player: close the window or Ctrl+C to stop\n";
            } else {
                std::cerr << "[pspweb] SDL unavailable, continuing headless\n";
            }
        }
#endif
        if (use_gl) {
            g_session.use_gl = true;
            // The GE (and its GL context) gets a thread of its own unless --threads 0.
            if (threads) g_session.kernel->ge_worker().start(&init_native_gl);
            else init_native_gl();
            std::cout << "[pspweb] GE " << (g_session.kernel->ge_worker().threaded() ? "on its own thread" : "on the main thread") << "\n";
        }
        // Watchdog: reports where the guest spins when no frame completes for 5 s.
        static std::atomic<int> progress{0};
        std::thread([] {
            int last = -1;
            for (;;) {
                std::this_thread::sleep_for(std::chrono::seconds(5));
                const int now = progress.load();
                if (now == last) {
                    const auto &c = g_session.runtime->cpu();
                    std::cerr << "[watchdog] no frame for 5 s: dispatch=0x" << std::hex << psprecomp::runtime_dispatch_pc()
                              << " thread=" << std::dec << psprecomp::runtime_thread_name() << std::hex
                              << " ra=0x" << c.gpr[31] << " sp=0x" << c.gpr[29] << " v0=0x" << c.gpr[2]
                              << " a0=0x" << c.gpr[4] << " a1=0x" << c.gpr[5] << " s0=0x" << c.gpr[16]
                              << " s1=0x" << c.gpr[17] << std::dec << std::endl;
                }
                last = now;
            }
        }).detach();
        int frame = 0;
        pspweb::NativeReplayControl replay;
        bool pause_captured = false;
        auto window_start = std::chrono::steady_clock::now();
        std::uint64_t last_flips = 0;
        while (frame < frames) {
            if (!control_file.empty()) {
                const auto previous_serial = replay.serial;
                replay.poll(control_file);
                if (replay.serial != previous_serial)
                    std::cout << "[replay] frame=" << frame << " serial=" << replay.serial
                              << " buttons=" << psprecomp::hex32(replay.buttons) << " axes=" << replay.lx << ',' << replay.ly
                              << " frames=" << replay.remaining << " command=" << replay.command << std::endl;
                if (replay.command == "quit") break;
                if (replay.command == "pause") {
                    if (!pause_captured) {
                        capture_gl();
                        write_ppm(dump, g_session.rgba, capture_width(), capture_height());
                        std::cout << "[replay] paused frame=" << frame << " capture=" << dump << std::endl;
                        pause_captured = true;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    continue;
                }
                pause_captured = false;
            }
            const auto frame_start = std::chrono::steady_clock::now();
#ifdef PSPWEB_USE_SDL
            if (sdl_open) {
                const sdl::Pad pad = sdl::poll();
                if (pad.quit) break;
                g_session.kernel->set_pad(pad.buttons, pad.lx, pad.ly);
            } else {
#endif
            std::uint32_t buttons = 0u;
            for (const auto &p : presses)
                if (frame >= p.frame && frame < p.frame + p.length) buttons |= p.buttons;
            int lx = 128, ly = 128;
            for (const auto &s : sticks)
                if (frame >= s.frame && frame < s.frame + s.length) lx = s.x, ly = s.y;
            if (!control_file.empty() && replay.remaining > 0) {
                buttons |= replay.buttons;
                lx = replay.lx;
                ly = replay.ly;
            }
            g_session.kernel->set_pad(buttons, static_cast<std::uint8_t>(lx), static_cast<std::uint8_t>(ly));
#ifdef PSPWEB_USE_SDL
            }
#endif
            if (!step(guest_budget_ms)) break;
            if (!control_file.empty()) replay.advance();
            if (realtime) std::this_thread::sleep_until(frame_start + std::chrono::microseconds(16683));
#ifdef PSPWEB_NCAA10
            if (std::getenv("PSPWEB_TRACE_NCAA_INPUT") != nullptr) {
                const auto &memory = g_session.runtime->memory();
                const std::uint32_t buttons = memory.load32(0x08EC6A10u);
                const std::uint32_t alternate = memory.load32(0x08EC6A20u);
                const auto lx = memory.load32(0x08EC6A08u), ly = memory.load32(0x08EC6A0Cu);
                static std::uint32_t last_buttons = 0u, last_alternate = 0u;
                static std::uint32_t last_lx = 128u, last_ly = 128u;
                if (buttons != last_buttons || alternate != last_alternate || lx != last_lx || ly != last_ly) {
                    std::cerr << "[ncaa-input] frame=" << frame << " held/pressed=0x" << std::hex << buttons
                              << " alternate=0x" << alternate << std::dec
                              << " axes=" << lx << ',' << ly
                              << " route=" << unsigned(memory.load8(0x08E3601Du))
                              << " analog=" << unsigned(memory.load8(0x08E36024u)) << '\n';
                    last_buttons = buttons;
                    last_alternate = alternate;
                    last_lx = lx;
                    last_ly = ly;
                }
            }
#endif
            ++frame;
            progress.store(frame);
#ifdef PSPWEB_USE_SDL
            if (sdl_open && !g_session.use_gl) sdl::present(g_session.rgba.data(), kWidth, kHeight);
#endif
            const auto &ge = g_session.kernel->ge().stats();
            if (frame % 60 == 0) {
                std::cout << "[frame " << frame << "] fb=0x" << std::hex << g_session.kernel->display().framebuffer
                          << std::dec << " ge lists=" << ge.lists << " prims=" << ge.prims << " tris=" << ge.triangles
                          << " culled=" << ge.culled
                          << " sprites=" << ge.sprites << " pixels=" << ge.pixels << " transfers=" << ge.transfers
                          << " flips=" << g_session.kernel->display().flips - last_flips
                          << " ge_ms/frame=" << ge.nanoseconds / 60000000.0
                          << " frame_ms=" << std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - window_start).count() / 60.0 << "\n";
                window_start = std::chrono::steady_clock::now();
                last_flips = g_session.kernel->display().flips;
                g_session.kernel->ge().reset_stats();
                if (g_session.gl) {
                    std::cout << "[frame " << frame << "] gl draw calls/frame=" << g_session.gl->draw_calls() / 60.0
                              << " downloads/frame=" << g_session.gl->downloads() / 60.0
                              << " uploads/frame=" << g_session.gl->uploads() / 60.0
                              << " upload_kpx/frame=" << g_session.gl->upload_pixels() / 60.0 / 1000.0 << "\n";
                    const auto &p = g_session.gl->passes();
                    std::cout << "[frame " << frame << "] gl passes/frame: alpha=" << p.alpha_resolves / 60.0
                              << " stencil=" << p.stencil_resolves / 60.0 << " reinterpret=" << p.reinterprets / 60.0
                              << " feedback=" << p.feedback_copies / 60.0 << " blits=" << p.blits / 60.0
                              << " blend_copies=" << p.blend_copies / 60.0 << " blend_mpx=" << p.blend_pixels / 60.0 / 1e6
                              << " extra_mpx=" << p.pixels / 60.0 / 1e6 << "\n";
                    g_session.gl->reset_passes();
                    if (frame >= 900 && std::getenv("PSPWEB_GE_PROFILE") != nullptr) g_session.gl->report_targets();
                    g_session.gl->reset_counters();
                }
                if (frame >= 900) pspweb::Ge::report_targets();
            }
            if (dump_every > 0 && frame % dump_every == 0) {
                capture_gl();
                write_ppm(dump + "." + std::to_string(frame) + ".ppm", g_session.rgba, capture_width(), capture_height());
            }
        }
        std::cout << "[pspweb] ran " << frame << " frames, vcount=" << g_session.kernel->display().vcount
                  << ", framebuffer=0x" << std::hex << g_session.kernel->display().framebuffer << std::dec << "\n";
        std::cout << "[pspweb] scheduled idle Vblank waits=" << g_session.kernel->idle_vblank_waits() << "\n";
        const auto budget = g_session.kernel->frame_budget_stats();
        std::cout << "[pspweb] frame budget hits: cpu=" << budget.cpu_hits << " ge=" << budget.ge_hits
                  << " idle=" << budget.idle_hits << "\n";
        capture_gl();
        write_ppm(dump, g_session.rgba, capture_width(), capture_height());
        g_session.runtime->report_hle_histogram(30u);
        std::cout << "[pspweb] wrote " << dump << "\n";
        if (!wav.empty()) {
            write_wav(wav);
            std::cout << "[pspweb] wrote " << wav << " (" << g_wav.size() / 2u / 44100.0 << " s)\n";
        }
#ifdef PSPWEB_USE_SDL
        if (sdl_open) sdl::shutdown();
#endif
        g_session.kernel->dump_threads();
        if (const char *ram_path = std::getenv("PSPWEB_DUMP_RAM")) {
            std::ofstream ram(ram_path, std::ios::binary);
            const auto &memory = g_session.runtime->memory();
            ram.write(reinterpret_cast<const char *>(memory.raw_pointer(0x08000000u, memory.size())), memory.size());
        }
    } catch (const std::exception &e) {
        std::cerr << "[pspweb] fatal: " << e.what() << "\n";
        return 1;
    }
    return 0;
}

#endif
