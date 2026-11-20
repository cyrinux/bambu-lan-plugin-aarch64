// RTSP 1.0 client helpers (RFC 2326), kept free of I/O so they can be unit
// tested: request building, response parsing, Basic/Digest authentication,
// and the SDP fields needed to receive one H.264 video track.
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace bbl_lan {

struct RtspResponse {
    int                                status = 0;
    std::map<std::string, std::string> headers; // lower-case names
    std::string                        body;

    std::string header(const std::string& lower_name) const;
    size_t      content_length() const;
};

// Parses the status line and headers ("RTSP/1.0 200 OK\r\n...\r\n\r\n").
std::optional<RtspResponse> parse_rtsp_head(const std::string& head);

std::string build_rtsp_request(const std::string& method, const std::string& url, int cseq,
                               const std::vector<std::string>& extra_headers);

// "Basic realm=..", "Digest realm=..,nonce=..,qop=.." (one challenge).
struct AuthChallenge {
    std::string                        scheme; // lower-case: "basic" or "digest"
    std::map<std::string, std::string> params; // lower-case names, unquoted values
};
std::optional<AuthChallenge> parse_www_authenticate(const std::string& value);

// Value of the Authorization header for one request ("" if the scheme is not
// supported). nc (nonce count, incremented per request with the same nonce)
// and cnonce are only used for Digest with qop=auth.
std::string authorization_value(const AuthChallenge& challenge, const std::string& user, const std::string& password,
                                const std::string& method, const std::string& uri, unsigned nc = 1,
                                const std::string& cnonce = "0a4f113b");

std::vector<uint8_t> base64_decode(const std::string& in);
std::string          base64_encode(const std::string& in);
std::string          md5_hex(const std::string& in);

struct SdpVideoTrack {
    std::string                       control;      // a=control of the video media (may be relative or "*")
    std::string                       session_control;
    int                               payload_type = -1;
    std::string                       encoding;     // upper-case rtpmap name, e.g. "H264"
    std::vector<std::vector<uint8_t>> parameter_sets; // sprop-parameter-sets (SPS, PPS), without start codes
    int                               frame_rate = 0; // a=framerate, 0 if absent
};
std::optional<SdpVideoTrack> parse_sdp_video(const std::string& sdp);

// Absolute URL of a media/session control attribute (RFC 2326 C.1.1).
std::string resolve_control(const std::string& base, const std::string& control);

} // namespace bbl_lan
