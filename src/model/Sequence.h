#pragma once

#include <optional>
#include <vector>

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

    Tick duration() const;

    // Topmost video clip covering `time`, if any.
    std::optional<Resolved> resolveVideoAt(Tick time) const;
    std::vector<Resolved> resolveAudioAt(Tick time) const;

    // Snaps to the nearest frame boundary at or before `time`.
    Tick snapToFrame(Tick time) const;

private:
    std::vector<Track> m_tracks;
    int m_rateNum = 30;
    int m_rateDen = 1;
    int m_width = 1920;
    int m_height = 1080;
};

}  // namespace hopline
