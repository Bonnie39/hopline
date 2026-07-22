#include "model/ProjectIO.h"

#include <fstream>
#include <sstream>

#include <nlohmann/json.hpp>

#include "model/Project.h"

namespace hopline {
namespace {

constexpr int kFormatVersion = 4;

using nlohmann::json;

json mediaToJson(const MediaSource& m)
{
    return json{
        { "id", m.id }, { "folder", m.folder }, { "label", m.label },
        { "path", m.path }, { "duration", m.duration },
        { "hasVideo", m.hasVideo }, { "hasAudio", m.hasAudio },
        { "width", m.width }, { "height", m.height },
        { "rateNum", m.rateNum }, { "rateDen", m.rateDen },
        { "sampleRate", m.sampleRate }, { "channels", m.channels },
    };
}

MediaSource mediaFromJson(const json& j)
{
    MediaSource m;
    m.id = j.at("id").get<MediaId>();
    m.folder = j.value("folder", kRootFolder);
    m.label = j.value("label", 0);
    m.path = j.at("path").get<std::string>();
    m.duration = j.at("duration").get<Tick>();
    m.hasVideo = j.value("hasVideo", false);
    m.hasAudio = j.value("hasAudio", false);
    m.width = j.value("width", 0);
    m.height = j.value("height", 0);
    m.rateNum = j.value("rateNum", 0);
    m.rateDen = j.value("rateDen", 1);
    m.sampleRate = j.value("sampleRate", 0);
    m.channels = j.value("channels", 0);
    return m;
}

json clipToJson(const Clip& c)
{
    return json{
        { "id", c.id }, { "source", c.source }, { "timelineStart", c.timelineStart },
        { "sourceIn", c.sourceIn }, { "duration", c.duration }, { "linkGroup", c.linkGroup },
        { "label", c.label },
    };
}

Clip clipFromJson(const json& j)
{
    Clip c;
    c.id = j.at("id").get<ClipId>();
    c.source = j.at("source").get<MediaId>();
    c.timelineStart = j.at("timelineStart").get<Tick>();
    c.sourceIn = j.at("sourceIn").get<Tick>();
    c.duration = j.at("duration").get<Tick>();
    c.linkGroup = j.value("linkGroup", kNoLink);
    c.label = j.value("label", 0);
    return c;
}

json sequenceToJson(const Sequence& seq)
{
    json jseq;
    jseq["id"] = seq.id();
    jseq["name"] = seq.name();
    jseq["folder"] = seq.folder();
    jseq["rateNum"] = seq.rateNum();
    jseq["rateDen"] = seq.rateDen();
    jseq["width"] = seq.width();
    jseq["height"] = seq.height();
    for (size_t i = 0; i < seq.trackCount(); ++i) {
        const Track& track = seq.track(i);
        json jtrack;
        jtrack["kind"] = track.kind() == Track::Kind::Video ? "video" : "audio";
        jtrack["name"] = track.name();
        for (const Clip& clip : track.clips()) {
            jtrack["clips"].push_back(clipToJson(clip));
        }
        jseq["tracks"].push_back(std::move(jtrack));
    }
    return jseq;
}

Sequence sequenceFromJson(const json& jseq, Project& out)
{
    Sequence seq;
    seq.clear();  // drop the default V1/A1; rebuild from json
    seq.setId(jseq.value("id", SequenceId{ 0 }));
    seq.setName(jseq.value("name", std::string("Sequence")));
    seq.setFolder(jseq.value("folder", kRootFolder));
    seq.setFrameRate(jseq.value("rateNum", 30), jseq.value("rateDen", 1));
    seq.setResolution(jseq.value("width", 1920), jseq.value("height", 1080));
    for (const json& jt : jseq.value("tracks", json::array())) {
        const auto kind = jt.value("kind", "video") == "audio" ? Track::Kind::Audio : Track::Kind::Video;
        const size_t index = seq.addTrack(kind, jt.value("name", std::string("Track")));
        for (const json& jc : jt.value("clips", json::array())) {
            const Clip clip = clipFromJson(jc);
            seq.track(index).insert(clip);
            out.reserveClipId(clip.id);
            out.reserveLinkGroup(clip.linkGroup);
        }
    }
    return seq;
}

}  // namespace

std::string serializeProject(const Project& project)
{
    json root;
    root["version"] = kFormatVersion;

    for (const BinFolder& folder : project.folders()) {
        root["folders"].push_back({ { "id", folder.id }, { "parent", folder.parent },
                                    { "name", folder.name }, { "label", folder.label } });
    }
    for (const MediaSource& media : project.mediaPool()) {
        root["media"].push_back(mediaToJson(media));
    }

    for (const Sequence& seq : project.sequences()) {
        root["sequences"].push_back(sequenceToJson(seq));
    }
    root["activeSequence"] = project.activeSequenceId();

    return root.dump(2);
}

bool deserializeProject(const std::string& text, Project& out, std::string& error)
{
    json root = json::parse(text, nullptr, false);
    if (root.is_discarded()) {
        error = "not valid JSON";
        return false;
    }
    const int version = root.value("version", 0);
    if (version < 1 || version > kFormatVersion) {
        error = "unsupported project version";
        return false;
    }

    out.clearForLoad();

    for (const json& jf : root.value("folders", json::array())) {
        out.restoreFolder({ jf.at("id").get<FolderId>(), jf.value("parent", FolderId{ 0 }),
                            jf.at("name").get<std::string>(), jf.value("label", 0) });
    }
    for (const json& jm : root.value("media", json::array())) {
        out.restoreMedia(mediaFromJson(jm));
    }

    if (root.contains("sequences")) {
        for (const json& js : root["sequences"]) {
            out.restoreSequence(sequenceFromJson(js, out));
        }
        out.setActiveSequence(root.value("activeSequence", SequenceId{ 0 }));
    } else if (root.contains("sequence")) {
        // Backward compat: v1-3 stored a single unnamed sequence.
        Sequence seq = sequenceFromJson(root["sequence"], out);
        if (seq.id() == kInvalidSequence) {
            seq.setId(1);
        }
        if (seq.name().empty()) {
            seq.setName("Sequence 1");
        }
        const SequenceId id = seq.id();
        out.restoreSequence(std::move(seq));
        out.setActiveSequence(id);
    }

    return true;
}

bool saveProject(const Project& project, const std::string& path, std::string& error)
{
    std::ofstream file(path, std::ios::binary);
    if (!file) {
        error = "could not open file for writing";
        return false;
    }
    file << serializeProject(project);
    return true;
}

bool loadProject(const std::string& path, Project& out, std::string& error)
{
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        error = "could not open file";
        return false;
    }
    std::ostringstream buffer;
    buffer << file.rdbuf();
    return deserializeProject(buffer.str(), out, error);
}

}  // namespace hopline
