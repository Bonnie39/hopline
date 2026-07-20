#pragma once

#include <memory>
#include <string>
#include <vector>

struct AVCodecContext;
struct AVFormatContext;
struct AVFrame;
struct AVPacket;
struct SwrContext;

namespace hopline {

// Decodes one audio stream and resamples it to interleaved float at the device's
// rate and channel count, so the mixer and output stage never see source formats.
class AudioDecoder {
public:
    AudioDecoder();
    ~AudioDecoder();

    AudioDecoder(const AudioDecoder&) = delete;
    AudioDecoder& operator=(const AudioDecoder&) = delete;

    bool open(const std::string& path, int sampleRate, int channels, std::string& error);
    void close();

    // Appends interleaved samples. False at end of stream.
    bool nextChunk(std::vector<float>& out);

    bool isOpen() const { return m_format != nullptr; }
    int sampleRate() const { return m_sampleRate; }
    int channels() const { return m_channels; }

private:
    std::unique_ptr<AVFormatContext, void (*)(AVFormatContext*)> m_format;
    std::unique_ptr<AVCodecContext, void (*)(AVCodecContext*)> m_codec;
    std::unique_ptr<AVFrame, void (*)(AVFrame*)> m_frame;
    std::unique_ptr<AVPacket, void (*)(AVPacket*)> m_packet;
    SwrContext* m_swr = nullptr;

    int m_streamIndex = -1;
    int m_sampleRate = 0;
    int m_channels = 0;
    bool m_draining = false;
};

}  // namespace hopline
