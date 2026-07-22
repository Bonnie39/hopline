#include <catch2/catch_test_macros.hpp>

#include "model/Commands.h"
#include "model/Project.h"

using namespace hopline;

namespace {

// Snapshot of everything undo is supposed to restore.
struct Snapshot {
    std::vector<std::vector<Clip>> tracks;

    bool operator==(const Snapshot& other) const
    {
        if (tracks.size() != other.tracks.size()) {
            return false;
        }
        for (size_t t = 0; t < tracks.size(); ++t) {
            if (tracks[t].size() != other.tracks[t].size()) {
                return false;
            }
            for (size_t c = 0; c < tracks[t].size(); ++c) {
                const Clip& a = tracks[t][c];
                const Clip& b = other.tracks[t][c];
                if (a.id != b.id || a.source != b.source || a.timelineStart != b.timelineStart
                    || a.sourceIn != b.sourceIn || a.duration != b.duration) {
                    return false;
                }
            }
        }
        return true;
    }
};

Snapshot snapshot(const Project& project)
{
    Snapshot snap;
    for (size_t i = 0; i < project.sequence().trackCount(); ++i) {
        snap.tracks.push_back(project.sequence().track(i).clips());
    }
    return snap;
}

MediaId addSource(Project& project, Tick duration = 100000)
{
    MediaSource source;
    source.path = "test.mp4";
    source.duration = duration;
    source.hasVideo = true;
    return project.addMedia(source);
}

Clip makeClip(MediaId source, Tick start, Tick duration, Tick sourceIn = 0)
{
    Clip clip;
    clip.source = source;
    clip.timelineStart = start;
    clip.duration = duration;
    clip.sourceIn = sourceIn;
    return clip;
}

// A project with one active sequence (V1/A1) — commands operate on the active one.
Project seqProject()
{
    Project project;
    project.setActiveSequence(project.addSequence("Sequence", 30, 1, 1920, 1080));
    return project;
}

}  // namespace

TEST_CASE("add then undo restores the empty timeline", "[commands]")
{
    Project project = seqProject();
    CommandStack stack;
    const MediaId source = addSource(project);
    const Snapshot before = snapshot(project);

    REQUIRE(stack.execute(project, std::make_unique<AddClipCommand>(0, makeClip(source, 0, 1000))));
    CHECK(project.sequence().track(0).clips().size() == 1);

    stack.undo(project);
    CHECK(snapshot(project) == before);
}

TEST_CASE("redo reproduces the edit exactly, id included", "[commands]")
{
    Project project = seqProject();
    CommandStack stack;
    const MediaId source = addSource(project);

    REQUIRE(stack.execute(project, std::make_unique<AddClipCommand>(0, makeClip(source, 0, 1000))));
    const Snapshot afterAdd = snapshot(project);

    stack.undo(project);
    stack.redo(project);

    // Ids must match too: a fresh id here would break any later command
    // referring to the clip, and would diverge from the original edit.
    CHECK(snapshot(project) == afterAdd);
}

TEST_CASE("a rejected edit never lands on the undo stack", "[commands]")
{
    Project project = seqProject();
    CommandStack stack;
    const MediaId source = addSource(project);

    REQUIRE(stack.execute(project, std::make_unique<AddClipCommand>(0, makeClip(source, 0, 1000))));
    const Snapshot before = snapshot(project);

    // Overlaps the existing clip, so it must fail.
    CHECK_FALSE(stack.execute(project, std::make_unique<AddClipCommand>(0, makeClip(source, 500, 1000))));
    CHECK(snapshot(project) == before);

    // Undo must still reach the successful add, not a phantom entry.
    stack.undo(project);
    CHECK(project.sequence().track(0).empty());
}

