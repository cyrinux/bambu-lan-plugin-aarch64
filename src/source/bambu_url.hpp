// Parses the LAN camera URLs the slicers hand to libBambuSource:
//   bambu:///local/<ip>.?port=6000&user=..&passwd=..&device=..         A1/P1 (JPEG)
//   bambu:///rtsps___<user>:<code>@<ip>/streaming/live/1?proto=rtsps   X1 and newer (H.264)
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>

namespace bbl_lan {

struct LocalCameraUrl {
    std::string host;
    uint16_t    port = 6000;
    std::string user = "bblp";
    std::string password;
    std::string device;
    std::map<std::string, std::string> params;
};

struct RtspCameraUrl {
    bool        secure = true; // rtsps (TLS); plain rtsp otherwise
    std::string host;
    uint16_t    port = 322;
    std::string user = "bblp";
    std::string password;
    std::string path = "/streaming/live/1";

    // rtsp[s]://host:port/path, without credentials.
    std::string request_url() const;
};

std::string percent_decode(const std::string& in);

// Returns nullopt for URLs that are not "bambu:///local/..." (cloud tutk/agora,
// rtsp variants, ...).
std::optional<LocalCameraUrl> parse_local_camera_url(const std::string& url);

// Returns nullopt for URLs that are not "bambu:///rtsp[s]___...".
std::optional<RtspCameraUrl> parse_rtsp_camera_url(const std::string& url);

} // namespace bbl_lan
