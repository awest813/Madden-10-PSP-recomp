#include "webfs.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>

#if defined(__EMSCRIPTEN__)
#include <emscripten/emscripten.h>

extern "C" EMSCRIPTEN_KEEPALIVE void pspweb_chunk_done(std::uint32_t request, const std::uint8_t *data,
                                                         std::uint32_t length);

EM_JS(void, pspweb_js_fetch, (std::uint32_t request, const char *url, double start, double end), {
    const target = UTF8ToString(url);
    const local = Module.pspReadDisc;
    const attempt = (tries) => (local ? Promise.resolve().then(() => local(target, start, end)) : fetch(target, { headers: { Range: 'bytes=' + start + '-' + end } })
        .then((response) => {
            if (response.status !== 206 && response.status !== 200) throw new Error('HTTP ' + response.status);
            return response.arrayBuffer().then((buffer) => response.status === 200 ? buffer.slice(start, end + 1) : buffer);
        }))
        .then((buffer) => {
            const bytes = new Uint8Array(buffer);
            if (bytes.length !== end - start + 1) throw new Error('Short disc range');
            const pointer = _malloc(bytes.length || 1);
            HEAPU8.set(bytes, pointer);
            _pspweb_chunk_done(request, pointer, bytes.length);
            _free(pointer);
        })
        .catch((error) => {
            if (!local && tries > 0) return new Promise((r) => setTimeout(r, 500)).then(() => attempt(tries - 1));
            console.error('[webfs] ' + target + ' ' + start + '-' + end + ': ' + error);
            _pspweb_chunk_done(request, 0, 0);
        });
    attempt(4);
});
#endif

namespace pspweb {
namespace {

constexpr std::uint64_t kCacheLimit = 256ull * 1024ull * 1024ull;

std::string lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

std::string normalize(const std::string &path) {
    std::string out;
    std::stringstream parts(path);
    std::string part;
    while (std::getline(parts, part, '/')) {
        if (part.empty() || part == ".") continue;
        if (!out.empty()) out.push_back('/');
        out += part;
    }
    return out;
}

#if !defined(__EMSCRIPTEN__)
// Native test path: "fetch" from a local directory, completing on poll so the
// asynchronous wait logic is exercised exactly like in the browser.
std::vector<std::pair<std::uint32_t, std::vector<std::uint8_t>>> g_native_completions;
#endif

} // namespace

#if defined(__EMSCRIPTEN__)
WebFs *g_webfs = nullptr;
#endif

bool WebFs::load_manifest(const std::string &manifest_path, std::string url_prefix) {
    std::ifstream in(manifest_path);
    if (!in) return false;
    url_prefix_ = std::move(url_prefix);
    std::string line;
    while (std::getline(in, line)) {
        while (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.size() < 3u) continue;
        WebFile file;
        if (line[0] == 'D') {
            file.directory = true;
            file.path = normalize(line.substr(2));
        } else if (line[0] == 'F') {
            const auto space = line.find(' ', 2);
            file.size = std::stoull(line.substr(2, space - 2));
            file.path = normalize(line.substr(space + 1));
        } else {
            continue;
        }
        file.id = static_cast<std::uint32_t>(files_.size());
        by_lower_path_[lower(file.path)] = file.id;
        files_.push_back(std::move(file));
    }
#if defined(__EMSCRIPTEN__)
    g_webfs = this;
#endif
    std::cout << "[webfs] manifest: " << files_.size() << " entries, data from " << url_prefix_ << "\n";
    return true;
}

const WebFile *WebFs::find(const std::string &relative_path) const {
    const std::string wanted = lower(normalize(relative_path));
    if (wanted.empty()) {
        static const WebFile root{"", 0u, true, 0u};
        return &root;
    }
    const auto it = by_lower_path_.find(wanted);
    return it == by_lower_path_.end() ? nullptr : &files_[it->second];
}

std::vector<const WebFile *> WebFs::list(const std::string &directory) const {
    const std::string prefix = lower(normalize(directory));
    std::vector<const WebFile *> out;
    for (const auto &file : files_) {
        const std::string path = lower(file.path);
        const auto slash = path.rfind('/');
        const std::string parent = slash == std::string::npos ? std::string() : path.substr(0, slash);
        if (parent == prefix) out.push_back(&file);
    }
    return out;
}

bool WebFs::read_cached(const WebFile &file, std::uint64_t offset, std::uint32_t length, std::uint8_t *dst) {
    if (length == 0u) return true;
    const std::uint64_t first = offset / kChunkSize, last = (offset + length - 1u) / kChunkSize;
    for (std::uint64_t c = first; c <= last; ++c)
        if (!chunks_.contains(key(file.id, c))) return false;
    std::uint64_t position = offset;
    std::uint32_t copied = 0u;
    while (copied < length) {
        const auto &chunk = chunks_.at(key(file.id, position / kChunkSize));
        const std::uint64_t within = position % kChunkSize;
        const std::uint32_t take = static_cast<std::uint32_t>(
            std::min<std::uint64_t>(length - copied, chunk.size() > within ? chunk.size() - within : 0u));
        if (take == 0u) break;
        std::copy_n(chunk.data() + within, take, dst + copied);
        copied += take;
        position += take;
    }
    return true;
}

void WebFs::fetch(const WebFile &file, std::uint64_t offset, std::uint32_t length, std::function<void(bool)> done) {
    Waiter waiter{{}, std::move(done)};
    const std::uint64_t first = offset / kChunkSize;
    const std::uint64_t last = length == 0u ? first : (offset + length - 1u) / kChunkSize;
    for (std::uint64_t c = first; c <= last; ++c) {
        const std::uint64_t k = key(file.id, c);
        if (chunks_.contains(k)) continue;
        waiter.missing.push_back(k);
        if (in_flight_.contains(k)) continue;
        const std::uint32_t request = next_request_++;
        in_flight_[k] = request;
        requests_[request] = k;
        const std::uint64_t start = c * kChunkSize;
        const std::uint64_t end = std::min<std::uint64_t>(start + kChunkSize, file.size) - 1u;
        const std::string url = url_prefix_ + "/" + file.path;
#if defined(__EMSCRIPTEN__)
        pspweb_js_fetch(request, url.c_str(), static_cast<double>(start), static_cast<double>(end));
#else
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(end - start + 1u));
        if (std::FILE *f = std::fopen(url.c_str(), "rb")) {
            std::fseek(f, static_cast<long>(start), SEEK_SET);
            bytes.resize(std::fread(bytes.data(), 1u, bytes.size(), f));
            std::fclose(f);
        } else {
            bytes.clear();
        }
        g_native_completions.emplace_back(request, std::move(bytes));
#endif
    }
    if (waiter.missing.empty()) {
        waiter.done(true);
        return;
    }
    waiters_.push_back(std::move(waiter));
}

