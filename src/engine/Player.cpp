#include "engine/Player.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <execution>
#include <numeric>
#include <unordered_map>
#include <utility>
#include <vector>

#include "engine/Compositor.h"
#include "model/Project.h"

namespace hopline {
namespace {

constexpr size_t kQueueDepth = 8;
constexpr int kSampleRate = 48000;
constexpr int kChannels = 2;
constexpr int64_t kMixChunkFrames = 1024;
// How far ahead the audio thread mixes into the ring. This is the ceiling on how long
// a live Volume/Pan change takes to reach the speakers (a gain edit only affects freshly
// mixed chunks; they sit behind whatever is already queued). Small for responsive live
// audio; still well above a per-clip reseek at a cut. Raise if you hear underruns.
constexpr int kAudioLookaheadMs = 150;

using namespace std::chrono_literals;

}  // namespace

// Persistent per-track decoders. Kept open across seeks: a seek only invalidates
// (forcing a reseek without reopening the file), while a sequence change clears.
struct Player::DecodeState {
    std::unordered_map<std::size_t, VideoLayer> video;
    std::unordered_map<std::size_t, TrackMix> audio;

    void invalidate()  // after a seek: reseek, but keep the files open
    {
        for (auto& [i, layer] : video) {
            layer.loaded = kInvalidClip;
            layer.resetStream();
        }
        for (auto& [i, track] : audio) {
            track.loaded = kInvalidClip;
            track.resetStream();
        }
    }

