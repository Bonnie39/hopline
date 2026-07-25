#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "media/VideoFrame.h"

struct AVFormatContext;
struct AVCodecContext;
struct AVStream;
struct AVFrame;
struct SwsContext;
struct AVAudioFifo;

namespace hopline {

// Muxes an MP4 with an H.264 (libx264) video stream and, optionally, an AAC audio
// stream. Headless: takes RGBA canvas frames and interleaved float audio, exactly what
// the compositor/mixer produce. Frames arrive in order; PTS is derived from counters.
class Encoder {
public:
    Encoder();
    ~Encoder();
    Encoder(const Encoder&) = delete;
    Encoder& operator=(const Encoder&) = delete;

    bool open(const std::string& path, int width, int height, int fpsNum, int fpsDen,
              int sampleRate, int channels, bool withAudio, std::string& error);

    bool writeVideo(const VideoFrame& frame, std::string& error);            // one canvas frame
    bool writeAudio(const float* interleaved, int nbFrames, std::string& error);

    bool finish(std::string& error);  // flush encoders, write the trailer
    void close();

private:
    bool drain(AVCodecContext* ctx, AVStream* stream, std::string& error);
    bool encodeAudioFrame(int nbSamples, std::string& error);  // pull from the FIFO and encode

    AVFormatContext* m_fmt = nullptr;
    AVCodecContext* m_vctx = nullptr;
    AVCodecContext* m_actx = nullptr;
    AVStream* m_vstream = nullptr;
    AVStream* m_astream = nullptr;
    AVFrame* m_vframe = nullptr;
    AVFrame* m_aframe = nullptr;
    SwsContext* m_sws = nullptr;
    AVAudioFifo* m_fifo = nullptr;

    int m_width = 0, m_height = 0, m_channels = 0, m_sampleRate = 0;
    int m_audioFrameSize = 0;
    int64_t m_videoPts = 0;   // frame index (video stream time_base = 1/fps)
    int64_t m_audioPts = 0;   // cumulative samples (audio stream time_base = 1/sampleRate)
    std::vector<float> m_planar;  // deinterleave scratch for the FIFO
    bool m_hasAudio = false;
    bool m_headerWritten = false;
};

}  // namespace hopline
