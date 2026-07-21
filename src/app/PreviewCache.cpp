#include "app/PreviewCache.h"

#include <algorithm>
#include <cmath>

#include <QMetaObject>

#include "media/AudioDecoder.h"
#include "media/VideoDecoder.h"

namespace hopline {
namespace {

constexpr int kWaveformSampleRate = 8000;   // mono; plenty for an envelope
constexpr int kBucketsPerSecond = 100;      // 10 ms peaks
constexpr int kThumbHeight = 48;
constexpr int kMaxThumbs = 40;
constexpr double kThumbInterval = 3.0;  // aim for a thumbnail roughly every few seconds

PreviewCache::Waveform generateWaveform(const QString& path)
{
    PreviewCache::Waveform wave;
    wave.bucketsPerSecond = kBucketsPerSecond;

    AudioDecoder decoder;
    std::string error;
    if (!decoder.open(path.toStdString(), kWaveformSampleRate, 1, error)) {
        return wave;
    }

    std::vector<float>& peaks = wave.peaks;
    int64_t sampleIndex = 0;
    std::vector<float> chunk;
    while (decoder.nextChunk(chunk)) {
        for (float sample : chunk) {
            const size_t bucket = static_cast<size_t>(sampleIndex * kBucketsPerSecond / kWaveformSampleRate);
            if (bucket >= peaks.size()) {
                peaks.resize(bucket + 1024, 0.0f);
            }
            peaks[bucket] = std::max(peaks[bucket], std::fabs(sample));
            ++sampleIndex;
        }
        chunk.clear();
    }

    const size_t used = static_cast<size_t>(sampleIndex * kBucketsPerSecond / kWaveformSampleRate) + 1;
    if (peaks.size() > used) {
        peaks.resize(used);
    }
    return wave;
}

PreviewCache::Thumbnails generateThumbnails(const QString& path, int width, int height)
{
    PreviewCache::Thumbnails thumbs;

    VideoDecoder decoder;
    std::string error;
    if (!decoder.open(path.toStdString(), error) || decoder.duration() <= 0.0) {
        return thumbs;
    }

    const double duration = decoder.duration();
    const int count = std::clamp(static_cast<int>(std::llround(duration / kThumbInterval)), 1, kMaxThumbs);
    thumbs.interval = duration / count;

    const int thumbWidth = height > 0 ? kThumbHeight * width / height : kThumbHeight;

    for (int i = 0; i < count; ++i) {
        const double t = (i + 0.5) * thumbs.interval;
        if (!decoder.seek(t)) {
            break;
        }
        VideoFrame frame;
        if (!decoder.nextFrame(frame) || !frame.valid()) {
            continue;
        }
        const QImage image(frame.rgba.data(), frame.width, frame.height, frame.width * 4,
                           QImage::Format_RGBA8888);
        thumbs.images.push_back(
            image.scaled(thumbWidth, kThumbHeight, Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
                .convertToFormat(QImage::Format_RGB32));
    }
    return thumbs;
}

}  // namespace

PreviewCache::PreviewCache(QObject* parent)
    : QObject(parent)
{
    m_thread = std::thread(&PreviewCache::workerLoop, this);
}

PreviewCache::~PreviewCache()
{
    {
        std::lock_guard lock(m_queueMutex);
        m_stop = true;
    }
    m_cv.notify_all();
    if (m_thread.joinable()) {
        m_thread.join();
    }
}

void PreviewCache::request(MediaId id, const QString& path, bool video, bool audio, int width, int height)
{
    {
        std::lock_guard lock(m_queueMutex);
        m_jobs.push({ id, path, video, audio, width, height });
    }
    m_cv.notify_one();
}

void PreviewCache::clear()
{
    std::lock_guard lock(m_resultMutex);
    m_waveforms.clear();
    m_thumbnails.clear();
}

const PreviewCache::Waveform* PreviewCache::waveform(MediaId id) const
{
    std::lock_guard lock(m_resultMutex);
    const auto it = m_waveforms.find(id);
    return it != m_waveforms.end() ? &it->second : nullptr;
}

const PreviewCache::Thumbnails* PreviewCache::thumbnails(MediaId id) const
{
    std::lock_guard lock(m_resultMutex);
    const auto it = m_thumbnails.find(id);
    return it != m_thumbnails.end() ? &it->second : nullptr;
}

void PreviewCache::workerLoop()
{
    while (true) {
        Job job;
        {
            std::unique_lock lock(m_queueMutex);
            m_cv.wait(lock, [this] { return m_stop || !m_jobs.empty(); });
            if (m_stop) {
                return;
            }
            job = std::move(m_jobs.front());
            m_jobs.pop();
        }

        if (job.audio) {
            Waveform wave = generateWaveform(job.path);
            std::lock_guard lock(m_resultMutex);
            m_waveforms[job.id] = std::move(wave);
        }
        if (job.video) {
            Thumbnails thumbs = generateThumbnails(job.path, job.width, job.height);
            std::lock_guard lock(m_resultMutex);
            m_thumbnails[job.id] = std::move(thumbs);
        }

        // Marshal the signal to the UI thread; the painter reads the cache there.
        const MediaId id = job.id;
        QMetaObject::invokeMethod(this, [this, id] { emit ready(id); }, Qt::QueuedConnection);
    }
}

}  // namespace hopline
