#include "media/AudioDecoder.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
}

namespace hopline {
namespace {

std::string avError(int code)
{
    char buf[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(code, buf, sizeof(buf));
    return buf;
}

void freeFormat(AVFormatContext* ctx) { avformat_close_input(&ctx); }
void freeCodec(AVCodecContext* ctx) { avcodec_free_context(&ctx); }
void freeFrame(AVFrame* f) { av_frame_free(&f); }
void freePacket(AVPacket* p) { av_packet_free(&p); }

}  // namespace

AudioDecoder::AudioDecoder()
    : m_format(nullptr, freeFormat)
    , m_codec(nullptr, freeCodec)
    , m_frame(nullptr, freeFrame)
    , m_packet(nullptr, freePacket)
{
}

AudioDecoder::~AudioDecoder() { close(); }

void AudioDecoder::close()
{
    if (m_swr) {
        swr_free(&m_swr);
    }
    m_packet.reset();
    m_frame.reset();
    m_codec.reset();
    m_format.reset();
    m_streamIndex = -1;
    m_sampleRate = m_channels = 0;
    m_timeBase = m_startTime = 0.0;
    m_skipUntil = -1.0;
    m_draining = false;
}

bool AudioDecoder::open(const std::string& path, int sampleRate, int channels, std::string& error)
{
    close();

    AVFormatContext* fmt = nullptr;
    if (int rc = avformat_open_input(&fmt, path.c_str(), nullptr, nullptr); rc < 0) {
        error = avError(rc);
        return false;
    }
    m_format.reset(fmt);

    if (int rc = avformat_find_stream_info(m_format.get(), nullptr); rc < 0) {
        error = avError(rc);
        close();
        return false;
    }

    const AVCodec* decoder = nullptr;
    const int index = av_find_best_stream(m_format.get(), AVMEDIA_TYPE_AUDIO, -1, -1, &decoder, 0);
    if (index < 0) {
        error = "no audio stream";
        close();
        return false;
    }
    m_streamIndex = index;

    AVStream* stream = m_format->streams[index];
    m_codec.reset(avcodec_alloc_context3(decoder));
    if (int rc = avcodec_parameters_to_context(m_codec.get(), stream->codecpar); rc < 0) {
        error = avError(rc);
        close();
        return false;
    }
    if (int rc = avcodec_open2(m_codec.get(), decoder, nullptr); rc < 0) {
        error = avError(rc);
        close();
        return false;
    }

    AVChannelLayout outLayout;
    av_channel_layout_default(&outLayout, channels);
    if (int rc = swr_alloc_set_opts2(&m_swr, &outLayout, AV_SAMPLE_FMT_FLT, sampleRate,
                                     &m_codec->ch_layout, m_codec->sample_fmt, m_codec->sample_rate,
                                     0, nullptr);
        rc < 0) {
        error = avError(rc);
        av_channel_layout_uninit(&outLayout);
        close();
        return false;
    }
    av_channel_layout_uninit(&outLayout);

    if (int rc = swr_init(m_swr); rc < 0) {
        error = avError(rc);
        close();
        return false;
    }

    m_frame.reset(av_frame_alloc());
    m_packet.reset(av_packet_alloc());
    m_sampleRate = sampleRate;
    m_channels = channels;
    m_timeBase = av_q2d(stream->time_base);
    m_startTime = stream->start_time != AV_NOPTS_VALUE ? stream->start_time * m_timeBase : 0.0;
    return true;
}

bool AudioDecoder::seek(double seconds)
{
    if (!isOpen()) {
        return false;
    }

    const int64_t target = static_cast<int64_t>((seconds + m_startTime) / m_timeBase);
    if (av_seek_frame(m_format.get(), m_streamIndex, target, AVSEEK_FLAG_BACKWARD) < 0) {
        return false;
    }

    avcodec_flush_buffers(m_codec.get());
    // Drop swr's buffered tail, or resampled audio from before the seek leaks through.
    swr_init(m_swr);
    m_skipUntil = seconds;
    m_draining = false;
    return true;
}

bool AudioDecoder::nextChunk(std::vector<float>& out)
{
    if (!isOpen()) {
        return false;
    }

    while (true) {
        const int rc = avcodec_receive_frame(m_codec.get(), m_frame.get());
        if (rc == 0) {
            AVFrame* src = m_frame.get();

            const int64_t rawPts = src->best_effort_timestamp != AV_NOPTS_VALUE ? src->best_effort_timestamp : src->pts;
            const double pts = rawPts != AV_NOPTS_VALUE ? rawPts * m_timeBase - m_startTime : 0.0;
            const double frameEnd = pts + static_cast<double>(src->nb_samples) / src->sample_rate;
            if (m_skipUntil >= 0.0 && frameEnd < m_skipUntil) {
                av_frame_unref(src);
                continue;
            }
            m_skipUntil = -1.0;

            // swr may hold buffered samples, so the output count isn't just a ratio.
            const int maxOut = static_cast<int>(
                swr_get_delay(m_swr, m_sampleRate) + src->nb_samples * static_cast<int64_t>(m_sampleRate) / src->sample_rate + 256);

            const size_t offset = out.size();
            out.resize(offset + static_cast<size_t>(maxOut) * m_channels);

            uint8_t* dst = reinterpret_cast<uint8_t*>(out.data() + offset);
            const int written = swr_convert(m_swr, &dst, maxOut,
                                            const_cast<const uint8_t**>(src->data), src->nb_samples);
            out.resize(offset + static_cast<size_t>(written > 0 ? written : 0) * m_channels);

            av_frame_unref(src);
            return true;
        }
        if (rc != AVERROR(EAGAIN)) {
            return false;
        }

        if (av_read_frame(m_format.get(), m_packet.get()) < 0) {
            if (m_draining) {
                return false;
            }
            avcodec_send_packet(m_codec.get(), nullptr);
            m_draining = true;
            continue;
        }

        if (m_packet->stream_index == m_streamIndex) {
            avcodec_send_packet(m_codec.get(), m_packet.get());
        }
        av_packet_unref(m_packet.get());
    }
}

}  // namespace hopline
