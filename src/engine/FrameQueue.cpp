#include "engine/FrameQueue.h"

namespace hopline {

bool FrameQueue::push(VideoFrame&& frame)
{
    std::unique_lock lock(m_mutex);
    m_notFull.wait(lock, [this] { return m_closed || m_frames.size() < m_capacity; });
    if (m_closed) {
        return false;
    }
    m_frames.push_back(std::move(frame));
    return true;
}

bool FrameQueue::tryPop(VideoFrame& out)
{
    std::lock_guard lock(m_mutex);
    if (m_frames.empty()) {
        return false;
    }
    out = std::move(m_frames.front());
    m_frames.pop_front();
    m_notFull.notify_one();
    return true;
}

bool FrameQueue::peekPts(double& pts) const
{
    std::lock_guard lock(m_mutex);
    if (m_frames.empty()) {
        return false;
    }
    pts = m_frames.front().pts;
    return true;
}

void FrameQueue::close()
{
    {
        std::lock_guard lock(m_mutex);
        m_closed = true;
    }
    m_notFull.notify_all();
}

void FrameQueue::reopen()
{
    std::lock_guard lock(m_mutex);
    m_closed = false;
}

void FrameQueue::clear()
{
    std::lock_guard lock(m_mutex);
    m_frames.clear();
    m_notFull.notify_all();
}

size_t FrameQueue::size() const
{
    std::lock_guard lock(m_mutex);
    return m_frames.size();
}

}  // namespace hopline