TEST_CASE("every command round-trips through undo", "[commands]")
{
    Project project = seqProject();
    CommandStack stack;
    const MediaId source = addSource(project);

    REQUIRE(stack.execute(project, std::make_unique<AddClipCommand>(0, makeClip(source, 0, 1000, 500))));
    const ClipId id = project.sequence().track(0).clips()[0].id;
    const Snapshot before = snapshot(project);

    SECTION("move within a track")
    {
        REQUIRE(stack.execute(project, std::make_unique<MoveClipCommand>(0, id, 0, 5000)));
        CHECK(project.sequence().track(0).clips()[0].timelineStart == 5000);
        stack.undo(project);
        CHECK(snapshot(project) == before);
    }

    SECTION("move across tracks")
    {
        const size_t v2 = project.sequence().addTrack(Track::Kind::Video, "V2");
        const Snapshot beforeMove = snapshot(project);

        REQUIRE(stack.execute(project, std::make_unique<MoveClipCommand>(0, id, v2, 2000)));
        CHECK(project.sequence().track(0).empty());
        CHECK(project.sequence().track(v2).clips().size() == 1);

        stack.undo(project);
        CHECK(snapshot(project) == beforeMove);
    }

    SECTION("trim head")
    {
        REQUIRE(stack.execute(project,
                              std::make_unique<TrimClipCommand>(0, id, TrimClipCommand::Edge::Head, 200)));
        const Clip& trimmed = project.sequence().track(0).clips()[0];
        CHECK(trimmed.timelineStart == 200);
        CHECK(trimmed.duration == 800);
        // Head trim must advance sourceIn too, or the picture would shift.
        CHECK(trimmed.sourceIn == 700);

        stack.undo(project);
        CHECK(snapshot(project) == before);
    }

    SECTION("trim tail")
    {
        REQUIRE(stack.execute(project,
                              std::make_unique<TrimClipCommand>(0, id, TrimClipCommand::Edge::Tail, -300)));
        const Clip& trimmed = project.sequence().track(0).clips()[0];
        CHECK(trimmed.duration == 700);
        CHECK(trimmed.sourceIn == 500);  // tail trim leaves the head alone

        stack.undo(project);
        CHECK(snapshot(project) == before);
    }

    SECTION("split")
    {
        REQUIRE(stack.execute(project, std::make_unique<SplitClipCommand>(0, id, 400)));
        const auto& clips = project.sequence().track(0).clips();
        REQUIRE(clips.size() == 2);

        CHECK(clips[0].duration == 400);
        CHECK(clips[0].sourceIn == 500);
        CHECK(clips[1].timelineStart == 400);
        CHECK(clips[1].duration == 600);
        CHECK(clips[1].sourceIn == 900);  // continues where the left piece stopped

        stack.undo(project);
        CHECK(snapshot(project) == before);
    }

    SECTION("remove")
    {
        REQUIRE(stack.execute(project, std::make_unique<RemoveClipCommand>(0, id)));
        CHECK(project.sequence().track(0).empty());
        stack.undo(project);
        CHECK(snapshot(project) == before);
    }
}

TEST_CASE("trims that would invert or run past the source are rejected", "[commands]")
{
    Project project = seqProject();
    CommandStack stack;
    const MediaId source = addSource(project);

    REQUIRE(stack.execute(project, std::make_unique<AddClipCommand>(0, makeClip(source, 0, 1000, 100))));
    const ClipId id = project.sequence().track(0).clips()[0].id;
    const Snapshot before = snapshot(project);

    // Trimming the head by more than the duration would invert the clip.
    CHECK_FALSE(stack.execute(project,
                              std::make_unique<TrimClipCommand>(0, id, TrimClipCommand::Edge::Head, 1000)));
    // Trimming before the start of the source is meaningless.
    CHECK_FALSE(stack.execute(project,
                              std::make_unique<TrimClipCommand>(0, id, TrimClipCommand::Edge::Head, -200)));
    CHECK_FALSE(stack.execute(project,
                              std::make_unique<TrimClipCommand>(0, id, TrimClipCommand::Edge::Tail, -1000)));

    CHECK(snapshot(project) == before);
}

