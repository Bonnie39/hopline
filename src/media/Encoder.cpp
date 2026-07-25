#include "media/Encoder.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/channel_layout.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
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

}  // namespace

Encoder::Encoder() = default;
Encoder::~Encoder() { close(); }

bool Encoder::open(const std::string& path, int width, int height, int fpsNum, int fpsDen,
                   int sampleRate, int channels, bool withAudio, std::string& error)
{
    close();
    m_width = width;
    m_height = height;
    m_channels = channels;
    m_sampleRate = sampleRate;
    m_hasAudio = withAudio;

    if (int rc = avformat_alloc_output_context2(&m_fmt, nullptr, nullptr, path.c_str()); rc < 0 || !m_fmt) {
        error = "could not create output for " + path;
        return false;
    }

    // Video: H.264 (libx264), yuv420p, sequence size and frame rate.
    const AVCodec* vcodec = avcodec_find_encoder_by_name("libx264");
    if (!vcodec) {
        vcodec = avcodec_find_encoder(AV_CODEC_ID_H264);
    }
    if (!vcodec) {
        error = "no H.264 encoder available";
        close();
        return false;
    }
    m_vstream = avformat_new_stream(m_fmt, nullptr);
    m_vctx = avcodec_alloc_context3(vcodec);
    m_vctx->width = width;
    m_vctx->height = height;
    m_vctx->pix_fmt = AV_PIX_FMT_YUV420P;
    m_vctx->time_base = AVRational{ fpsDen, fpsNum };
    m_vctx->framerate = AVRational{ fpsNum, fpsDen };
    m_vctx->gop_size = 12;
    av_opt_set(m_vctx->priv_data, "preset", "medium", 0);
    av_opt_set(m_vctx->priv_data, "crf", "18", 0);
    if (m_fmt->oformat->flags & AVFMT_GLOBALHEADER) {
        m_vctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }
    if (int rc = avcodec_open2(m_vctx, vcodec, nullptr); rc < 0) {
        error = "video encoder: " + avError(rc);
        close();
        return false;
    }
    avcodec_parameters_from_context(m_vstream->codecpar, m_vctx);
    m_vstream->time_base = m_vctx->time_base;

    m_vframe = av_frame_alloc();
    m_vframe->format = AV_PIX_FMT_YUV420P;
    m_vframe->width = width;
    m_vframe->height = height;
    if (int rc = av_frame_get_buffer(m_vframe, 0); rc < 0) {
        error = "video frame alloc: " + avError(rc);
        close();
        return false;
    }
    m_sws = sws_getContext(width, height, AV_PIX_FMT_RGBA, width, height, AV_PIX_FMT_YUV420P,
                           SWS_BILINEAR, nullptr, nullptr, nullptr);

    // Audio: AAC, planar float, sequence audio settings.
    if (m_hasAudio) {
        const AVCodec* acodec = avcodec_find_encoder(AV_CODEC_ID_AAC);
        if (!acodec) {
            error = "no AAC encoder available";
            close();
            return false;
        }
        m_astream = avformat_new_stream(m_fmt, nullptr);
        m_actx = avcodec_alloc_context3(acodec);
        m_actx->sample_fmt = AV_SAMPLE_FMT_FLTP;
        m_actx->sample_rate = sampleRate;
        m_actx->bit_rate = 192000;
        av_channel_layout_default(&m_actx->ch_layout, channels);
        m_actx->time_base = AVRational{ 1, sampleRate };
        if (m_fmt->oformat->flags & AVFMT_GLOBALHEADER) {
            m_actx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        }
        if (int rc = avcodec_open2(m_actx, acodec, nullptr); rc < 0) {
            error = "audio encoder: " + avError(rc);
            close();
            return false;
        }
        avcodec_parameters_from_context(m_astream->codecpar, m_actx);
        m_astream->time_base = AVRational{ 1, sampleRate };

        m_audioFrameSize = m_actx->frame_size > 0 ? m_actx->frame_size : 1024;
        m_aframe = av_frame_alloc();
        m_aframe->format = AV_SAMPLE_FMT_FLTP;
        m_aframe->sample_rate = sampleRate;
        m_aframe->nb_samples = m_audioFrameSize;
        av_channel_layout_default(&m_aframe->ch_layout, channels);
        if (int rc = av_frame_get_buffer(m_aframe, 0); rc < 0) {
            error = "audio frame alloc: " + avError(rc);
            close();
            return false;
        }
        m_fifo = av_audio_fifo_alloc(AV_SAMPLE_FMT_FLTP, channels, m_audioFrameSize);
    }

    if (!(m_fmt->oformat->flags & AVFMT_NOFILE)) {
        if (int rc = avio_open(&m_fmt->pb, path.c_str(), AVIO_FLAG_WRITE); rc < 0) {
            error = "could not open " + path + ": " + avError(rc);
            close();
            return false;
        }
    }
    if (int rc = avformat_write_header(m_fmt, nullptr); rc < 0) {
        error = "write header: " + avError(rc);
        close();
        return false;
    }
    m_headerWritten = true;
    return true;
}

