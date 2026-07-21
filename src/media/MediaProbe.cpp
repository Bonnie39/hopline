#include "media/MediaProbe.h"

#include <memory>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
}

namespace hopline {
namespace {

std::string avError(int code)
{
    char buf[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(code, buf, sizeof(buf));
    return buf;
}

struct FormatContextDeleter {
    void operator()(AVFormatContext* ctx) const { avformat_close_input(&ctx); }
};
using FormatContextPtr = std::unique_ptr<AVFormatContext, FormatContextDeleter>;

}  // namespace

std::optional<MediaInfo> probeMedia(const std::string& path, std::string& error)
{
    AVFormatContext* raw = nullptr;
    if (int rc = avformat_open_input(&raw, path.c_str(), nullptr, nullptr); rc < 0) {
        error = avError(rc);
        return std::nullopt;
    }
    FormatContextPtr ctx(raw);

    if (int rc = avformat_find_stream_info(ctx.get(), nullptr); rc < 0) {
        error = avError(rc);
        return std::nullopt;
    }

    MediaInfo info;
    info.path = path;
    info.formatName = ctx->iformat->long_name ? ctx->iformat->long_name : ctx->iformat->name;
    info.bitRate = ctx->bit_rate;
    if (ctx->duration != AV_NOPTS_VALUE) {
        info.duration = static_cast<double>(ctx->duration) / AV_TIME_BASE;
    }

    for (unsigned i = 0; i < ctx->nb_streams; ++i) {
        const AVStream* stream = ctx->streams[i];
        const AVCodecParameters* par = stream->codecpar;

        StreamInfo s;
        s.index = static_cast<int>(i);
        s.type = av_get_media_type_string(par->codec_type) ? av_get_media_type_string(par->codec_type) : "unknown";
        s.codec = avcodec_get_name(par->codec_id);
        s.bitRate = par->bit_rate;

        if (par->codec_type == AVMEDIA_TYPE_VIDEO) {
            s.width = par->width;
            s.height = par->height;
            const AVRational fps = stream->avg_frame_rate;
            if (fps.den > 0 && fps.num > 0) {
                s.frameRate = av_q2d(fps);
                s.rateNum = fps.num;
                s.rateDen = fps.den;
            }
        } else if (par->codec_type == AVMEDIA_TYPE_AUDIO) {
            s.sampleRate = par->sample_rate;
            s.channels = par->ch_layout.nb_channels;
        }

        info.streams.push_back(std::move(s));
    }

    if (info.streams.empty()) {
        error = "no streams found";
        return std::nullopt;
    }

    return info;
}

}  // namespace hopline
