#include "engine/Player.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <unordered_map>
#include <utility>
#include <vector>

#include "model/Project.h"

namespace hopline {
namespace {

constexpr size_t kQueueDepth = 8;
constexpr int kSampleRate = 48000;
constexpr int kChannels = 2;
constexpr int64_t kMixChunkFrames = 1024;

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

// Draw `src` at its native size, centered on the canvas and cropped — the same
// placement the preview used to do, now done once here so upper layers that don't
// fill the canvas reveal the layers below. Opaque copy (decoded frames have no
// alpha); per-clip transforms/opacity/blend come later.
void blitCentered(VideoFrame& canvas, const VideoFrame& src)
{
    if (!src.valid()) {
        return;
    }
    const int cw = canvas.width, ch = canvas.height;
    const int sw = src.width, sh = src.height;
    const int dx = (cw - sw) / 2;
    const int dy = (ch - sh) / 2;
    for (int sy = 0; sy < sh; ++sy) {
        const int cy = dy + sy;
        if (cy < 0 || cy >= ch) {
            continue;
        }
        const int sx0 = std::max(0, -dx);
        const int sx1 = std::min(sw, cw - dx);
        if (sx1 <= sx0) {
            continue;
        }
        const int cx0 = dx + sx0;
        std::memcpy(&canvas.rgba[(static_cast<size_t>(cy) * cw + cx0) * 4],
                    &src.rgba[(static_cast<size_t>(sy) * sw + sx0) * 4],
                    static_cast<size_t>(sx1 - sx0) * 4);
    }
}

// Photoshop-style per-channel blend of a source value over a base (all 0..255).
int blendChannel(BlendMode mode, int b, int s)
{
    switch (mode) {
    case BlendMode::Normal:    return s;
    case BlendMode::Add:       return std::min(255, b + s);
    case BlendMode::Screen:    return 255 - (255 - b) * (255 - s) / 255;
    case BlendMode::Multiply:  return b * s / 255;
    case BlendMode::Overlay:   return b < 128 ? 2 * b * s / 255 : 255 - 2 * (255 - b) * (255 - s) / 255;
    case BlendMode::Darken:    return std::min(b, s);
    case BlendMode::ColorBurn: return s == 0 ? 0 : std::max(0, 255 - (255 - b) * 255 / s);
    case BlendMode::Lighten:   return std::max(b, s);
    case BlendMode::ColorDodge: return s >= 255 ? 255 : std::min(255, b * 255 / (255 - s));
    case BlendMode::SoftLight: {  // Pegtop approximation
        const double bn = b / 255.0, sn = s / 255.0;
        return static_cast<int>(std::lround(((1.0 - 2.0 * sn) * bn * bn + 2.0 * sn * bn) * 255.0));
    }
    case BlendMode::HardLight:  return s < 128 ? 2 * b * s / 255 : 255 - 2 * (255 - b) * (255 - s) / 255;
    case BlendMode::Difference: return b > s ? b - s : s - b;
    case BlendMode::Exclusion:  return b + s - 2 * b * s / 255;
    case BlendMode::Subtract:   return std::max(0, b - s);
    }
    return s;
}

// Apply a clip's Transform effect: scale + rotation + position + opacity, bilinear
// sampled, then blended with the layers below per its blend mode. The identity case
// (no scale/rotate/move, full opacity, Normal) falls back to the fast centered blit.
void blitTransformed(VideoFrame& canvas, const VideoFrame& src, const Transform& tf)
{
    if (!src.valid() || tf.opacity <= 0.0 || tf.scale <= 0.0) {
        return;
    }
    if (tf.isIdentity()) {
        blitCentered(canvas, src);
        return;
    }
    constexpr double kPi = 3.14159265358979323846;
    const int cw = canvas.width, ch = canvas.height;
    const int sw = src.width, sh = src.height;
    const double s = tf.scale;
    const double rad = tf.rotation * kPi / 180.0;
    const double cosr = std::cos(rad), sinr = std::sin(rad);
    const double ccx = cw / 2.0 + tf.posX, ccy = ch / 2.0 + tf.posY;  // clip center on canvas
    const double scx = sw / 2.0, scy = sh / 2.0;

    // Destination bounding box from the source's four corners.
    double minx = 1e18, miny = 1e18, maxx = -1e18, maxy = -1e18;
    const double cx[4] = { -scx, sw - scx, -scx, sw - scx };
    const double cy[4] = { -scy, -scy, sh - scy, sh - scy };
    for (int k = 0; k < 4; ++k) {
        const double px = cx[k] * s, py = cy[k] * s;
        const double x = cosr * px - sinr * py + ccx;
        const double y = sinr * px + cosr * py + ccy;
        minx = std::min(minx, x);
        maxx = std::max(maxx, x);
        miny = std::min(miny, y);
        maxy = std::max(maxy, y);
    }
    const int x0 = std::max(0, static_cast<int>(std::floor(minx)));
    const int x1 = std::min(cw, static_cast<int>(std::ceil(maxx)));
    const int y0 = std::max(0, static_cast<int>(std::floor(miny)));
    const int y1 = std::min(ch, static_cast<int>(std::ceil(maxy)));

    const double invS = 1.0 / s;
    const double a = std::clamp(tf.opacity, 0.0, 1.0);
    const BlendMode mode = tf.blend;
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            const double rx = x + 0.5 - ccx, ry = y + 0.5 - ccy;
            const double sxf = (cosr * rx + sinr * ry) * invS + scx;   // inverse rotate/scale
            const double syf = (-sinr * rx + cosr * ry) * invS + scy;
            if (sxf < 0.0 || sxf >= sw || syf < 0.0 || syf >= sh) {
                continue;
            }
            // Bilinear sample of the source.
            const int sx0 = static_cast<int>(sxf), sy0 = static_cast<int>(syf);
            const int sx1 = std::min(sx0 + 1, sw - 1), sy1 = std::min(sy0 + 1, sh - 1);
            const double fx = sxf - sx0, fy = syf - sy0;
            const uint8_t* p00 = &src.rgba[(static_cast<size_t>(sy0) * sw + sx0) * 4];
            const uint8_t* p10 = &src.rgba[(static_cast<size_t>(sy0) * sw + sx1) * 4];
            const uint8_t* p01 = &src.rgba[(static_cast<size_t>(sy1) * sw + sx0) * 4];
            const uint8_t* p11 = &src.rgba[(static_cast<size_t>(sy1) * sw + sx1) * 4];
            uint8_t* cp = &canvas.rgba[(static_cast<size_t>(y) * cw + x) * 4];
            for (int c = 0; c < 3; ++c) {
                const double top = p00[c] * (1 - fx) + p10[c] * fx;
                const double bot = p01[c] * (1 - fx) + p11[c] * fx;
                const int sv = static_cast<int>(std::lround(top * (1 - fy) + bot * fy));
                const int blended = blendChannel(mode, cp[c], sv);
                cp[c] = static_cast<uint8_t>(a >= 1.0 ? blended : std::lround(cp[c] * (1 - a) + blended * a));
            }
        }
    }
}

