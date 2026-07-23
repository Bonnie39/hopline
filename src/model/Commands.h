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

// Splits at a timeline instant; the right-hand piece gets a fresh id.
class SplitClipCommand : public Command {
public:
    SplitClipCommand(size_t trackIndex, ClipId id, Tick at)
        : m_track(trackIndex)
        , m_id(id)
        , m_at(at)
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
    Clip m_original;
    ClipId m_rightId = kInvalidClip;
};

}  // namespace hopline
