#include "model/Track.h"

#include <algorithm>

namespace hopline {

bool Track::isFree(const TimeRange& range, ClipId ignore) const
{
    for (const Clip& clip : m_clips) {
        if (clip.id != ignore && clip.range().overlaps(range)) {
            return false;
        }
    }
    return true;
}

bool Track::insert(const Clip& clip)
{
    if (clip.duration <= 0 || !isFree(clip.range())) {
        return false;
    }

    const auto pos = std::lower_bound(m_clips.begin(), m_clips.end(), clip.timelineStart,
                                      [](const Clip& c, Tick t) { return c.timelineStart < t; });
    m_clips.insert(pos, clip);
    return true;
}

bool Track::remove(ClipId id, Clip* removed)
{
    const auto it = std::find_if(m_clips.begin(), m_clips.end(),
                                 [id](const Clip& c) { return c.id == id; });
    if (it == m_clips.end()) {
        return false;
    }
    if (removed) {
        *removed = *it;
    }
    m_clips.erase(it);
    return true;
}

const Clip* Track::find(ClipId id) const
{
    const auto it = std::find_if(m_clips.begin(), m_clips.end(),
                                 [id](const Clip& c) { return c.id == id; });
    return it != m_clips.end() ? &*it : nullptr;
}

const Clip* Track::clipAt(Tick time) const
{
    for (const Clip& clip : m_clips) {
        if (clip.range().contains(time)) {
            return &clip;
        }
        if (clip.timelineStart > time) {
            break;  // sorted, so nothing later can match
        }
    }
    return nullptr;
}

Tick Track::duration() const
{
    return m_clips.empty() ? 0 : m_clips.back().range().end();
}

}  // namespace hopline