// One video track's decoder, with a one-frame lookahead so it can advance to an
// output time and hold the most recent frame at/before it (repeating or skipping
// source frames as the source rate differs from the sequence rate).
struct VideoLayer {
    VideoDecoder decoder;
    ClipId loaded = kInvalidClip;
    MediaId source = kInvalidMedia;
    Tick clipStart = 0;
    Tick clipSourceIn = 0;
    Transform transform;  // the clip's Transform effect, refreshed when the clip loads

    VideoFrame current;
    bool hasCurrent = false;
    VideoFrame pending;
    Tick pendingPts = 0;
    bool hasPending = false;
    bool ended = false;

    void resetStream()
    {
        hasCurrent = false;
        hasPending = false;
        ended = false;
    }

    Tick toTimeline(double srcPts) const { return clipStart + (ticksFromSeconds(srcPts) - clipSourceIn); }

    void advanceTo(Tick t, std::atomic<bool>& stop)
    {
        while (!stop.load(std::memory_order_relaxed)) {
            if (!hasPending) {
                if (ended) {
                    break;
                }
                VideoFrame f;
                if (!decoder.nextFrame(f)) {
                    ended = true;
                    break;
                }
                pendingPts = toTimeline(f.pts);
                pending = std::move(f);
                hasPending = true;
            }
            if (pendingPts <= t) {
                current = std::move(pending);
                hasCurrent = true;
                hasPending = false;
            } else {
                break;  // the next frame belongs to a later output time
            }
        }
        // Right after a (re)seek the decoder's earliest available frame can be just
        // past t (seek lands on a keyframe and skips up to the target), so there's no
        // frame at/before t. Use that nearest frame rather than dropping the layer —
        // otherwise a seek intermittently loses a layer (black / lower-track only).
        if (!hasCurrent && hasPending) {
            current = std::move(pending);
            hasCurrent = true;
            hasPending = false;
        }
    }
};

