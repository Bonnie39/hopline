#pragma once

#include <cstdint>
#include <vector>

namespace hopline {

// CPU-side RGBA frame. Deliberately decoupled from FFmpeg and Qt types so the
// GPU path can supply frames through the same interface later.
struct VideoFrame {
    int width = 0;
    int height = 0;
    double pts = 0.0;  // seconds
    std::vector<uint8_t> rgba;

    bool valid() const { return width > 0 && height > 0 && !rgba.empty(); }
};

}  // namespace hopline
