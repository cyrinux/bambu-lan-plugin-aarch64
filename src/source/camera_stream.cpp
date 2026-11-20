#include "camera_stream.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <exception>

namespace bbl_lan {
namespace {

constexpr size_t   kMaxFrameBytes   = 4 * 1024 * 1024;
constexpr size_t   kMaxQueuedFrames = 3;
constexpr int      kConnectTimeout  = 8000;
constexpr int      kFrameTimeoutMs  = 15000; // A1 sends a frame every 1-2 s
constexpr int      kMaxAttempts     = 3;

uint32_t read_u32le(const uint8_t* p)
{
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

void put_u32le(std::vector<uint8_t>* out, uint32_t v)
{
    for (int i = 0; i < 4; ++i)
        out->push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xFF));
}

uint64_t now_ms()
{
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

} // namespace

JpegSize parse_jpeg_size(const uint8_t* data, size_t len)
{
    if (len < 4 || data[0] != 0xFF || data[1] != 0xD8)
        return {};
    size_t pos = 2;
    while (pos + 4 <= len) {
        if (data[pos] != 0xFF) {
            ++pos;
            continue;
        }
        const uint8_t marker = data[pos + 1];
        if (marker == 0xFF) {
            ++pos;
            continue;
        }
        if (marker == 0xD8 || marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7)) {
            pos += 2;
            continue;
        }
        const size_t seg_len = (static_cast<size_t>(data[pos + 2]) << 8) | data[pos + 3];
        const bool   is_sof  = marker >= 0xC0 && marker <= 0xCF && marker != 0xC4 && marker != 0xC8 && marker != 0xCC;
        if (is_sof && pos + 9 <= len) {
            JpegSize s;
            s.height = (data[pos + 5] << 8) | data[pos + 6];
            s.width  = (data[pos + 7] << 8) | data[pos + 8];
            return s;
        }
        if (marker == 0xDA || seg_len < 2)
            break; // start of scan: no SOF found before entropy data
        pos += 2 + seg_len;
    }
    return {};
}

std::vector<uint8_t> build_camera_auth(const std::string& user, const std::string& password)
{
    std::vector<uint8_t> auth;
    auth.reserve(80);
    put_u32le(&auth, 0x40);
    put_u32le(&auth, 0x3000);
    put_u32le(&auth, 0);
    put_u32le(&auth, 0);
    for (const std::string* field : {&user, &password}) {
        std::vector<uint8_t> padded(32, 0);
        std::memcpy(padded.data(), field->data(), std::min<size_t>(field->size(), 32));
        auth.insert(auth.end(), padded.begin(), padded.end());
    }
    return auth;
}

CameraStream::CameraStream(LocalCameraUrl url, LogFn log) : m_url(std::move(url)), m_log(std::move(log)) {}

CameraStream::~CameraStream() { stop(); }

void CameraStream::start()
{
    if (m_thread.joinable())
        return;
    m_stop  = false;
    m_state = CameraState::Connecting;
    m_thread = std::thread([this] { run(); });
}

void CameraStream::stop()
{
    m_stop = true;
    m_sock.cancel(); // aborts connect/reads within ~100 ms
    if (m_thread.joinable())
        m_thread.join();
    m_sock.close();
    if (m_state != CameraState::Failed)
        m_state = CameraState::Closed;
}

std::string CameraStream::last_error() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_error;
}

bool CameraStream::ready_for_info() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_size.width > 0 && m_fps > 0;
}

JpegSize CameraStream::size() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_size;
}

uint64_t CameraStream::frames_received() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_frames;
}

int CameraStream::frame_rate() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_fps;
}

bool CameraStream::pop_frame(CameraFrame* out)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_queue.empty())
        return false;
    *out = std::move(m_queue.front());
    m_queue.pop_front();
    return true;
}

void CameraStream::set_error(const std::string& msg)
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_error = msg;
    }
    if (m_log)
        m_log(1, msg);
}

void CameraStream::run()
{
    try {
        // Consecutive failures without any frame; reset by every frame received.
        int failures = 0;
        while (!m_stop && failures < kMaxAttempts) {
            const uint64_t frames_before = frames_received();
            if (stream_once() || m_stop)
                break;
            failures = frames_received() > frames_before ? 1 : failures + 1;
            for (int i = 0; i < 10 && !m_stop; ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    } catch (const std::exception& e) {
        set_error(std::string("camera thread aborted: ") + e.what());
    }
    if (!m_stop)
        m_state = CameraState::Failed;
}

// Returns true when the stream ended in a way that must not be retried
// (explicit stop, or authentication refused).
bool CameraStream::stream_once()
{
    const std::string err = m_sock.connect(m_url.host, m_url.port, kConnectTimeout);
    if (!err.empty()) {
        set_error("camera connect " + m_url.host + ":" + std::to_string(m_url.port) + " failed: " + err);
        return false;
    }
    const std::vector<uint8_t> auth = build_camera_auth(m_url.user, m_url.password);
    if (m_sock.write_all(auth.data(), auth.size()) != IoResult::Ok) {
        set_error("camera auth write failed");
        m_sock.close();
        return false;
    }
    if (m_log)
        m_log(0, "camera connected to " + m_url.host);

    bool got_any = false;
    while (!m_stop) {
        uint8_t        header[16];
        const IoResult r = m_sock.read_exact(header, sizeof(header), kFrameTimeoutMs);
        if (r != IoResult::Ok) {
            m_sock.close();
            if (m_stop)
                return true;
            // A wrong access code makes the printer close the socket before
            // sending anything: report it and stop retrying. A timeout or a
            // drop after frames is retried.
            const bool auth_refused = !got_any && r == IoResult::Closed;
            set_error(auth_refused ? "camera closed before first frame (wrong access code or camera disabled?)"
                                   : got_any ? "camera stream ended" : "camera sent no frame in time");
            return auth_refused;
        }
        const uint32_t size = read_u32le(header);
        if (size == 0 || size > kMaxFrameBytes) {
            set_error("camera frame with invalid size " + std::to_string(size));
            m_sock.close();
            return false;
        }
        CameraFrame frame;
        frame.jpeg.resize(size);
        if (m_sock.read_exact(frame.jpeg.data(), size, kFrameTimeoutMs) != IoResult::Ok) {
            set_error("camera frame truncated");
            m_sock.close();
            return false;
        }
        frame.timestamp_ms = now_ms();
        got_any            = true;
        m_state            = CameraState::Streaming;

        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_size.width == 0)
            m_size = parse_jpeg_size(frame.jpeg.data(), frame.jpeg.size());
        if (m_frames == 0)
            m_first_ts = frame.timestamp_ms;
        ++m_frames;
        if (m_fps == 0 && m_frames >= 2) {
            const double interval = static_cast<double>(frame.timestamp_ms - m_first_ts) / static_cast<double>(m_frames - 1);
            m_fps = std::max(1, static_cast<int>(std::ceil(1000.0 / std::max(interval, 1.0))));
            m_fps = std::min(m_fps, 30);
        }
        if (m_queue.size() >= kMaxQueuedFrames)
            m_queue.pop_front(); // keep latency low: drop the oldest frame
        m_queue.push_back(std::move(frame));
    }
    m_sock.close();
    return true;
}

} // namespace bbl_lan
