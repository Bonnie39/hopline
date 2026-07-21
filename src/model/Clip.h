#pragma once

#include "model/Media.h"
#include "model/Time.h"

namespace hopline {

using ClipId = uint64_t;
inline constexpr ClipId kInvalidClip = 0;

using LinkGroup = uint64_t;
inline constexpr LinkGroup kNoLink = 0;

// A view onto part of a source, placed on the timeline. Editing only ever moves
// these numbers around — the source file is never touched.
struct Clip {
    ClipId id = kInvalidClip;
    MediaId source = kInvalidMedia;

    Tick timelineStart = 0;
    Tick sourceIn = 0;
    Tick duration = 0;

    // Clips sharing a nonzero group move and trim together (e.g. a clip's video
    // and audio halves). kNoLink means the clip is independent.
    LinkGroup linkGroup = kNoLink;

    bool linked() const { return linkGroup != kNoLink; }

    TimeRange range() const { return { timelineStart, duration }; }

    // Where in the source file the given timeline instant falls.
    Tick sourceTimeAt(Tick timelineTime) const
    {
        return sourceIn + (timelineTime - timelineStart);
    }
};

}  // namespace hopline
