#include "ssdp_listener.hpp"

#include "../common/log.hpp"

#include <nlohmann/json.hpp>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <exception>
#include <vector>

namespace bbl_lan {
namespace {

constexpr uint16_t kNotifyPorts[] = {2021, 1990};
constexpr int      kSearchIntervalS = 30;

std::string trim(const std::string& s)
{
    const auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos)
        return {};
    const auto e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

std::string lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string get(const std::map<std::string, std::string>& h, const std::string& key)
{
    const auto it = h.find(key);
    return it == h.end() ? std::string{} : it->second;
}

int open_udp(uint16_t port)
{
    const int fd = ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return -1;
    const int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one));
    ::setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &one, sizeof(one));
    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        LOG_W("ssdp: bind udp/", port, " failed: ", std::strerror(errno));
        ::close(fd);
        return -1;
    }
    ip_mreq mreq{};
    mreq.imr_multiaddr.s_addr = inet_addr("239.255.255.250");
    mreq.imr_interface.s_addr = htonl(INADDR_ANY);
    ::setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq));
    return fd;
}

void send_msearch(int fd)
{
    static const char kSearch[] =
        "M-SEARCH * HTTP/1.1\r\n"
        "HOST: 239.255.255.250:1990\r\n"
        "MAN: \"ssdp:discover\"\r\n"
        "MX: 1\r\n"
        "ST: urn:bambulab-com:device:3dprinter:1\r\n\r\n";
    sockaddr_in dst{};
    dst.sin_family      = AF_INET;
    dst.sin_port        = htons(1990);
    dst.sin_addr.s_addr = inet_addr("239.255.255.250");
    ::sendto(fd, kSearch, sizeof(kSearch) - 1, 0, reinterpret_cast<sockaddr*>(&dst), sizeof(dst));
    dst.sin_port = htons(2021);
    ::sendto(fd, kSearch, sizeof(kSearch) - 1, 0, reinterpret_cast<sockaddr*>(&dst), sizeof(dst));
}

} // namespace

std::map<std::string, std::string> parse_ssdp_headers(const std::string& datagram)
{
    std::map<std::string, std::string> headers;
    size_t                             start = 0;
    while (start < datagram.size()) {
        size_t end = datagram.find('\n', start);
        if (end == std::string::npos)
            end = datagram.size();
        const std::string line  = datagram.substr(start, end - start);
        const auto        colon = line.find(':');
        if (colon != std::string::npos)
            headers.emplace(lower(trim(line.substr(0, colon))), trim(line.substr(colon + 1)));
        start = end + 1;
    }
    return headers;
}

std::string ssdp_to_device_json(const std::map<std::string, std::string>& h, const std::string& sender_ip)
{
    const std::string nt = get(h, "nt") + get(h, "st");
    const std::string sn = get(h, "usn");
    if (nt.find("bambulab") == std::string::npos || sn.empty())
        return {};

    std::string ip = get(h, "location");
    if (ip.empty())
        ip = sender_ip;
    std::string signal = get(h, "devsignal.bambu.com");
    if (!signal.empty() && signal.find("dBm") == std::string::npos)
        signal += "dBm";
    const std::string name = get(h, "devname.bambu.com");

    nlohmann::json j = {
        {"dev_name", name.empty() ? sn : name},
        {"dev_id", sn},
        {"dev_ip", ip},
        {"dev_type", get(h, "devmodel.bambu.com")},
        {"dev_signal", signal},
        {"connect_type", get(h, "devconnect.bambu.com")},
        {"bind_state", get(h, "devbind.bambu.com")},
    };
    // Optional keys: only forward them when the printer actually sent them.
    const std::pair<const char*, const char*> optional[] = {
        {"sec_link", "devseclink.bambu.com"},
        {"ssdp_version", "devversion.bambu.com"},
        {"connection_name", "devinf.bambu.com"},
    };
    for (const auto& [key, header] : optional) {
        const std::string v = get(h, header);
        if (!v.empty())
            j[key] = v;
    }
    return j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

SsdpListener::SsdpListener(DeviceFn on_device) : m_on_device(std::move(on_device)) {}

SsdpListener::~SsdpListener() { stop(); }

void SsdpListener::start()
{
    if (m_running.exchange(true))
        return;
    if (m_thread.joinable()) // previous run() exited on its own (e.g. bind failure)
        m_thread.join();
    m_thread = std::thread([this] {
        try {
            run();
        } catch (const std::exception& e) {
            LOG_E("ssdp: listener aborted: ", e.what());
            m_running = false;
        }
    });
}

void SsdpListener::stop()
{
    m_running = false;
    if (m_thread.joinable())
        m_thread.join();
}

void SsdpListener::run()
{
    std::vector<pollfd> fds;
    for (const uint16_t port : kNotifyPorts) {
        const int fd = open_udp(port);
        if (fd >= 0)
            fds.push_back({fd, POLLIN, 0});
    }
    if (fds.empty()) {
        LOG_E("ssdp: no UDP socket could be opened, discovery disabled");
        m_running = false;
        return;
    }
    LOG_I("ssdp: listening on ", fds.size(), " socket(s)");

    auto last_search = std::chrono::steady_clock::time_point{};
    char buf[4096];
    while (m_running) {
        const auto now = std::chrono::steady_clock::now();
        if (now - last_search > std::chrono::seconds(kSearchIntervalS)) {
            send_msearch(fds.front().fd);
            last_search = now;
        }
        if (::poll(fds.data(), fds.size(), 500) <= 0)
            continue;
        for (auto& p : fds) {
            if (!(p.revents & POLLIN))
                continue;
            sockaddr_in   from{};
            socklen_t     flen = sizeof(from);
            const ssize_t n    = ::recvfrom(p.fd, buf, sizeof(buf), 0, reinterpret_cast<sockaddr*>(&from), &flen);
            if (n <= 0)
                continue;
            char ip[INET_ADDRSTRLEN] = {0};
            inet_ntop(AF_INET, &from.sin_addr, ip, sizeof(ip));
            const std::string json = ssdp_to_device_json(parse_ssdp_headers(std::string(buf, static_cast<size_t>(n))), ip);
            if (!json.empty() && m_on_device)
                m_on_device(json);
        }
    }
    for (const auto& p : fds)
        ::close(p.fd);
}

} // namespace bbl_lan
