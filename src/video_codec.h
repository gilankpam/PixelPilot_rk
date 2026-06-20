#ifndef VIDEO_CODEC_H
#define VIDEO_CODEC_H

#include <cstring>

enum class VideoCodec {
    UNKNOWN = 0,
    H264,
    H265
};

inline VideoCodec video_codec(const char* str) {
    if (!strcmp(str, "h264")) return VideoCodec::H264;
    if (!strcmp(str, "h265")) return VideoCodec::H265;
    return VideoCodec::UNKNOWN;
}

#endif // VIDEO_CODEC_H
