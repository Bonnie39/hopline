#pragma once

#include <atomic>
#include <string>
#include <thread>

#include "engine/Clock.h"
#include "engine/FrameQueue.h"
#include "media/VideoDecoder.h"

namespace hopline {

// Owns the decode thread and decides which frame is due. Headless: no Qt here.
// The caller polls update() and presents whatever it hands back.
class Player {
public:
    Player();
    ~Player();

    Player(const Player&) = delete;
    Player& operator=(const Player&) = delete;

    bool open(const std::string& path, std::string& error);
    void close();

    void play();
    void pause();
    void togglePlay();

    // Pulls every frame now due, returning the newest in `out`. Late frames are
    // dropped rather than shown behind the clock. False if nothing is due yet.
    bool update(VideoFrame& out);

    bool isOpen() const { return m_decoder.isOpen(); }
    bool isPlaying() const { return m_clock.running(); }
    bool atEnd() const;
    double position() const;
    double duration() const { return m_decoder.duration(); }
    int droppedFrames() const { return m_dropped; }

private:
    void decodeLoop();
    void stopThread();

    VideoDecoder m_decoder;
    FrameQueue m_queue;
    Clock m_clock;
    std::thread m_thread;
    std::atomic<bool> m_stop{ false };
    std::atomic<bool> m_eof{ false };
    int m_dropped = 0;
};

}  // namespace hopline