bool Encoder::drain(AVCodecContext* ctx, AVStream* stream, std::string& error)
{
    AVPacket* pkt = av_packet_alloc();
    bool ok = true;
    while (true) {
        const int rc = avcodec_receive_packet(ctx, pkt);
        if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) {
            break;
        }
        if (rc < 0) {
            error = "encode: " + avError(rc);
            ok = false;
            break;
        }
        av_packet_rescale_ts(pkt, ctx->time_base, stream->time_base);
        pkt->stream_index = stream->index;
        if (int wr = av_interleaved_write_frame(m_fmt, pkt); wr < 0) {
            error = "mux: " + avError(wr);
            ok = false;
            break;
        }
        av_packet_unref(pkt);
    }
    av_packet_free(&pkt);
    return ok;
}

bool Encoder::writeVideo(const VideoFrame& frame, std::string& error)
{
    if (av_frame_make_writable(m_vframe) < 0) {
        error = "video frame not writable";
        return false;
    }
    const uint8_t* src[1] = { frame.rgba.data() };
    const int srcStride[1] = { m_width * 4 };
    sws_scale(m_sws, src, srcStride, 0, m_height, m_vframe->data, m_vframe->linesize);
    m_vframe->pts = m_videoPts++;

    if (int rc = avcodec_send_frame(m_vctx, m_vframe); rc < 0) {
        error = "video send: " + avError(rc);
        return false;
    }
    return drain(m_vctx, m_vstream, error);
}

bool Encoder::encodeAudioFrame(int nbSamples, std::string& error)
{
    if (av_frame_make_writable(m_aframe) < 0) {
        error = "audio frame not writable";
        return false;
    }
    m_aframe->nb_samples = nbSamples;
    if (int rc = av_audio_fifo_read(m_fifo, reinterpret_cast<void**>(m_aframe->data), nbSamples); rc < nbSamples) {
        error = "audio fifo read";
        return false;
    }
    m_aframe->pts = m_audioPts;
    m_audioPts += nbSamples;
    if (int rc = avcodec_send_frame(m_actx, m_aframe); rc < 0) {
        error = "audio send: " + avError(rc);
        return false;
    }
    return drain(m_actx, m_astream, error);
}

bool Encoder::writeAudio(const float* interleaved, int nbFrames, std::string& error)
{
    if (!m_hasAudio || nbFrames <= 0) {
        return true;
    }
    // Deinterleave into planar (FLTP) planes for the FIFO.
    m_planar.resize(static_cast<size_t>(nbFrames) * m_channels);
    for (int ch = 0; ch < m_channels; ++ch) {
        float* plane = m_planar.data() + static_cast<size_t>(ch) * nbFrames;
        for (int i = 0; i < nbFrames; ++i) {
            plane[i] = interleaved[static_cast<size_t>(i) * m_channels + ch];
        }
    }
    std::vector<void*> planes(m_channels);
    for (int ch = 0; ch < m_channels; ++ch) {
        planes[ch] = m_planar.data() + static_cast<size_t>(ch) * nbFrames;
    }
    if (av_audio_fifo_write(m_fifo, planes.data(), nbFrames) < nbFrames) {
        error = "audio fifo write";
        return false;
    }
    while (av_audio_fifo_size(m_fifo) >= m_audioFrameSize) {
        if (!encodeAudioFrame(m_audioFrameSize, error)) {
            return false;
        }
    }
    return true;
}

bool Encoder::finish(std::string& error)
{
    if (!m_fmt || !m_headerWritten) {
        error = "encoder not open";
        return false;
    }
    // Trailing partial audio frame, then flush both encoders.
    if (m_hasAudio) {
        const int rem = av_audio_fifo_size(m_fifo);
        if (rem > 0 && !encodeAudioFrame(rem, error)) {
            return false;
        }
        avcodec_send_frame(m_actx, nullptr);
        if (!drain(m_actx, m_astream, error)) {
            return false;
        }
    }
    avcodec_send_frame(m_vctx, nullptr);
    if (!drain(m_vctx, m_vstream, error)) {
        return false;
    }
    if (int rc = av_write_trailer(m_fmt); rc < 0) {
        error = "write trailer: " + avError(rc);
        return false;
    }
    return true;
}

void Encoder::close()
{
    if (m_sws) {
        sws_freeContext(m_sws);
        m_sws = nullptr;
    }
    if (m_fifo) {
        av_audio_fifo_free(m_fifo);
        m_fifo = nullptr;
    }
    if (m_vframe) {
        av_frame_free(&m_vframe);
    }
    if (m_aframe) {
        av_frame_free(&m_aframe);
    }
    if (m_vctx) {
        avcodec_free_context(&m_vctx);
    }
    if (m_actx) {
        avcodec_free_context(&m_actx);
    }
    if (m_fmt) {
        if (m_fmt->pb && !(m_fmt->oformat->flags & AVFMT_NOFILE)) {
            avio_closep(&m_fmt->pb);
        }
        avformat_free_context(m_fmt);
        m_fmt = nullptr;
    }
    m_vstream = m_astream = nullptr;
    m_videoPts = m_audioPts = 0;
    m_headerWritten = false;
}

}  // namespace hopline
