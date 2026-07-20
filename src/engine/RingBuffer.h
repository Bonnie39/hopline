#pragma once

#include <atomic>
#include <cstddef>
#include <vector>

namespace hopline {

// Single-producer / single-consumer float ring. The consumer is the audio device
// callback, so read() must never allocate, lock, or block.
class RingBuffer {
public:
    // Rounded up to a power of two so indices can be masked rather than divided.
    void resize(size_t samples);

    size_t write(const float* src, size_t samples);
    size_t read(float* dst, size_t samples);

    size_t available() const;
    size_t space() const;
    void clear();

private:
    std::vector<float> m_data;
    size_t m_mask = 0;
    std::atomic<size_t> m_read{ 0 };
    std::atomic<size_t> m_write{ 0 };
};

}  // namespace hopline
