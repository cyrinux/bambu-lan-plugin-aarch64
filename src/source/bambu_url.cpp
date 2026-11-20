#include "bambu_url.hpp"

#include <cstdlib>

namespace bbl_lan {

std::string percent_decode(const std::string& in)
{
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i] == '%' && i + 2 < in.size() && std::isxdigit(static_cast<unsigned char>(in[i + 1])) &&
            std::isxdigit(static_cast<unsigned char>(in[i + 2]))) {
            out.push_back(static_cast<char>(std::strtol(in.substr(i + 1, 2).c_str(), nullptr, 16)));
            i += 2;
        } else {
            out.push_back(in[i]);
        }
    }
    return out;
}

std::optional<LocalCameraUrl> parse_local_camera_url(const std::string& url)
{
    static const std::string kPrefix = "bambu:///local/";
    if (url.compare(0, kPrefix.size(), kPrefix) != 0)
        return std::nullopt;

    const std::string rest  = url.substr(kPrefix.size());
    const auto        qpos  = rest.find('?');
    std::string       host  = percent_decode(rest.substr(0, qpos));
    const std::string query = qpos == std::string::npos ? std::string{} : rest.substr(qpos + 1);

    // The live-view form carries a literal '.' after the IP ("<ip>.?port=").
    while (!host.empty() && (host.back() == '.' || host.back() == '/'))
        host.pop_back();
    if (host.empty())
        return std::nullopt;

    LocalCameraUrl result;
    result.host = host;
    size_t start = 0;
    while (start <= query.size() && !query.empty()) {
        size_t end = query.find('&', start);
        if (end == std::string::npos)
            end = query.size();
        const std::string pair = query.substr(start, end - start);
        const auto        eq   = pair.find('=');
        if (eq != std::string::npos)
            result.params[percent_decode(pair.substr(0, eq))] = percent_decode(pair.substr(eq + 1));
        start = end + 1;
        if (end == query.size())
            break;
    }

    const auto take = [&](const char* key) -> std::string {
        const auto it = result.params.find(key);
        return it == result.params.end() ? std::string{} : it->second;
    };
    const std::string port = take("port");
    if (!port.empty()) {
        const long p = std::strtol(port.c_str(), nullptr, 10);
        if (p <= 0 || p > 65535)
            return std::nullopt;
        result.port = static_cast<uint16_t>(p);
    }
    if (!take("user").empty())
        result.user = take("user");
    result.password = take("passwd");
    result.device   = take("device");
    return result;
}

} // namespace bbl_lan

namespace bbl_lan {

std::string RtspCameraUrl::request_url() const
{
    return std::string(secure ? "rtsps://" : "rtsp://") + host + ":" + std::to_string(port) + path;
}

std::optional<RtspCameraUrl> parse_rtsp_camera_url(const std::string& url)
{
    static const std::string kSecure = "bambu:///rtsps___";
    static const std::string kPlain  = "bambu:///rtsp___";
    RtspCameraUrl            result;
    std::string              rest;
    if (url.compare(0, kSecure.size(), kSecure) == 0) {
        rest = url.substr(kSecure.size());
    } else if (url.compare(0, kPlain.size(), kPlain) == 0) {
        result.secure = false;
        result.port   = 554;
        rest          = url.substr(kPlain.size());
    } else {
        return std::nullopt;
    }
    rest = rest.substr(0, rest.find('?'));

    // <user>:<code>@<host>[:port]/<path>. The host and path have no '@', the
    // code may contain '@' or ':'.
    const auto at         = rest.rfind('@');
    size_t     host_start = 0;
    if (at != std::string::npos) {
        const std::string cred  = rest.substr(0, at);
        const auto        colon = cred.find(':');
        result.user             = percent_decode(cred.substr(0, colon));
        if (colon != std::string::npos)
            result.password = percent_decode(cred.substr(colon + 1));
        host_start = at + 1;
    }
    const auto  slash     = rest.find('/', host_start);
    std::string authority = rest.substr(host_start, slash == std::string::npos ? std::string::npos : slash - host_start);
    if (slash != std::string::npos)
        result.path = rest.substr(slash);
    // "[v6addr]:port", "host:port" or a bare host; brackets are dropped.
    std::string port;
    if (!authority.empty() && authority[0] == '[') {
        const auto close = authority.find(']');
        if (close == std::string::npos)
            return std::nullopt;
        if (close + 1 < authority.size()) {
            if (authority[close + 1] != ':')
                return std::nullopt;
            port = authority.substr(close + 2);
        }
        authority = authority.substr(1, close - 1);
    } else if (const auto colon = authority.rfind(':'); colon != std::string::npos) {
        port = authority.substr(colon + 1);
        authority.resize(colon);
    }
    if (!port.empty()) {
        const long p = std::strtol(port.c_str(), nullptr, 10);
        if (p <= 0 || p > 65535)
            return std::nullopt;
        result.port = static_cast<uint16_t>(p);
    }
    if (authority.empty() || result.user.empty())
        return std::nullopt;
    result.host = authority;
    return result;
}

} // namespace bbl_lan
