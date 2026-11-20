#include "rtsp_stream.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <exception>

namespace bbl_lan {
namespace {

constexpr int     kConnectTimeout         = 8000;
constexpr int     kIoTimeoutMs            = 10000;
constexpr int     kMaxAttempts            = 3;
constexpr size_t  kMaxHeadBytes           = 64 * 1024;
constexpr size_t  kMaxQueuedFrames        = 90; // ~3 s at 30 fps
constexpr size_t  kMaxEarlyPackets        = 2048;
constexpr int     kFpsSampleFrames        = 5;
constexpr int     kRtpClock               = 90000;
constexpr int64_t kMaxTsStep              = 5 * kRtpClock; // larger jumps are discontinuities
constexpr int     kDefaultSessionTimeoutS = 60;

using Clock = std::chrono::steady_clock;

std::string with_auth_hint(int status)
{
    if (status == 401)
        return " (access code refused)";
    if (status == 404)
        return " (no such stream: is LAN live view enabled on the printer?)";
    return {};
}

// "12345678;timeout=60" -> id, timeout seconds.
std::pair<std::string, int> parse_session(const std::string& value)
{
    const auto  semi    = value.find(';');
    std::string id      = value.substr(0, semi);
    int         timeout = kDefaultSessionTimeoutS;
    const auto  t       = value.find("timeout=");
    if (t != std::string::npos)
        timeout = std::max(10, std::atoi(value.c_str() + t + 8));
    while (!id.empty() && id.back() == ' ')
        id.pop_back();
    return {id, timeout};
}

} // namespace

RtspStream::RtspStream(RtspCameraUrl url, LogFn log) : m_url(std::move(url)), m_log(std::move(log)) {}

RtspStream::~RtspStream() { stop(); }

void RtspStream::start()
{
    if (m_thread.joinable())
        return;
    m_stop   = false;
    m_state  = CameraState::Connecting;
    m_thread = std::thread([this] { run(); });
}

void RtspStream::stop()
{
    m_stop = true;
    m_sock.cancel();
    if (m_thread.joinable())
        m_thread.join();
    m_sock.close();
    if (m_state != CameraState::Failed)
        m_state = CameraState::Closed;
}

std::string RtspStream::last_error() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_error;
}

bool RtspStream::ready_for_info() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_format.width > 0 && m_format.height > 0 && m_format.frame_rate > 0;
}

VideoFormat RtspStream::format() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_format;
}

bool RtspStream::pop_frame(VideoFrame* out)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_queue.empty())
        return false;
    *out = std::move(m_queue.front());
    m_queue.pop_front();
    return true;
}

void RtspStream::set_error(const std::string& msg)
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_error = msg;
    }
    if (m_log)
        m_log(1, msg);
}

uint64_t RtspStream::total_frames() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_total_frames;
}

