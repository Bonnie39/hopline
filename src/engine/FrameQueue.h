#pragma once

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>

#include "media/VideoFrame.h"

namespace hopline {

// Bounded producer/consumer queue between the decode thread and the UI.
// The bound is the backpressure: a full queue blocks the decoder instead of
// letting it run ahead and eat memory.
class FrameQueue {
public:
    explicit FrameQueue(size_t capacity)
        : m_capacity(capacity)
    {
    }

    // Blocks while full. Returns false if the queue was closed.
    bool push(VideoFrame&& frame);

    bool tryPop(VideoFrame& out);
    bool peekPts(double& pts) const;

    void close();
    void reopen();
    void clear();
    size_t size() const;

private:
    mutable std::mutex m_mutex;
    std::condition_variable m_notFull;
    std::deque<VideoFrame> m_frames;
    size_t m_capacity;
    bool m_closed = false;
};

}  // namespace hopline
