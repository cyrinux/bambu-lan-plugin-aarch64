#include "rtsp.hpp"

#include <openssl/evp.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <sstream>

namespace bbl_lan {
namespace {

std::string lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string trim(const std::string& s)
{
    const auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos)
        return {};
    const auto e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

bool starts_with(const std::string& s, const std::string& prefix) { return s.compare(0, prefix.size(), prefix) == 0; }

std::vector<std::string> split_lines(const std::string& text)
{
    std::vector<std::string> lines;
    std::istringstream       in(text);
    for (std::string line; std::getline(in, line);) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        lines.push_back(line);
    }
    return lines;
}

// key=value pairs of an a=fmtp line ("96 packetization-mode=1;sprop-...").
std::map<std::string, std::string> fmtp_params(const std::string& value)
{
    std::map<std::string, std::string> out;
    const auto                         space = value.find(' ');
    std::istringstream                 in(space == std::string::npos ? std::string{} : value.substr(space + 1));
    for (std::string item; std::getline(in, item, ';');) {
        const auto eq = item.find('=');
        if (eq != std::string::npos)
            out[lower(trim(item.substr(0, eq)))] = trim(item.substr(eq + 1));
    }
    return out;
}

} // namespace

std::string RtspResponse::header(const std::string& lower_name) const
{
    const auto it = headers.find(lower_name);
    return it == headers.end() ? std::string{} : it->second;
}

size_t RtspResponse::content_length() const
{
    const std::string v = header("content-length");
    return v.empty() ? 0 : static_cast<size_t>(std::strtoul(v.c_str(), nullptr, 10));
}

std::optional<RtspResponse> parse_rtsp_head(const std::string& head)
{
    const std::vector<std::string> lines = split_lines(head);
    if (lines.empty() || !starts_with(lines[0], "RTSP/1."))
        return std::nullopt;
    const auto sp = lines[0].find(' ');
    if (sp == std::string::npos)
        return std::nullopt;
    RtspResponse r;
    r.status = std::atoi(lines[0].c_str() + sp + 1);
    if (r.status < 100 || r.status > 999)
        return std::nullopt;
    for (size_t i = 1; i < lines.size(); ++i) {
        const auto colon = lines[i].find(':');
        if (colon == std::string::npos)
            continue;
        const std::string name = lower(trim(lines[i].substr(0, colon)));
        const std::string val  = trim(lines[i].substr(colon + 1));
        // Several WWW-Authenticate headers: prefer Digest over Basic.
        if (name == "www-authenticate" && r.headers.count(name) && starts_with(lower(r.headers[name]), "digest"))
            continue;
        r.headers[name] = val;
    }
    return r;
}

std::string build_rtsp_request(const std::string& method, const std::string& url, int cseq,
                               const std::vector<std::string>& extra_headers)
{
    std::string req = method + " " + url + " RTSP/1.0\r\nCSeq: " + std::to_string(cseq) +
                      "\r\nUser-Agent: bambu-lan-plugin\r\n";
    for (const auto& h : extra_headers)
        req += h + "\r\n";
    return req + "\r\n";
}

std::optional<AuthChallenge> parse_www_authenticate(const std::string& value)
{
    const std::string v  = trim(value);
    const auto        sp = v.find(' ');
    AuthChallenge     c;
    c.scheme = lower(v.substr(0, sp));
    if (c.scheme != "basic" && c.scheme != "digest")
        return std::nullopt;
    size_t pos = sp == std::string::npos ? v.size() : sp + 1;
    while (pos < v.size()) {
        while (pos < v.size() && (v[pos] == ' ' || v[pos] == ','))
            ++pos;
        const auto eq = v.find('=', pos);
        if (eq == std::string::npos)
            break;
        const std::string name = lower(trim(v.substr(pos, eq - pos)));
        std::string       val;
        pos = eq + 1;
        if (pos < v.size() && v[pos] == '"') {
            const auto end = v.find('"', pos + 1);
            val            = v.substr(pos + 1, end == std::string::npos ? std::string::npos : end - pos - 1);
            pos            = end == std::string::npos ? v.size() : end + 1;
        } else {
            const auto end = v.find(',', pos);
            val            = trim(v.substr(pos, end == std::string::npos ? std::string::npos : end - pos));
            pos            = end == std::string::npos ? v.size() : end;
        }
        c.params[name] = val;
    }
    return c;
}

std::string authorization_value(const AuthChallenge& c, const std::string& user, const std::string& password,
                                const std::string& method, const std::string& uri, unsigned nc_value,
                                const std::string& cnonce)
{
    if (c.scheme == "basic")
        return "Basic " + base64_encode(user + ":" + password);
    if (c.scheme != "digest")
        return {};
    const auto param = [&](const char* k) {
        const auto it = c.params.find(k);
        return it == c.params.end() ? std::string{} : it->second;
    };
    const std::string algorithm = lower(param("algorithm"));
    if (!algorithm.empty() && algorithm != "md5")
        return {};
    const std::string realm = param("realm"), nonce = param("nonce"), opaque = param("opaque");
    const std::string ha1 = md5_hex(user + ":" + realm + ":" + password);
    const std::string ha2 = md5_hex(method + ":" + uri);
    // qop may list several options ("auth,auth-int"); only "auth" is used.
    const bool        qop = param("qop").find("auth") != std::string::npos;
    char nc_buf[9];
    std::snprintf(nc_buf, sizeof(nc_buf), "%08x", nc_value);
    const std::string nc = nc_buf;
    const std::string response =
        qop ? md5_hex(ha1 + ":" + nonce + ":" + nc + ":" + cnonce + ":auth:" + ha2) : md5_hex(ha1 + ":" + nonce + ":" + ha2);
    std::string out = "Digest username=\"" + user + "\", realm=\"" + realm + "\", nonce=\"" + nonce + "\", uri=\"" + uri +
                      "\", response=\"" + response + "\"";
    if (qop)
        out += ", qop=auth, nc=" + nc + ", cnonce=\"" + cnonce + "\"";
    if (!opaque.empty())
        out += ", opaque=\"" + opaque + "\"";
    if (!algorithm.empty())
        out += ", algorithm=MD5";
    return out;
}