void RtspStream::run()
{
    try {
        int failures = 0; // consecutive sessions without any frame
        while (!m_stop && failures < kMaxAttempts) {
            const uint64_t before = total_frames();
            const End      end    = session();
            m_sock.close();
            if (end != End::Retry || m_stop)
                break;
            failures = total_frames() > before ? 1 : failures + 1;
            for (int i = 0; i < 10 && !m_stop; ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    } catch (const std::exception& e) {
        set_error(std::string("camera thread aborted: ") + e.what());
    }
    if (!m_stop)
        m_state = CameraState::Failed;
}

// ---- receive buffer: bytes [m_pos, m_buf.size()) are unread ----

IoResult RtspStream::fill(size_t need, int timeout_ms)
{
    while (available() < need) {
        if (m_pos > 0 && m_pos * 2 >= m_buf.size()) { // compact now and then, not per packet
            m_buf.erase(m_buf.begin(), m_buf.begin() + static_cast<long>(m_pos));
            m_pos = 0;
        }
        uint8_t        chunk[16384];
        size_t         got = 0;
        const IoResult r   = m_sock.read_some(chunk, sizeof(chunk), &got, timeout_ms);
        if (r != IoResult::Ok)
            return r;
        m_buf.insert(m_buf.end(), chunk, chunk + got);
    }
    return IoResult::Ok;
}

void RtspStream::consume(size_t n) { m_pos = std::min(m_buf.size(), m_pos + n); }

void RtspStream::reset_buffer()
{
    m_buf.clear();
    m_pos = 0;
}

// Reads one "$" <channel> <u16 len> <payload> frame; *payload points into m_buf
// until the next fill().
IoResult RtspStream::read_interleaved(uint8_t* channel, const uint8_t** payload, size_t* len)
{
    IoResult r = fill(4, kIoTimeoutMs);
    if (r != IoResult::Ok)
        return r;
    *channel = data()[1];
    *len     = (size_t(data()[2]) << 8) | data()[3];
    if ((r = fill(4 + *len, kIoTimeoutMs)) != IoResult::Ok)
        return r;
    *payload = data() + 4;
    consume(4 + *len);
    return IoResult::Ok;
}

bool RtspStream::send_request(const std::string& method, const std::string& url, std::vector<std::string> headers)
{
    if (m_auth) {
        const std::string auth = authorization_value(*m_auth, m_url.user, m_url.password, method, url, ++m_nc);
        if (!auth.empty())
            headers.push_back("Authorization: " + auth);
    }
    if (!m_session.empty())
        headers.push_back("Session: " + m_session);
    const std::string req = build_rtsp_request(method, url, ++m_cseq, headers);
    return m_sock.write_all(req.data(), req.size()) == IoResult::Ok;
}

std::optional<RtspResponse> RtspStream::read_response()
{
    static const char kEnd[] = "\r\n\r\n";
    for (;;) {
        if (fill(1, kIoTimeoutMs) != IoResult::Ok)
            return std::nullopt;
        if (data()[0] == '$') {
            // RTP may arrive before the PLAY response: keep it, it usually
            // starts with the first keyframe.
            uint8_t        channel = 0;
            const uint8_t* payload = nullptr;
            size_t         len     = 0;
            if (read_interleaved(&channel, &payload, &len) != IoResult::Ok)
                return std::nullopt;
            if (channel == 0 && m_early.size() < kMaxEarlyPackets)
                m_early.emplace_back(payload, payload + len);
            continue;
        }
        const uint8_t* begin = data();
        const uint8_t* end   = begin + available();
        const uint8_t* it    = std::search(begin, end, kEnd, kEnd + 4);
        if (it == end) {
            if (available() > kMaxHeadBytes || fill(available() + 1, kIoTimeoutMs) != IoResult::Ok)
                return std::nullopt;
            continue;
        }
        const size_t head_len = static_cast<size_t>(it - begin) + 4;
        auto         resp     = parse_rtsp_head(std::string(begin, begin + head_len));
        if (!resp)
            return std::nullopt;
        const size_t body = resp->content_length();
        if (body > kMaxHeadBytes || fill(head_len + body, kIoTimeoutMs) != IoResult::Ok)
            return std::nullopt;
        resp->body.assign(data() + head_len, data() + head_len + body);
        consume(head_len + body);
        return resp;
    }
}

std::optional<RtspResponse> RtspStream::request(const std::string& method, const std::string& url,
                                                std::vector<std::string> headers)
{
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (!send_request(method, url, headers))
            return std::nullopt;
        auto resp = read_response();
        if (!resp || resp->status != 401 || attempt == 1)
            return resp;
        auto challenge = parse_www_authenticate(resp->header("www-authenticate"));
        if (!challenge)
            return resp;
        // A 401 after credentials were sent means a wrong code, unless the
        // server only says the nonce went stale.
        const bool had_auth = m_auth.has_value();
        m_auth              = std::move(challenge);
        m_nc                = 0;
        if (had_auth && resp->header("www-authenticate").find("stale=true") == std::string::npos)
            return resp;
    }
    return std::nullopt;
}

RtspStream::End RtspStream::session()
{
    if (!m_url.secure) {
        set_error("plain rtsp:// cameras are not supported by this plugin (only rtsps)");
        return End::Fatal;
    }
    const std::string err = m_sock.connect(m_url.host, m_url.port, kConnectTimeout);
    if (!err.empty()) {
        set_error("camera connect " + m_url.host + ":" + std::to_string(m_url.port) + " failed: " + err);
        return m_stop ? End::Stopped : End::Retry;
    }
    reset_buffer();
    m_early.clear();
    m_cseq = 0;
    m_nc   = 0;
    m_auth.reset();
    m_session.clear();

    const std::string url  = m_url.request_url();
    const auto        desc = request("DESCRIBE", url, {"Accept: application/sdp"});
    if (!desc) {
        set_error("camera: no answer to RTSP DESCRIBE");
        return m_stop ? End::Stopped : End::Retry;
    }
    if (desc->status != 200) {
        set_error("camera: RTSP DESCRIBE failed with " + std::to_string(desc->status) + with_auth_hint(desc->status));
        return End::Fatal;
    }
    const auto track = parse_sdp_video(desc->body);
    if (!track || track->encoding != "H264") {
        set_error("camera: no H.264 video track in the stream description");
        return End::Fatal;
    }
    std::string base = desc->header("content-base");
    if (base.empty())
        base = desc->header("content-location");
    if (base.empty())
        base = url;

    const auto setup = request("SETUP", resolve_control(base, track->control),
                               {"Transport: RTP/AVP/TCP;unicast;interleaved=0-1"});
    if (!setup || setup->status != 200 || setup->header("session").empty()) {
        set_error("camera: RTSP SETUP failed" + (setup ? " with " + std::to_string(setup->status) : std::string{}));
        return setup ? End::Fatal : (m_stop ? End::Stopped : End::Retry);
    }
    const auto [session_id, timeout_s] = parse_session(setup->header("session"));
    m_session                          = session_id;

    const std::string play_url = resolve_control(base, track->session_control);
    const auto        play     = request("PLAY", play_url, {"Range: npt=0.000-"});
    if (!play || play->status != 200) {
        set_error("camera: RTSP PLAY failed" + (play ? " with " + std::to_string(play->status) : std::string{}));
        return play ? End::Fatal : (m_stop ? End::Stopped : End::Retry);
    }
    if (m_log)
        m_log(0, "camera: RTSP playing " + url + " (H.264, pt " + std::to_string(track->payload_type) + ")");

    begin_session(track->frame_rate);
    H264Depacketizer depack(track->payload_type);
    depack.set_parameter_sets(track->parameter_sets);
    std::vector<AccessUnit> units;
    for (const auto& pkt : m_early)
        depack.push_rtp(pkt.data(), pkt.size(), &units);
    m_early.clear();
    on_access_units(units, depack);

    const End end = receive_loop(depack, timeout_s, play_url);
    if (end == End::Stopped)
        send_request("TEARDOWN", play_url, {}); // best effort, no answer awaited
    return end;
}

void RtspStream::begin_session(int sdp_fps)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    // Frames of the previous session cannot be decoded with the new one:
    // start over from its first keyframe, continuing the old timeline.
    m_queue.clear();
    m_drop_to_key       = false;
    m_sdp_fps           = sdp_fps;
    m_format.frame_rate = sdp_fps;
    m_session_frames    = 0;
    m_session_ts        = 0;
    m_session_start_ms  = m_total_frames == 0 ? 0 : m_last_timestamp_ms + 1;
}

