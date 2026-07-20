#pragma once

#include <vector>

#include "model/Media.h"
#include "model/Sequence.h"

namespace hopline {

// Owns the media pool and the sequence, and hands out ids. Commands mutate the
// project; nothing else should.
class Project {
public:
    MediaId addMedia(MediaSource source);
    const MediaSource* media(MediaId id) const;
    const std::vector<MediaSource>& mediaPool() const { return m_media; }

    Sequence& sequence() { return m_sequence; }
    const Sequence& sequence() const { return m_sequence; }

    // Ids are never reused, so undo/redo can round-trip a clip unchanged.
    ClipId nextClipId() { return ++m_lastClipId; }
    ClipId peekClipId() const { return m_lastClipId; }
    void reserveClipId(ClipId id);

private:
    std::vector<MediaSource> m_media;
    Sequence m_sequence;
    MediaId m_lastMediaId = kInvalidMedia;
    ClipId m_lastClipId = kInvalidClip;
};

}  // namespace hopline
