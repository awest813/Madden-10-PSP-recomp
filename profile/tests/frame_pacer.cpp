#include "../host/frame_pacer.hpp"
#include <cassert>
#include <cmath>
#include <iostream>
#include <limits>

int main() {
    for (const double hz : {30.0, 60.0, 75.0, 120.0, 144.0, 240.0}) {
        pspweb::FramePacer pacer;
        unsigned frames = 0;
        for (unsigned callback = 0; callback < static_cast<unsigned>(hz * 60); ++callback)
            frames += pacer.consume(callback * 1000.0 / hz);
        assert(std::abs(static_cast<double>(frames) - 60 * pspweb::kPspVblankHz) < 2.0);
    }
    for (const double gap : {40.0, 60.0, 90.0, 110.0}) {
        pspweb::FramePacer pacer;
        assert(pacer.consume(0.0) == 1);
        assert(pacer.consume(gap) == 2);
        // Excess stall debt must not trigger a second catch-up burst.
        assert(pacer.consume(gap + pspweb::kPspVblankMs) == 1);
        assert(pacer.consume(gap + 2 * pspweb::kPspVblankMs) == 1);
    }
    pspweb::FramePacer pacer;
    assert(pacer.consume(100.0) == 1);
    assert(pacer.consume(100.0) == 0);
    assert(pacer.consume(std::numeric_limits<double>::quiet_NaN()) == 0);
    assert(pacer.consume(std::numeric_limits<double>::infinity()) == 0);
    assert(pacer.consume(50.0) == 1); // clock reset
    assert(pacer.consume(10000.0) == 1); // background tab / long stall
    pacer.reset();
    assert(pacer.consume(10001.0) == 1);
    std::cout << "PASS: 30-240 Hz pacing, bounded stall recovery, clock/visibility resets\n";
}
