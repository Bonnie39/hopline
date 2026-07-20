#pragma once

#include <memory>
#include <string>

#include "media/VideoFrame.h"

struct AVCodecContext;
struct AVFormatContext;
struct AVFrame;
struct AVPacket;
struct SwsContext;

namespace hopline {

// Single-threaded decode of one video stream to RGBA. This is the stepping-stone
// implementation: swscale on the CPU caps out well below 4K60. The GPU path
// (D3D11VA decode, YUV upload, shader conversion) replaces the innards later.
class VideoDecoder {
public:
    VideoDecoder();
    ~VideoDecoder();

    VideoDecoder(const VideoDecoder&) = delete;
    VideoDecoder& operator=(const VideoDecoder&) = delete;

    bool open(const std::string& path, std::string& error);
    void close();

    // Returns false at end of stream or on decode error.
    bool nextFrame(VideoFrame& out);

    bool isOpen() const { return m_format != nullptr; }
    int width() const { return m_width; }
    int height() const { return m_height; }
    double duration() const { return m_duration; }

private:
    struct Deleters;
    void convert(VideoFrame& out);

    std::unique_ptr<AVFormatContext, void (*)(AVFormatContext*)> m_format;
    std::unique_ptr<AVCodecContext, void (*)(AVCodecContext*)> m_codec;
    std::unique_ptr<AVFrame, void (*)(AVFrame*)> m_frame;
    std::unique_ptr<AVPacket, void (*)(AVPacket*)> m_packet;
    SwsContext* m_sws = nullptr;

    int m_streamIndex = -1;
    int m_width = 0;
    int m_height = 0;
    double m_duration = 0.0;
    double m_timeBase = 0.0;
    bool m_draining = false;
};

}  // namespace hopline
