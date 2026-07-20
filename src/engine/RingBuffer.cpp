#include "engine/RingBuffer.h"

#include <algorithm>

namespace hopline {

void RingBuffer::resize(size_t samples)
{
    size_t capacity = 1;
    while (capacity < samples) {
        capacity <<= 1;
    }
    m_data.assign(capacity, 0.0f);
    m_mask = capacity - 1;
    m_read.store(0, std::memory_order_relaxed);
    m_write.store(0, std::memory_order_relaxed);
}

size_t RingBuffer::write(const float* src, size_t samples)
{
    const size_t w = m_write.load(std::memory_order_relaxed);
    const size_t r = m_read.load(std::memory_order_acquire);
    const size_t free = m_data.size() - (w - r);
    const size_t n = std::min(samples, free);

    for (size_t i = 0; i < n; ++i) {
        m_data[(w + i) & m_mask] = src[i];
    }
    m_write.store(w + n, std::memory_order_release);
    return n;
}

size_t RingBuffer::read(float* dst, size_t samples)
{
    const size_t r = m_read.load(std::memory_order_relaxed);
    const size_t w = m_write.load(std::memory_order_acquire);
    const size_t n = std::min(samples, w - r);

    for (size_t i = 0; i < n; ++i) {
        dst[i] = m_data[(r + i) & m_mask];
    }
    m_read.store(r + n, std::memory_order_release);
    return n;
}

size_t RingBuffer::available() const
{
    return m_write.load(std::memory_order_acquire) - m_read.load(std::memory_order_acquire);
}

size_t RingBuffer::space() const { return m_data.size() - available(); }

void RingBuffer::clear()
{
    m_read.store(0, std::memory_order_relaxed);
    m_write.store(0, std::memory_order_relaxed);
}

}  // namespace hopline
