#include <catch2/catch_test_macros.hpp>

#include "model/Track.h"

using namespace hopline;

namespace {

Clip makeClip(ClipId id, Tick start, Tick duration, Tick sourceIn = 0)
{
    Clip clip;
    clip.id = id;
    clip.source = 1;
    clip.timelineStart = start;
    clip.duration = duration;
    clip.sourceIn = sourceIn;
    return clip;
}

}  // namespace

TEST_CASE("track keeps clips sorted regardless of insert order", "[track]")
{
    Track track(Track::Kind::Video, "V1");
    REQUIRE(track.insert(makeClip(3, 2000, 500)));
    REQUIRE(track.insert(makeClip(1, 0, 500)));
    REQUIRE(track.insert(makeClip(2, 1000, 500)));

    REQUIRE(track.clips().size() == 3);
    CHECK(track.clips()[0].id == 1);
    CHECK(track.clips()[1].id == 2);
    CHECK(track.clips()[2].id == 3);
}

TEST_CASE("track rejects overlaps but allows butt joins", "[track]")
{
    Track track(Track::Kind::Video, "V1");
    REQUIRE(track.insert(makeClip(1, 0, 1000)));

    CHECK_FALSE(track.insert(makeClip(2, 500, 1000)));   // straddles
    CHECK_FALSE(track.insert(makeClip(3, 0, 100)));      // inside
    CHECK_FALSE(track.insert(makeClip(4, 999, 1)));      // last tick
    CHECK(track.insert(makeClip(5, 1000, 1000)));        // starts exactly at the end

    CHECK(track.clips().size() == 2);
}

TEST_CASE("rejected insert leaves the track untouched", "[track]")
{
    Track track(Track::Kind::Video, "V1");
    REQUIRE(track.insert(makeClip(1, 0, 1000)));
    const auto before = track.clips();

    CHECK_FALSE(track.insert(makeClip(2, 500, 100)));
    CHECK_FALSE(track.insert(makeClip(3, 5000, 0)));  // zero duration

    REQUIRE(track.clips().size() == before.size());
    CHECK(track.clips()[0].id == before[0].id);
}

TEST_CASE("clipAt finds the covering clip", "[track]")
{
    Track track(Track::Kind::Video, "V1");
    REQUIRE(track.insert(makeClip(1, 0, 1000)));
    REQUIRE(track.insert(makeClip(2, 2000, 1000)));

    CHECK(track.clipAt(0)->id == 1);
    CHECK(track.clipAt(999)->id == 1);
    CHECK(track.clipAt(1000) == nullptr);  // gap
    CHECK(track.clipAt(1500) == nullptr);
    CHECK(track.clipAt(2000)->id == 2);
    CHECK(track.clipAt(3000) == nullptr);
}

TEST_CASE("source time maps through the clip's in point", "[track]")
{
    const Clip clip = makeClip(1, 1000, 500, 7000);

    CHECK(clip.sourceTimeAt(1000) == 7000);
    CHECK(clip.sourceTimeAt(1250) == 7250);
    CHECK(clip.sourceTimeAt(1499) == 7499);
}

TEST_CASE("track duration is the end of the last clip", "[track]")
{
    Track track(Track::Kind::Video, "V1");
    CHECK(track.duration() == 0);

    REQUIRE(track.insert(makeClip(1, 500, 1000)));
    CHECK(track.duration() == 1500);

    REQUIRE(track.insert(makeClip(2, 3000, 1000)));
    CHECK(track.duration() == 4000);
}
