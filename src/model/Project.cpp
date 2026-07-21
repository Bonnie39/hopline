#include "model/Project.h"

#include <algorithm>

namespace hopline {

Project::Project() { reset(); }

void Project::reset()
{
    m_media.clear();
    m_folders.clear();
    m_sequence = Sequence{};
    m_lastMediaId = kInvalidMedia;
    m_lastClipId = kInvalidClip;
    m_lastLinkGroup = kNoLink;
    m_lastFolderId = 0;

    m_folders.push_back({ kRootFolder, 0, "Media" });
    m_lastFolderId = kRootFolder;
}

MediaId Project::addMedia(MediaSource source, FolderId folder)
{
    source.id = ++m_lastMediaId;
    source.folder = folder;
    m_media.push_back(std::move(source));
    return m_lastMediaId;
}

const MediaSource* Project::media(MediaId id) const
{
    const auto it = std::find_if(m_media.begin(), m_media.end(),
                                 [id](const MediaSource& m) { return m.id == id; });
    return it != m_media.end() ? &*it : nullptr;
}

void Project::setMediaFolder(MediaId id, FolderId folder)
{
    const auto it = std::find_if(m_media.begin(), m_media.end(),
                                 [id](const MediaSource& m) { return m.id == id; });
    if (it != m_media.end()) {
        it->folder = folder;
    }
}

FolderId Project::addFolder(FolderId parent, std::string name)
{
    const FolderId id = ++m_lastFolderId;
    m_folders.push_back({ id, parent, std::move(name) });
    return id;
}

bool Project::renameFolder(FolderId id, std::string name)
{
    const auto it = std::find_if(m_folders.begin(), m_folders.end(),
                                 [id](const BinFolder& f) { return f.id == id; });
    if (it == m_folders.end() || id == kRootFolder) {
        return false;
    }
    it->name = std::move(name);
    return true;
}

void Project::removeFolder(FolderId id)
{
    if (id == kRootFolder) {
        return;
    }
    const auto it = std::find_if(m_folders.begin(), m_folders.end(),
                                 [id](const BinFolder& f) { return f.id == id; });
    if (it == m_folders.end()) {
        return;
    }
    const FolderId parent = it->parent;

    for (MediaSource& source : m_media) {
        if (source.folder == id) {
            source.folder = parent;
        }
    }
    for (BinFolder& folder : m_folders) {
        if (folder.parent == id) {
            folder.parent = parent;
        }
    }
    m_folders.erase(it);
}

void Project::reserveClipId(ClipId id) { m_lastClipId = std::max(m_lastClipId, id); }
void Project::reserveLinkGroup(LinkGroup group) { m_lastLinkGroup = std::max(m_lastLinkGroup, group); }
void Project::reserveMediaId(MediaId id) { m_lastMediaId = std::max(m_lastMediaId, id); }
void Project::reserveFolderId(FolderId id) { m_lastFolderId = std::max(m_lastFolderId, id); }

void Project::clearForLoad()
{
    m_media.clear();
    m_folders.clear();
    m_sequence.clear();
    m_lastMediaId = kInvalidMedia;
    m_lastClipId = kInvalidClip;
    m_lastLinkGroup = kNoLink;
    m_lastFolderId = 0;
}

void Project::restoreMedia(const MediaSource& source)
{
    m_media.push_back(source);
    reserveMediaId(source.id);
}

void Project::restoreFolder(const BinFolder& folder)
{
    m_folders.push_back(folder);
    reserveFolderId(folder.id);
}

}  // namespace hopline
