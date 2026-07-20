#include "model/Commands.h"

#include "model/Project.h"

namespace hopline {

bool AddClipCommand::apply(Project& project)
{
    if (m_track >= project.sequence().trackCount()) {
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

    if (trimmed.duration <= 0 || trimmed.sourceIn < 0) {
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
