#pragma once

#include <algorithm>
#include <vector>

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

struct Keyframe {
    Tick time = 0;  // clip-local: offset from the clip's timelineStart
    double value = 0.0;

    bool operator==(const Keyframe& o) const { return time == o.time && value == o.value; }
};

// An effect property that's either a constant or a set of linearly-interpolated
// keyframes. `animated()` (any keys) picks which. Times are clip-local ticks.
struct AnimatedValue {
    double constant = 0.0;
    std::vector<Keyframe> keys;  // sorted by time

    AnimatedValue() = default;
    AnimatedValue(double c) : constant(c) {}

    bool animated() const { return !keys.empty(); }

    double at(Tick localT) const
    {
        if (keys.empty()) {
            return constant;
        }
        if (localT <= keys.front().time) {
            return keys.front().value;  // hold before the first key
        }
        if (localT >= keys.back().time) {
            return keys.back().value;   // hold after the last
        }
        for (std::size_t i = 1; i < keys.size(); ++i) {
            if (localT <= keys[i].time) {
                const Keyframe& a = keys[i - 1];
                const Keyframe& b = keys[i];
                const double f = static_cast<double>(localT - a.time) / static_cast<double>(b.time - a.time);
                return a.value + (b.value - a.value) * f;
            }
        }
        return keys.back().value;
    }

    // Add a key at `localT`, or update the existing one on that exact tick.
    void setKeyframe(Tick localT, double value)
    {
        for (Keyframe& k : keys) {
            if (k.time == localT) {
                k.value = value;
                return;
            }
        }
        const auto pos = std::lower_bound(keys.begin(), keys.end(), localT,
                                          [](const Keyframe& k, Tick t) { return k.time < t; });
        keys.insert(pos, Keyframe{ localT, value });
    }

    void removeKeyframe(Tick localT)
    {
        keys.erase(std::remove_if(keys.begin(), keys.end(),
                                  [localT](const Keyframe& k) { return k.time == localT; }),
                   keys.end());
    }

    void clearKeys() { keys.clear(); }

    bool operator==(const AnimatedValue& o) const { return constant == o.constant && keys == o.keys; }
    bool operator!=(const AnimatedValue& o) const { return !(*this == o); }
};

// The default "Transform" effect on a video clip: where/how it lands on the
// sequence canvas. Each value can be keyframed; `blend` is not animatable.
struct Transform {
    AnimatedValue scale{ 1.0 };      // uniform, 1.0 = 100%
    AnimatedValue posX{ 0.0 };       // canvas-pixel offset from center (+right)
    AnimatedValue posY{ 0.0 };       // (+down)
    AnimatedValue rotation{ 0.0 };   // degrees, clockwise
    AnimatedValue opacity{ 1.0 };    // 0..1
    BlendMode blend = BlendMode::Normal;

    bool operator==(const Transform& o) const
    {
        return scale == o.scale && posX == o.posX && posY == o.posY && rotation == o.rotation
               && opacity == o.opacity && blend == o.blend;
    }
    bool operator!=(const Transform& o) const { return !(*this == o); }
};

// The default "Volume Controls" effect on an audio clip.
struct AudioLevels {
    AnimatedValue volumeDb{ 0.0 };  // gain in dB, 0 = unity
    AnimatedValue pan{ 0.0 };       // -1 full left .. +1 full right

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
