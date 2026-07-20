#pragma once

#include <cstdint>

namespace hopline {

// Timeline time is an integer count of ticks, never a float. 705600000/s
// ("flicks") divides exactly by 24, 25, 30, 48, 50, 60, the 1000/1001 NTSC
// rates, and by 44100 and 48000 — so frame and sample boundaries are all
// exactly representable and edits never accumulate rounding error.
using Tick = int64_t;

inline constexpr Tick kTicksPerSecond = 705600000;

inline constexpr Tick ticksFromSeconds(double seconds)
{
    return static_cast<Tick>(seconds * kTicksPerSecond + (seconds < 0 ? -0.5 : 0.5));
}

inline constexpr double secondsFromTicks(Tick ticks)
{
    return static_cast<double>(ticks) / kTicksPerSecond;
}

// Exact for every rate flicks was designed for; rateDen is 1001 for NTSC.
inline constexpr Tick ticksPerFrame(int rateNum, int rateDen = 1)
{
    return kTicksPerSecond * rateDen / rateNum;
}

inline constexpr Tick ticksFromFrames(int64_t frames, int rateNum, int rateDen = 1)
{
    return frames * ticksPerFrame(rateNum, rateDen);
}

inline constexpr int64_t framesFromTicks(Tick ticks, int rateNum, int rateDen = 1)
{
    return ticks / ticksPerFrame(rateNum, rateDen);
}

inline constexpr Tick ticksFromSamples(int64_t samples, int sampleRate)
{
    return samples * (kTicksPerSecond / sampleRate);
}

// Half-open [start, end): a clip ending where the next begins does not overlap it.
struct TimeRange {
    Tick start = 0;
    Tick duration = 0;

    constexpr Tick end() const { return start + duration; }
    constexpr bool empty() const { return duration <= 0; }
    constexpr bool contains(Tick t) const { return t >= start && t < end(); }

    constexpr bool overlaps(const TimeRange& other) const
    {
        return start < other.end() && other.start < end();
    }
};

}  // namespace hopline