// One audio track's decoder plus its leftover-sample state. A track has at most
// one clip at a time, so a decoder per track is enough; kept across segments so a
// clip that spans a cut keeps streaming without a reseek.
struct TrackMix {
    AudioDecoder decoder;
    ClipId loaded = kInvalidClip;
    MediaId source = kInvalidMedia;
    std::vector<float> chunk;
    size_t consumed = 0;
    bool ended = false;
    float gainL = 1.0f;  // from the clip's Volume Controls, refreshed when the clip loads
    float gainR = 1.0f;

    void resetStream()
    {
        chunk.clear();
        consumed = 0;
        ended = false;
    }

    // Adds up to `frames` interleaved frames into `out`, applying per-channel gain;
    // exhausted source is silence.
    void mixInto(float* out, int64_t frames, int channels, std::atomic<bool>& stop)
    {
        int64_t produced = 0;
        while (produced < frames && !ended && !stop.load(std::memory_order_relaxed)) {
            if (consumed >= chunk.size()) {
                chunk.clear();
                consumed = 0;
                if (!decoder.nextChunk(chunk)) {
                    ended = true;
                    break;
                }
                if (chunk.empty()) {
                    continue;
                }
            }
            const int64_t avail = static_cast<int64_t>(chunk.size() - consumed) / channels;
            const int64_t take = std::min(frames - produced, avail);
            const size_t floats = static_cast<size_t>(take) * channels;
            const size_t base = static_cast<size_t>(produced) * channels;
            for (size_t i = 0; i < floats; ++i) {
                out[base + i] += chunk[consumed + i] * ((i & 1) == 0 ? gainL : gainR);
            }
            consumed += floats;
            produced += take;
        }
    }
};

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
            if (track.kind() != Track::Kind::Video) {
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
                blitTransformed(canvas, layer.current, layer.transform);
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

    // A decoder per audio track (persistent across seeks; see DecodeState). All active
    // tracks are summed each output chunk (basic mix; per-clip gain/pan is deferred).
    std::unordered_map<std::size_t, TrackMix>& mixes = m_decode->audio;
    std::vector<TrackMix*> active;
    std::vector<float> mix;

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
            // Volume Controls → per-channel gain (balance pan). Refreshed each load,
            // so effect edits (which come through a reload) take hold.
            const double gain = std::pow(10.0, clip->audio.volumeDb / 20.0);
            const double pan = std::clamp(clip->audio.pan, -1.0, 1.0);
            tm.gainL = static_cast<float>(gain * (pan > 0.0 ? 1.0 - pan : 1.0));
            tm.gainR = static_cast<float>(gain * (pan < 0.0 ? 1.0 + pan : 1.0));
            if (tm.decoder.isOpen()) {
                active.push_back(&tm);
            }
        }

        // Write exactly (segEnd - segStart) frames of the summed mix (silence when
        // no track is active).
        while (framesWritten < target && !m_stop) {
            const int64_t want = std::min<int64_t>(kMixChunkFrames, target - framesWritten);
            mix.assign(static_cast<size_t>(want) * kChannels, 0.0f);
            for (TrackMix* tm : active) {
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

void Player::refresh(const Project& project)
{
    if (!isOpen()) {
        return;
    }
    const Tick position = ticksFromSeconds(this->position());
    const bool wasPlaying = isPlaying();
    pause();
    stopThreads();

    const std::size_t oldTrackCount = m_seq.trackCount();
    m_seq = project.sequence();  // pick up the new effect values
    m_paths.clear();
    for (const MediaSource& media : project.mediaPool()) {
        m_paths[media.id] = media.path;
    }
    // Effect-only change: keep the decoders and their buffered frames so the current
    // frame re-composites without a reseek/decode. (Guard against an unexpected
    // structure change.)
    if (m_decode && m_seq.trackCount() != oldTrackCount) {
        m_decode->clear();
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