void WebFs::chunk_arrived(std::uint32_t request, const std::uint8_t *data, std::uint32_t length) {
    const auto it = requests_.find(request);
    if (it == requests_.end()) return;
    const std::uint64_t k = it->second;
    requests_.erase(it);
    in_flight_.erase(k);
    const bool ok = data != nullptr && length != 0u;
    if (ok) {
        chunks_[k] = std::vector<std::uint8_t>(data, data + length);
        chunk_order_.push_back(k);
        cached_bytes_ += length;
        fetched_bytes_ += length;
    }
    std::vector<std::function<void(bool)>> ready;
    for (auto w = waiters_.begin(); w != waiters_.end();) {
        auto m = std::find(w->missing.begin(), w->missing.end(), k);
        if (m != w->missing.end()) {
            w->missing.erase(m);
            if (!ok) w->failed = true;
        }
        if (w->missing.empty()) {
            ready.push_back([done = std::move(w->done), failed = w->failed](bool) { done(!failed); });
            w = waiters_.erase(w);
        } else {
            ++w;
        }
    }
    for (auto &callback : ready) callback(true);
    evict();
}

void WebFs::evict() {
    while (cached_bytes_ > kCacheLimit && !chunk_order_.empty()) {
        const std::uint64_t k = chunk_order_.front();
        chunk_order_.pop_front();
        bool needed = false;
        for (const auto &w : waiters_)
            if (std::find(w.missing.begin(), w.missing.end(), k) != w.missing.end()) needed = true;
        const auto it = chunks_.find(k);
        if (it == chunks_.end() || needed) continue;
        cached_bytes_ -= it->second.size();
        chunks_.erase(it);
    }
}

#if !defined(__EMSCRIPTEN__)
void webfs_native_poll(WebFs &fs) {
    auto completions = std::move(g_native_completions);
    g_native_completions.clear();
    for (auto &[request, bytes] : completions)
        fs.chunk_arrived(request, bytes.empty() ? nullptr : bytes.data(), static_cast<std::uint32_t>(bytes.size()));
}
#endif

} // namespace pspweb

#if defined(__EMSCRIPTEN__)
extern "C" EMSCRIPTEN_KEEPALIVE void pspweb_chunk_done(std::uint32_t request, const std::uint8_t *data,
                                                         std::uint32_t length) {
    if (pspweb::g_webfs != nullptr) pspweb::g_webfs->chunk_arrived(request, data, length);
}
#endif
