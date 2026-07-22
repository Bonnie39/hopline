#include "engine/Player.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <utility>
#include <vector>

#include "model/Project.h"

namespace hopline {
namespace {

constexpr size_t kQueueDepth = 8;
constexpr int kSampleRate = 48000;
constexpr int kChannels = 2;

using namespace std::chrono_literals;

VideoFrame makeBlack(int width, int height, double pts)
{
    VideoFrame frame;
    frame.width = width;
    frame.height = height;
    frame.pts = pts;
    frame.rgba.assign(static_cast<size_t>(width) * height * 4, 0);
    for (size_t i = 3; i < frame.rgba.size(); i += 4) {
        frame.rgba[i] = 255;  // opaque black
    }
    return frame;
}

}  // namespace

Player::Player()
    : m_queue(kQueueDepth)
{
}

Player::~Player() { close(); }

Tick Player::nextCut(const std::vector<Tick>& cuts, Tick after, Tick fallback) const
{
    for (Tick c : cuts) {
        if (c > after) {
            return std::min(c, fallback);
        }
    }
    return fallback;
}

bool Player::open(const Project& project, std::string& error)
{
    close();

    m_seq = project.sequence();  // snapshot: decode threads never touch the live model
    m_paths.clear();
    for (const MediaSource& media : project.mediaPool()) {
        m_paths[media.id] = media.path;
    }

    m_startTick = 0;
    m_eof = false;
    m_stop = false;
    m_dropped = 0;
    m_presentNext = false;
    m_clock.reset(0.0);
    m_queue.reopen();

    error.clear();
    if (m_seq.hasClips(Track::Kind::Audio)) {
        m_audioOut.open(kSampleRate, kChannels, error);  // failure falls back to wall clock
    }

    startThreads();
    m_open = true;
    return true;
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
    m_curVideoSource = kInvalidMedia;
    m_curAudioSource = kInvalidMedia;
    m_eof = false;
    m_dropped = 0;
    m_open = false;
}

void Player::startThreads()
{
    m_videoThread = std::thread(&Player::videoLoop, this);
    if (m_audioOut.isOpen()) {
        m_audioThread = std::thread(&Player::audioLoop, this);
    }
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

bool Player::ensureVideoSource(MediaId id)
{
    if (m_curVideoSource == id && m_video.isOpen()) {
        return true;
    }
    const auto it = m_paths.find(id);
    if (it == m_paths.end()) {
        return false;
    }
    std::string error;
    if (!m_video.open(it->second, error)) {
        m_curVideoSource = kInvalidMedia;
        return false;
    }
    m_curVideoSource = id;
    return true;
}

bool Player::ensureAudioSource(MediaId id)
{
    if (m_curAudioSource == id && m_audio.isOpen()) {
        return true;
    }
    const auto it = m_paths.find(id);
    if (it == m_paths.end()) {
        return false;
    }
    std::string error;
    if (!m_audio.open(it->second, kSampleRate, kChannels, error)) {
        m_curAudioSource = kInvalidMedia;
        return false;
    }
    m_curAudioSource = id;
    return true;
}

bool Player::emitBlackRange(Tick from, Tick to)
{
    if (m_seq.width() <= 0 || m_seq.height() <= 0) {
        return !m_stop;  // nothing sensible to show
    }
    const Tick frame = m_seq.frameDuration();
    for (Tick t = from; t < to && !m_stop; t += frame) {
        if (!m_queue.push(makeBlack(m_seq.width(), m_seq.height(), secondsFromTicks(t)))) {
            return false;
        }
    }
    return !m_stop;
}

bool Player::decodeVideoSegment(const Clip& clip, Tick segStart, Tick segEnd)
{
    if (!ensureVideoSource(clip.source)) {
        return emitBlackRange(segStart, segEnd);  // missing source shows black, not a freeze
    }

    m_video.seek(secondsFromTicks(clip.sourceTimeAt(segStart)));

    while (!m_stop) {
        VideoFrame frame;
        if (!m_video.nextFrame(frame)) {
            break;  // source ran out; a valid clip shouldn't, but don't hang if it does
        }

        const Tick sourceTicks = ticksFromSeconds(frame.pts);
        const Tick timelinePts = clip.timelineStart + (sourceTicks - clip.sourceIn);
        if (timelinePts >= segEnd) {
            break;
        }
        if (timelinePts < segStart) {
            continue;  // frames between the keyframe seek landed on and the target
        }

        frame.pts = secondsFromTicks(timelinePts);
        if (!m_queue.push(std::move(frame))) {
            return false;
        }
    }
    return !m_stop;
}

void Player::videoLoop()
{
    const Tick seqEnd = m_seq.duration();
    const std::vector<Tick> cuts = m_seq.cutPoints(Track::Kind::Video);

    Tick segStart = m_startTick;
    while (!m_stop && segStart < seqEnd) {
        const Tick segEnd = nextCut(cuts, segStart, seqEnd);
        const Clip* clip = m_seq.topVideoClipAt(segStart);

        const bool ok = clip ? decodeVideoSegment(*clip, segStart, segEnd)
                             : emitBlackRange(segStart, segEnd);
        if (!ok) {
            return;  // queue closed during shutdown
        }
        segStart = segEnd;
    }
    m_eof = true;
}

void Player::audioLoop()
{
    const Tick seqEnd = m_seq.duration();
    const std::vector<Tick> cuts = m_seq.cutPoints(Track::Kind::Audio);

    // Absolute sample target keeps timeline alignment exact: rounding per segment
    // would accumulate and drift the audio clock off the video.
    int64_t framesWritten = 0;

    auto writeFloats = [&](const float* data, size_t count) {
        size_t off = 0;
        while (off < count && !m_stop) {
            const size_t w = m_audioOut.buffer().write(data + off, count - off);
            off += w;
            if (w == 0) {
                std::this_thread::sleep_for(2ms);  // ring full
            }
        }
    };

    auto writeSilence = [&](int64_t frames) {
        static thread_local std::vector<float> zeros(1024 * kChannels, 0.0f);
        int64_t remaining = frames * kChannels;
        while (remaining > 0 && !m_stop) {
            const size_t n = static_cast<size_t>(std::min<int64_t>(remaining, zeros.size()));
            writeFloats(zeros.data(), n);
            remaining -= n;
        }
    };

    Tick segStart = m_startTick;
    while (!m_stop && segStart < seqEnd) {
        const Tick segEnd = nextCut(cuts, segStart, seqEnd);
        const int64_t target = std::llround(secondsFromTicks(segEnd) * kSampleRate);
        const Clip* clip = m_seq.firstAudioClipAt(segStart);

        if (clip && ensureAudioSource(clip->source)) {
            m_audio.seek(secondsFromTicks(clip->sourceTimeAt(segStart)));

            std::vector<float> chunk;
            size_t consumed = 0;
            while (framesWritten < target && !m_stop) {
                if (consumed >= chunk.size()) {
                    chunk.clear();
                    consumed = 0;
                    if (!m_audio.nextChunk(chunk)) {
                        break;  // source ended early; padded with silence below
                    }
                    if (chunk.empty()) {
                        continue;
                    }
                }
                const int64_t availFrames = (chunk.size() - consumed) / kChannels;
                const int64_t takeFrames = std::min<int64_t>(target - framesWritten, availFrames);
                const size_t takeFloats = static_cast<size_t>(takeFrames) * kChannels;
                writeFloats(chunk.data() + consumed, takeFloats);
                consumed += takeFloats;
                framesWritten += takeFrames;
            }
        }

        if (framesWritten < target) {
            writeSilence(target - framesWritten);
            framesWritten = target;
        }
        segStart = segEnd;
    }

    m_audioOut.setEndOfStream(true);
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

void Player::restartAt(Tick target, bool resumePlaying)
{
    // Clamp to content only when there is content; an empty sequence still lets the
    // playhead move freely (over black) so the user can position it.
    const Tick dur = m_seq.duration();
    target = dur > 0 ? std::clamp<Tick>(target, 0, dur) : std::max<Tick>(0, target);

    m_startTick = target;
    m_curVideoSource = kInvalidMedia;  // force a re-seek on both decoders
    m_curAudioSource = kInvalidMedia;

    m_queue.clear();
    m_audioOut.buffer().clear();
    m_audioOut.resetPosition(secondsFromTicks(target));
    m_audioOut.setEndOfStream(false);
    m_clock.reset(secondsFromTicks(target));
    m_eof = false;
    m_presentNext = true;

    startThreads();
    if (resumePlaying) {
        play();
    }
}

void Player::seek(double seconds)
{
    if (!isOpen()) {
        return;
    }

    const bool wasPlaying = isPlaying();
    pause();
    stopThreads();  // both decoders must be idle before repositioning

    restartAt(m_seq.snapToFrame(ticksFromSeconds(seconds)), wasPlaying);
}

void Player::reload(const Project& project)
{
    if (!isOpen()) {
        return;
    }

    const Tick position = ticksFromSeconds(this->position());
    const bool wasPlaying = isPlaying();
    pause();
    stopThreads();

    m_seq = project.sequence();
    m_paths.clear();
    for (const MediaSource& media : project.mediaPool()) {
        m_paths[media.id] = media.path;
    }

    // Audio presence can change under an edit (e.g. the last audio clip removed).
    const bool wantAudio = m_seq.hasClips(Track::Kind::Audio);
    if (wantAudio && !m_audioOut.isOpen()) {
        std::string error;
        m_audioOut.open(kSampleRate, kChannels, error);
    } else if (!wantAudio && m_audioOut.isOpen()) {
        m_audioOut.close();
    }

    restartAt(position, wasPlaying);
}

bool Player::isPlaying() const
{
    return m_audioOut.isOpen() ? m_audioOut.isRunning() : m_clock.running();
}

bool Player::update(VideoFrame& out)
{
    if (!m_open) {
        return false;
    }

    // After a seek the clock already sits at the target, so show the first frame
    // immediately rather than treating it as due.
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
