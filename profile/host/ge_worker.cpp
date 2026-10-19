#include "ge_worker.hpp"

#include "ge.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <thread>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#include <emscripten/html5.h>
#include <emscripten/threading.h>
#include <pthread.h>
#endif

namespace pspweb {
namespace {

double now_ms() {
#ifdef __EMSCRIPTEN__
    return emscripten_get_now();
#else
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
#endif
}

// While the main thread waits for the GE thread it must keep serving calls the
// GE thread hands to it (printing, for instance), or both would wait forever.
void relax() {
#ifdef __EMSCRIPTEN__
    emscripten_current_thread_process_queued_calls();
#else
    std::this_thread::yield();
#endif
}

} // namespace

GeWorker::~GeWorker() {
#ifndef __EMSCRIPTEN__
    if (threaded_ && thread_.joinable()) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopping_ = true;
        }
        wake_.notify_all();
        thread_.join();
    }
#endif
}

void GeWorker::start(std::function<void()> init, const char *canvas) {
    init_ = std::move(init);
    threaded_ = true;
#ifdef __EMSCRIPTEN__
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    if (canvas != nullptr) emscripten_pthread_attr_settransferredcanvases(&attr, canvas);
    pthread_t thread;
    const int result = pthread_create(&thread, &attr, &GeWorker::thread_main, this);
    pthread_attr_destroy(&attr);
    if (result != 0) {
        threaded_ = false;
        init_();
        return;
    }
#else
    (void)canvas;
    thread_ = std::thread([this] {
        init_();
        started_.store(true);
        loop();
    });
#endif
#ifndef __EMSCRIPTEN__
    while (!started_.load()) relax();
#endif
    // In the browser the main thread must not wait here: creating a WebGL
    // context on a worker's OffscreenCanvas needs the main thread to answer.
    // Work queued meanwhile simply runs once the GE thread is ready.
}

void GeWorker::run_inline(std::function<void()> &work) {
    const double start = now_ms();
    work();
    busy_ns_.fetch_add(static_cast<std::uint64_t>((now_ms() - start) * 1e6));
}

void GeWorker::post(std::function<void()> work) {
    if (!threaded_) {
        run_inline(work);
        return;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.push_back(std::move(work));
    }
    wake_.notify_one();
}

void GeWorker::call(std::function<void()> work) {
    if (!threaded_) {
        run_inline(work);
        return;
    }
    std::atomic<bool> finished{false};
    post([&] {
        work();
        finished.store(true);
    });
    while (!finished.load()) relax();
}

void GeWorker::post_present(std::function<bool()> work) {
    const double queued_at = now_ms();
    post([this, queued_at, work = std::move(work)] {
        const double started_at = now_ms();
        if (!work()) return;
        const double submitted_at = now_ms();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto &stats = presentation_stats_;
            ++stats.frames;
            const double wait = std::max(0.0, started_at - queued_at);
            stats.queue_ms += wait;
            stats.max_queue_ms = std::max(stats.max_queue_ms, wait);
            if (last_present_ms_ != 0.0) {
                const double interval = std::max(0.0, submitted_at - last_present_ms_);
                ++stats.intervals;
                stats.interval_ms += interval;
                stats.max_interval_ms = std::max(stats.max_interval_ms, interval);
                if (interval > 50.0) ++stats.late_intervals;
            }
            last_present_ms_ = submitted_at;
        }
        presented_ = true;
    });
}

PresentationStats GeWorker::take_presentation_stats() {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto stats = presentation_stats_;
    presentation_stats_ = {};
    return stats;
}

void GeWorker::reset_presentation_stats() {
    std::lock_guard<std::mutex> lock(mutex_);
    presentation_stats_ = {};
    last_present_ms_ = 0.0;
}

std::uint32_t GeWorker::enqueue(std::uint32_t list, std::uint32_t stall) {
    const std::uint32_t id = ++queued_;
    post([this, id, list, stall] {
        if (ge_.run_list(list, stall)) {
            stalled_pc_ = 0u;
            completed_.store(id);
        } else {
            // Stopped at the stall address; resumes from there when it moves.
            stalled_id_ = id;
            stalled_pc_ = stall & 0x0FFFFFFFu;
        }
    });
    return id;
}

void GeWorker::update_stall(std::uint32_t, std::uint32_t stall) {
    post([this, stall] {
        if (stalled_pc_ == 0u) return;
        if (ge_.run_list(stalled_pc_, stall)) {
            stalled_pc_ = 0u;
            completed_.store(stalled_id_);
        } else {
            stalled_pc_ = stall & 0x0FFFFFFFu;
        }
    });
}

bool GeWorker::done(std::uint32_t id) const noexcept {
    return completed_.load() >= (id == 0u ? queued_ : id);
}

void GeWorker::wait_for_progress(double max_ms) const {
    if (!threaded_) return;
    const std::uint32_t seen = completed_.load();
    const double start = now_ms();
    while (completed_.load() == seen && now_ms() - start < max_ms) {
#ifdef __EMSCRIPTEN__
        relax();
#else
        std::this_thread::sleep_for(std::chrono::microseconds(50));
#endif
    }
}

double GeWorker::busy_ms() const noexcept {
    return static_cast<double>(busy_ns_.load()) / 1e6;
}

bool GeWorker::work_once(double wait_ms) {
    std::function<void()> work;
    {
        std::unique_lock<std::mutex> lock(mutex_);
        if (queue_.empty()) {
            const auto timeout = std::chrono::microseconds(static_cast<std::int64_t>(wait_ms * 1000.0));
            if (!wake_.wait_for(lock, timeout, [this] { return !queue_.empty() || stopping_; }) || queue_.empty())
                return false;
        }
        work = std::move(queue_.front());
        queue_.pop_front();
    }
    run_inline(work);
    return true;
}

void GeWorker::loop() {
    for (;;) {
        {
            std::unique_lock<std::mutex> lock(mutex_);
            wake_.wait(lock, [this] { return !queue_.empty() || stopping_; });
            if (queue_.empty()) return; // stopping
        }
        work_once(0.0);
    }
}

#ifdef __EMSCRIPTEN__
void *GeWorker::thread_main(void *self_pointer) {
    auto *self = static_cast<GeWorker *>(self_pointer);
    self->init_();
    self->started_.store(true);
    // A worker only shows WebGL frames after returning to its event loop, so
    // the thread runs as a main loop of short ticks instead of blocking forever.
    emscripten_set_main_loop_arg(&GeWorker::tick, self, 0, false);
    // Repeated setTimeout(0) ticks are subject to browser timer clamping. Use
    // Emscripten's immediate scheduler only when it will call postTask directly;
    // its legacy worker postMessage fallback can depend on the busy main thread.
    const bool immediate = std::getenv("PSPWEB_GE_TIMER") == nullptr && EM_ASM_INT({
        return typeof globalThis.scheduler !== 'undefined' &&
               typeof globalThis.scheduler.postTask === 'function';
    });
    emscripten_set_main_loop_timing(immediate ? EM_TIMING_SETIMMEDIATE : EM_TIMING_SETTIMEOUT, 0);
    emscripten_unwind_to_js_event_loop();
    return nullptr;
}

void GeWorker::tick(void *self_pointer) {
    auto *self = static_cast<GeWorker *>(self_pointer);
    self->presented_ = false;
    const double start = now_ms();
    // Work until a frame has been presented, the queue stays empty for a few
    // milliseconds, or the tick gets long; then let the browser show the frame.
    while (!self->presented_ && now_ms() - start < 50.0)
        if (!self->work_once(4.0)) break;
}
#endif

} // namespace pspweb
