#include <algorithm>
#include <cstdio>

#include <catch2/catch_test_macros.hpp>

#include "model/Commands.h"
#include "model/Project.h"
#include "model/ProjectIO.h"

using namespace hopline;

namespace {

MediaSource makeSource(Tick duration)
{
    MediaSource s;
    s.path = "C:/clips/take.mp4";
    s.duration = duration;
    s.hasVideo = true;
    s.hasAudio = true;
    s.width = 1920;
    s.height = 1080;
    s.rateNum = 30000;
    s.rateDen = 1001;
    s.sampleRate = 48000;
    s.channels = 2;
    return s;
}

Project buildProject()
{
    Project project;
    const FolderId sub = project.addFolder(kRootFolder, "B-roll");
    const MediaId a = project.addMedia(makeSource(100000), kRootFolder);
    const MediaId b = project.addMedia(makeSource(50000), sub);
    project.setMediaLabel(a, 4);   // exercise media label round-trip
    project.setFolderLabel(sub, 5);  // exercise folder label round-trip
    const SequenceId seqId = project.addSequence("Main Edit", 30000, 1001, 1920, 1080, sub);
    project.setActiveSequence(seqId);

    CommandStack stack;
    const LinkGroup group = project.nextLinkGroup();
    Clip v;
    v.source = a;
    v.timelineStart = 0;
    v.duration = 40000;
    v.linkGroup = group;
    v.label = 3;  // exercise clip label round-trip
    Clip audio = v;
    stack.execute(project, std::make_unique<AddClipCommand>(0, v));
    stack.execute(project, std::make_unique<AddClipCommand>(1, audio));

    Clip second;
    second.source = b;
    second.timelineStart = 40000;
    second.sourceIn = 5000;
    second.duration = 20000;
    second.transform = { 0.5, 120.0, -30.0, 45.0, 0.8, BlendMode::Screen };  // exercise transform round-trip
    second.audio = { -6.0, 0.5 };                                            // exercise audio round-trip
    second.transform.posX.setKeyframe(0, 200.0);                             // exercise keyframe round-trip
    second.transform.posX.setKeyframe(10000, 500.0);
    second.audio.volumeDb.setKeyframe(5000, -12.0);
    stack.execute(project, std::make_unique<AddClipCommand>(0, second));

    project.sequence().track(0).setVisible(false);  // exercise track toggle round-trip
    project.sequence().track(1).setMuted(true);
    project.sequence().track(1).setSoloed(true);
    return project;
}

}  // namespace

