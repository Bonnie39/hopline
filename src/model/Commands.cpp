#include "model/Commands.h"

#include <algorithm>

#include "model/Project.h"

namespace hopline {
namespace {

// A clip must sit at a non-negative timeline position and stay inside its
// source, or playback would resolve to source time that doesn't exist.
bool isPlaceable(const Project& project, const Clip& clip)
{
    if (clip.duration <= 0 || clip.timelineStart < 0 || clip.sourceIn < 0) {
        return false;
    }
    const MediaSource* media = project.media(clip.source);
    return media != nullptr && clip.sourceIn + clip.duration <= media->duration;
}

}  // namespace

bool AddClipCommand::apply(Project& project)
{
    if (m_track >= project.sequence().trackCount() || !isPlaceable(project, m_clip)) {
        return false;
    }
    if (m_clip.id == kInvalidClip) {
        m_clip.id = project.nextClipId();
    } else {
        project.reserveClipId(m_clip.id);  // redo path: keep the original id
    }
    return project.sequence().track(m_track).insert(m_clip);
}

void AddClipCommand::undo(Project& project)
{
    project.sequence().track(m_track).remove(m_clip.id);
}

bool ClearRegionCommand::apply(Project& project)
{
    Sequence& sequence = project.sequence();
    if (m_track >= sequence.trackCount()) {
        return false;
    }

    if (m_captured) {  // redo: reproduce the same edit deterministically
        for (const auto& [t, c] : m_removed) {
            sequence.track(t).remove(c.id);
        }
        for (const auto& [t, p] : m_pieces) {
            project.reserveClipId(p.id);
            sequence.track(t).insert(p);
        }
        return true;
    }
    m_captured = true;
    if (m_region.duration <= 0) {
        return true;
    }

    // The excluded clips (e.g. the ones being moved) and their link groups are left alone.
    std::vector<LinkGroup> exGroups;
    for (ClipId ex : m_exclude) {
        if (const Clip* e = sequence.findClip(ex); e && e->linked()) {
            exGroups.push_back(e->linkGroup);
        }
    }
    auto excluded = [&](const Clip& c) {
        if (std::find(m_exclude.begin(), m_exclude.end(), c.id) != m_exclude.end()) {
            return true;
        }
        return c.linked() && std::find(exGroups.begin(), exGroups.end(), c.linkGroup) != exGroups.end();
    };

    // Clips overlapping the region on m_track, expanded to whole link groups so a linked
    // clip is cleared as a unit (its partners on other tracks are cleared over the region too).
    std::vector<std::pair<size_t, ClipId>> targets;
    auto known = [&](ClipId id) {
        for (const auto& [t, i] : targets) {
            if (i == id) {
                return true;
            }
        }
        return false;
    };
    for (const Clip& c : sequence.track(m_track).clips()) {
        if (excluded(c) || !c.range().overlaps(m_region)) {
            continue;
        }
        if (c.linked()) {
            for (const auto& member : sequence.clipsInGroup(c.linkGroup)) {
                if (!known(member.second)) {
                    targets.push_back(member);
                }
            }
        } else if (!known(c.id)) {
            targets.push_back({ m_track, c.id });
        }
    }

    for (const auto& [track, id] : targets) {
        const Clip* cp = sequence.track(track).find(id);
        if (!cp || excluded(*cp) || !cp->range().overlaps(m_region)) {
            continue;  // a partner may not overlap, or belongs to an excluded group
        }
        Clip c = *cp;
        m_removed.push_back({ track, c });
        sequence.track(track).remove(id);
        if (c.timelineStart < m_region.start) {  // keep the part before the region
            Clip left = c;
            left.id = project.nextClipId();
            left.duration = m_region.start - c.timelineStart;
            m_pieces.push_back({ track, left });
            sequence.track(track).insert(left);
        }
        if (c.range().end() > m_region.end()) {  // keep the part after the region
            Clip right = c;
            right.id = project.nextClipId();
            right.timelineStart = m_region.end();
            right.sourceIn = c.sourceTimeAt(m_region.end());
            right.duration = c.range().end() - m_region.end();
            m_pieces.push_back({ track, right });
            sequence.track(track).insert(right);
        }
    }
    return true;
}

void ClearRegionCommand::undo(Project& project)
{
    Sequence& sequence = project.sequence();
    for (const auto& [t, p] : m_pieces) {
        if (t < sequence.trackCount()) {
            sequence.track(t).remove(p.id);
        }
    }
    for (const auto& [t, c] : m_removed) {
        if (t < sequence.trackCount()) {
            sequence.track(t).insert(c);
        }
    }
}

bool MoveClipsCommand::apply(Project& project)
{
    Sequence& sequence = project.sequence();
    m_originals.clear();
    for (const auto& [track, id] : m_members) {
        if (track >= sequence.trackCount()) {
            return false;
        }
        const Clip* c = sequence.track(track).find(id);
        if (!c) {
            return false;
        }
        m_originals.push_back({ track, *c });
    }

    for (const auto& [track, c] : m_originals) {  // remove all first — no transient overlap
        sequence.track(track).remove(c.id);
    }
    for (const auto& [track, orig] : m_originals) {  // validate against non-members
        Clip moved = orig;
        moved.timelineStart += m_delta;
        if (!isPlaceable(project, moved) || !sequence.track(track).isFree(moved.range())) {
            for (const auto& [t2, c2] : m_originals) {
                sequence.track(t2).insert(c2);  // rollback
            }
            return false;
        }
    }
    for (const auto& [track, orig] : m_originals) {
        Clip moved = orig;
        moved.timelineStart += m_delta;
        sequence.track(track).insert(moved);
    }
    return true;
}

void MoveClipsCommand::undo(Project& project)
{
    Sequence& sequence = project.sequence();
    for (const auto& [track, orig] : m_originals) {
        sequence.track(track).remove(orig.id);
    }
    for (const auto& [track, orig] : m_originals) {
        sequence.track(track).insert(orig);
    }
}

bool LinkClipsCommand::apply(Project& project)
{
    if (m_members.size() < 2) {
        return false;
    }
    Sequence& sequence = project.sequence();
    if (m_group == kNoLink) {
        m_group = project.nextLinkGroup();
    } else {
        project.reserveLinkGroup(m_group);  // redo: keep the original group id
    }

    m_old.clear();
    for (const auto& [track, id] : m_members) {
        const Clip* c = track < sequence.trackCount() ? sequence.track(track).find(id) : nullptr;
        m_old.push_back(c ? c->linkGroup : kNoLink);
    }
    bool any = false;
    for (const auto& [track, id] : m_members) {
        if (track < sequence.trackCount() && sequence.track(track).setLinkGroup(id, m_group)) {
            any = true;
        }
    }
    return any;
}

void LinkClipsCommand::undo(Project& project)
{
    Sequence& sequence = project.sequence();
    for (std::size_t i = 0; i < m_members.size(); ++i) {
        const auto& [track, id] = m_members[i];
        if (track < sequence.trackCount()) {
            sequence.track(track).setLinkGroup(id, m_old[i]);
        }
    }
}

bool RemoveClipCommand::apply(Project& project)
{
    if (m_track >= project.sequence().trackCount()) {
        return false;
    }
    return project.sequence().track(m_track).remove(m_id, &m_removed);
}

void RemoveClipCommand::undo(Project& project)
{
    project.sequence().track(m_track).insert(m_removed);
}

bool MoveClipCommand::apply(Project& project)
{
    Sequence& sequence = project.sequence();
    if (m_from >= sequence.trackCount() || m_to >= sequence.trackCount()) {
        return false;
    }

    const Clip* existing = sequence.track(m_from).find(m_id);
    if (!existing) {
        return false;
    }
    m_original = *existing;

    Clip moved = m_original;
    moved.timelineStart = m_newStart;
    if (!isPlaceable(project, moved)) {
        return false;
    }

    // Ignore the clip itself only when it isn't leaving the track.
    const ClipId ignore = m_from == m_to ? m_id : kInvalidClip;
    if (!sequence.track(m_to).isFree(moved.range(), ignore)) {
        return false;
    }

    sequence.track(m_from).remove(m_id);
    return sequence.track(m_to).insert(moved);
}

void MoveClipCommand::undo(Project& project)
{
    project.sequence().track(m_to).remove(m_id);
    project.sequence().track(m_from).insert(m_original);
}

bool RollEditCommand::apply(Project& project)
{
    Sequence& sequence = project.sequence();
    if (m_track >= sequence.trackCount()) {
        return false;
    }
    Track& track = sequence.track(m_track);
    const Clip* lp = track.find(m_left);
    const Clip* rp = track.find(m_right);
    if (!lp || !rp) {
        return false;
    }
    m_origLeft = *lp;
    m_origRight = *rp;

    Clip nl = m_origLeft;
    nl.duration += m_delta;  // left tail
    Clip nr = m_origRight;
    nr.timelineStart += m_delta;  // right head (moves in source + timeline together)
    nr.sourceIn += m_delta;
    nr.duration -= m_delta;
    if (!isPlaceable(project, nl) || !isPlaceable(project, nr)) {
        return false;
    }

    track.remove(m_left);
    track.remove(m_right);
    if (!track.isFree(nl.range()) || !track.isFree(nr.range())) {
        track.insert(m_origLeft);
        track.insert(m_origRight);
        return false;
    }
    track.insert(nl);
    track.insert(nr);
    return true;
}

void RollEditCommand::undo(Project& project)
{
    Track& track = project.sequence().track(m_track);
    track.remove(m_left);
    track.remove(m_right);
    track.insert(m_origLeft);
    track.insert(m_origRight);
}

bool TrimClipCommand::apply(Project& project)
{
    Sequence& sequence = project.sequence();
    if (m_track >= sequence.trackCount()) {
        return false;
    }

    const Clip* existing = sequence.track(m_track).find(m_id);
    if (!existing) {
        return false;
    }
    m_original = *existing;

    Clip trimmed = m_original;
    if (m_edge == Edge::Head) {
        // Head trim consumes source and timeline together.
        trimmed.timelineStart += m_delta;
        trimmed.sourceIn += m_delta;
        trimmed.duration -= m_delta;
    } else {
        trimmed.duration += m_delta;
    }

    if (!isPlaceable(project, trimmed)) {
        return false;
    }
    if (!sequence.track(m_track).isFree(trimmed.range(), m_id)) {
        return false;
    }

    sequence.track(m_track).remove(m_id);
    return sequence.track(m_track).insert(trimmed);
}

void TrimClipCommand::undo(Project& project)
{
    project.sequence().track(m_track).remove(m_id);
    project.sequence().track(m_track).insert(m_original);
}

bool RippleTrimCommand::apply(Project& project)
{
    Sequence& sequence = project.sequence();
    if (m_track >= sequence.trackCount() || m_delta <= 0) {
        return false;
    }
    Track& track = sequence.track(m_track);
    const Clip* existing = track.find(m_id);
    if (!existing) {
        return false;
    }
    const Clip original = *existing;
    const Tick origEnd = original.range().end();

    Clip trimmed = original;
    if (m_edge == Edge::Head) {
        trimmed.sourceIn += m_delta;   // drop the head; timelineStart stays (ripple realigns it)
        trimmed.duration -= m_delta;
    } else {
        trimmed.duration -= m_delta;   // drop the tail
    }
    if (!isPlaceable(project, trimmed)) {
        return false;
    }

    // The trimmed clip plus every later clip on the track (they slide left to close the gap).
    m_originals.clear();
    m_originals.push_back(original);
    for (const Clip& c : track.clips()) {
        if (c.id != m_id && c.timelineStart >= origEnd) {
            m_originals.push_back(c);
        }
    }

    for (const Clip& c : m_originals) {
        track.remove(c.id);
    }
    std::vector<ClipId> inserted;
    auto rollback = [&]() {
        for (ClipId id : inserted) {
            track.remove(id);
        }
        for (const Clip& c : m_originals) {
            track.insert(c);
        }
        m_originals.clear();
    };
    for (const Clip& c : m_originals) {  // ascending start order: trimmed clip, then downstream
        Clip placed = c.id == m_id ? trimmed : c;
        if (c.id != m_id) {
            placed.timelineStart -= m_delta;
        }
        if (!isPlaceable(project, placed) || !track.insert(placed)) {
            rollback();
            return false;
        }
        inserted.push_back(placed.id);
    }
    return true;
}

void RippleTrimCommand::undo(Project& project)
{
    Track& track = project.sequence().track(m_track);
    for (const Clip& c : m_originals) {
        track.remove(c.id);
    }
    for (const Clip& c : m_originals) {
        track.insert(c);
    }
}

bool RippleShiftCommand::apply(Project& project)
{
    Sequence& sequence = project.sequence();
    if (m_track >= sequence.trackCount() || m_delta <= 0) {
        return false;
    }
    Track& track = sequence.track(m_track);

    m_shifted.clear();
    for (const Clip& c : track.clips()) {
        if (c.timelineStart >= m_from) {
            m_shifted.push_back(c);
        }
    }
    if (m_shifted.empty()) {
        return true;  // nothing at/after the ripple point on this track
    }

    for (const Clip& c : m_shifted) {
        track.remove(c.id);
    }
    std::vector<ClipId> inserted;
    auto rollback = [&]() {
        for (ClipId id : inserted) {
            track.remove(id);
        }
        for (const Clip& c : m_shifted) {
            track.insert(c);
        }
        m_shifted.clear();
    };
    for (const Clip& c : m_shifted) {  // ascending start order
        Clip moved = c;
        moved.timelineStart -= m_delta;
        if (!isPlaceable(project, moved) || !track.insert(moved)) {  // e.g. a straddling clip in the way
            rollback();
            return false;
        }
        inserted.push_back(moved.id);
    }
    return true;
}

void RippleShiftCommand::undo(Project& project)
{
    Track& track = project.sequence().track(m_track);
    for (const Clip& c : m_shifted) {
        track.remove(c.id);
    }
    for (const Clip& c : m_shifted) {
        track.insert(c);
    }
}

bool UnlinkGroupCommand::apply(Project& project)
{
    if (m_members.empty()) {
        m_members = project.sequence().clipsInGroup(m_group);
    }
    if (m_members.empty()) {
        return false;
    }
    for (const auto& [track, id] : m_members) {
        project.sequence().track(track).setLinkGroup(id, kNoLink);
    }
    return true;
}

void UnlinkGroupCommand::undo(Project& project)
{
    for (const auto& [track, id] : m_members) {
        project.sequence().track(track).setLinkGroup(id, m_group);
    }
}

bool SetClipLabelCommand::apply(Project& project)
{
    if (m_track >= project.sequence().trackCount()) {
        return false;
    }
    const Clip* clip = project.sequence().track(m_track).find(m_id);
    if (!clip) {
        return false;
    }
    m_oldLabel = clip->label;
    return project.sequence().track(m_track).setLabel(m_id, m_label);
}

void SetClipLabelCommand::undo(Project& project)
{
    project.sequence().track(m_track).setLabel(m_id, m_oldLabel);
}

bool SetClipTransformCommand::apply(Project& project)
{
    if (m_track >= project.sequence().trackCount()) {
        return false;
    }
    const Clip* clip = project.sequence().track(m_track).find(m_id);
    if (!clip) {
        return false;
    }
    m_old = clip->transform;
    return project.sequence().track(m_track).setTransform(m_id, m_transform);
}

void SetClipTransformCommand::undo(Project& project)
{
    project.sequence().track(m_track).setTransform(m_id, m_old);
}

bool SetClipAudioCommand::apply(Project& project)
{
    if (m_track >= project.sequence().trackCount()) {
        return false;
    }
    const Clip* clip = project.sequence().track(m_track).find(m_id);
    if (!clip) {
        return false;
    }
    m_old = clip->audio;
    return project.sequence().track(m_track).setAudioLevels(m_id, m_audio);
}

void SetClipAudioCommand::undo(Project& project)
{
    project.sequence().track(m_track).setAudioLevels(m_id, m_old);
}

bool SplitClipCommand::apply(Project& project)
{
    Sequence& sequence = project.sequence();
    if (m_track >= sequence.trackCount()) {
        return false;
    }

    const Clip* existing = sequence.track(m_track).find(m_id);
    // Strictly inside: contains() includes the start, and splitting there would
    // leave a zero-length piece.
    if (!existing || m_at <= existing->timelineStart || m_at >= existing->range().end()) {
        return false;
    }
    m_original = *existing;

    Clip left = m_original;
    left.duration = m_at - m_original.timelineStart;

    Clip right = m_original;
    right.id = m_rightId != kInvalidClip ? m_rightId : project.nextClipId();
    right.timelineStart = m_at;
    right.sourceIn = m_original.sourceTimeAt(m_at);
    right.duration = m_original.range().end() - m_at;
    if (m_rightLink != kNoLink) {
        right.linkGroup = m_rightLink;  // caller relinks the right pieces into their own group
    }
    m_rightId = right.id;
    project.reserveClipId(m_rightId);

    sequence.track(m_track).remove(m_id);
    sequence.track(m_track).insert(left);
    sequence.track(m_track).insert(right);
    return true;
}

void SplitClipCommand::undo(Project& project)
{
    project.sequence().track(m_track).remove(m_id);
    project.sequence().track(m_track).remove(m_rightId);
    project.sequence().track(m_track).insert(m_original);
}

}  // namespace hopline
