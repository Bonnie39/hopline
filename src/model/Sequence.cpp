#include "model/Sequence.h"

#include <algorithm>

namespace hopline {

Sequence::Sequence()
{
    addTrack(Track::Kind::Video, "V1");
    addTrack(Track::Kind::Audio, "A1");
}

void Sequence::setFrameRate(int num, int den)
{
    if (num > 0 && den > 0) {
        m_rateNum = num;
        m_rateDen = den;
    }
}

void Sequence::setResolution(int width, int height)
{
    if (width > 0 && height > 0) {
        m_width = width;
        m_height = height;
    }
}

size_t Sequence::addTrack(Track::Kind kind, std::string name)
{
    m_tracks.emplace_back(kind, std::move(name));
    return m_tracks.size() - 1;
}

Tick Sequence::duration() const
{
    Tick longest = 0;
    for (const Track& track : m_tracks) {
        longest = std::max(longest, track.duration());
    }
    return longest;
}

std::optional<Resolved> Sequence::resolveVideoAt(Tick time) const
{
    // Reverse: later video tracks composite over earlier ones.
    for (auto it = m_tracks.rbegin(); it != m_tracks.rend(); ++it) {
        if (it->kind() != Track::Kind::Video) {
            continue;
        }
        if (const Clip* clip = it->clipAt(time)) {
            return Resolved{ clip->id, clip->source, clip->sourceTimeAt(time) };
        }
    }
    return std::nullopt;
}

std::vector<Resolved> Sequence::resolveAudioAt(Tick time) const
{
    std::vector<Resolved> active;
    for (const Track& track : m_tracks) {
        if (track.kind() != Track::Kind::Audio) {
            continue;
        }
        if (const Clip* clip = track.clipAt(time)) {
            active.push_back({ clip->id, clip->source, clip->sourceTimeAt(time) });
        }
    }
    return active;
}

Tick Sequence::snapToFrame(Tick time) const
{
    const Tick frame = frameDuration();
    // Floor division, correct for negatives too.
    const Tick index = time >= 0 ? time / frame : -((-time + frame - 1) / frame);
    return index * frame;
}

}  // namespace hopline
