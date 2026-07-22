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

void Sequence::removeTrackAt(size_t index)
{
    if (index < m_tracks.size()) {
        m_tracks.erase(m_tracks.begin() + index);
    }
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

const Clip* Sequence::topVideoClipAt(Tick time) const
{
    for (auto it = m_tracks.rbegin(); it != m_tracks.rend(); ++it) {
        if (it->kind() != Track::Kind::Video) {
            continue;
        }
        if (const Clip* clip = it->clipAt(time)) {
            return clip;
        }
    }
    return nullptr;
}

const Clip* Sequence::firstAudioClipAt(Tick time) const
{
    for (const Track& track : m_tracks) {
        if (track.kind() != Track::Kind::Audio) {
            continue;
        }
        if (const Clip* clip = track.clipAt(time)) {
            return clip;
        }
    }
    return nullptr;
}

std::vector<Tick> Sequence::cutPoints(Track::Kind kind) const
{
    std::vector<Tick> points;
    for (const Track& track : m_tracks) {
        if (track.kind() != kind) {
            continue;
        }
        for (const Clip& clip : track.clips()) {
            points.push_back(clip.timelineStart);
            points.push_back(clip.range().end());
        }
    }
    std::sort(points.begin(), points.end());
    points.erase(std::unique(points.begin(), points.end()), points.end());
    return points;
}

const Clip* Sequence::findClip(ClipId id, size_t* trackIndex) const
{
    for (size_t i = 0; i < m_tracks.size(); ++i) {
        if (const Clip* clip = m_tracks[i].find(id)) {
            if (trackIndex) {
                *trackIndex = i;
            }
            return clip;
        }
    }
    return nullptr;
}

std::vector<std::pair<size_t, ClipId>> Sequence::clipsInGroup(LinkGroup group) const
{
    std::vector<std::pair<size_t, ClipId>> members;
    if (group == kNoLink) {
        return members;
    }
    for (size_t i = 0; i < m_tracks.size(); ++i) {
        for (const Clip& clip : m_tracks[i].clips()) {
            if (clip.linkGroup == group) {
                members.emplace_back(i, clip.id);
            }
        }
    }
    return members;
}

bool Sequence::hasClips(Track::Kind kind) const
{
    for (const Track& track : m_tracks) {
        if (track.kind() == kind && !track.empty()) {
            return true;
        }
    }
    return false;
}

Tick Sequence::snapToFrame(Tick time) const
{
    const Tick frame = frameDuration();
    // Floor division, correct for negatives too.
    const Tick index = time >= 0 ? time / frame : -((-time + frame - 1) / frame);
    return index * frame;
}

}  // namespace hopline
