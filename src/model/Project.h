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

    Sequence& sequence() { return m_sequence; }
    const Sequence& sequence() const { return m_sequence; }

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
    Sequence m_sequence;
    MediaId m_lastMediaId = kInvalidMedia;
    ClipId m_lastClipId = kInvalidClip;
    LinkGroup m_lastLinkGroup = kNoLink;
    FolderId m_lastFolderId = 0;
};

}  // namespace hopline
