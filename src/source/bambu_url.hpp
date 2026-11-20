// Parses the "bambu:///local/<ip>.?port=6000&user=..&passwd=..&device=.." URLs
// Bambu Studio hands to libBambuSource for LAN printers.
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

std::string percent_decode(const std::string& in);

// Returns nullopt for URLs that are not "bambu:///local/..." (cloud tutk/agora,
// rtsp variants, ...).
std::optional<LocalCameraUrl> parse_local_camera_url(const std::string& url);

} // namespace bbl_lan