RtspStream::End RtspStream::receive_loop(H264Depacketizer& depack, int keepalive_s, const std::string& base_url)
{
    auto                    keepalive = Clock::now();
    std::vector<AccessUnit> units;
    size_t                  garbage = 0;
    uint64_t                packets = 0;
    const auto              fail    = [&](IoResult r) {
        if (m_stop)
            return End::Stopped;
        const VideoFormat f = format();
        set_error(std::string(r == IoResult::Timeout ? "camera: no video data in time" : "camera stream ended") + " (" +
                  std::to_string(packets) + " RTP packets, " + std::to_string(depack.lost_packets()) + " lost, " +
                  std::to_string(f.width) + "x" + std::to_string(f.height) + " @" + std::to_string(f.frame_rate) + "fps)");
        return End::Retry;
    };
    while (!m_stop) {
        if (Clock::now() - keepalive > std::chrono::seconds(keepalive_s / 2)) {
            send_request("OPTIONS", base_url, {}); // keeps the session alive; reply skipped below
            keepalive = Clock::now();
        }
        const IoResult r = fill(1, kIoTimeoutMs);
        if (r != IoResult::Ok)
            return fail(r);
        if (data()[0] == '$') {
            uint8_t        channel = 0;
            const uint8_t* payload = nullptr;
            size_t         len     = 0;
            const IoResult rr      = read_interleaved(&channel, &payload, &len);
            if (rr != IoResult::Ok)
                return fail(rr);
            if (channel == 0) {
                ++packets;
                depack.push_rtp(payload, len, &units);
                if (!units.empty())
                    on_access_units(units, depack);
            }
            garbage = 0;
        } else if (data()[0] == 'R') {
            const auto resp = read_response(); // keep-alive answer
            if (!resp)
                return fail(IoResult::Error);
            if (resp->status == 454) {
                set_error("camera: RTSP session expired");
                return End::Retry;
            }
        } else {
            // Resynchronise on the next '$' or 'R'.
            const uint8_t* begin = data();
            const uint8_t* end   = begin + available();
            const uint8_t* next  = std::find_if(begin + 1, end, [](uint8_t c) { return c == '$' || c == 'R'; });
            garbage += static_cast<size_t>(next - begin);
            consume(static_cast<size_t>(next - begin));
            if (garbage > kMaxHeadBytes) {
                set_error("camera: unexpected data on the RTSP connection");
                return End::Retry;
            }
        }
    }
    return End::Stopped;
}

