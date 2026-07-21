#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace hopline {

struct StreamInfo {
    int index = 0;
    std::string type;        // "video", "audio", ...
    std::string codec;
    int width = 0;           // video only
    int height = 0;          // video only
    double frameRate = 0.0;  // video only
    int rateNum = 0;         // same rate as an exact rational (30000/1001 for NTSC)
    int rateDen = 1;
    int sampleRate = 0;      // audio only
    int channels = 0;        // audio only
    int64_t bitRate = 0;
};

struct MediaInfo {
    std::string path;
    std::string formatName;
    double duration = 0.0;  // seconds
    int64_t bitRate = 0;
    std::vector<StreamInfo> streams;
};

// Returns nullopt if the file cannot be opened or has no decodable streams.
std::optional<MediaInfo> probeMedia(const std::string& path, std::string& error);

}  // namespace hopline
