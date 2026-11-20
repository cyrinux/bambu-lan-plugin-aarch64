// A1 / P1 LAN camera: TLS on port 6000, 80-byte auth packet, then a stream of
// [16-byte header][JPEG] frames. Runs a background reader and keeps a small
// queue of complete JPEG frames for Bambu_ReadSample.
#pragma once

#include "../common/tls_socket.hpp"
#include "bambu_url.hpp"
#include "video_source.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace bbl_lan {

struct JpegSize {
    int width  = 0;
    int height = 0;
};

// Reads width/height from the first SOFn marker; zeros if not found.
JpegSize parse_jpeg_size(const uint8_t* data, size_t len);

// Builds the 80-byte authentication packet.
std::vector<uint8_t> build_camera_auth(const std::string& user, const std::string& password);

class CameraStream final : public VideoSource {
public:
    using LogFn = std::function<void(int level, const std::string& msg)>;

    CameraStream(LocalCameraUrl url, LogFn log);
    ~CameraStream() override;
    CameraStream(const CameraStream&)            = delete;
    CameraStream& operator=(const CameraStream&) = delete;

    void start() override;
    void stop() override;

    CameraState state() const override { return m_state.load(); }
    std::string last_error() const override;

    // Size from the first frame and measured frame rate.
    bool        ready_for_info() const override;
    VideoFormat format() const override;
    uint64_t    frames_received() const;

    bool pop_frame(VideoFrame* out) override;

private:
    void run();
    bool stream_once();
    void set_error(const std::string& msg);

    LocalCameraUrl          m_url;
    LogFn                   m_log;
    TlsSocket               m_sock;
    std::thread             m_thread;
    std::atomic<bool>       m_stop{false};
    std::atomic<CameraState> m_state{CameraState::Idle};

    mutable std::mutex      m_mutex;
    std::deque<VideoFrame>  m_queue;
    std::string             m_error;
    JpegSize                m_size;
    uint64_t                m_first_ts = 0;
    uint64_t                m_frames   = 0;
    int                     m_fps      = 0;
};

} // namespace bbl_lan
