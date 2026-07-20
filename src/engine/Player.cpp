#include "engine/Player.h"

#include <utility>

namespace hopline {
namespace {

// Deep enough to absorb decode jitter, shallow enough that 4K frames don't blow
// up memory. Revisit when frames live on the GPU.
constexpr size_t kQueueDepth = 8;

}  // namespace

Player::Player()
    : m_queue(kQueueDepth)
{
}

Player::~Player() { close(); }

bool Player::open(const std::string& path, std::string& error)
{
    close();

    if (!m_decoder.open(path, error)) {
        return false;
    }

    m_eof = false;
    m_stop = false;
    m_dropped = 0;
    m_clock.reset(0.0);
    m_queue.reopen();
    m_thread = std::thread(&Player::decodeLoop, this);
    return true;
}

void Player::close()
{
    stopThread();
    m_decoder.close();
    m_queue.clear();
    m_clock.pause();
    m_clock.reset(0.0);
    m_eof = false;
    m_dropped = 0;
}

void Player::stopThread()
{
    if (!m_thread.joinable()) {
        return;
    }
    m_stop = true;
    m_queue.close();  // unblocks a decoder parked on a full queue
    m_thread.join();
    m_queue.reopen();
    m_stop = false;
}

void Player::decodeLoop()
{
    while (!m_stop) {
        VideoFrame frame;
        if (!m_decoder.nextFrame(frame)) {
            m_eof = true;
            return;
        }
        if (!m_queue.push(std::move(frame))) {
            return;  // queue closed during shutdown
        }
    }
}

void Player::play()
{
    if (isOpen() && !atEnd()) {
        m_clock.start();
    }
}

void Player::pause() { m_clock.pause(); }

void Player::togglePlay()
{
    if (m_clock.running()) {
        pause();
    } else {
        play();
    }
}

bool Player::update(VideoFrame& out)
{
    const double now = m_clock.seconds();
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
        m_clock.pause();
        m_clock.reset(duration());
    }

    return got;
}

double Player::position() const
{
    const double total = duration();
    const double now = m_clock.seconds();
    return total > 0.0 && now > total ? total : now;
}

bool Player::atEnd() const
{
    return m_eof && m_queue.size() == 0 && position() >= duration();
}

}  // namespace hopline
