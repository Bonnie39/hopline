#pragma once

#include <atomic>
#include <functional>
#include <string>

namespace hopline {

class Project;

// Renders the active sequence [0, duration) offline to an MP4 (H.264 + AAC) using the
// sequence's own resolution and frame rate, compositing/mixing through the same primitives
// as playback (engine/Compositor.h) so the export matches the preview. Headless: no Qt.
class Exporter {
public:
    // onProgress(0..1) is called periodically; setting `cancel` aborts. Returns true on
    // success; on failure or cancel returns false with `error` set.
    bool run(const Project& project, const std::string& outPath,
             const std::function<void(double)>& onProgress,
             std::atomic<bool>& cancel, std::string& error);
};

}  // namespace hopline
