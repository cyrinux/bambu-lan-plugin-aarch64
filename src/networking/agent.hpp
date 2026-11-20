// The object behind the opaque `void* agent` handle the slicer passes to every
// bambu_network_* call. Owns callbacks, SSDP discovery and the LAN MQTT
// sessions to printers.
#pragma once

#include "bambu_networking.hpp"
#include "mqtt_client.hpp"
#include "ssdp_listener.hpp"

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace bbl_lan {

struct Callbacks {
    BBL::OnMsgArrivedFn        on_ssdp_msg;
    BBL::OnLocalConnectedFn    on_local_connect;
    BBL::OnMessageFn           on_local_message;
    BBL::OnUserLoginFn         on_user_login;
    BBL::OnPrinterConnectedFn  on_printer_connected;
    BBL::OnServerConnectedFn   on_server_connected;
    BBL::OnHttpErrorFn         on_http_error;
    BBL::GetSubscribeFailureFn on_subscribe_failure;
    BBL::OnMessageFn           on_message;
    BBL::OnMessageFn           on_user_message;
    BBL::QueueOnMainFn         queue_on_main;
    BBL::GetCountryCodeFn      get_country_code;
    BBL::OnServerErrFn         on_server_err;
};

// Called with every report payload of a printer; used by print jobs to wait
// for the printer's acknowledgement. Return false to unregister.
using ReportWatcher = std::function<bool(const std::string& payload)>;

class Agent {
public:
    explicit Agent(std::string log_dir);
    ~Agent();
    Agent(const Agent&)            = delete;
    Agent& operator=(const Agent&) = delete;

    // Callback registry: copies are returned so they can be invoked without
    // holding the lock.
    Callbacks callbacks() const;
    template <typename Fn> void update_callbacks(Fn&& mutate)
    {
        std::lock_guard<std::mutex> lock(m_cb_mutex);
        mutate(m_callbacks);
    }

    void set_config_dir(const std::string& dir) { m_config_dir = dir; }
    const std::string& log_dir() const { return m_log_dir; }

    bool start_discovery(bool start);

    // Asynchronous: the result is reported via on_local_connect. Never blocks
    // for more than ~100 ms (stopping a previous session for the same printer).
    int  connect_printer(const std::string& dev_id, const std::string& ip, const std::string& user,
                         const std::string& password, bool use_ssl);
    int  disconnect_printers();
    // Returns 0 on success, BAMBU_NETWORK_ERR_SEND_MSG_FAILED otherwise.
    int  send_to_printer(const std::string& dev_id, const std::string& json, int qos);
    bool is_printer_connected(const std::string& dev_id) const;

    void add_report_watcher(const std::string& dev_id, ReportWatcher watcher);

    std::string next_sequence_id();

private:
    // One per connect_printer() call. Its thread owns the MQTT session for
    // its whole lifetime; other threads only publish() or stop() it.
    struct PrinterLink {
        std::string       dev_id;
        std::string       ip;
        MqttClient        mqtt;
        std::mutex        start_mutex; // held while `thread` is being assigned
        std::thread       thread;
        std::atomic<bool> user_closed{false};
    };
    using LinkPtr = std::shared_ptr<PrinterLink>;

    void run_link(const LinkPtr& link, const MqttOptions& opts);
    void dispatch_report(const std::string& dev_id, const std::string& payload);
    static void stop_and_join(const LinkPtr& link);

    std::string m_log_dir;
    std::string m_config_dir;

    mutable std::mutex m_cb_mutex;
    Callbacks          m_callbacks;

    std::mutex                    m_ssdp_mutex;
    std::unique_ptr<SsdpListener> m_ssdp;

    mutable std::mutex             m_links_mutex;
    std::map<std::string, LinkPtr> m_links;

    std::mutex                                m_watch_mutex;
    std::multimap<std::string, ReportWatcher> m_watchers;

    std::atomic<uint64_t> m_sequence{20000};
};

} // namespace bbl_lan
