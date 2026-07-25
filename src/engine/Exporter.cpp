#include "engine/Exporter.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <vector>

#include "engine/Compositor.h"
#include "media/Encoder.h"
#include "model/Project.h"

namespace hopline {
namespace {

constexpr int kSampleRate = 48000;
constexpr int kChannels = 2;
constexpr int64_t kMixChunk = 1024;

}  // namespace

bool Exporter::run(const Project& project, const std::string& outPath,
                   const std::function<void(double)>& onProgress,
                   std::atomic<bool>& cancel, std::string& error)
{
    const Sequence seq = project.sequence();  // value copy; the exporter never touches the live model
    std::unordered_map<MediaId, std::string> paths;
    for (const MediaSource& m : project.mediaPool()) {
        paths[m.id] = m.path;
    }

    const Tick seqEnd = seq.duration();
    const Tick frameDur = seq.frameDuration();
    const int cw = seq.width(), ch = seq.height();
    if (seqEnd <= 0) {
        error = "the sequence is empty — nothing to export";
        return false;
    }
    if (frameDur <= 0 || cw <= 0 || ch <= 0) {
        error = "invalid sequence settings";
        return false;
    }

    const bool withAudio = seq.hasClips(Track::Kind::Audio);
    Encoder encoder;
    if (!encoder.open(outPath, cw, ch, seq.rateNum(), seq.rateDen(), kSampleRate, kChannels, withAudio, error)) {
        return false;
    }

    std::unordered_map<std::size_t, VideoLayer> video;
    std::unordered_map<std::size_t, TrackMix> audio;

    // Video: one composited canvas per sequence frame, bottom-to-top (mirrors Player::videoLoop,
    // but streamed forward synchronously so it never reseeks mid-clip).
    VideoFrame canvas;
    Tick videoT = 0;
    auto produceVideoFrame = [&]() -> bool {
        fillBlack(canvas, cw, ch, secondsFromTicks(videoT));
        for (std::size_t i = 0; i < seq.trackCount(); ++i) {
            const Track& track = seq.track(i);
            if (track.kind() != Track::Kind::Video || !track.visible()) {
                continue;
            }
            const Clip* clip = track.clipAt(videoT);
            if (!clip) {
                if (auto it = video.find(i); it != video.end()) {
                    it->second.loaded = kInvalidClip;
                }
                continue;
            }
            VideoLayer& layer = video[i];
            const bool sameClip = layer.loaded == clip->id && layer.source == clip->source
                                  && layer.decoder.isOpen();
            if (!sameClip) {
                if (layer.source != clip->source || !layer.decoder.isOpen()) {
                    layer.decoder.close();
                    if (auto pit = paths.find(clip->source); pit != paths.end()) {
                        std::string e;
                        layer.decoder.open(pit->second, e);
                    }
                    layer.source = clip->source;
                }
                if (layer.decoder.isOpen()) {
                    layer.decoder.seek(secondsFromTicks(clip->sourceTimeAt(videoT)));
                }
                layer.clipStart = clip->timelineStart;
                layer.clipSourceIn = clip->sourceIn;
                layer.loaded = clip->id;
                layer.resetStream();
            }
            layer.transform = clip->transform;
            if (!layer.decoder.isOpen()) {
                continue;
            }
            layer.advanceTo(videoT, cancel);
            if (layer.hasCurrent) {
                blitTransformed(canvas, layer.current, resolveTransform(layer.transform, videoT - layer.clipStart));
            }
        }
        if (!encoder.writeVideo(canvas, error)) {
            return false;
        }
        videoT += frameDur;
        return true;
    };

    // Audio: segment-based mix (mirrors Player::audioLoop), summing active tracks per chunk.
    const std::vector<Tick> cuts = seq.cutPoints(Track::Kind::Audio);
    const int64_t totalAudioFrames = withAudio ? std::llround(secondsFromTicks(seqEnd) * kSampleRate) : 0;
    int64_t audioFramesWritten = 0;
    bool anySolo = false;
    for (std::size_t i = 0; i < seq.trackCount(); ++i) {
        if (seq.track(i).kind() == Track::Kind::Audio && seq.track(i).soloed()) {
            anySolo = true;
            break;
        }
    }

    std::vector<TrackMix*> active;
    std::vector<float> mix;
    Tick segStart = 0, segEnd = 0;
    auto nextCut = [&](Tick after) -> Tick {
        for (Tick c : cuts) {
            if (c > after) {
                return std::min(c, seqEnd);
            }
        }
        return seqEnd;
    };
    auto configureSegment = [&](Tick s) {
        active.clear();
        for (std::size_t i = 0; i < seq.trackCount(); ++i) {
            const Track& track = seq.track(i);
            if (track.kind() != Track::Kind::Audio) {
                continue;
            }
            if (track.muted() || (anySolo && !track.soloed())) {
                continue;
            }
            const Clip* clip = track.clipAt(s);
            if (!clip) {
                continue;
            }
            TrackMix& tm = audio[i];
            const bool sameClip = tm.loaded == clip->id && tm.source == clip->source && tm.decoder.isOpen();
            if (!sameClip) {
                if (tm.source != clip->source || !tm.decoder.isOpen()) {
                    tm.decoder.close();
                    if (auto it = paths.find(clip->source); it != paths.end()) {
                        std::string e;
                        tm.decoder.open(it->second, kSampleRate, kChannels, e);
                    }
                    tm.source = clip->source;
                }
                if (tm.decoder.isOpen()) {
                    tm.decoder.seek(secondsFromTicks(clip->sourceTimeAt(s)));
                }
                tm.resetStream();
                tm.loaded = clip->id;
            }
            tm.levels = clip->audio;
            tm.clipStart = clip->timelineStart;
            if (tm.decoder.isOpen()) {
                active.push_back(&tm);
            }
        }
    };
    if (withAudio) {
        segEnd = nextCut(0);
        configureSegment(0);
    }

    auto produceAudioChunk = [&]() -> bool {
        while (audioFramesWritten >= std::llround(secondsFromTicks(segEnd) * kSampleRate) && segEnd < seqEnd) {
            segStart = segEnd;
            segEnd = nextCut(segStart);
            configureSegment(segStart);
        }
        const int64_t segTarget = std::llround(secondsFromTicks(segEnd) * kSampleRate);
        const int64_t want = std::min({ kMixChunk, segTarget - audioFramesWritten,
                                        totalAudioFrames - audioFramesWritten });
        if (want <= 0) {
            return true;
        }
        mix.assign(static_cast<size_t>(want) * kChannels, 0.0f);
        const Tick chunkTick = ticksFromSeconds(static_cast<double>(audioFramesWritten) / kSampleRate);
        for (TrackMix* tm : active) {
            const Tick localT = chunkTick - tm->clipStart;
            computeGains(tm->levels.volumeDb.at(localT), tm->levels.pan.at(localT), tm->gainL, tm->gainR);
            tm->mixInto(mix.data(), want, kChannels, cancel);
        }
        for (float& sample : mix) {
            sample = std::clamp(sample, -1.0f, 1.0f);
        }
        if (!encoder.writeAudio(mix.data(), static_cast<int>(want), error)) {
            return false;
        }
        audioFramesWritten += want;
        return true;
    };

    // Interleave production in presentation-time order so the muxer's buffering stays bounded.
    const double totalSec = secondsFromTicks(seqEnd);
    int report = 0;
    while (!cancel.load(std::memory_order_relaxed)) {
        const bool vDone = videoT >= seqEnd;
        const bool aDone = !withAudio || audioFramesWritten >= totalAudioFrames;
        if (vDone && aDone) {
            break;
        }
        const double vTime = vDone ? 1e18 : secondsFromTicks(videoT);
        const double aTime = aDone ? 1e18 : static_cast<double>(audioFramesWritten) / kSampleRate;
        if (!vDone && (aDone || vTime <= aTime)) {
            if (!produceVideoFrame()) {
                return false;
            }
        } else if (!produceAudioChunk()) {
            return false;
        }
        if (onProgress && (++report % 8) == 0) {
            onProgress(std::clamp(secondsFromTicks(std::min(videoT, seqEnd)) / totalSec, 0.0, 1.0));
        }
    }

    if (cancel.load(std::memory_order_relaxed)) {
        error = "export canceled";
        return false;
    }
    if (onProgress) {
        onProgress(1.0);
    }
    return encoder.finish(error);
}

}  // namespace hopline
