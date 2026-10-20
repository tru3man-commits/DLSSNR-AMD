#include "nr_temporal_gate.hpp"
#include <cassert>
#include <iostream>

int main() {
    // Complete truth table: only a latched, non-reset frame in the same
    // preprocessing domain WITH a correspondence source may read history.
    for (unsigned bits = 0; bits != 16; ++bits)
        assert(nr::temporal_history_gate(bits & 8, bits & 4, bits & 2, bits & 1) == (bits == 11));

    // A running stream loses guides for two frames and then gets them back.
    // Missing guides must not be mistaken for a stationary camera; a genuine
    // zero-motion source must still retain temporal accumulation.
    const bool guides[] = {true, true, false, false, true, true};
    const bool reset[] = {true, false, false, false, false, true};
    const bool expected[] = {false, true, false, false, true, false};
    bool latched = false;
    unsigned old_history_reads = 0, fixed_history_reads = 0, false_reads_removed = 0;
    for (unsigned frame = 0; frame != 6; ++frame) {
        // Baseline before the fix: the host gate knew reset/latch/preprocess,
        // but it treated the dummy zero-vector image as a correspondence source.
        const bool before = latched && !reset[frame];
        const bool after = nr::temporal_history_gate(latched, reset[frame], true, guides[frame]);
        assert(after == expected[frame]);
        old_history_reads += before;
        fixed_history_reads += after;
        false_reads_removed += before && !after;
        if (guides[frame]) assert(before == after);
        latched = true; // recorded output remains available even without guides
    }
    assert(old_history_reads == 4);
    assert(fixed_history_reads == 2);
    assert(false_reads_removed == 2);
    std::cout << "PASS: 16 gate states; same 6-frame sequence: history reads "
              << old_history_reads << " -> " << fixed_history_reads
              << ", false reads removed=" << false_reads_removed << "\n";
}

