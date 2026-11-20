// Listens for the SSDP NOTIFY datagrams Bambu printers broadcast on UDP
// 2021 (and 1990) and converts them into the flat JSON object the slicer's
// DeviceManager::on_machine_alive() expects.
#pragma once

#include <atomic>
#include <functional>
#include <map>
#include <string>
#include <thread>

namespace bbl_lan {

// Parses "Header: value" lines of an SSDP datagram. Keys are lower-cased.
std::map<std::string, std::string> parse_ssdp_headers(const std::string& datagram);

// Converts parsed headers to the slicer JSON. Returns an empty string when
// the datagram is not from a Bambu printer.
std::string ssdp_to_device_json(const std::map<std::string, std::string>& headers, const std::string& sender_ip);

class SsdpListener {
public:
    using DeviceFn = std::function<void(const std::string& device_json)>;

    explicit SsdpListener(DeviceFn on_device);
    ~SsdpListener();
    SsdpListener(const SsdpListener&)            = delete;
    SsdpListener& operator=(const SsdpListener&) = delete;

    void start();
    void stop();
    bool running() const { return m_running.load(); }

private:
    void run();

    DeviceFn          m_on_device;
    std::thread       m_thread;
    std::atomic<bool> m_running{false};
};

} // namespace bbl_lan
