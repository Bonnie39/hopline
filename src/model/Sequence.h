#pragma once

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "model/Media.h"
#include "model/Track.h"

namespace hopline {

// What playback needs at a given instant: which clip, and where in its source.
struct Resolved {
    ClipId clip = kInvalidClip;
    MediaId source = kInvalidMedia;
    Tick sourceTime = 0;
};

// Video tracks composite bottom-up, so the last video track wins. Audio tracks
// all contribute and get mixed.
class Sequence {
public:
    Sequence();

    // Identity in the project bin (0/empty for the legacy default sequence).
    SequenceId id() const { return m_id; }
    void setId(SequenceId id) { m_id = id; }
    const std::string& name() const { return m_name; }
    void setName(std::string name) { m_name = std::move(name); }
    FolderId folder() const { return m_folder; }
    void setFolder(FolderId folder) { m_folder = folder; }

    int rateNum() const { return m_rateNum; }
    int rateDen() const { return m_rateDen; }
    void setFrameRate(int num, int den = 1);
    Tick frameDuration() const { return ticksPerFrame(m_rateNum, m_rateDen); }

    int width() const { return m_width; }
    int height() const { return m_height; }
    void setResolution(int width, int height);

    const std::vector<Track>& tracks() const { return m_tracks; }
    Track& track(size_t index) { return m_tracks[index]; }
    const Track& track(size_t index) const { return m_tracks[index]; }
    size_t trackCount() const { return m_tracks.size(); }

    size_t addTrack(Track::Kind kind, std::string name);
    void removeTrackAt(size_t index);
    void clear() { m_tracks.clear(); }  // for deserialization; leaves no default tracks

    Tick duration() const;

    // Topmost video clip covering `time`, if any.
    std::optional<Resolved> resolveVideoAt(Tick time) const;
    std::vector<Resolved> resolveAudioAt(Tick time) const;

    // Clip pointers valid until the sequence is mutated. Playback walks a
    // snapshot copy, so the lifetime is the snapshot's.
    const Clip* topVideoClipAt(Tick time) const;
    const Clip* firstAudioClipAt(Tick time) const;

    // Sorted, unique clip start/end ticks across tracks of one kind. Between two
    // consecutive cut points the active clip never changes, so playback can
    // decode a whole segment without re-resolving per frame.
    std::vector<Tick> cutPoints(Track::Kind kind) const;
    bool hasClips(Track::Kind kind) const;

    // Locates a clip by id anywhere in the sequence; sets *trackIndex if given.
    const Clip* findClip(ClipId id, size_t* trackIndex = nullptr) const;

    // All clips sharing a nonzero link group, as (trackIndex, clipId). Empty for
    // kNoLink.
    std::vector<std::pair<size_t, ClipId>> clipsInGroup(LinkGroup group) const;

    // Snaps to the nearest frame boundary at or before `time`.
    Tick snapToFrame(Tick time) const;

private:
    SequenceId m_id = kInvalidSequence;
    std::string m_name;
    FolderId m_folder = kRootFolder;
    std::vector<Track> m_tracks;
    int m_rateNum = 30;
    int m_rateDen = 1;
    int m_width = 1920;
    int m_height = 1080;
};

}  // namespace hopline
