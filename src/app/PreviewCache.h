#pragma once

#include <condition_variable>
#include <mutex>
#include <queue>
#include <thread>
#include <unordered_map>
#include <vector>

#include <QImage>
#include <QObject>
#include <QString>

#include "model/Media.h"

namespace hopline {

// Generates timeline previews off the UI thread and caches them per source.
// A single worker thread drains a job queue; when a source is done it posts
// ready(id) back to the UI thread, which then repaints. FFmpeg decoding here
// uses its own decoder instances, independent of the Player's.
class PreviewCache : public QObject {
    Q_OBJECT

public:
    struct Waveform {
        std::vector<float> left;          // max abs amplitude per bucket, 0..1
        std::vector<float> right;         // == left for mono sources
        int bucketsPerSecond = 0;
    };

    struct Thumbnails {
        std::vector<QImage> images;       // evenly spaced across the source
        double interval = 0.0;            // seconds between images
    };

    explicit PreviewCache(QObject* parent = nullptr);
    ~PreviewCache() override;

    void request(MediaId id, const QString& path, bool video, bool audio, int width, int height);
    void clear();

    const Waveform* waveform(MediaId id) const;
    const Thumbnails* thumbnails(MediaId id) const;

signals:
    void ready(MediaId id);

private:
    struct Job {
        MediaId id;
        QString path;
        bool video;
        bool audio;
        int width;
        int height;
    };

    void workerLoop();

    std::thread m_thread;
    std::mutex m_queueMutex;
    std::condition_variable m_cv;
    std::queue<Job> m_jobs;
    bool m_stop = false;

    // Inserts only (plus clear() on the UI thread), so pointers returned to the
    // painter stay valid; unordered_map never invalidates them on insert.
    mutable std::mutex m_resultMutex;
    std::unordered_map<MediaId, Waveform> m_waveforms;
    std::unordered_map<MediaId, Thumbnails> m_thumbnails;
};

}  // namespace hopline
