// X1 / H2 / P2 / X2 LAN camera: RTSP over TLS on port 322 (rtsps), H.264 in
// RTP interleaved on the RTSP connection (RTP/AVP/TCP). A background thread
// runs DESCRIBE/SETUP/PLAY and queues Annex-B access units.
#pragma once

#include "../common/tls_socket.hpp"
#include "bambu_url.hpp"
#include "h264.hpp"
#include "rtsp.hpp"
#include "video_source.hpp"

#include <atomic>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace bbl_lan {

class RtspStream final : public VideoSource {
public:
    using LogFn = std::function<void(int level, const std::string& msg)>;

    RtspStream(RtspCameraUrl url, LogFn log);
    ~RtspStream() override;
    RtspStream(const RtspStream&)            = delete;
    RtspStream& operator=(const RtspStream&) = delete;

    void start() override;
    void stop() override;

    CameraState state() const override { return m_state.load(); }
    std::string last_error() const override;

    bool        ready_for_info() const override;
    VideoFormat format() const override;

    bool pop_frame(VideoFrame* out) override;

private:
    // Why a session ended: Retry (network), Fatal (auth/protocol), Stopped.
    enum class End { Retry, Fatal, Stopped };

    void     run();
    End      session();
    void     begin_session(int sdp_fps);
    End      receive_loop(H264Depacketizer& depack, int keepalive_s, const std::string& base_url);
    void     on_access_units(std::vector<AccessUnit>& units, const H264Depacketizer& depack);
    uint64_t total_frames() const;

    // RTSP request/response on the shared connection. Retries once with
    // credentials after a 401. Returns nullopt on I/O errors.
    std::optional<RtspResponse> request(const std::string& method, const std::string& url,
                                        std::vector<std::string> headers);
    bool                        send_request(const std::string& method, const std::string& url,
                                             std::vector<std::string> headers);
    std::optional<RtspResponse> read_response();
    // Receive buffer: unread bytes are m_buf[m_pos..]. fill() reads until at
    // least `need` unread bytes are available.
    IoResult       fill(size_t need, int timeout_ms);
    void           consume(size_t n);
    void           reset_buffer();
    size_t         available() const { return m_buf.size() - m_pos; }
    const uint8_t* data() const { return m_buf.data() + m_pos; }
    IoResult       read_interleaved(uint8_t* channel, const uint8_t** payload, size_t* len);

    void set_error(const std::string& msg);

    RtspCameraUrl           m_url;
    LogFn                   m_log;
    TlsSocket               m_sock;
    std::thread             m_thread;
    std::atomic<bool>       m_stop{false};
    std::atomic<CameraState> m_state{CameraState::Idle};

    // Connection state (reader thread only).
    std::vector<uint8_t>              m_buf;
    size_t                            m_pos  = 0;
    int                               m_cseq = 0;
    unsigned                          m_nc   = 0; // Digest nonce count
    std::optional<AuthChallenge>      m_auth;
    std::string                       m_session;
    std::vector<std::vector<uint8_t>> m_early; // RTP received before the PLAY answer

    mutable std::mutex     m_mutex;
    std::deque<VideoFrame> m_queue;
    std::string            m_error;
    VideoFormat            m_format{VideoCodec::H264, 0, 0, 0};
    int                    m_sdp_fps        = 0;
    uint64_t               m_total_frames   = 0; // all sessions (retry accounting)
    uint64_t               m_session_frames = 0;
    uint64_t               m_session_ts     = 0; // RTP ticks since the session's first AU
    uint32_t               m_last_rtp_ts    = 0;
    // Sample timestamps stay monotonic across reconnects.
    uint64_t               m_session_start_ms  = 0;
    uint64_t               m_last_timestamp_ms = 0;
    bool                   m_drop_to_key       = false;
};

} // namespace bbl_lan
