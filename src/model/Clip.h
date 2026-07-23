#pragma once

#include "model/Media.h"
#include "model/Time.h"

namespace hopline {

using ClipId = uint64_t;
inline constexpr ClipId kInvalidClip = 0;

using LinkGroup = uint64_t;
inline constexpr LinkGroup kNoLink = 0;

// Values 0..4 kept stable for older (v5) files; new modes appended.
enum class BlendMode {
    Normal, Add, Screen, Multiply, Overlay,
    Darken, ColorBurn, Lighten, ColorDodge,
    SoftLight, HardLight, Difference, Exclusion, Subtract
};

// The default "Transform" effect on a video clip: where/how it lands on the
// sequence canvas.
struct Transform {
    double scale = 1.0;     // uniform, 1.0 = 100%
    double posX = 0.0;      // canvas-pixel offset from center (+right)
    double posY = 0.0;      // (+down)
    double rotation = 0.0;  // degrees, clockwise
    double opacity = 1.0;   // 0..1
    BlendMode blend = BlendMode::Normal;

    bool isIdentity() const
    {
        return scale == 1.0 && posX == 0.0 && posY == 0.0 && rotation == 0.0
               && opacity >= 1.0 && blend == BlendMode::Normal;
    }

    bool operator==(const Transform& o) const
    {
        return scale == o.scale && posX == o.posX && posY == o.posY && rotation == o.rotation
               && opacity == o.opacity && blend == o.blend;
    }
    bool operator!=(const Transform& o) const { return !(*this == o); }
};

// The default "Volume Controls" effect on an audio clip.
struct AudioLevels {
    double volumeDb = 0.0;  // gain in dB, 0 = unity
    double pan = 0.0;       // -1 full left .. +1 full right

    bool operator==(const AudioLevels& o) const { return volumeDb == o.volumeDb && pan == o.pan; }
    bool operator!=(const AudioLevels& o) const { return !(*this == o); }
};

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

    // Cosmetic color tag (0 = none). Inherited from the source media at placement,
    // then editable on the timeline independently of the media's browser color.
    int label = 0;

    // Default effects. A clip on a video track uses `transform`; on an audio track,
    // `audio`. Both are carried so a clip needn't know its track kind.
    Transform transform;
    AudioLevels audio;

    bool linked() const { return linkGroup != kNoLink; }

    TimeRange range() const { return { timelineStart, duration }; }

    // Where in the source file the given timeline instant falls.
    Tick sourceTimeAt(Tick timelineTime) const
    {
        return sourceIn + (timelineTime - timelineStart);
    }
};

}  // namespace hopline
