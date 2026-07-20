#include "engine/Player.h"

#include <chrono>
#include <utility>
#include <vector>

namespace hopline {
namespace {

// Deep enough to absorb decode jitter, shallow enough that 4K frames don't blow
// up memory. Revisit when frames live on the GPU.
constexpr size_t kQueueDepth = 8;

constexpr int kSampleRate = 48000;
constexpr int kChannels = 2;

}  // namespace

Player::Player()
    : m_queue(kQueueDepth)
{
}

Player::~Player() { close(); }

bool Player::open(const std::string& path, std::string& error)
{
    close();

    if (!m_video.open(path, error)) {
        return false;
    }

    // Audio is optional: a file without it just falls back to the wall clock.
    std::string audioError;
    if (m_audio.open(path, kSampleRate, kChannels, audioError)) {
        if (!m_audioOut.open(kSampleRate, kChannels, audioError)) {
            m_audio.close();
        }
    }

    m_eof = false;
    m_stop = false;
    m_dropped = 0;
    m_clock.reset(0.0);
    m_queue.reopen();

    startThreads();
    return true;
}

void Player::startThreads()
{
    m_videoThread = std::thread(&Player::videoLoop, this);
    if (m_audioOut.isOpen()) {
        m_audioThread = std::thread(&Player::audioLoop, this);
    }
}

void Player::seek(double seconds)
{
    if (!isOpen()) {
        return;
    }

    const bool wasPlaying = isPlaying();
    pause();

    // Both decoders must be idle before repositioning them, so the seek is a
    // stop / reposition / restart rather than a message to a running thread.
    stopThreads();

    const double total = duration();
    seconds = seconds < 0.0 ? 0.0 : (total > 0.0 && seconds > total ? total : seconds);

    m_video.seek(seconds);
    if (m_audio.isOpen()) {
        m_audio.seek(seconds);
    }

    m_queue.clear();
    m_audioOut.buffer().clear();
    m_audioOut.resetPosition(seconds);
    m_audioOut.setEndOfStream(false);
    m_clock.reset(seconds);
    m_eof = false;
    m_presentNext = true;

    startThreads();
    if (wasPlaying) {
        play();
    }
}

void Player::close()
{
    stopThreads();
    m_audioOut.close();
    m_audio.close();
    m_video.close();
    m_queue.clear();
    m_clock.pause();
    m_clock.reset(0.0);
    m_eof = false;
    m_dropped = 0;
}

void Player::stopThreads()
{
    m_stop = true;
    m_queue.close();  // unblocks a decoder parked on a full queue
    m_audioOut.setPaused(true);

    if (m_videoThread.joinable()) {
        m_videoThread.join();
    }
    if (m_audioThread.joinable()) {
        m_audioThread.join();
    }

    m_queue.reopen();
    m_stop = false;
}

void Player::videoLoop()
{
    while (!m_stop) {
        VideoFrame frame;
        if (!m_video.nextFrame(frame)) {
            m_eof = true;
            return;
        }
        if (!m_queue.push(std::move(frame))) {
            return;  // queue closed during shutdown
        }
    }
}

void Player::audioLoop()
{
    std::vector<float> chunk;
    size_t consumed = 0;

    while (!m_stop) {
        if (consumed >= chunk.size()) {
            chunk.clear();
            consumed = 0;
            if (!m_audio.nextChunk(chunk)) {
                m_audioOut.setEndOfStream(true);
                return;
            }
            if (chunk.empty()) {
                continue;
            }
        }

        const size_t written = m_audioOut.buffer().write(chunk.data() + consumed, chunk.size() - consumed);
        consumed += written;
        if (written == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));  // ring full
        }
    }
}

void Player::play()
{
    if (!isOpen() || atEnd()) {
        return;
    }
    if (m_audioOut.isOpen()) {
        m_audioOut.setPaused(false);
    } else {
        m_clock.start();
    }
}

void Player::pause()
{
    if (m_audioOut.isOpen()) {
        m_audioOut.setPaused(true);
    } else {
        m_clock.pause();
    }
}

void Player::togglePlay()
{
    if (isPlaying()) {
        pause();
    } else {
        play();
    }
}

bool Player::isPlaying() const
{
    return m_audioOut.isOpen() ? m_audioOut.isRunning() : m_clock.running();
}

bool Player::update(VideoFrame& out)
{
    // After a seek the clock is already at the target, so wait for the first
    // decoded frame and show it immediately rather than treating it as due.
    if (m_presentNext) {
        VideoFrame frame;
        if (!m_queue.tryPop(frame)) {
            return false;
        }
        out = std::move(frame);
        m_presentNext = false;
        return true;
    }

    const double now = position();
    bool got = false;
    double pts = 0.0;

    while (m_queue.peekPts(pts) && pts <= now) {
        VideoFrame frame;
        if (!m_queue.tryPop(frame)) {
            break;
        }
        if (got) {
            ++m_dropped;  // clock outran decode; only the newest is worth showing
        }
        out = std::move(frame);
        got = true;
    }

    // Playback ends when the clock reaches the media duration, not when the last
    // frame is popped: that frame still owes its own display time, and audio can
    // outlast video.
    if (m_eof && m_queue.size() == 0 && now >= duration()) {
        pause();
        m_clock.reset(duration());
    }

    return got;
}

double Player::position() const
{
    const double total = duration();
    const double now = m_audioOut.isOpen() ? m_audioOut.position() : m_clock.seconds();
    return total > 0.0 && now > total ? total : now;
}

bool Player::atEnd() const
{
    return m_eof && m_queue.size() == 0 && position() >= duration();
}

}  // namespace hopline
