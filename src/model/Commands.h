#pragma once

#include <utility>
#include <vector>

#include "model/Clip.h"
#include "model/Command.h"

namespace hopline {

// Each command captures whatever undo needs before mutating, and re-applies with
// the same ids so redo is identical to the original edit.

class AddClipCommand : public Command {
public:
    AddClipCommand(size_t trackIndex, Clip clip)
        : m_track(trackIndex)
        , m_clip(clip)
    {
    }

    bool apply(Project& project) override;
    void undo(Project& project) override;
    std::string name() const override { return "Add Clip"; }

    ClipId clipId() const { return m_clip.id; }

private:
    size_t m_track;
    Clip m_clip;
};

// Overwrite support: clears a time region so a new/moved clip can occupy it — trims clips that
// partly overlap, removes those fully covered, and splits one that spans the region (leaving the
// parts outside). A clip that overlaps on `trackIndex` is cleared as a **whole linked clip**: its
// partners on other tracks are cleared over the same region too. The excluded clip (e.g. the one
// being moved) and its link group are left alone. Undo restores the originals. Always succeeds;
// compose it before an Add/Move.
class ClearRegionCommand : public Command {
public:
    ClearRegionCommand(size_t trackIndex, TimeRange region, std::vector<ClipId> exclude = {})
        : m_track(trackIndex)
        , m_region(region)
        , m_exclude(std::move(exclude))
    {
    }

    bool apply(Project& project) override;
    void undo(Project& project) override;
    std::string name() const override { return "Clear Region"; }

private:
    size_t m_track;
    TimeRange m_region;
    std::vector<ClipId> m_exclude;  // these clips and their link groups are left alone
    bool m_captured = false;
    std::vector<std::pair<size_t, Clip>> m_removed;  // (track, original), restored on undo
    std::vector<std::pair<size_t, Clip>> m_pieces;   // (track, inserted remainder, new id)
};

// Moves a set of clips by the same time delta, atomically: removes them all, then re-inserts
// them shifted (so they never transiently overlap each other during the move). Rejects if any
// destination is unplaceable or hits a non-member clip. Compose ClearRegionCommands before it
// to overwrite non-members. Undo restores the originals.
class MoveClipsCommand : public Command {
public:
    MoveClipsCommand(std::vector<std::pair<size_t, ClipId>> members, Tick delta)
        : m_members(std::move(members))
        , m_delta(delta)
    {
    }

    bool apply(Project& project) override;
    void undo(Project& project) override;
    std::string name() const override { return "Move Clips"; }

private:
    std::vector<std::pair<size_t, ClipId>> m_members;
    Tick m_delta;
    std::vector<std::pair<size_t, Clip>> m_originals;  // captured on apply
};

// Links a set of clips into one new link group. Undo restores each clip's previous group.
class LinkClipsCommand : public Command {
public:
    explicit LinkClipsCommand(std::vector<std::pair<size_t, ClipId>> members)
        : m_members(std::move(members))
    {
    }

    bool apply(Project& project) override;
    void undo(Project& project) override;
    std::string name() const override { return "Link Clips"; }

private:
    std::vector<std::pair<size_t, ClipId>> m_members;
    std::vector<LinkGroup> m_old;  // previous group of each member, captured on apply
    LinkGroup m_group = kNoLink;
};

class RemoveClipCommand : public Command {
public:
    RemoveClipCommand(size_t trackIndex, ClipId id)
        : m_track(trackIndex)
        , m_id(id)
    {
    }

    bool apply(Project& project) override;
    void undo(Project& project) override;
    std::string name() const override { return "Remove Clip"; }

private:
    size_t m_track;
    ClipId m_id;
    Clip m_removed;  // captured by apply() so undo can put it back verbatim
};

// Moves within a track, or across tracks when toTrack differs.
class MoveClipCommand : public Command {
public:
    MoveClipCommand(size_t fromTrack, ClipId id, size_t toTrack, Tick newStart)
        : m_from(fromTrack)
        , m_to(toTrack)
        , m_id(id)
        , m_newStart(newStart)
    {
    }

    bool apply(Project& project) override;
    void undo(Project& project) override;
    std::string name() const override { return "Move Clip"; }

private:
    size_t m_from;
    size_t m_to;
    ClipId m_id;
    Tick m_newStart;
    Clip m_original;
};

// Roll edit: moves the shared boundary between two butt-joined clips on one track — the left
// clip's tail and the right clip's head shift together by delta (left grows / right shrinks for
// delta > 0, and vice versa). The caller clamps delta to what both clips allow. Undo restores both.
class RollEditCommand : public Command {
public:
    RollEditCommand(size_t track, ClipId left, ClipId right, Tick delta)
        : m_track(track)
        , m_left(left)
        , m_right(right)
        , m_delta(delta)
    {
    }

    bool apply(Project& project) override;
    void undo(Project& project) override;
    std::string name() const override { return "Roll Edit"; }

private:
    size_t m_track;
    ClipId m_left;
    ClipId m_right;
    Tick m_delta;
    Clip m_origLeft;
    Clip m_origRight;
};

// Trimming the head moves sourceIn and timelineStart together, so the picture
// under the cursor doesn't shift.
class TrimClipCommand : public Command {
public:
    enum class Edge { Head, Tail };

