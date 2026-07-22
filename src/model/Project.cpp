#include "model/Project.h"

#include <algorithm>

namespace hopline {

Project::Project() { reset(); }

void Project::reset()
{
    m_media.clear();
    m_folders.clear();
    m_sequences.clear();
    m_placeholder.clear();  // empty timeline until a sequence exists
    m_activeSequence = kInvalidSequence;
    m_lastMediaId = kInvalidMedia;
    m_lastClipId = kInvalidClip;
    m_lastLinkGroup = kNoLink;
    m_lastFolderId = 0;
    m_lastSequenceId = kInvalidSequence;

    m_folders.push_back({ kRootFolder, 0, "Media" });
    m_lastFolderId = kRootFolder;
}

Sequence& Project::sequence()
{
    Sequence* active = activeSequence();
    return active ? *active : m_placeholder;
}

const Sequence& Project::sequence() const
{
    const Sequence* active = activeSequence();
    return active ? *active : m_placeholder;
}

Sequence* Project::activeSequence() { return sequenceById(m_activeSequence); }
const Sequence* Project::activeSequence() const { return sequenceById(m_activeSequence); }

Sequence* Project::sequenceById(SequenceId id)
{
    const auto it = std::find_if(m_sequences.begin(), m_sequences.end(),
                                 [id](const Sequence& s) { return s.id() == id; });
    return it != m_sequences.end() ? &*it : nullptr;
}

const Sequence* Project::sequenceById(SequenceId id) const
{
    const auto it = std::find_if(m_sequences.begin(), m_sequences.end(),
                                 [id](const Sequence& s) { return s.id() == id; });
    return it != m_sequences.end() ? &*it : nullptr;
}

void Project::setActiveSequence(SequenceId id)
{
    if (id == kInvalidSequence || sequenceById(id)) {
        m_activeSequence = id;
    }
}

SequenceId Project::addSequence(std::string name, int rateNum, int rateDen, int width, int height,
                                FolderId folder)
{
    Sequence seq;  // default V1/A1 tracks
    seq.setId(++m_lastSequenceId);
    seq.setName(std::move(name));
    seq.setFolder(folder);
    seq.setFrameRate(rateNum, rateDen);
    seq.setResolution(width, height);
    m_sequences.push_back(std::move(seq));
    return m_lastSequenceId;
}

bool Project::renameSequence(SequenceId id, std::string name)
{
    if (Sequence* seq = sequenceById(id)) {
        seq->setName(std::move(name));
        return true;
    }
    return false;
}

void Project::removeSequence(SequenceId id)
{
    const auto it = std::find_if(m_sequences.begin(), m_sequences.end(),
                                 [id](const Sequence& s) { return s.id() == id; });
    if (it == m_sequences.end()) {
        return;
    }
    m_sequences.erase(it);
    if (m_activeSequence == id) {
        m_activeSequence = m_sequences.empty() ? kInvalidSequence : m_sequences.front().id();
    }
}

void Project::setSequenceFolder(SequenceId id, FolderId folder)
{
    if (Sequence* seq = sequenceById(id)) {
        seq->setFolder(folder);
    }
}

void Project::restoreSequence(Sequence seq)
{
    reserveSequenceId(seq.id());
    m_sequences.push_back(std::move(seq));
}

void Project::reserveSequenceId(SequenceId id) { m_lastSequenceId = std::max(m_lastSequenceId, id); }

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

void Project::setMediaLabel(MediaId id, int label)
{
    const auto it = std::find_if(m_media.begin(), m_media.end(),
                                 [id](const MediaSource& m) { return m.id == id; });
    if (it != m_media.end()) {
        it->label = label;
    }
}

void Project::removeMedia(MediaId id)
{
    const auto it = std::find_if(m_media.begin(), m_media.end(),
                                 [id](const MediaSource& m) { return m.id == id; });
    if (it != m_media.end()) {
        m_media.erase(it);
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

void Project::setFolderLabel(FolderId id, int label)
{
    const auto it = std::find_if(m_folders.begin(), m_folders.end(),
                                 [id](const BinFolder& f) { return f.id == id; });
    if (it != m_folders.end()) {
        it->label = label;
    }
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
    for (Sequence& seq : m_sequences) {
        if (seq.folder() == id) {
            seq.setFolder(parent);
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
    m_sequences.clear();
    m_placeholder.clear();
    m_activeSequence = kInvalidSequence;
    m_lastMediaId = kInvalidMedia;
    m_lastClipId = kInvalidClip;
    m_lastLinkGroup = kNoLink;
    m_lastFolderId = 0;
    m_lastSequenceId = kInvalidSequence;
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