std::vector<uint8_t> base64_decode(const std::string& in)
{
    std::vector<uint8_t> out;
    uint32_t             acc  = 0;
    int                  bits = 0;
    for (const unsigned char ch : in) {
        int v;
        if (ch >= 'A' && ch <= 'Z')
            v = ch - 'A';
        else if (ch >= 'a' && ch <= 'z')
            v = ch - 'a' + 26;
        else if (ch >= '0' && ch <= '9')
            v = ch - '0' + 52;
        else if (ch == '+' || ch == '-')
            v = 62;
        else if (ch == '/' || ch == '_')
            v = 63;
        else
            continue; // padding and whitespace
        acc = (acc << 6) | static_cast<uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<uint8_t>((acc >> bits) & 0xFF));
        }
    }
    return out;
}

std::string base64_encode(const std::string& in)
{
    static const char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string       out;
    size_t            i = 0;
    for (; i + 2 < in.size(); i += 3) {
        const uint32_t n = (uint8_t(in[i]) << 16) | (uint8_t(in[i + 1]) << 8) | uint8_t(in[i + 2]);
        for (int s = 18; s >= 0; s -= 6)
            out.push_back(kAlphabet[(n >> s) & 63]);
    }
    if (i < in.size()) {
        const bool     two = i + 1 < in.size();
        const uint32_t n   = (uint8_t(in[i]) << 16) | (two ? uint8_t(in[i + 1]) << 8 : 0);
        out.push_back(kAlphabet[(n >> 18) & 63]);
        out.push_back(kAlphabet[(n >> 12) & 63]);
        out.push_back(two ? kAlphabet[(n >> 6) & 63] : '=');
        out.push_back('=');
    }
    return out;
}

std::string md5_hex(const std::string& in)
{
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int  len = 0;
    if (EVP_Digest(in.data(), in.size(), digest, &len, EVP_md5(), nullptr) != 1)
        return {};
    static const char kHex[] = "0123456789abcdef";
    std::string       out;
    for (unsigned int i = 0; i < len; ++i) {
        out.push_back(kHex[digest[i] >> 4]);
        out.push_back(kHex[digest[i] & 15]);
    }
    return out;
}

std::optional<SdpVideoTrack> parse_sdp_video(const std::string& sdp)
{
    SdpVideoTrack track;
    bool          in_session = true, in_video = false, found = false;
    std::string   fmtp;
    for (const std::string& line : split_lines(sdp)) {
        if (starts_with(line, "m=")) {
            if (found && in_video)
                break; // first video media only
            in_session = false;
            in_video   = starts_with(line, "m=video ");
            if (in_video) {
                found = true;
                // m=video <port> RTP/AVP <pt> [...]
                std::istringstream in(line.substr(8));
                std::string        port, proto;
                in >> port >> proto >> track.payload_type;
            }
            continue;
        }
        if (!in_video) {
            if (in_session && starts_with(line, "a=control:"))
                track.session_control = trim(line.substr(10));
            continue;
        }
        const std::string pt = std::to_string(track.payload_type) + " ";
        if (starts_with(line, "a=control:")) {
            track.control = trim(line.substr(10));
        } else if (starts_with(line, "a=rtpmap:" + pt)) {
            const std::string enc = line.substr(9 + pt.size());
            track.encoding        = enc.substr(0, enc.find('/'));
            std::transform(track.encoding.begin(), track.encoding.end(), track.encoding.begin(),
                           [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
        } else if (starts_with(line, "a=fmtp:" + pt)) {
            fmtp = line.substr(7);
        } else if (starts_with(line, "a=framerate:")) {
            track.frame_rate = static_cast<int>(std::strtod(line.c_str() + 12, nullptr) + 0.5);
        }
    }
    if (!found || track.payload_type < 0)
        return std::nullopt;
    const auto params = fmtp_params(fmtp);
    const auto it     = params.find("sprop-parameter-sets");
    if (it != params.end()) {
        std::istringstream in(it->second);
        for (std::string item; std::getline(in, item, ',');) {
            auto nal = base64_decode(item);
            if (!nal.empty())
                track.parameter_sets.push_back(std::move(nal));
        }
    }
    return track;
}

std::string resolve_control(const std::string& base, const std::string& control)
{
    if (control.empty() || control == "*")
        return base;
    if (starts_with(lower(control), "rtsp://") || starts_with(lower(control), "rtsps://"))
        return control;
    if (!base.empty() && base.back() == '/')
        return base + control;
    return base + "/" + control;
}

} // namespace bbl_lan
