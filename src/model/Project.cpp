#include "model/Project.h"

#include <algorithm>

namespace hopline {

MediaId Project::addMedia(MediaSource source)
{
    source.id = ++m_lastMediaId;
    m_media.push_back(std::move(source));
    return m_lastMediaId;
}

const MediaSource* Project::media(MediaId id) const
{
    const auto it = std::find_if(m_media.begin(), m_media.end(),
                                 [id](const MediaSource& m) { return m.id == id; });
    return it != m_media.end() ? &*it : nullptr;
}

void Project::reserveClipId(ClipId id)
{
    m_lastClipId = std::max(m_lastClipId, id);
}

void Project::reserveLinkGroup(LinkGroup group)
{
    m_lastLinkGroup = std::max(m_lastLinkGroup, group);
}

}  // namespace hopline
