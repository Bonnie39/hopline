#pragma once

#include <chrono>

namespace hopline {

// Playback time source. Wall-clock driven for now; once audio output exists the
// audio thread becomes the master and this gets slaved to the samples actually
// played. Video syncs to this, never the other way around.
class Clock {
public:
    void start()
    {
        if (!m_running) {
            m_origin = std::chrono::steady_clock::now();
            m_running = true;
        }
    }

    void pause()
    {
        if (m_running) {
            m_base = seconds();
            m_running = false;
        }
    }

    void reset(double seconds = 0.0)
    {
        m_base = seconds;
        m_origin = std::chrono::steady_clock::now();
    }

    double seconds() const
    {
        if (!m_running) {
            return m_base;
        }
        const std::chrono::duration<double> elapsed = std::chrono::steady_clock::now() - m_origin;
        return m_base + elapsed.count();
    }

    bool running() const { return m_running; }

private:
    std::chrono::steady_clock::time_point m_origin;
    double m_base = 0.0;
    bool m_running = false;
};

}  // namespace hopline
