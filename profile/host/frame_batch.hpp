#pragma once
#include <cstdint>

namespace pspweb {
struct FrameBatch {
    unsigned steps{};
    std::uint64_t commits{};
};

// Step returns the number of guest display commits in that Vblank. The caller
// queues presentation on the same ordered graphics worker as guest drawing.
template<class Step, class Present>
FrameBatch advance_frame_batch(unsigned due, const bool &running, bool committed_only,
                               Step step, Present present) {
    FrameBatch batch;
    while (batch.steps < due && running) {
        const auto commits = step();
        batch.commits += commits;
        ++batch.steps;
        // A later step may redraw the same framebuffer. Queue this commit's
        // presentation before that drawing, rather than at the end of the batch.
        if (!committed_only || commits != 0u) present();
    }
    return batch;
}
}