void RtspStream::on_access_units(std::vector<AccessUnit>& units, const H264Depacketizer& depack)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    for (AccessUnit& au : units) {
        if (m_session_frames > 0) {
            // Signed step: a timestamp going back (or jumping) must not wrap.
            const int64_t step = static_cast<int32_t>(au.rtp_timestamp - m_last_rtp_ts);
            if (step > 0 && step <= kMaxTsStep)
                m_session_ts += static_cast<uint64_t>(step);
            else if (step != 0 && m_format.frame_rate > 0)
                m_session_ts += kRtpClock / m_format.frame_rate;
        }
        m_last_rtp_ts = au.rtp_timestamp;
        ++m_session_frames;
        ++m_total_frames;

        if (const auto size = depack.size()) {
            m_format.width  = size->width;
            m_format.height = size->height;
        }
        if (m_sdp_fps == 0 && m_format.frame_rate == 0 && m_session_frames >= kFpsSampleFrames && m_session_ts > 0) {
            const double fps    = double(kRtpClock) * double(m_session_frames - 1) / double(m_session_ts);
            m_format.frame_rate = std::clamp(static_cast<int>(std::lround(fps)), 1, 60);
        }
        m_state = CameraState::Streaming;

        if (m_drop_to_key && !au.keyframe)
            continue;
        m_drop_to_key = false;
        if (m_queue.size() >= kMaxQueuedFrames) {
            // The reader fell behind: inter frames depend on each other, so
            // restart cleanly from the next keyframe.
            m_queue.clear();
            if (!au.keyframe) {
                m_drop_to_key = true;
                continue;
            }
        }
        VideoFrame frame;
        frame.data          = std::move(au.annexb);
        frame.timestamp_ms  = m_session_start_ms + m_session_ts * 1000 / kRtpClock;
        frame.keyframe      = au.keyframe;
        m_last_timestamp_ms = frame.timestamp_ms;
        m_queue.push_back(std::move(frame));
    }
    units.clear();
}

} // namespace bbl_lan
