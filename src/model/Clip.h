#pragma once

#include "model/Media.h"
#include "model/Time.h"

namespace hopline {

using ClipId = uint64_t;
inline constexpr ClipId kInvalidClip = 0;

// A view onto part of a source, placed on the timeline. Editing only ever moves
// these numbers around — the source file is never touched.
struct Clip {
    ClipId id = kInvalidClip;
    MediaId source = kInvalidMedia;

    Tick timelineStart = 0;
    Tick sourceIn = 0;
    Tick duration = 0;

    TimeRange range() const { return { timelineStart, duration }; }

    // Where in the source file the given timeline instant falls.
    Tick sourceTimeAt(Tick timelineTime) const
    {
        return sourceIn + (timelineTime - timelineStart);
    }
};

}  // namespace hopline
