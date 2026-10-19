#pragma once

#include "psprecomp/runtime.hpp"
#include "psprecomp/common.hpp"
#include <algorithm>
#include <array>
#include <ostream>
#include <vector>

namespace pspweb {
// Read-only, opt-in provenance for ULUS10440's failing animation allocation.
// Only instrumented AOT instructions are observed. A store is confirmed by
// the following observed instruction in the same execution context; invalid
// accesses are never reported as completed writes.
class NcaaPointerTrace {
    struct Field {
        const char *name{};
        std::uint32_t address{}, writer{}, value{}, pending_pc{}, pending_value{};
        psprecomp::RuntimeExecutionContextToken pending_context{};
        bool pending{}, written{};
    };
    struct Request {
        unsigned id{};
        std::uint32_t owner{}, buffer{}, stream_request{}, stream{}, descriptor{}, tag{};
        std::uint32_t offset{}, count{}, fd{};
        std::int32_t reader{-1};
        bool constructing{};
        std::array<Field, 6> fields{};
    };
    std::vector<Request> requests_;
    unsigned next_id_{};

    static void prefix(std::ostream &out, const Request &r, const char *event,
                       std::uint32_t pc, std::uint32_t vcount) {
        out << "[ncaa-provenance] id=" << r.id << " event=" << event
            << " pc=" << psprecomp::hex32(pc) << " vcount=" << vcount
            << " thread-id=" << psprecomp::runtime_thread_uid()
            << " thread=" << psprecomp::runtime_thread_name();
    }
    static void watch(Field &f, const char *name, std::uint32_t address) {
        if (f.address == address && f.name) return;
        f = {}; f.name = name; f.address = address;
    }
    static void read_field(std::ostream &out, const Request &r, const Field &f,
                           const psprecomp::GuestMemory &m, std::uint32_t pc, std::uint32_t vcount) {
        prefix(out, r, "read", pc, vcount);
        out << " field=" << f.name << " address=" << psprecomp::hex32(f.address)
            << " value=" << psprecomp::hex32(m.load32(f.address))
            << " last-observed-writer=" << psprecomp::hex32(f.writer)
            << " writer-confirmed=" << f.written << '\n';
    }
public:
    void reset() { requests_.clear(); next_id_ = 0; }
    void observe(const psprecomp::Runtime &rt, const psprecomp::AllegrexContext &c,
                 std::uint32_t pc, std::uint32_t word, bool delay,
                 std::uint32_t vcount, std::ostream &out) {
        const auto &m = rt.memory();
        const auto reg = [&](unsigned n) { return n ? c.gpr[n] : 0u; };
        // These stores have no intervening HLE calls or uninstrumented guest
        // instructions before confirmation. If context changes, do not assert
        // that a matching value alone proves that the store executed.
        for (auto &r : requests_) for (auto &f : r.fields) {
            if (!f.pending) continue;
            f.pending = false;
            if (pc != f.pending_pc + 4u ||
                !psprecomp::runtime_execution_context_matches(f.pending_context) ||
                !m.contains(f.address, 4u) || m.load32(f.address) != f.pending_value) {
                prefix(out, r, "write-unconfirmed", f.pending_pc, vcount);
                out << " field=" << f.name << '\n';
                continue;
            }
            f.writer = f.pending_pc; f.value = f.pending_value; f.written = true;
            prefix(out, r, "write-completed", f.writer, vcount);
            out << " field=" << f.name << " address=" << psprecomp::hex32(f.address)
                << " value=" << psprecomp::hex32(f.value) << '\n';
        }
        if (pc == 0x08CDB4A4u && m.load32(reg(29) + 8u) == 0x39480u) {
            Request r{}; r.id = ++next_id_; r.owner = reg(30); r.buffer = reg(2);
            watch(r.fields[0], "owner.buffer", r.owner + 28u);
            requests_.push_back(r);
            prefix(out, r, "allocator-return", pc, vcount);
            out << " size=0x00039480 v0=" << psprecomp::hex32(r.buffer)
                << " owner=" << psprecomp::hex32(r.owner)
                << " allocator-call-pc=0x08CDB49C caller-continuation=0x08CDB4A4";
            const auto heap_id = m.load32(0x08E31228u) & 63u;
            const auto heap = m.load32(0x08EC5AF0u + heap_id * 4u);
            if (heap && m.contains(heap, 64u)) {
                const auto sentinel = heap + 16u;
                auto node = m.load32(heap + 32u);
                std::uint64_t total = 0; std::uint32_t largest = 0;
                unsigned blocks = 0;
                while (node && node != sentinel && blocks < 256u && m.contains(node, 24u)) {
                    const auto size = m.load32(node + 4u);
                    if (size < 16u) break;
                    total += size - 16u; largest = std::max(largest, size - 16u);
                    node = m.load32(node + 16u); ++blocks;
                }
                out << " heap=" << psprecomp::hex32(heap) << " total-free=" << psprecomp::hex32(static_cast<std::uint32_t>(total))
                    << " largest-free=" << psprecomp::hex32(largest) << " free-blocks=" << blocks;
            }
            out << '\n';
        }
        for (auto &r : requests_) {
            if (pc == 0x08CCC63Cu && reg(18) == r.owner && reg(21) == 0x39480u) {
                r.stream_request = reg(2);
                watch(r.fields[1], "stream-request.buffer", r.stream_request + 4u);
            }
            if (pc == 0x08C745D4u && reg(16) == r.stream_request && r.stream_request &&
                reg(7) == 0x39480u && reg(6) == r.buffer) r.constructing = true;
            if (pc == 0x08C7462Cu && r.constructing && reg(17) == r.buffer) {
                r.stream = reg(20);
                watch(r.fields[2], "file-stream.current-buffer", r.stream + 44u);
                watch(r.fields[5], "file-stream.base-buffer", r.stream + 20u);
            }
            if (pc == 0x08CCC6CCu && reg(16) == r.stream_request && reg(18) == r.owner) {
                prefix(out, r, "file-stream-return", pc, vcount);
                out << " returned-handle=" << psprecomp::hex32(reg(2))
                    << " stream-object=" << psprecomp::hex32(r.stream)
                    << " returned-handle-matches=" << (m.contains(r.stream, 4u) && m.load32(r.stream) == reg(2)) << '\n';
                r.constructing = false;
            }
            if (pc == 0x08C794D0u && reg(21) == r.stream && r.stream) {
                r.descriptor = reg(19); r.tag = m.load32(r.descriptor);
                r.offset = reg(18); r.count = reg(16);
                watch(r.fields[3], "descriptor.destination", r.descriptor + 32u);
                prefix(out, r, "descriptor", pc, vcount);
                out << " descriptor=" << psprecomp::hex32(r.descriptor)
                    << " guest-request-id=" << psprecomp::hex32(r.tag)
                    << " file-stream=" << psprecomp::hex32(r.stream)
                    << " offset=" << psprecomp::hex32(r.offset) << " count=" << psprecomp::hex32(r.count) << '\n';
            }
            if (pc == 0x08C7F660u && r.descriptor && reg(5) == r.buffer &&
                reg(6) == r.offset && reg(7) == r.count && m.contains(reg(29), 184u) &&
                m.load32(reg(29) + 100u) == 0x08C785F0u &&
                (m.load32(reg(29) + 180u) == 0x08C79FA8u || m.load32(reg(29) + 180u) == 0x08C79F84u ||
                 m.load32(reg(29) + 180u) == 0x08C79F68u) &&
                m.load32(reg(29) + 148u) == r.descriptor && m.load32(r.descriptor) == r.tag) {
                r.fd = reg(4);
                prefix(out, r, "reader-submit", pc, vcount);
                out << " descriptor=" << psprecomp::hex32(r.descriptor)
                    << " guest-request-id=" << psprecomp::hex32(r.tag) << " fd=" << psprecomp::hex32(r.fd)
                    << " destination=" << psprecomp::hex32(reg(5)) << '\n';
                read_field(out, r, r.fields[3], m, pc, vcount);
            }
            if (pc == 0x08C7ECFCu && r.fd && reg(4) == r.fd && reg(5) == r.buffer &&
                reg(6) == r.count && reg(7) == r.offset) {
                r.reader = psprecomp::runtime_thread_uid();
                watch(r.fields[4], "reader.stack-destination", reg(29) - 112u + 36u);
                prefix(out, r, "reader-entry", pc, vcount);
                out << " descriptor=" << psprecomp::hex32(r.descriptor)
                    << " guest-request-id=" << psprecomp::hex32(r.tag)
                    << " destination=" << psprecomp::hex32(reg(5))
                    << " stack-slot=" << psprecomp::hex32(r.fields[4].address) << '\n';
            }
            if (r.reader == psprecomp::runtime_thread_uid() && r.fields[4].address == reg(29) + 36u) {
                if (pc == 0x08C7EDACu) read_field(out, r, r.fields[4], m, pc, vcount);
                if (pc == 0x08C7EE40u || pc == 0x08C7EE50u || pc == 0x08C7EE90u) {
                    prefix(out, r, pc == 0x08C7EE40u ? "register-move" : "final-store-attempt", pc, vcount);
                    out << " word=" << psprecomp::hex32(word) << " a0=" << psprecomp::hex32(reg(4))
                        << " a1=" << psprecomp::hex32(reg(5)) << " s1=" << psprecomp::hex32(reg(17))
                        << " delay-slot=" << delay;
                    if (pc != 0x08C7EE40u) out << " effective-address=" << psprecomp::hex32(reg(5))
                        << " width=4 result=" << (m.contains(reg(5), 4u) ? "pending" : "FAULT-no-write");
                    out << '\n';
                }
            }
            if (word >> 26u != 0x2Bu) continue;
            const auto address = reg((word >> 21u) & 31u) +
                static_cast<std::uint32_t>(static_cast<std::int32_t>(static_cast<std::int16_t>(word)));
            for (auto &f : r.fields) if (f.name && address == f.address && m.contains(address, 4u)) {
                f.pending = true; f.pending_pc = pc; f.pending_value = reg((word >> 16u) & 31u);
                f.pending_context = psprecomp::capture_runtime_execution_context();
            }
        }
    }
};
}
