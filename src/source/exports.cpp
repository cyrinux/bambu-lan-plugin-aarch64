// libBambuSource.so replacement: Bambu_* C API backed by the LAN camera
// streams: A1/P1 JPEG (port 6000) and X1/H2/P2/X2 RTSPS H.264 (port 322).
// Cloud (tutk/agora) and the printer file browser are not supported and fail
// cleanly so the slicer shows its normal error state.
// BAMBU_EXPORT is empty on Linux and we build with -fvisibility=hidden:
// give the declared API default visibility explicitly.
#pragma GCC visibility push(default)
#include "BambuTunnel.h"
#pragma GCC visibility pop

#include "../common/log.hpp"
#include "camera_stream.hpp"
#include "rtsp_stream.hpp"

#include <cstdlib>
#include <cstring>
#include <exception>
#include <memory>
#include <optional>
#include <string>

namespace {

using bbl_lan::CameraState;
using bbl_lan::CameraStream;
using bbl_lan::LocalCameraUrl;
using bbl_lan::RtspCameraUrl;
using bbl_lan::RtspStream;
using bbl_lan::VideoCodec;
using bbl_lan::VideoFrame;
using bbl_lan::VideoSource;

constexpr int kErrorGeneric   = -1;
constexpr int kCtrlFileBrowse = 0x3001;

thread_local std::string t_last_error;

struct Tunnel {
    std::string                   url;
    std::optional<LocalCameraUrl> local;
    std::optional<RtspCameraUrl>  rtsp;
    Logger                        logger     = nullptr;
    void*                         logger_ctx = nullptr;
    std::unique_ptr<VideoSource>  camera;
    VideoFrame                    current; // buffer of the last returned sample
    uint64_t                      first_ts = 0;
    int                           samples  = 0;
};

Tunnel* as_tunnel(Bambu_Tunnel t) { return static_cast<Tunnel*>(t); }

// Messages handed to the slicer's logger are heap allocated: the file
// browser frees them with Bambu_FreeLogMsg.
void tunnel_log(Tunnel* t, int level, const std::string& msg)
{
    LOG_I("source: ", msg);
    if (t == nullptr || t->logger == nullptr)
        return;
    t->logger(t->logger_ctx, level, strdup(msg.c_str()));
}

int fail(Tunnel* t, const std::string& msg)
{
    t_last_error = msg;
    tunnel_log(t, 1, msg + " [" + std::to_string(kErrorGeneric) + "]");
    return kErrorGeneric;
}

std::string redact_url(const std::string& url)
{
    // rtsp form: "bambu:///rtsps___<user>:<code>@<ip>/..."
    const auto at = url.rfind('@');
    const auto us = url.find("___");
    if (at != std::string::npos && us != std::string::npos && us < at) {
        const auto colon = url.find(':', us);
        if (colon != std::string::npos && colon < at)
            return url.substr(0, colon + 1) + "***" + url.substr(at);
    }
    const auto p = url.find("passwd=");
    if (p == std::string::npos)
        return url;
    const auto e = url.find('&', p);
    return url.substr(0, p + 7) + "***" + (e == std::string::npos ? std::string{} : url.substr(e));
}

} // namespace