TEST_CASE("splitting at a clip edge is rejected", "[commands]")
{
    Project project = seqProject();
    CommandStack stack;
    const MediaId source = addSource(project);

    REQUIRE(stack.execute(project, std::make_unique<AddClipCommand>(0, makeClip(source, 1000, 1000))));
    const ClipId id = project.sequence().track(0).clips()[0].id;

    // Either edge would produce a zero-length piece.
    CHECK_FALSE(stack.execute(project, std::make_unique<SplitClipCommand>(0, id, 1000)));
    CHECK_FALSE(stack.execute(project, std::make_unique<SplitClipCommand>(0, id, 2000)));
    CHECK(project.sequence().track(0).clips().size() == 1);
}

TEST_CASE("clips cannot be placed at negative timeline positions", "[commands]")
{
    Project project = seqProject();
    CommandStack stack;
    const MediaId source = addSource(project);

    CHECK_FALSE(stack.execute(project, std::make_unique<AddClipCommand>(0, makeClip(source, -500, 1000))));
    CHECK(project.sequence().track(0).empty());

    REQUIRE(stack.execute(project, std::make_unique<AddClipCommand>(0, makeClip(source, 1000, 1000, 100))));
    const ClipId id = project.sequence().track(0).clips()[0].id;
    const Snapshot before = snapshot(project);

    CHECK_FALSE(stack.execute(project, std::make_unique<MoveClipCommand>(0, id, 0, -1)));
    // Head trim far enough left would drag the clip before zero.
    CHECK_FALSE(stack.execute(project,
                              std::make_unique<TrimClipCommand>(0, id, TrimClipCommand::Edge::Head, -2000)));
    CHECK(snapshot(project) == before);
}

TEST_CASE("clips cannot extend past the end of their source", "[commands]")
{
    Project project = seqProject();
    CommandStack stack;
    const MediaId source = addSource(project, 1000);  // source is only 1000 ticks long

    CHECK_FALSE(stack.execute(project, std::make_unique<AddClipCommand>(0, makeClip(source, 0, 1500))));
    CHECK_FALSE(stack.execute(project, std::make_unique<AddClipCommand>(0, makeClip(source, 0, 600, 500))));
    CHECK(project.sequence().track(0).empty());

    // Exactly reaching the end is fine.
    REQUIRE(stack.execute(project, std::make_unique<AddClipCommand>(0, makeClip(source, 0, 500, 500))));
    const ClipId id = project.sequence().track(0).clips()[0].id;

    // Extending the tail would run past the source.
    CHECK_FALSE(stack.execute(project,
                              std::make_unique<TrimClipCommand>(0, id, TrimClipCommand::Edge::Tail, 1)));
}

TEST_CASE("clips referencing unknown media are rejected", "[commands]")
{
    Project project = seqProject();
    CommandStack stack;

    CHECK_FALSE(stack.execute(project, std::make_unique<AddClipCommand>(0, makeClip(999, 0, 1000))));
    CHECK(project.sequence().track(0).empty());
}

