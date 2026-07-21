#include <catch2/catch_test_macros.hpp>

#include "model/Sequence.h"

using namespace hopline;

namespace {

Clip makeClip(ClipId id, MediaId source, Tick start, Tick duration, Tick sourceIn = 0)
{
    Clip clip;
    clip.id = id;
    clip.source = source;
    clip.timelineStart = start;
    clip.duration = duration;
    clip.sourceIn = sourceIn;
    return clip;
}

}  // namespace

TEST_CASE("sequence starts with one video and one audio track", "[sequence]")
{
    const Sequence sequence;
    REQUIRE(sequence.trackCount() == 2);
    CHECK(sequence.track(0).kind() == Track::Kind::Video);
    CHECK(sequence.track(1).kind() == Track::Kind::Audio);
}

TEST_CASE("upper video tracks composite over lower ones", "[sequence]")
{
    Sequence sequence;
    const size_t v2 = sequence.addTrack(Track::Kind::Video, "V2");

    REQUIRE(sequence.track(0).insert(makeClip(1, 10, 0, 1000)));
    REQUIRE(sequence.track(v2).insert(makeClip(2, 20, 500, 1000)));

    // Only V1 covers this instant.
    CHECK(sequence.resolveVideoAt(100)->clip == 1);

    // Both cover it; the higher track wins.
    CHECK(sequence.resolveVideoAt(600)->clip == 2);

    // Only V2 covers this one.
    CHECK(sequence.resolveVideoAt(1200)->clip == 2);

    CHECK_FALSE(sequence.resolveVideoAt(2000).has_value());
}

TEST_CASE("resolve reports the source time, not the timeline time", "[sequence]")
{
    Sequence sequence;
    REQUIRE(sequence.track(0).insert(makeClip(1, 10, 1000, 500, 8000)));

    const auto resolved = sequence.resolveVideoAt(1200);
    REQUIRE(resolved.has_value());
    CHECK(resolved->source == 10);
    CHECK(resolved->sourceTime == 8200);
}

TEST_CASE("all audio tracks contribute", "[sequence]")
{
    Sequence sequence;
    const size_t a2 = sequence.addTrack(Track::Kind::Audio, "A2");

    REQUIRE(sequence.track(1).insert(makeClip(1, 10, 0, 1000)));
    REQUIRE(sequence.track(a2).insert(makeClip(2, 20, 0, 1000)));

    CHECK(sequence.resolveAudioAt(500).size() == 2);
    CHECK(sequence.resolveAudioAt(1500).empty());
}

TEST_CASE("sequence duration spans the longest track", "[sequence]")
{
    Sequence sequence;
    CHECK(sequence.duration() == 0);

    REQUIRE(sequence.track(0).insert(makeClip(1, 10, 0, 1000)));
    REQUIRE(sequence.track(1).insert(makeClip(2, 20, 0, 5000)));

    CHECK(sequence.duration() == 5000);
}

TEST_CASE("cut points are the sorted unique clip boundaries", "[sequence]")
{
    Sequence sequence;
    REQUIRE(sequence.track(0).insert(makeClip(1, 10, 0, 1000)));
    REQUIRE(sequence.track(0).insert(makeClip(2, 10, 1000, 500)));  // butt-joined, shares 1000

    const auto cuts = sequence.cutPoints(Track::Kind::Video);
    // starts 0, 1000 and ends 1000, 1500 -> {0, 1000, 1500}
    REQUIRE(cuts.size() == 3);
    CHECK(cuts[0] == 0);
    CHECK(cuts[1] == 1000);
    CHECK(cuts[2] == 1500);

    CHECK(sequence.cutPoints(Track::Kind::Audio).empty());
    CHECK(sequence.hasClips(Track::Kind::Video));
    CHECK_FALSE(sequence.hasClips(Track::Kind::Audio));
}

TEST_CASE("top video clip and gaps resolve to pointers", "[sequence]")
{
    Sequence sequence;
    REQUIRE(sequence.track(0).insert(makeClip(1, 10, 0, 1000)));
    REQUIRE(sequence.track(0).insert(makeClip(2, 10, 2000, 1000)));

    CHECK(sequence.topVideoClipAt(500)->id == 1);
    CHECK(sequence.topVideoClipAt(1500) == nullptr);  // gap
    CHECK(sequence.topVideoClipAt(2500)->id == 2);
}

TEST_CASE("frame snapping floors to the frame boundary", "[sequence]")
{
    Sequence sequence;
    sequence.setFrameRate(30);
    const Tick frame = sequence.frameDuration();

    CHECK(sequence.snapToFrame(0) == 0);
    CHECK(sequence.snapToFrame(frame - 1) == 0);
    CHECK(sequence.snapToFrame(frame) == frame);
    CHECK(sequence.snapToFrame(frame + 1) == frame);
    CHECK(sequence.snapToFrame(frame * 10 + 5) == frame * 10);
}

TEST_CASE("frame duration is exact for NTSC", "[sequence]")
{
    Sequence sequence;
    sequence.setFrameRate(30000, 1001);
    // 30 frames of 29.97 is 1.001 seconds exactly, with no remainder.
    CHECK(sequence.frameDuration() * 30 == kTicksPerSecond * 1001 / 1000);
}
