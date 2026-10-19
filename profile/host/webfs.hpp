#pragma once

// Disc filesystem for the browser build.  A manifest (generated next to the
// extracted disc) lists every file and directory; file contents are fetched
// on demand in fixed-size chunks with HTTP Range requests and cached in memory.
// A read that misses the cache completes asynchronously, which the kernel
// models as the calling PSP thread waiting on a slow UMD.

#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace pspweb {

struct WebFile {
    std::string path;       // relative to the disc root, original case
    std::uint64_t size{};
    bool directory{};
    std::uint32_t id{};
};

class WebFs {
public:
    static constexpr std::uint32_t kChunkSize = 256u * 1024u;

    // Lines: "D <path>" or "F <size> <path>", paths relative to the disc root.
    bool load_manifest(const std::string &manifest_path, std::string url_prefix);

    [[nodiscard]] const WebFile *find(const std::string &relative_path) const;
    [[nodiscard]] std::vector<const WebFile *> list(const std::string &directory) const;

    // Copies the range into dst when every chunk is cached.
    bool read_cached(const WebFile &file, std::uint64_t offset, std::uint32_t length, std::uint8_t *dst);
    // Fetches the missing chunks of the range; `done(ok)` runs once all arrived.
    void fetch(const WebFile &file, std::uint64_t offset, std::uint32_t length, std::function<void(bool)> done);
    // Called by the JavaScript fetch completion.
    void chunk_arrived(std::uint32_t request, const std::uint8_t *data, std::uint32_t length);

    [[nodiscard]] std::uint64_t cached_bytes() const noexcept { return cached_bytes_; }
    [[nodiscard]] std::uint64_t fetched_bytes() const noexcept { return fetched_bytes_; }
    [[nodiscard]] std::uint32_t in_flight() const noexcept { return static_cast<std::uint32_t>(in_flight_.size()); }

private:
    struct Waiter {
        std::vector<std::uint64_t> missing;
        std::function<void(bool)> done;
        bool failed{};
    };
    static std::uint64_t key(std::uint32_t file, std::uint64_t chunk) { return (static_cast<std::uint64_t>(file) << 32u) | chunk; }
    void evict();

    std::vector<WebFile> files_;
    std::unordered_map<std::string, std::uint32_t> by_lower_path_;
    std::string url_prefix_;
    std::unordered_map<std::uint64_t, std::vector<std::uint8_t>> chunks_;
    std::deque<std::uint64_t> chunk_order_;
    std::unordered_map<std::uint64_t, std::uint32_t> in_flight_;   // chunk key -> request id
    std::map<std::uint32_t, std::uint64_t> requests_;               // request id -> chunk key
    std::vector<Waiter> waiters_;
    std::uint32_t next_request_{1};
    std::uint64_t cached_bytes_{};
    std::uint64_t fetched_bytes_{};
};

} // namespace pspweb
