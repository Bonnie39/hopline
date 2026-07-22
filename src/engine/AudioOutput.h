#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

#include "engine/RingBuffer.h"

namespace hopline {

// miniaudio playback device. Owns the master clock: position() reports how much
// audio has actually reached the device, which is what video syncs against.
class AudioOutput {
public:
    AudioOutput();
    ~AudioOutput();

    AudioOutput(const AudioOutput&) = delete;
    AudioOutput& operator=(const AudioOutput&) = delete;

    // The device runs from open() to close(). Pausing gates the callback rather
    // than stopping the device: ma_device_stop discards frames already handed
    // over, which would jump the clock forward on every resume.
    bool open(int sampleRate, int channels, std::string& error);
    void close();

    void setPaused(bool paused);

    // Silence past end-of-stream is expected, not starvation.
    void setEndOfStream(bool eos) { m_endOfStream.store(eos, std::memory_order_release); }

    RingBuffer& buffer() { return m_buffer; }
    bool isOpen() const { return m_device != nullptr; }
    bool isRunning() const { return m_device != nullptr && !m_paused; }

    // Seconds of audio actually audible, i.e. frames handed to the device less
    // the device's own buffering. This is the clock everything else follows.
    double position() const;

    // Rebases the clock after a seek: frames played are counted from here.
    void resetPosition(double baseSeconds = 0.0);

    int sampleRate() const { return m_sampleRate; }
    int channels() const { return m_channels; }
    int underruns() const { return m_underruns; }

    // Most recent per-channel peak (0..1 linear), for the UI level meter.
    float peak(int channel) const
    {
        return m_peak[channel <= 0 ? 0 : 1].load(std::memory_order_relaxed);
    }

private:
    struct Device;
    friend struct AudioCallback;

    void fill(float* output, uint32_t frameCount);

    std::unique_ptr<Device> m_device;
    RingBuffer m_buffer;

    std::atomic<uint64_t> m_framesOut{ 0 };
    std::atomic<double> m_base{ 0.0 };
    std::atomic<int> m_underruns{ 0 };
    std::atomic<bool> m_paused{ true };
    std::atomic<bool> m_endOfStream{ false };
    std::atomic<float> m_peak[2]{};

    int m_sampleRate = 0;
    int m_channels = 0;
    uint32_t m_latencyFrames = 0;
};

}  // namespace hopline
