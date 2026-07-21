#pragma once

#include <cstdint>
#include <string>

#include "model/Time.h"

namespace hopline {

using MediaId = uint64_t;
inline constexpr MediaId kInvalidMedia = 0;

// Media-browser bin folders. The root always exists.
using FolderId = uint64_t;
inline constexpr FolderId kRootFolder = 1;

struct BinFolder {
    FolderId id = 0;
    FolderId parent = 0;  // 0 for the root
    std::string name;
    int label = 0;  // cosmetic color tag; 0 = none
};

// What the edit model knows about a file. Probed once on import; the model
// never opens it. Deliberately free of FFmpeg types.
struct MediaSource {
    MediaId id = kInvalidMedia;
    FolderId folder = kRootFolder;  // which bin folder it lives in
    int label = 0;                  // cosmetic color tag; 0 = none
    std::string path;
    Tick duration = 0;

    bool hasVideo = false;
    bool hasAudio = false;
    int width = 0;
    int height = 0;
    int rateNum = 0;  // video frame rate as a rational
    int rateDen = 1;
    int sampleRate = 0;
    int channels = 0;
};

}  // namespace hopline