    void clear()  // after a sequence change: drop everything
    {
        video.clear();
        audio.clear();
    }
};

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
    m_decode = std::make_unique<DecodeState>();

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
    stopThreads();  // threads joined first, so releasing the decoders is safe
    m_decode.reset();
    m_audioOut.close();
    m_queue.clear();
    m_clock.pause();
    m_clock.reset(0.0);
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

void Player::videoLoop()
{
    const Tick seqEnd = m_seq.duration();
    const Tick frameDur = m_seq.frameDuration();
    const int cw = m_seq.width();
    const int ch = m_seq.height();
    if (frameDur <= 0 || cw <= 0 || ch <= 0) {
        m_eof = true;  // no canvas to render onto
        return;
    }

    // One decoder per video track (persistent across seeks; see DecodeState),
    // composited bottom-to-top each output frame.
    std::unordered_map<std::size_t, VideoLayer>& layers = m_decode->video;

    // Output frames land on sequence-rate boundaries (the common timebase every
    // layer resamples to). Align the start to a frame boundary.
    for (Tick t = (m_startTick / frameDur) * frameDur; !m_stop && t < seqEnd; t += frameDur) {
        VideoFrame canvas = makeBlack(cw, ch, secondsFromTicks(t));

        // Forward track order is bottom-to-top for video (later tracks composite over
        // earlier ones), so the last one drawn ends up on top.
        for (std::size_t i = 0; i < m_seq.trackCount() && !m_stop; ++i) {
            const Track& track = m_seq.track(i);
            if (track.kind() != Track::Kind::Video || !track.visible()) {
                continue;
            }
            const Clip* clip = track.clipAt(t);
            if (!clip) {
                if (auto it = layers.find(i); it != layers.end()) {
                    it->second.loaded = kInvalidClip;  // idle across this gap
                }
                continue;
            }

            VideoLayer& layer = layers[i];
            const bool sameClip = layer.loaded == clip->id && layer.source == clip->source
                                  && layer.decoder.isOpen();
            if (!sameClip) {
                if (layer.source != clip->source || !layer.decoder.isOpen()) {
                    layer.decoder.close();
                    const auto pit = m_paths.find(clip->source);
                    if (pit != m_paths.end()) {
                        std::string err;
                        layer.decoder.open(pit->second, err);
                    }
                    layer.source = clip->source;
                }
                if (layer.decoder.isOpen()) {
                    layer.decoder.seek(secondsFromTicks(clip->sourceTimeAt(t)));
                }
                layer.clipStart = clip->timelineStart;
                layer.clipSourceIn = clip->sourceIn;
                layer.loaded = clip->id;
                layer.resetStream();
            }
            layer.transform = clip->transform;  // cheap; picks up effect edits after a reload
            if (!layer.decoder.isOpen()) {
                continue;  // missing source: this layer stays transparent (shows below)
            }
            layer.advanceTo(t, m_stop);
            if (layer.hasCurrent) {
                blitTransformed(canvas, layer.current, resolveTransform(layer.transform, t - layer.clipStart));
            }
        }

        if (!m_queue.push(std::move(canvas))) {
            return;  // queue closed during shutdown
        }
    }
    m_eof = true;
}

void Player::audioLoop()
{
    const Tick seqEnd = m_seq.duration();
    const std::vector<Tick> cuts = m_seq.cutPoints(Track::Kind::Audio);

    // Absolute sample target keeps timeline alignment exact across cuts. Seed the
    // running count from the start position, so after a seek each segment writes its
    // own length (starting from 0 would misalign audio past the first cut).
    int64_t framesWritten = std::llround(secondsFromTicks(m_startTick) * kSampleRate);

    // Cap the queued audio so a live gain change (applied to freshly mixed chunks)
    // isn't stuck behind a full ring — it reaches the device within ~kAudioLookaheadMs.
    const size_t targetFill = static_cast<size_t>(kSampleRate) * kChannels * kAudioLookaheadMs / 1000;
    auto writeFloats = [&](const float* data, size_t count) {
        size_t off = 0;
        while (off < count && !m_stop) {
            if (m_audioOut.buffer().available() >= targetFill) {
                std::this_thread::sleep_for(2ms);  // enough queued; don't run further ahead
                continue;
            }
            const size_t w = m_audioOut.buffer().write(data + off, count - off);
            off += w;
            if (w == 0) {
                std::this_thread::sleep_for(2ms);  // ring full
            }
        }
    };

    // A decoder per audio track (persistent across seeks; see DecodeState). All active
    // tracks are summed each output chunk (basic mix; per-clip gain/pan is deferred).
    std::unordered_map<std::size_t, TrackMix>& mixes = m_decode->audio;
    std::vector<TrackMix*> active;
    std::vector<float> mix;

    // When any audio track is soloed, only soloed tracks are audible.
    bool anySolo = false;
    for (std::size_t i = 0; i < m_seq.trackCount(); ++i) {
        if (m_seq.track(i).kind() == Track::Kind::Audio && m_seq.track(i).soloed()) {
            anySolo = true;
            break;
        }
    }

    Tick segStart = m_startTick;
    while (!m_stop && segStart < seqEnd) {
        const Tick segEnd = nextCut(cuts, segStart, seqEnd);
        const int64_t target = std::llround(secondsFromTicks(segEnd) * kSampleRate);

        // Position each audio track's decoder for this segment.
        active.clear();
        for (std::size_t i = 0; i < m_seq.trackCount() && !m_stop; ++i) {
            const Track& track = m_seq.track(i);
            if (track.kind() != Track::Kind::Audio) {
                continue;
            }
            if (track.muted() || (anySolo && !track.soloed())) {
                continue;  // inaudible
            }
            const Clip* clip = track.clipAt(segStart);
            if (!clip) {
                continue;  // this track is silent across the segment
            }
            TrackMix& tm = mixes[i];
            const bool sameClip = tm.loaded == clip->id && tm.source == clip->source && tm.decoder.isOpen();
            if (!sameClip) {
                if (tm.source != clip->source || !tm.decoder.isOpen()) {
                    tm.decoder.close();
                    const auto it = m_paths.find(clip->source);
                    if (it != m_paths.end()) {
                        std::string err;
                        tm.decoder.open(it->second, kSampleRate, kChannels, err);
                    }
                    tm.source = clip->source;
                }
                if (tm.decoder.isOpen()) {
                    tm.decoder.seek(secondsFromTicks(clip->sourceTimeAt(segStart)));
                }
                tm.resetStream();
                tm.loaded = clip->id;
            }
            tm.levels = clip->audio;  // resolved per chunk in the mix loop (keyframes)
            tm.clipStart = clip->timelineStart;
            if (tm.decoder.isOpen()) {
                active.push_back(&tm);
            }
        }

        // Write exactly (segEnd - segStart) frames of the summed mix (silence when
        // no track is active).
        while (framesWritten < target && !m_stop) {
            const int64_t want = std::min<int64_t>(kMixChunkFrames, target - framesWritten);
            mix.assign(static_cast<size_t>(want) * kChannels, 0.0f);
            const Tick chunkTick = ticksFromSeconds(static_cast<double>(framesWritten) / kSampleRate);
            const uint64_t pvClip = m_previewAudioClip.load(std::memory_order_relaxed);
            for (TrackMix* tm : active) {
                if (pvClip != 0 && tm->loaded == pvClip) {  // live Volume Controls preview
                    tm->gainL = m_previewGainL.load(std::memory_order_relaxed);
                    tm->gainR = m_previewGainR.load(std::memory_order_relaxed);
                } else {  // resolve keyframed volume/pan at this chunk's time
                    const Tick localT = chunkTick - tm->clipStart;
                    computeGains(tm->levels.volumeDb.at(localT), tm->levels.pan.at(localT), tm->gainL, tm->gainR);
                }
                tm->mixInto(mix.data(), want, kChannels, m_stop);
            }
            for (float& s : mix) {
                s = std::clamp(s, -1.0f, 1.0f);
            }
            writeFloats(mix.data(), mix.size());
            framesWritten += want;
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

    if (m_decode) {
        m_decode->invalidate();  // reseek the open decoders; don't reopen the files
    }
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

    const std::size_t oldTrackCount = m_seq.trackCount();
    m_seq = project.sequence();
    m_paths.clear();
    for (const MediaSource& media : project.mediaPool()) {
        m_paths[media.id] = media.path;
    }
    if (m_decode) {
        // Same track layout (move/trim/effect edits): reseek the open files. A changed
        // track count shifts the per-index decoder mapping, so drop and reopen.
        if (m_seq.trackCount() == oldTrackCount) {
            m_decode->invalidate();
        } else {
            m_decode->clear();
        }
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

void Player::beginPreview()
{
    if (!isOpen() || !m_decode) {
        return;
    }
    pause();
    stopThreads();  // idle the decode threads; we composite on the caller's thread now

    m_previewLayers.clear();
    const Tick frameDur = m_seq.frameDuration();
    const int cw = m_seq.width(), ch = m_seq.height();
    if (frameDur <= 0 || cw <= 0 || ch <= 0) {
        return;
    }
    const Tick t = (ticksFromSeconds(position()) / frameDur) * frameDur;  // the displayed frame
    m_previewTick = t;

    // Position each active video track's decoder at the playhead and grab that frame.
    for (std::size_t i = 0; i < m_seq.trackCount(); ++i) {
        const Track& track = m_seq.track(i);
        if (track.kind() != Track::Kind::Video || !track.visible()) {
            continue;
        }
        const Clip* clip = track.clipAt(t);
        if (!clip) {
            continue;
        }
        VideoLayer& layer = m_decode->video[i];
        if (layer.source != clip->source || !layer.decoder.isOpen()) {
            layer.decoder.close();
            const auto pit = m_paths.find(clip->source);
            if (pit != m_paths.end()) {
                std::string err;
                layer.decoder.open(pit->second, err);
            }
            layer.source = clip->source;
        }
        if (!layer.decoder.isOpen()) {
            continue;
        }
        layer.decoder.seek(secondsFromTicks(clip->sourceTimeAt(t)));
        layer.clipStart = clip->timelineStart;
        layer.clipSourceIn = clip->sourceIn;
        layer.loaded = clip->id;
        layer.resetStream();
        layer.advanceTo(t, m_stop);
        if (layer.hasCurrent) {
            m_previewLayers.push_back({ clip->id, layer.current });  // copy once, reuse per drag frame
        }
    }
}

const VideoFrame& Player::previewComposite(const Project& project)
{
    const int cw = m_seq.width(), ch = m_seq.height();
    if (cw <= 0 || ch <= 0) {
        m_previewCanvas = VideoFrame{};
        return m_previewCanvas;
    }
    fillBlack(m_previewCanvas, cw, ch, secondsFromTicks(m_previewTick));  // reuses the buffer
    const Sequence& seq = project.sequence();
    for (const PreviewFrame& pl : m_previewLayers) {  // bottom-to-top
        const Clip* clip = seq.findClip(pl.clip);
        const ResolvedTransform rt = clip ? resolveTransform(clip->transform, m_previewTick - clip->timelineStart)
                                          : ResolvedTransform{};
        blitTransformed(m_previewCanvas, pl.frame, rt);
    }
    return m_previewCanvas;
}

void Player::setAudioPreview(ClipId clip, double volumeDb, double pan)
{
    float l = 1.0f, r = 1.0f;
    computeGains(volumeDb, pan, l, r);
    m_previewGainL.store(l, std::memory_order_relaxed);
    m_previewGainR.store(r, std::memory_order_relaxed);
    m_previewAudioClip.store(clip, std::memory_order_relaxed);  // set last: publishes the gains
}

void Player::beginScrub()
{
    if (!isOpen()) {
        return;
    }
    pause();
    stopThreads();  // idle the decode threads; scrubComposite drives them ad-hoc
    m_scrubbing = true;
}

const VideoFrame& Player::scrubComposite(Tick t)
{
    const Tick frameDur = m_seq.frameDuration();
    const int cw = m_seq.width(), ch = m_seq.height();
    if (frameDur > 0) {
        t = (t / frameDur) * frameDur;
    }
    t = std::max<Tick>(0, t);
    m_previewTick = t;
    if (cw <= 0 || ch <= 0) {
        m_previewCanvas = VideoFrame{};
        return m_previewCanvas;
    }
    fillBlack(m_previewCanvas, cw, ch, secondsFromTicks(t));

    if (m_decode) {
        const Tick fwdLimit = ticksFromSeconds(1.5);  // decode forward for small steps; reseek for jumps
        for (std::size_t i = 0; i < m_seq.trackCount(); ++i) {
            const Track& track = m_seq.track(i);
            if (track.kind() != Track::Kind::Video || !track.visible()) {
                continue;
            }
            const Clip* clip = track.clipAt(t);
            if (!clip) {
                continue;
            }
            VideoLayer& layer = m_decode->video[i];
            const bool sameClip = layer.loaded == clip->id && layer.source == clip->source
                                  && layer.decoder.isOpen();
            const bool forward = sameClip && layer.hasCurrent && t >= layer.currentPts
                                 && (t - layer.currentPts) <= fwdLimit;
            if (!sameClip) {
                if (layer.source != clip->source || !layer.decoder.isOpen()) {
                    layer.decoder.close();
                    const auto pit = m_paths.find(clip->source);
                    if (pit != m_paths.end()) {
                        std::string err;
                        layer.decoder.open(pit->second, err);
                    }
                    layer.source = clip->source;
                }
                if (layer.decoder.isOpen()) {
                    layer.decoder.seek(secondsFromTicks(clip->sourceTimeAt(t)));
                }
                layer.clipStart = clip->timelineStart;
                layer.clipSourceIn = clip->sourceIn;
                layer.loaded = clip->id;
                layer.resetStream();
            } else if (!forward) {
                layer.decoder.seek(secondsFromTicks(clip->sourceTimeAt(t)));  // backward / big jump
                layer.resetStream();
            }
            if (!layer.decoder.isOpen()) {
                continue;
            }
            layer.advanceTo(t, m_stop);
            if (layer.hasCurrent) {
                blitTransformed(m_previewCanvas, layer.current,
                                resolveTransform(clip->transform, t - clip->timelineStart));
            }
        }
    }

    // Keep the clock in step so the time readout follows the scrub.
    m_startTick = t;
    if (m_audioOut.isOpen()) {
        m_audioOut.resetPosition(secondsFromTicks(t));
    } else {
        m_clock.reset(secondsFromTicks(t));
    }
    return m_previewCanvas;
}

void Player::endScrub()
{
    if (!m_scrubbing) {
        return;
    }
    m_scrubbing = false;
    if (m_decode) {
        m_decode->invalidate();  // decoders were driven ad-hoc; reseek cleanly on restart
    }
    restartAt(m_startTick, false);  // resume paused at the scrub position
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