extern "C" {

int Bambu_Init() { return 0; }

void Bambu_Deinit() {}

int Bambu_Create(Bambu_Tunnel* tunnel, char const* path)
{
    if (tunnel == nullptr || path == nullptr)
        return kErrorGeneric;
    auto t   = std::make_unique<Tunnel>();
    t->url   = path;
    t->local = bbl_lan::parse_local_camera_url(t->url);
    if (!t->local)
        t->rtsp = bbl_lan::parse_rtsp_camera_url(t->url);
    LOG_I("source: create ", redact_url(t->url), t->local || t->rtsp ? "" : " (unsupported, LAN-only build)");
    *tunnel = t.release();
    return 0;
}

void Bambu_SetLogger(Bambu_Tunnel tunnel, Logger logger, void* context)
{
    if (Tunnel* t = as_tunnel(tunnel)) {
        t->logger     = logger;
        t->logger_ctx = context;
    }
}

void Bambu_SetStreamInfoCallback(Bambu_Tunnel, StreamInfoCallback, void*) {}

void Bambu_SetTrackReporter(Bambu_Tunnel, TrackReporter, void*) {}

int Bambu_Open(Bambu_Tunnel tunnel)
{
    Tunnel* t = as_tunnel(tunnel);
    if (t == nullptr)
        return kErrorGeneric;
    if (!t->local && !t->rtsp)
        return fail(t, "only LAN camera URLs (bambu:///local/..., bambu:///rtsps___...) are supported by this plugin");
    if (t->rtsp && !t->rtsp->secure)
        return fail(t, "plain rtsp:// cameras are not supported by this plugin (only rtsps)");
    if ((t->local ? t->local->password : t->rtsp->password).empty())
        return fail(t, "missing access code in camera URL");
    try {
        auto log = [t](int level, const std::string& msg) { tunnel_log(t, level, msg); };
        if (t->local)
            t->camera = std::make_unique<CameraStream>(*t->local, log);
        else
            t->camera = std::make_unique<RtspStream>(*t->rtsp, log);
        t->camera->start();
    } catch (const std::exception& e) {
        return fail(t, std::string("cannot start camera: ") + e.what());
    }
    return 0;
}

int Bambu_StartStream(Bambu_Tunnel tunnel, bool video)
{
    Tunnel* t = as_tunnel(tunnel);
    if (t == nullptr || !t->camera)
        return kErrorGeneric;
    if (!video)
        return fail(t, "printer file browser is not supported by this LAN plugin");
    if (t->camera->state() == CameraState::Failed)
        return fail(t, t->camera->last_error());
    return t->camera->ready_for_info() ? Bambu_success : Bambu_would_block;
}

int Bambu_StartStreamEx(Bambu_Tunnel tunnel, int type)
{
    if (type == kCtrlFileBrowse)
        return fail(as_tunnel(tunnel), "printer file browser is not supported by this LAN plugin");
    return Bambu_StartStream(tunnel, true);
}

int Bambu_GetStreamCount(Bambu_Tunnel tunnel)
{
    Tunnel* t = as_tunnel(tunnel);
    return (t != nullptr && t->camera && t->camera->ready_for_info()) ? 1 : 0;
}

int Bambu_GetStreamInfo(Bambu_Tunnel tunnel, int index, Bambu_StreamInfo* info)
{
    Tunnel* t = as_tunnel(tunnel);
    if (t == nullptr || !t->camera || info == nullptr || index != 0 || !t->camera->ready_for_info())
        return kErrorGeneric;
    std::memset(info, 0, sizeof(*info));
    const bbl_lan::VideoFormat f  = t->camera->format();
    const bool                 h264 = f.codec == VideoCodec::H264;
    info->type                    = VIDE;
    info->sub_type                = h264 ? AVC1 : MJPG;
    info->format.video.width      = f.width;
    info->format.video.height     = f.height;
    info->format.video.frame_rate = f.frame_rate;
    // H.264: Annex-B access units with SPS/PPS before every IDR, so no
    // codec extradata (format_buffer) is needed.
    info->format_type             = h264 ? video_avc_byte_stream : video_jpeg;
    info->format_size             = 0;
    info->max_frame_size          = 8 * 1024 * 1024;
    info->format_buffer           = nullptr;
    tunnel_log(t, 0, std::string("stream info ") + (h264 ? "H.264 " : "JPEG ") + std::to_string(f.width) + "x" +
                         std::to_string(f.height) + " @" + std::to_string(f.frame_rate) + "fps");
    return Bambu_success;
}

unsigned long Bambu_GetDuration(Bambu_Tunnel) { return 0; }

int Bambu_Seek(Bambu_Tunnel, unsigned long) { return kErrorGeneric; }

int Bambu_ReadSample(Bambu_Tunnel tunnel, Bambu_Sample* sample)
{
    Tunnel* t = as_tunnel(tunnel);
    if (t == nullptr || !t->camera || sample == nullptr)
        return kErrorGeneric;
    if (!t->camera->pop_frame(&t->current)) {
        const CameraState s = t->camera->state();
        if (s == CameraState::Failed)
            return fail(t, t->camera->last_error());
        return s == CameraState::Closed ? Bambu_stream_end : Bambu_would_block;
    }
    if (t->samples++ == 0)
        t->first_ts = t->current.timestamp_ms;
    sample->itrack      = 0;
    sample->size        = static_cast<int>(t->current.data.size());
    sample->flags       = t->current.keyframe ? f_sync : 0;
    sample->buffer      = t->current.data.data();
    sample->decode_time = (t->current.timestamp_ms - t->first_ts) * 10000ULL; // 100 ns units
    return Bambu_success;
}

int Bambu_SendMessage(Bambu_Tunnel tunnel, int, char const*, int)
{
    return fail(as_tunnel(tunnel), "control messages are not supported by this LAN plugin");
}

int Bambu_RecvMessage(Bambu_Tunnel, int*, char*, int*) { return kErrorGeneric; }

void Bambu_Close(Bambu_Tunnel tunnel)
{
    Tunnel* t = as_tunnel(tunnel);
    if (t != nullptr && t->camera)
        t->camera->stop();
}

void Bambu_Destroy(Bambu_Tunnel tunnel)
{
    Tunnel* t = as_tunnel(tunnel);
    if (t != nullptr && t->camera)
        t->camera->stop();
    delete t;
}

void Bambu_GetSessionStat(Bambu_Tunnel tunnel, Bambu_SessionStat* stat)
{
    if (stat == nullptr)
        return;
    std::memset(stat, 0, sizeof(*stat));
    Tunnel* t = as_tunnel(tunnel);
    if (t != nullptr && t->camera)
        stat->avg_fps = static_cast<float>(t->camera->format().frame_rate);
}

char const* Bambu_GetLastErrorMsg() { return t_last_error.c_str(); }

void Bambu_FreeLogMsg(tchar const* msg) { std::free(const_cast<tchar*>(msg)); }

} // extern "C"
