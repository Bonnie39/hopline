#pragma once

#include <string>
#include <vector>

#include "model/Clip.h"

namespace hopline {

// Clips are kept sorted by timelineStart and never overlap. Callers go through
// insert()/remove() so that invariant holds; commands rely on it.
class Track {
public:
    enum class Kind { Video, Audio };

    Track() = default;
    Track(Kind kind, std::string name)
        : m_kind(kind)
        , m_name(std::move(name))
    {
    }

    Kind kind() const { return m_kind; }
    const std::string& name() const { return m_name; }

    // Playback toggles. visible hides a video track from the composite; muted/soloed gate an
    // audio track in the mix (any soloed track silences the un-soloed ones). Playback-only, so
    // they mutate in place like a label.
    bool visible() const { return m_visible; }
    void setVisible(bool v) { m_visible = v; }
    bool muted() const { return m_muted; }
    void setMuted(bool m) { m_muted = m; }
    bool soloed() const { return m_soloed; }
    void setSoloed(bool s) { m_soloed = s; }

    const std::vector<Clip>& clips() const { return m_clips; }
    bool empty() const { return m_clips.empty(); }

    // False if it would overlap an existing clip; the track is left unchanged.
    bool insert(const Clip& clip);
    bool remove(ClipId id, Clip* removed = nullptr);

    const Clip* find(ClipId id) const;
    const Clip* clipAt(Tick time) const;

    // Link group doesn't affect ordering or overlap, so it's safe to change in place.
    bool setLinkGroup(ClipId id, LinkGroup group);
    // Label is cosmetic, so like the link group it's safe to change in place.
    bool setLabel(ClipId id, int label);
    // Effects don't affect ordering/overlap either, so they change in place too.
    bool setTransform(ClipId id, const Transform& transform);
    bool setAudioLevels(ClipId id, const AudioLevels& audio);

    // Whether `range` would fit, ignoring the clip being moved or trimmed.
    bool isFree(const TimeRange& range, ClipId ignore = kInvalidClip) const;

    Tick duration() const;

private:
    Kind m_kind = Kind::Video;
    std::string m_name;
    bool m_visible = true;
    bool m_muted = false;
    bool m_soloed = false;
    std::vector<Clip> m_clips;
};

}  // namespace hopline
