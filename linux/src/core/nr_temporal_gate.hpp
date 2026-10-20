#pragma once

namespace nr {
// A descriptor-compatible dummy motion image is not evidence of correspondence.
// Keep this separate from Reset: missing guides must not reset the noise counter
// or the exposure adaptation, and valid guides may resume on the next frame.
constexpr bool temporal_history_gate(bool latched, bool reset, bool same_preprocess,
                                     bool motion_available) {
    return latched && !reset && same_preprocess && motion_available;
}
}  // namespace nr