TEST_CASE("compound command applies and undoes as one unit", "[commands]")
{
    Project project = seqProject();
    CommandStack stack;
    const MediaId source = addSource(project);

    // Two linked clips, V1 and A1, sharing a group.
    const LinkGroup group = project.nextLinkGroup();
    Clip v = makeClip(source, 0, 1000);
    v.linkGroup = group;
    Clip a = makeClip(source, 0, 1000);
    a.linkGroup = group;
    REQUIRE(stack.execute(project, std::make_unique<AddClipCommand>(0, v)));
    REQUIRE(stack.execute(project, std::make_unique<AddClipCommand>(1, a)));

    const ClipId vId = project.sequence().track(0).clips()[0].id;
    const ClipId aId = project.sequence().track(1).clips()[0].id;
    const Snapshot before = snapshot(project);

    SECTION("linked move shifts both, undo restores both")
    {
        auto compound = std::make_unique<CompoundCommand>("Move Clips");
        compound->add(std::make_unique<MoveClipCommand>(0, vId, 0, 3000));
        compound->add(std::make_unique<MoveClipCommand>(1, aId, 1, 3000));
        REQUIRE(stack.execute(project, std::move(compound)));

        CHECK(project.sequence().track(0).clips()[0].timelineStart == 3000);
        CHECK(project.sequence().track(1).clips()[0].timelineStart == 3000);

        stack.undo(project);
        CHECK(snapshot(project) == before);
    }

    SECTION("if one child fails the whole compound rolls back")
    {
        // Block the audio move with a second clip it would overlap.
        Clip blocker = makeClip(source, 3000, 1000);
        REQUIRE(stack.execute(project, std::make_unique<AddClipCommand>(1, blocker)));
        const Snapshot withBlocker = snapshot(project);

        auto compound = std::make_unique<CompoundCommand>("Move Clips");
        compound->add(std::make_unique<MoveClipCommand>(0, vId, 0, 3000));  // would succeed
        compound->add(std::make_unique<MoveClipCommand>(1, aId, 1, 3000));  // overlaps blocker
        CHECK_FALSE(stack.execute(project, std::move(compound)));

        // The video move must have been rolled back, not left applied.
        CHECK(snapshot(project) == withBlocker);

        // The failed compound is not on the stack: undo reaches the blocker add.
        stack.undo(project);
        CHECK(project.sequence().track(1).clips().size() == 1);
    }
}

TEST_CASE("unlink clears the group and undo restores it", "[commands]")
{
    Project project = seqProject();
    CommandStack stack;
    const MediaId source = addSource(project);

    const LinkGroup group = project.nextLinkGroup();
    Clip v = makeClip(source, 0, 1000);
    v.linkGroup = group;
    Clip a = makeClip(source, 0, 1000);
    a.linkGroup = group;
    REQUIRE(stack.execute(project, std::make_unique<AddClipCommand>(0, v)));
    REQUIRE(stack.execute(project, std::make_unique<AddClipCommand>(1, a)));

    REQUIRE(project.sequence().clipsInGroup(group).size() == 2);

    REQUIRE(stack.execute(project, std::make_unique<UnlinkGroupCommand>(group)));
    CHECK(project.sequence().clipsInGroup(group).empty());
    CHECK_FALSE(project.sequence().track(0).clips()[0].linked());

    stack.undo(project);
    CHECK(project.sequence().clipsInGroup(group).size() == 2);
    CHECK(project.sequence().track(0).clips()[0].linkGroup == group);
}

TEST_CASE("a new edit clears the redo branch", "[commands]")
{
    Project project = seqProject();
    CommandStack stack;
    const MediaId source = addSource(project);

    REQUIRE(stack.execute(project, std::make_unique<AddClipCommand>(0, makeClip(source, 0, 1000))));
    stack.undo(project);
    REQUIRE(stack.canRedo());

    REQUIRE(stack.execute(project, std::make_unique<AddClipCommand>(0, makeClip(source, 5000, 1000))));
    CHECK_FALSE(stack.canRedo());
}

TEST_CASE("long undo/redo chains stay consistent", "[commands]")
{
    Project project = seqProject();
    CommandStack stack;
    const MediaId source = addSource(project);

    std::vector<Snapshot> history{ snapshot(project) };
    for (int i = 0; i < 10; ++i) {
        REQUIRE(stack.execute(project,
                              std::make_unique<AddClipCommand>(0, makeClip(source, i * 2000, 1000))));
        history.push_back(snapshot(project));
    }

    // Unwind completely, checking every step matches what it was on the way up.
    for (int i = 10; i > 0; --i) {
        stack.undo(project);
        CHECK(snapshot(project) == history[i - 1]);
    }

    for (int i = 1; i <= 10; ++i) {
        stack.redo(project);
        CHECK(snapshot(project) == history[i]);
    }
}
