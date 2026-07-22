#pragma once

#include <string>
#include <vector>

#include "model/Media.h"
#include "model/Sequence.h"

namespace hopline {

// Owns the media pool, the bin folder tree, and the sequence, and hands out ids.
// Commands mutate the sequence; media-pool and folder edits go through here
// directly (they aren't undoable, same as addMedia always was).
class Project {
public:
    Project();

    // Empties the project back to a fresh state (root folder, default sequence).
    void reset();

    MediaId addMedia(MediaSource source, FolderId folder = kRootFolder);
    const MediaSource* media(MediaId id) const;
    const std::vector<MediaSource>& mediaPool() const { return m_media; }
    void setMediaFolder(MediaId id, FolderId folder);
    void setMediaLabel(MediaId id, int label);
    // Erases a source from the pool. The caller is responsible for ensuring no
    // clip references it (media edits are non-undoable); ids are never reused.
    void removeMedia(MediaId id);

    const std::vector<BinFolder>& folders() const { return m_folders; }
    FolderId addFolder(FolderId parent, std::string name);
    bool renameFolder(FolderId id, std::string name);
    void setFolderLabel(FolderId id, int label);
    // Removes a folder; its media and subfolders move up to its parent.
    void removeFolder(FolderId id);

    // The active sequence drives the timeline and playback. sequence() returns it,
    // or an empty placeholder when the project has no sequence yet, so command/
    // playback code can stay reference-based. activeSequence() is nullptr when none.
    Sequence& sequence();
    const Sequence& sequence() const;
    Sequence* activeSequence();
    const Sequence* activeSequence() const;
    bool hasActiveSequence() const { return sequenceById(m_activeSequence) != nullptr; }
    SequenceId activeSequenceId() const { return m_activeSequence; }
    void setActiveSequence(SequenceId id);

    const std::vector<Sequence>& sequences() const { return m_sequences; }
    Sequence* sequenceById(SequenceId id);
    const Sequence* sequenceById(SequenceId id) const;
    SequenceId addSequence(std::string name, int rateNum, int rateDen, int width, int height,
                           FolderId folder = kRootFolder);
    bool renameSequence(SequenceId id, std::string name);
    void removeSequence(SequenceId id);
    void setSequenceFolder(SequenceId id, FolderId folder);

    // For deserialization.
    void restoreSequence(Sequence seq);
    void reserveSequenceId(SequenceId id);

    // Ids are never reused, so undo/redo can round-trip a clip unchanged.
    ClipId nextClipId() { return ++m_lastClipId; }
    ClipId peekClipId() const { return m_lastClipId; }
    void reserveClipId(ClipId id);

    LinkGroup nextLinkGroup() { return ++m_lastLinkGroup; }
    void reserveLinkGroup(LinkGroup group);

    // For deserialization: empty everything (no default root or tracks), then
    // push verbatim and bump the id counters.
    void clearForLoad();
    void restoreMedia(const MediaSource& source);
    void restoreFolder(const BinFolder& folder);
    void reserveMediaId(MediaId id);
    void reserveFolderId(FolderId id);

private:
    std::vector<MediaSource> m_media;
    std::vector<BinFolder> m_folders;
    std::vector<Sequence> m_sequences;
    Sequence m_placeholder;  // returned by sequence() when no sequence is active
    SequenceId m_activeSequence = kInvalidSequence;
    MediaId m_lastMediaId = kInvalidMedia;
    ClipId m_lastClipId = kInvalidClip;
    LinkGroup m_lastLinkGroup = kNoLink;
    FolderId m_lastFolderId = 0;
    SequenceId m_lastSequenceId = kInvalidSequence;
};

}  // namespace hopline
