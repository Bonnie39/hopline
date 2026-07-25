#pragma once

// Shared CPU render primitives used by both real-time playback (Player) and offline
// export (Exporter), so an export composites/mixes bit-for-bit like the preview. A GPU
// pipeline would replace the innards here behind the same shapes.

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <execution>
#include <numeric>
#include <vector>

#include "media/AudioDecoder.h"
#include "media/VideoDecoder.h"
#include "media/VideoFrame.h"
#include "model/Clip.h"
#include "model/Media.h"
#include "model/Time.h"

namespace hopline {

inline VideoFrame makeBlack(int width, int height, double pts)
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

// Reset a frame buffer to opaque black at the given size (reusing its allocation).
inline void fillBlack(VideoFrame& f, int cw, int ch, double pts)
{
    f.width = cw;
    f.height = ch;
    f.pts = pts;
    f.rgba.assign(static_cast<size_t>(cw) * ch * 4, 0);
    for (size_t i = 3; i < f.rgba.size(); i += 4) {
        f.rgba[i] = 255;
    }
}

// Draw `src` at its native size, centered on the canvas and cropped, so upper layers
// that don't fill the canvas reveal the layers below. Opaque copy (decoded frames
// have no alpha).
inline void blitCentered(VideoFrame& canvas, const VideoFrame& src)
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

// Volume Controls → per-channel linear gain (dB gain + balance pan).
inline void computeGains(double volumeDb, double panValue, float& gainL, float& gainR)
{
    const double gain = std::pow(10.0, volumeDb / 20.0);
    const double pan = std::clamp(panValue, -1.0, 1.0);
    gainL = static_cast<float>(gain * (pan > 0.0 ? 1.0 - pan : 1.0));
    gainR = static_cast<float>(gain * (pan < 0.0 ? 1.0 + pan : 1.0));
}

// A Transform evaluated at one instant (clip-local ticks) — plain values the
// compositor consumes, after resolving each keyframed property.
struct ResolvedTransform {
    double scale = 1.0, posX = 0.0, posY = 0.0, rotation = 0.0, opacity = 1.0;
    BlendMode blend = BlendMode::Normal;

    bool isIdentity() const
    {
        return scale == 1.0 && posX == 0.0 && posY == 0.0 && rotation == 0.0
               && opacity >= 1.0 && blend == BlendMode::Normal;
    }
};

inline ResolvedTransform resolveTransform(const Transform& tf, Tick localT)
{
    return { tf.scale.at(localT), tf.posX.at(localT), tf.posY.at(localT),
             tf.rotation.at(localT), tf.opacity.at(localT), tf.blend };
}

// Photoshop-style per-channel blend of a source value over a base (all 0..255).
inline int blendChannel(BlendMode mode, int b, int s)
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
// falls back to the fast centered blit.
inline void blitTransformed(VideoFrame& canvas, const VideoFrame& src, const ResolvedTransform& tf)
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
    uint8_t* const dst = canvas.rgba.data();
    const uint8_t* const srcData = src.rgba.data();
    auto renderRow = [&](int y) {
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
            const uint8_t* p00 = &srcData[(static_cast<size_t>(sy0) * sw + sx0) * 4];
            const uint8_t* p10 = &srcData[(static_cast<size_t>(sy0) * sw + sx1) * 4];
            const uint8_t* p01 = &srcData[(static_cast<size_t>(sy1) * sw + sx0) * 4];
            const uint8_t* p11 = &srcData[(static_cast<size_t>(sy1) * sw + sx1) * 4];
            uint8_t* cp = &dst[(static_cast<size_t>(y) * cw + x) * 4];
            for (int c = 0; c < 3; ++c) {
                const double top = p00[c] * (1 - fx) + p10[c] * fx;
                const double bot = p01[c] * (1 - fx) + p11[c] * fx;
                const int sv = static_cast<int>(std::lround(top * (1 - fy) + bot * fy));
                const int blended = blendChannel(mode, cp[c], sv);
                cp[c] = static_cast<uint8_t>(a >= 1.0 ? blended : std::lround(cp[c] * (1 - a) + blended * a));
            }
        }
    };

    // Rows are disjoint in the canvas, so parallelize across cores when the work is
    // large enough to be worth the dispatch overhead.
    if (static_cast<long long>(y1 - y0) * (x1 - x0) > 150000) {
        std::vector<int> rows(static_cast<size_t>(y1 - y0));
        std::iota(rows.begin(), rows.end(), y0);
        std::for_each(std::execution::par, rows.begin(), rows.end(), renderRow);
    } else {
        for (int y = y0; y < y1; ++y) {
            renderRow(y);
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
    Tick currentPts = 0;  // timeline pts of `current` (for forward-scrub decisions)
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
                currentPts = pendingPts;
                hasCurrent = true;
                hasPending = false;
            } else {
                break;  // the next frame belongs to a later output time
            }
        }
        // Right after a (re)seek the decoder's earliest available frame can be just past t
        // (seek lands on a keyframe and skips up to the target), so there's no frame at/before
        // t. Use that nearest frame rather than dropping the layer.
        if (!hasCurrent && hasPending) {
            current = std::move(pending);
            currentPts = pendingPts;
            hasCurrent = true;
            hasPending = false;
        }
    }
};

// One audio track's decoder plus its leftover-sample state. A track has at most one clip
// at a time, so a decoder per track is enough; kept across segments so a clip that spans a
// cut keeps streaming without a reseek.
struct TrackMix {
    AudioDecoder decoder;
    ClipId loaded = kInvalidClip;
    MediaId source = kInvalidMedia;
    std::vector<float> chunk;
    size_t consumed = 0;
    bool ended = false;
    float gainL = 1.0f;  // resolved per mix chunk from `levels` (keyframed), or preview override
    float gainR = 1.0f;
    AudioLevels levels;  // the clip's Volume Controls
    Tick clipStart = 0;  // for clip-local keyframe times

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

}  // namespace hopline
