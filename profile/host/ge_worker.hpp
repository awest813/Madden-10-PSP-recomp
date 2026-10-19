#pragma once

// Runs the GE on a thread of its own, the way the PSP's graphics chip works
// next to its CPU.  The kernel queues display lists and gets an id back at
// once; sceGeListSync / sceGeDrawSync wait until the lists are done.  Other
// GE-side work (the per-frame reset, presenting, changing the resolution) is
// queued too, so everything the GE thread does stays in order.  The GE thread
// owns the GL context.
//
// Without a thread every command simply runs where it is queued, which is how
// the GE worked before and remains available for debugging.

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

namespace pspweb {

class Ge;

struct PresentationStats {
    std::uint64_t frames{}, intervals{}, late_intervals{};
    double interval_ms{}, max_interval_ms{}, queue_ms{}, max_queue_ms{};
};

class GeWorker {
public:
    explicit GeWorker(Ge &ge) : ge_(ge) {}
    ~GeWorker();
    GeWorker(const GeWorker &) = delete;
    GeWorker &operator=(const GeWorker &) = delete;

    // Starts the GE thread and runs `init` on it first (e.g. creating the GL
    // context).  Natively it returns once `init` has finished; in the browser at
    // once.  `canvas` (web only) names the canvas whose rendering moves to the thread.
    void start(std::function<void()> init, const char *canvas = nullptr);
    [[nodiscard]] bool threaded() const noexcept { return threaded_; }

    // Guest side.
    std::uint32_t enqueue(std::uint32_t list, std::uint32_t stall);
    void update_stall(std::uint32_t id, std::uint32_t stall);
    // True once list `id` (0: every list queued so far) has finished.
    [[nodiscard]] bool done(std::uint32_t id) const noexcept;
    // Lets real time pass while the GE thread works (at most `max_ms`).
    void wait_for_progress(double max_ms) const;

    // Any other work for the GE thread, in order with the lists; call() also
    // waits for it to finish.
    void post(std::function<void()> work);
    void call(std::function<void()> work);
    // Ordered presentation work; false means no frame was submitted (e.g. GL
    // is still starting). Timing measures host submission, not compositor scanout.
    void post_present(std::function<bool()> work);
    PresentationStats take_presentation_stats();
    void reset_presentation_stats();

    // Total milliseconds the GE has spent working (callers keep their own baseline).
    [[nodiscard]] double busy_ms() const noexcept;

private:
    void run_inline(std::function<void()> &work);
    void loop();                   // native thread body
    bool work_once(double wait_ms); // runs one queued item; false if none arrived in time
#ifdef __EMSCRIPTEN__
    static void *thread_main(void *self);
    static void tick(void *self);
#endif

    Ge &ge_;
    bool threaded_{};
    std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<std::function<void()>> queue_;
    bool stopping_{};
#ifndef __EMSCRIPTEN__
    std::thread thread_;
#endif
    std::function<void()> init_;
    std::atomic<bool> started_{false};

    std::uint32_t queued_{};                 // guest side: newest list id
    std::atomic<std::uint32_t> completed_{}; // newest finished list id
    std::atomic<std::uint64_t> busy_ns_{};
    // GE side: a list waiting for its stall address to move.
    std::uint32_t stalled_id_{}, stalled_pc_{};
    bool presented_{}; // set by work that drew to the screen (ends a web tick)
    // Protected by mutex_ along with the work queue; one update per submitted
    // picture keeps cross-thread diagnostic reads consistent.
    PresentationStats presentation_stats_{};
    double last_present_ms_{};
};

} // namespace pspweb
