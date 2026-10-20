#pragma once
#include <algorithm>
#include <cstdint>

namespace nr {
struct GuideRegion {
    uint32_t x, y, width, height;
};

// Sampling and motion normalization must describe the same active region.
// Preserve the session's existing rule: a zero extent means the remainder,
// an oversized extent is clipped, and an invalid origin uses the whole image.
inline GuideRegion resolve_guide_region(uint32_t aw, uint32_t ah,
                                        uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    if (x >= aw || y >= ah) return {0, 0, aw, ah};
    return {x, y, w ? std::min(w, aw - x) : aw - x,
                  h ? std::min(h, ah - y) : ah - y};
}

// NGX motion scale produces pixels of the guide region. The runtime consumes
// normalized screen displacement, even when NR colour is at display resolution.
inline float normalized_guide_motion(float scale, uint32_t extent) {
    return extent ? scale / static_cast<float>(extent) : scale;
}
} // namespace nr