TEST_CASE("a project round-trips through serialize/deserialize", "[projectio]")
{
    const Project original = buildProject();
    const std::string json = serializeProject(original);

    Project loaded;
    std::string error;
    REQUIRE(deserializeProject(json, loaded, error));

    // Folders, including label colors.
    REQUIRE(loaded.folders().size() == original.folders().size());
    for (const BinFolder& f : original.folders()) {
        const auto it = std::find_if(loaded.folders().begin(), loaded.folders().end(),
                                     [&](const BinFolder& g) { return g.id == f.id; });
        REQUIRE(it != loaded.folders().end());
        CHECK(it->label == f.label);
    }

    // Media pool, ids and metadata preserved.
    REQUIRE(loaded.mediaPool().size() == original.mediaPool().size());
    for (const MediaSource& m : original.mediaPool()) {
        const MediaSource* l = loaded.media(m.id);
        REQUIRE(l != nullptr);
        CHECK(l->path == m.path);
        CHECK(l->folder == m.folder);
        CHECK(l->label == m.label);
        CHECK(l->duration == m.duration);
        CHECK(l->rateNum == m.rateNum);
        CHECK(l->rateDen == m.rateDen);
    }

    // Sequences: list, active id, and per-sequence identity round-trip.
    REQUIRE(loaded.sequences().size() == original.sequences().size());
    CHECK(loaded.activeSequenceId() == original.activeSequenceId());
    CHECK(loaded.sequence().name() == original.sequence().name());
    CHECK(loaded.sequence().folder() == original.sequence().folder());
    CHECK(loaded.sequence().id() == original.sequence().id());

    // Sequence structure.
    REQUIRE(loaded.sequence().trackCount() == original.sequence().trackCount());
    CHECK(loaded.sequence().rateNum() == original.sequence().rateNum());
    CHECK(loaded.sequence().rateDen() == original.sequence().rateDen());

    for (size_t t = 0; t < original.sequence().trackCount(); ++t) {
        CHECK(loaded.sequence().track(t).visible() == original.sequence().track(t).visible());
        CHECK(loaded.sequence().track(t).muted() == original.sequence().track(t).muted());
        CHECK(loaded.sequence().track(t).soloed() == original.sequence().track(t).soloed());
        const auto& oc = original.sequence().track(t).clips();
        const auto& lc = loaded.sequence().track(t).clips();
        REQUIRE(lc.size() == oc.size());
        for (size_t i = 0; i < oc.size(); ++i) {
            CHECK(lc[i].id == oc[i].id);
            CHECK(lc[i].source == oc[i].source);
            CHECK(lc[i].timelineStart == oc[i].timelineStart);
            CHECK(lc[i].sourceIn == oc[i].sourceIn);
            CHECK(lc[i].duration == oc[i].duration);
            CHECK(lc[i].linkGroup == oc[i].linkGroup);
            CHECK(lc[i].label == oc[i].label);
            CHECK(lc[i].transform.scale == oc[i].transform.scale);
            CHECK(lc[i].transform.posX == oc[i].transform.posX);
            CHECK(lc[i].transform.posY == oc[i].transform.posY);
            CHECK(lc[i].transform.rotation == oc[i].transform.rotation);
            CHECK(lc[i].transform.opacity == oc[i].transform.opacity);
            CHECK(lc[i].transform.blend == oc[i].transform.blend);
            CHECK(lc[i].audio.volumeDb == oc[i].audio.volumeDb);
            CHECK(lc[i].audio.pan == oc[i].audio.pan);
        }
    }
}

TEST_CASE("id counters survive a round-trip so new ids don't collide", "[projectio]")
{
    const Project original = buildProject();
    Project loaded;
    std::string error;
    REQUIRE(deserializeProject(serializeProject(original), loaded, error));

    // The next clip id must exceed every id already loaded.
    const ClipId next = loaded.nextClipId();
    for (size_t t = 0; t < loaded.sequence().trackCount(); ++t) {
        for (const Clip& c : loaded.sequence().track(t).clips()) {
            CHECK(c.id < next);
        }
    }
}

TEST_CASE("project survives a save/load through a file", "[projectio]")
{
    const Project original = buildProject();
    const std::string path = "hopline_roundtrip.hop";

    std::string error;
    REQUIRE(saveProject(original, path, error));

    Project loaded;
    REQUIRE(loadProject(path, loaded, error));
    CHECK(loaded.mediaPool().size() == original.mediaPool().size());
    CHECK(loaded.sequence().track(0).clips().size() == original.sequence().track(0).clips().size());

    std::remove(path.c_str());
}

TEST_CASE("deserialize rejects junk and wrong versions", "[projectio]")
{
    Project project;
    std::string error;
    CHECK_FALSE(deserializeProject("not json", project, error));
    CHECK_FALSE(deserializeProject(R"({"version":999})", project, error));
}

TEST_CASE("a v1 project without labels still loads (forward-compatible)", "[projectio]")
{
    const char* v1 = R"({
        "version": 1,
        "media": [ { "id": 7, "folder": 1, "path": "C:/a.mp4", "duration": 1000 } ],
        "sequence": { "rateNum": 30, "rateDen": 1, "width": 1920, "height": 1080, "tracks": [] }
    })";

    Project loaded;
    std::string error;
    REQUIRE(deserializeProject(v1, loaded, error));
    const MediaSource* m = loaded.media(7);
    REQUIRE(m != nullptr);
    CHECK(m->label == 0);  // missing field defaults, no crash
}
