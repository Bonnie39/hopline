#include "media/VideoDecoder.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
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

VideoDecoder::VideoDecoder()
    : m_format(nullptr, freeFormat)
    , m_codec(nullptr, freeCodec)
    , m_frame(nullptr, freeFrame)
    , m_packet(nullptr, freePacket)
{
}

VideoDecoder::~VideoDecoder() { close(); }

void VideoDecoder::close()
{
    if (m_sws) {
        sws_freeContext(m_sws);
        m_sws = nullptr;
    }
    m_packet.reset();
    m_frame.reset();
    m_codec.reset();
    m_format.reset();
    m_streamIndex = -1;
    m_width = m_height = 0;
    m_duration = m_timeBase = m_startTime = 0.0;
    m_skipUntil = -1.0;
    m_draining = false;
}

bool VideoDecoder::open(const std::string& path, std::string& error)
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
    const int index = av_find_best_stream(m_format.get(), AVMEDIA_TYPE_VIDEO, -1, -1, &decoder, 0);
    if (index < 0) {
        error = "no video stream";
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

    m_codec->thread_count = 0;  // let FFmpeg pick based on core count
    if (int rc = avcodec_open2(m_codec.get(), decoder, nullptr); rc < 0) {
        error = avError(rc);
        close();
        return false;
    }

    m_frame.reset(av_frame_alloc());
    m_packet.reset(av_packet_alloc());
    m_width = m_codec->width;
    m_height = m_codec->height;
    m_timeBase = av_q2d(stream->time_base);
    m_startTime = stream->start_time != AV_NOPTS_VALUE ? stream->start_time * m_timeBase : 0.0;
    if (m_format->duration != AV_NOPTS_VALUE) {
        m_duration = static_cast<double>(m_format->duration) / AV_TIME_BASE;
    }

    return true;
}

void VideoDecoder::convert(VideoFrame& out)
{
    AVFrame* src = m_frame.get();

    m_sws = sws_getCachedContext(m_sws, src->width, src->height,
                                 static_cast<AVPixelFormat>(src->format),
                                 src->width, src->height, AV_PIX_FMT_RGBA,
                                 SWS_BILINEAR, nullptr, nullptr, nullptr);

    out.width = src->width;
    out.height = src->height;
    out.rgba.resize(static_cast<size_t>(src->width) * src->height * 4);

    uint8_t* dst[4] = { out.rgba.data(), nullptr, nullptr, nullptr };
    int stride[4] = { src->width * 4, 0, 0, 0 };
    sws_scale(m_sws, src->data, src->linesize, 0, src->height, dst, stride);

}

double VideoDecoder::framePts() const
{
    const AVFrame* src = m_frame.get();
    const int64_t pts = src->best_effort_timestamp != AV_NOPTS_VALUE ? src->best_effort_timestamp : src->pts;
    return pts != AV_NOPTS_VALUE ? static_cast<double>(pts) * m_timeBase - m_startTime : 0.0;
}

bool VideoDecoder::seek(double seconds)
{
    if (!isOpen()) {
        return false;
    }

    const int64_t target = static_cast<int64_t>((seconds + m_startTime) / m_timeBase);
    if (av_seek_frame(m_format.get(), m_streamIndex, target, AVSEEK_FLAG_BACKWARD) < 0) {
        return false;
    }

    avcodec_flush_buffers(m_codec.get());
    m_skipUntil = seconds;
    m_draining = false;
    return true;
}

bool VideoDecoder::nextFrame(VideoFrame& out)
{
    if (!isOpen()) {
        return false;
    }

    while (true) {
        const int rc = avcodec_receive_frame(m_codec.get(), m_frame.get());
        if (rc == 0) {
            const double pts = framePts();
            // Check before converting: no point running swscale on frames the
            // seek is going to discard.
            if (m_skipUntil >= 0.0 && pts < m_skipUntil) {
                av_frame_unref(m_frame.get());
                continue;
            }
            m_skipUntil = -1.0;

            convert(out);
            out.pts = pts;
            av_frame_unref(m_frame.get());
            return true;
        }
        if (rc != AVERROR(EAGAIN)) {
            return false;  // EOF or hard error
        }

        if (av_read_frame(m_format.get(), m_packet.get()) < 0) {
            if (m_draining) {
                return false;
            }
            avcodec_send_packet(m_codec.get(), nullptr);  // flush buffered frames
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
