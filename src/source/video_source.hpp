// Common interface of the LAN camera back ends behind libBambuSource:
//   CameraStream  A1/P1: TLS port 6000, one JPEG per frame
//   RtspStream    X1 and newer models: RTSPS port 322, H.264
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace bbl_lan {

enum class CameraState { Idle, Connecting, Streaming, Failed, Closed };

enum class VideoCodec { Jpeg, H264 };

struct VideoFormat {
    VideoCodec codec      = VideoCodec::Jpeg;
    int        width      = 0;
    int        height     = 0;
    int        frame_rate = 0;
};

// One sample for Bambu_ReadSample: a whole JPEG, or one H.264 access unit in
// Annex-B form (start codes, SPS/PPS in front of every IDR).
struct VideoFrame {
    std::vector<uint8_t> data;
    uint64_t             timestamp_ms = 0;
    bool                 keyframe     = true;
};

class VideoSource {
public:
    virtual ~VideoSource() = default;

    virtual void start() = 0;
    virtual void stop()  = 0;

    virtual CameraState state() const      = 0;
    virtual std::string last_error() const = 0;

    // format() is valid once ready_for_info() is true.
    virtual bool        ready_for_info() const = 0;
    virtual VideoFormat format() const         = 0;

    // Pops the oldest queued frame. Returns false when none is available.
    virtual bool pop_frame(VideoFrame* out) = 0;
};

} // namespace bbl_lan
