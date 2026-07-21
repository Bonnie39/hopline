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
    project.sequence().setFrameRate(30000, 1001);
    project.sequence().setResolution(1920, 1080);

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
    stack.execute(project, std::make_unique<AddClipCommand>(0, second));
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

    // Sequence structure.
    REQUIRE(loaded.sequence().trackCount() == original.sequence().trackCount());
    CHECK(loaded.sequence().rateNum() == original.sequence().rateNum());
    CHECK(loaded.sequence().rateDen() == original.sequence().rateDen());

    for (size_t t = 0; t < original.sequence().trackCount(); ++t) {
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
