#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>
#include <unordered_map>

#include "engine/AudioOutput.h"
#include "engine/Clock.h"
#include "engine/FrameQueue.h"
#include "media/AudioDecoder.h"
#include "media/VideoDecoder.h"
#include "model/Media.h"
#include "model/Sequence.h"

namespace hopline {

class Project;

// Plays a sequence, not a file. The decode threads walk the timeline in
// segments (between clip cut points) and, per segment, decode the active clip or
// emit black / silence for a gap. Headless: no Qt. Works on a snapshot copy of
// the sequence taken at open()/seek(), so UI edits can't race the decode threads.
class Player {
public:
    Player();
    ~Player();

    Player(const Player&) = delete;
    Player& operator=(const Player&) = delete;

    bool open(const Project& project, std::string& error);
    void close();

    void play();
    void pause();
    void togglePlay();
    void seek(double seconds);

    // Re-snapshots the sequence after an edit, keeping playback position and
    // play state. The decode threads only ever see snapshots, so this is how a
    // model change reaches playback.
    void reload(const Project& project);

    // Pulls every frame now due, returning the newest in `out`. Late frames are
    // dropped rather than shown behind the clock. False if nothing is due yet.
    bool update(VideoFrame& out);

    bool isOpen() const { return m_open; }
    bool isPlaying() const;
    bool hasAudio() const { return m_audioOut.isOpen(); }
    float audioPeak(int channel) const { return m_audioOut.peak(channel); }
    bool atEnd() const;
    double position() const;
    double duration() const { return secondsFromTicks(m_seq.duration()); }
    int droppedFrames() const { return m_dropped; }
    int underruns() const { return m_audioOut.underruns(); }

private:
    void videoLoop();
    void audioLoop();
    void startThreads();
    void stopThreads();

    void restartAt(Tick target, bool resumePlaying);
    bool ensureVideoSource(MediaId id);
    bool decodeVideoSegment(const Clip& clip, Tick segStart, Tick segEnd);
    bool emitBlackRange(Tick from, Tick to);
    Tick nextCut(const std::vector<Tick>& cuts, Tick after, Tick fallback) const;

    Sequence m_seq;
    std::unordered_map<MediaId, std::string> m_paths;

    VideoDecoder m_video;
    AudioOutput m_audioOut;
    FrameQueue m_queue;
    Clock m_clock;

    MediaId m_curVideoSource = kInvalidMedia;
    Tick m_startTick = 0;

    std::thread m_videoThread;
    std::thread m_audioThread;
    std::atomic<bool> m_stop{ false };
    std::atomic<bool> m_eof{ false };
    bool m_open = false;
    int m_dropped = 0;
    bool m_presentNext = false;  // show the first frame after a seek without waiting for the clock
};

}  // namespace hopline
