#include <catch2/catch_test_macros.hpp>

#include "model/Time.h"

using namespace hopline;

TEST_CASE("tick rate divides every rate we care about", "[time]")
{
    // The whole point of 705600000: these must be exact, not approximate.
    CHECK(kTicksPerSecond % 24 == 0);
    CHECK(kTicksPerSecond % 25 == 0);
    CHECK(kTicksPerSecond % 30 == 0);
    CHECK(kTicksPerSecond % 48 == 0);
    CHECK(kTicksPerSecond % 50 == 0);
    CHECK(kTicksPerSecond % 60 == 0);
    CHECK(kTicksPerSecond % 44100 == 0);
    CHECK(kTicksPerSecond % 48000 == 0);
}

TEST_CASE("frame arithmetic is exact", "[time]")
{
    SECTION("integer rates round-trip")
    {
        for (int rate : { 24, 25, 30, 50, 60 }) {
            const Tick oneSecond = ticksFromFrames(rate, rate);
            CHECK(oneSecond == kTicksPerSecond);
            CHECK(framesFromTicks(oneSecond, rate) == rate);
        }
    }

    SECTION("NTSC 29.97 does not drift over an hour")
    {
        // 1000/1001 rates are where float time goes wrong. 107892 frames is the
        // drop-frame timecode hour; in real time that's 3599.9964 s, not 3600.
        const int64_t frames = 107892;
        const Tick ticks = ticksFromFrames(frames, 30000, 1001);
        CHECK(framesFromTicks(ticks, 30000, 1001) == frames);

        // frames * 1001/30000 seconds, exactly, with no remainder anywhere.
        CHECK(ticks == frames * 1001 * (kTicksPerSecond / 30000));
        CHECK(secondsFromTicks(ticks) == 3599.9964);
    }

    SECTION("audio samples land on exact ticks")
    {
        CHECK(ticksFromSamples(48000, 48000) == kTicksPerSecond);
        CHECK(ticksFromSamples(44100, 44100) == kTicksPerSecond);
        CHECK(ticksFromSamples(1, 48000) * 48000 == kTicksPerSecond);
    }
}

TEST_CASE("ranges are half-open", "[time]")
{
    const TimeRange a{ 0, 100 };
    const TimeRange b{ 100, 100 };

    CHECK(a.contains(0));
    CHECK(a.contains(99));
    CHECK_FALSE(a.contains(100));  // end is exclusive

    // Butt-joined clips must not count as overlapping, or edits get rejected.
    CHECK_FALSE(a.overlaps(b));
    CHECK(a.overlaps(TimeRange{ 99, 50 }));
    CHECK(a.end() == 100);
}
