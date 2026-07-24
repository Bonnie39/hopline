#include "engine/AudioOutput.h"

#define MINIAUDIO_IMPLEMENTATION
#define MA_NO_DECODING
#define MA_NO_ENCODING
#define MA_NO_GENERATION
// QApplication puts the main thread in an STA; miniaudio defaults to
// COINIT_MULTITHREADED, and the resulting apartment conflict faults inside
// ma_device_init.
#define MA_COINIT_VALUE COINIT_APARTMENTTHREADED
#include <miniaudio.h>

#include <algorithm>
#include <cmath>

namespace hopline {

struct AudioOutput::Device {
    ma_device handle{};
};

AudioOutput::AudioOutput() = default;

AudioOutput::~AudioOutput() { close(); }

void AudioOutput::fill(float* dst, uint32_t frameCount)
{
    const size_t wanted = static_cast<size_t>(frameCount) * m_channels;

    // Paused: emit silence and leave the clock alone. Nothing is consumed, so
    // resuming picks up exactly where it left off.
    if (m_paused.load(std::memory_order_acquire)) {
        std::fill(dst, dst + wanted, 0.0f);
        m_peak[0].store(0.0f, std::memory_order_relaxed);
        m_peak[1].store(0.0f, std::memory_order_relaxed);
        return;
    }

    const size_t got = m_buffer.read(dst, wanted);
    if (got < wanted) {
        std::fill(dst + got, dst + wanted, 0.0f);
        if (!m_endOfStream.load(std::memory_order_acquire)) {
            m_underruns.fetch_add(1, std::memory_order_relaxed);
        }
    }

    // Peak level per channel over this buffer, for the UI meter.
    float p0 = 0.0f, p1 = 0.0f;
    for (uint32_t f = 0; f < frameCount; ++f) {
        const float a = std::fabs(dst[static_cast<size_t>(f) * m_channels]);
        if (a > p0) p0 = a;
        if (m_channels > 1) {
            const float b = std::fabs(dst[static_cast<size_t>(f) * m_channels + 1]);
            if (b > p1) p1 = b;
        }
    }
    m_peak[0].store(p0, std::memory_order_relaxed);
    m_peak[1].store(m_channels > 1 ? p1 : p0, std::memory_order_relaxed);

    // Silence from a genuine underrun still counts: the clock must track real
    // time, or a glitch would stall it and let video run away.
    m_framesOut.fetch_add(frameCount, std::memory_order_release);
}

struct AudioCallback {
    static void run(ma_device* device, void* output, const void*, ma_uint32 frameCount)
    {
        static_cast<AudioOutput*>(device->pUserData)->fill(static_cast<float*>(output), frameCount);
    }
};

bool AudioOutput::open(int sampleRate, int channels, std::string& error)
{
    close();

    auto device = std::make_unique<Device>();

    ma_device_config config = ma_device_config_init(ma_device_type_playback);
    config.playback.format = ma_format_f32;
    config.playback.channels = static_cast<ma_uint32>(channels);
    config.sampleRate = static_cast<ma_uint32>(sampleRate);
    config.dataCallback = &AudioCallback::run;
    config.pUserData = this;

    if (ma_device_init(nullptr, &config, &device->handle) != MA_SUCCESS) {
        error = "failed to open audio device";
        return false;
    }

    m_device = std::move(device);
    m_sampleRate = static_cast<int>(m_device->handle.sampleRate);
    m_channels = channels;
    m_latencyFrames = m_device->handle.playback.internalPeriodSizeInFrames
        * m_device->handle.playback.internalPeriods;

    // Ring capacity. The audio thread caps how full it actually keeps this (see
    // kAudioLookaheadMs in Player.cpp) — that cap, not this capacity, sets the
    // look-ahead latency; the capacity just needs comfortable headroom above it.
    m_buffer.resize(static_cast<size_t>(m_sampleRate) * m_channels / 2);
    resetPosition();

    m_paused.store(true, std::memory_order_release);
    m_endOfStream.store(false, std::memory_order_release);
    if (ma_device_start(&m_device->handle) != MA_SUCCESS) {
        error = "failed to start audio device";
        close();
        return false;
    }
    return true;
}

void AudioOutput::close()
{
    if (!m_device) {
        return;
    }
    m_paused.store(true, std::memory_order_release);
    ma_device_uninit(&m_device->handle);  // stops the device and joins its thread
    m_device.reset();
    m_sampleRate = m_channels = 0;
    m_latencyFrames = 0;
}

void AudioOutput::setPaused(bool paused)
{
    m_paused.store(paused, std::memory_order_release);
}

double AudioOutput::position() const
{
    if (m_sampleRate <= 0) {
        return 0.0;
    }
    const uint64_t out = m_framesOut.load(std::memory_order_acquire);
    const uint64_t audible = out > m_latencyFrames ? out - m_latencyFrames : 0;
    return m_base.load(std::memory_order_acquire) + static_cast<double>(audible) / m_sampleRate;
}

void AudioOutput::resetPosition(double baseSeconds)
{
    m_framesOut.store(0, std::memory_order_release);
    m_base.store(baseSeconds, std::memory_order_release);
    m_underruns.store(0, std::memory_order_relaxed);
}

}  // namespace hopline
