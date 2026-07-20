#pragma once

#include <atomic>
#include <string>
#include <thread>

#include "engine/AudioOutput.h"
#include "engine/Clock.h"
#include "engine/FrameQueue.h"
#include "media/AudioDecoder.h"
#include "media/VideoDecoder.h"

namespace hopline {

// Owns the decode threads and decides which frame is due. Headless: no Qt here.
// When the file has audio, the audio device is the master clock and video is
// slaved to it. Files without audio fall back to the wall clock.
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

    // Repositions both streams and rebases the clock. Preserves play state.
    void seek(double seconds);

    // Pulls every frame now due, returning the newest in `out`. Late frames are
    // dropped rather than shown behind the clock. False if nothing is due yet.
    bool update(VideoFrame& out);

    bool isOpen() const { return m_video.isOpen(); }
    bool isPlaying() const;
    bool hasAudio() const { return m_audioOut.isOpen(); }
    bool atEnd() const;
    double position() const;
    double duration() const { return m_video.duration(); }
    int droppedFrames() const { return m_dropped; }
    int underruns() const { return m_audioOut.underruns(); }

private:
    void videoLoop();
    void audioLoop();
    void startThreads();
    void stopThreads();

    VideoDecoder m_video;
    AudioDecoder m_audio;
    AudioOutput m_audioOut;
    FrameQueue m_queue;
    Clock m_clock;

    std::thread m_videoThread;
    std::thread m_audioThread;
    std::atomic<bool> m_stop{ false };
    std::atomic<bool> m_eof{ false };
    int m_dropped = 0;
    bool m_presentNext = false;  // show the first frame after a seek without waiting for the clock
};

}  // namespace hopline