    TrimClipCommand(size_t trackIndex, ClipId id, Edge edge, Tick delta)
        : m_track(trackIndex)
        , m_id(id)
        , m_edge(edge)
        , m_delta(delta)
    {
    }

    bool apply(Project& project) override;
    void undo(Project& project) override;
    std::string name() const override { return "Trim Clip"; }

private:
    size_t m_track;
    ClipId m_id;
    Edge m_edge;
    Tick m_delta;
    Clip m_original;
};

// Ripple trim: trims a clip by `delta` (> 0) to the playhead — Head drops the clip's head content
// (keeping its timeline start), Tail drops its tail — then shifts every later clip on the same
// track left by delta to close the gap. Compose one per link-group member for a linked V+A edit.
// Undo restores the clip and every shifted clip.
class RippleTrimCommand : public Command {
public:
    enum class Edge { Head, Tail };

    RippleTrimCommand(size_t trackIndex, ClipId id, Edge edge, Tick delta)
        : m_track(trackIndex)
        , m_id(id)
        , m_edge(edge)
        , m_delta(delta)
    {
    }

    bool apply(Project& project) override;
    void undo(Project& project) override;
    std::string name() const override { return "Ripple Trim"; }

private:
    size_t m_track;
    ClipId m_id;
    Edge m_edge;
    Tick m_delta;
    std::vector<Clip> m_originals;  // trimmed clip + shifted downstream, captured on apply
};

// Ripple shift: slides every clip on one track whose start is at/after `from` left by `delta`
// (> 0). Paired with a RippleTrimCommand on the edited track, this keeps the *other* tracks in
// sync during a ripple so they stay aligned with whatever they sat next to. Undo restores them.
class RippleShiftCommand : public Command {
public:
    RippleShiftCommand(size_t track, Tick from, Tick delta)
        : m_track(track)
        , m_from(from)
        , m_delta(delta)
    {
    }

    bool apply(Project& project) override;
    void undo(Project& project) override;
    std::string name() const override { return "Ripple Shift"; }

private:
    size_t m_track;
    Tick m_from;
    Tick m_delta;
    std::vector<Clip> m_shifted;  // originals of the moved clips, captured on apply
};

// Clears the link group on every clip that shares it, so they can be moved
// independently. Undo restores the group on exactly those clips.
class UnlinkGroupCommand : public Command {
public:
    explicit UnlinkGroupCommand(LinkGroup group)
        : m_group(group)
    {
    }

    bool apply(Project& project) override;
    void undo(Project& project) override;
    std::string name() const override { return "Unlink Clips"; }

private:
    LinkGroup m_group;
    std::vector<std::pair<size_t, ClipId>> m_members;  // captured on first apply
};

// Sets a clip's cosmetic label color. Undo restores the previous label.
class SetClipLabelCommand : public Command {
public:
    SetClipLabelCommand(size_t trackIndex, ClipId id, int label)
        : m_track(trackIndex)
        , m_id(id)
        , m_label(label)
    {
    }

    bool apply(Project& project) override;
    void undo(Project& project) override;
    std::string name() const override { return "Label Clip"; }

private:
    size_t m_track;
    ClipId m_id;
    int m_label;
    int m_oldLabel = 0;
};

// Sets a video clip's Transform effect. Undo restores the previous transform.
class SetClipTransformCommand : public Command {
public:
    SetClipTransformCommand(size_t trackIndex, ClipId id, const Transform& transform)
        : m_track(trackIndex)
        , m_id(id)
        , m_transform(transform)
    {
    }

    bool apply(Project& project) override;
    void undo(Project& project) override;
    std::string name() const override { return "Transform"; }

private:
    size_t m_track;
    ClipId m_id;
    Transform m_transform;
    Transform m_old;
};

// Sets an audio clip's Volume Controls (gain + pan). Undo restores the previous.
class SetClipAudioCommand : public Command {
public:
    SetClipAudioCommand(size_t trackIndex, ClipId id, const AudioLevels& audio)
        : m_track(trackIndex)
        , m_id(id)
        , m_audio(audio)
    {
    }

    bool apply(Project& project) override;
    void undo(Project& project) override;
    std::string name() const override { return "Volume"; }

private:
    size_t m_track;
    ClipId m_id;
    AudioLevels m_audio;
    AudioLevels m_old;
};

// Splits at a timeline instant; the right-hand piece gets a fresh id. By default it
// keeps the original's link group; pass rightLink to put the right piece in a new group
// (so splitting a linked V+A pair yields a left pair and a separate right pair).
class SplitClipCommand : public Command {
public:
    SplitClipCommand(size_t trackIndex, ClipId id, Tick at, LinkGroup rightLink = kNoLink)
        : m_track(trackIndex)
        , m_id(id)
        , m_at(at)
        , m_rightLink(rightLink)
    {
    }

    bool apply(Project& project) override;
    void undo(Project& project) override;
    std::string name() const override { return "Split Clip"; }

    ClipId rightId() const { return m_rightId; }

private:
    size_t m_track;
    ClipId m_id;
    Tick m_at;
    LinkGroup m_rightLink;
    Clip m_original;
    ClipId m_rightId = kInvalidClip;
};

}  // namespace hopline
