#include "agent.hpp"

#include "../common/log.hpp"

#include <exception>
#include <random>
#include <vector>

namespace bbl_lan {
namespace {

std::string random_client_id()
{
    std::random_device rd;
    std::mt19937_64    gen(rd());
    static const char  kHex[] = "0123456789abcdef";
    std::string        id     = "studio-lan-";
    for (int i = 0; i < 12; ++i)
        id.push_back(kHex[gen() % 16]);
    return id;
}

// CONNACK 4 (bad user name or password) and 5 (not authorized) both mean a
// wrong access code; the slicer recognises the string "5".
std::string connect_failure_msg(const MqttConnectResult& r)
{
    if (r.connack_code == 4 || r.connack_code == 5)
        return "5";
    return r.connack_code > 0 ? std::to_string(r.connack_code) : std::to_string(BAMBU_NETWORK_ERR_CONNECTION_TO_PRINTER_FAILED);
}

template <typename Fn, typename... Args> void invoke_safely(const char* what, const Fn& fn, Args&&... args)
{
    if (!fn)
        return;
    try {
        fn(std::forward<Args>(args)...);
    } catch (const std::exception& e) {
        LOG_E(what, " callback threw: ", e.what());
    } catch (...) {
        LOG_E(what, " callback threw an unknown exception");
    }
}

} // namespace

Agent::Agent(std::string log_dir) : m_log_dir(std::move(log_dir)) {}

Agent::~Agent()
{
    start_discovery(false);
    disconnect_printers();
}

Callbacks Agent::callbacks() const
{
    std::lock_guard<std::mutex> lock(m_cb_mutex);
    return m_callbacks;
}

bool Agent::start_discovery(bool start)
{
    std::lock_guard<std::mutex> lock(m_ssdp_mutex);
    if (!start) {
        if (m_ssdp)
            m_ssdp->stop();
        return true;
    }
    if (!m_ssdp) {
        m_ssdp = std::make_unique<SsdpListener>([this](const std::string& json) {
            invoke_safely("ssdp", callbacks().on_ssdp_msg, json);
        });
    }
    m_ssdp->start();
    return true;
}

// Stops a link's session and waits for its thread (fast: every wait in the
// TLS layer is cancellable within ~100 ms). Called without any Agent lock.
void Agent::stop_and_join(const LinkPtr& link)
{
    link->user_closed = true;
    link->mqtt.stop();
    std::lock_guard<std::mutex> lock(link->start_mutex);
    if (!link->thread.joinable())
        return;
    if (link->thread.get_id() == std::this_thread::get_id())
        link->thread.detach(); // re-entered from the link's own callback; the thread holds a reference
    else
        link->thread.join();
}

int Agent::connect_printer(const std::string& dev_id, const std::string& ip, const std::string& user,
                           const std::string& password, bool use_ssl)
{
    if (!use_ssl)
        LOG_W("connect_printer: plain MQTT requested, using TLS anyway (printers only listen on 8883)");

    LinkPtr old;
    {
        std::lock_guard<std::mutex> lock(m_links_mutex);
        const auto                  it = m_links.find(dev_id);
        if (it != m_links.end()) {
            old = it->second;
            m_links.erase(it);
        }
    }
    if (old)
        stop_and_join(old);

    auto link    = std::make_shared<PrinterLink>();
    link->dev_id = dev_id;
    link->ip     = ip;

    MqttOptions opts;
    opts.host      = ip;
    opts.port      = 8883;
    opts.client_id = random_client_id();
    opts.username  = user.empty() ? "bblp" : user;
    opts.password  = password;
    LOG_I("connect_printer ", dev_id, " @ ", ip, " user=", opts.username, " code=", redact(password));

    {
        std::lock_guard<std::mutex> lock(m_links_mutex);
        m_links[dev_id] = link;
    }
    std::lock_guard<std::mutex> start_lock(link->start_mutex);
    link->thread = std::thread([this, link, opts] {
        { std::lock_guard<std::mutex> wait_for_assignment(link->start_mutex); }
        run_link(link, opts);
    });
    return 0;
}

void Agent::run_link(const LinkPtr& link, const MqttOptions& opts)
{
    const std::string dev_id = link->dev_id;
    try {
        const MqttConnectResult r = link->mqtt.connect(opts);
        if (link->user_closed) {
            link->mqtt.close();
            return;
        }
        const auto on_connect = callbacks().on_local_connect;
        if (!r.ok()) {
            LOG_E("connect_printer ", dev_id, " failed: ", r.error);
            invoke_safely("local_connect", on_connect, static_cast<int>(BBL::ConnectStatusFailed), dev_id, connect_failure_msg(r));
            return;
        }
        if (!link->mqtt.subscribe("device/" + dev_id + "/report", 0)) {
            LOG_E("subscribe failed for ", dev_id);
            link->mqtt.close();
            invoke_safely("local_connect", on_connect, static_cast<int>(BBL::ConnectStatusFailed), dev_id,
                          std::to_string(BAMBU_NETWORK_ERR_CONNECTION_TO_PRINTER_FAILED));
            return;
        }
        LOG_I("connected to printer ", dev_id);
        invoke_safely("local_connect", on_connect, static_cast<int>(BBL::ConnectStatusOk), dev_id, std::string("0"));

        const std::string reason = link->mqtt.run(
            [this, dev_id](const std::string&, const std::string& payload) { dispatch_report(dev_id, payload); });
        link->mqtt.close();
        if (!link->user_closed) {
            LOG_W("printer ", dev_id, " connection lost: ", reason);
            invoke_safely("local_connect", callbacks().on_local_connect, static_cast<int>(BBL::ConnectStatusLost), dev_id, reason);
        } else {
            LOG_I("disconnected from ", dev_id);
        }
    } catch (const std::exception& e) {
        LOG_E("printer link ", dev_id, " aborted: ", e.what());
        link->mqtt.close();
    }
}

int Agent::disconnect_printers()
{
    std::map<std::string, LinkPtr> links;
    {
        std::lock_guard<std::mutex> lock(m_links_mutex);
        links.swap(m_links);
    }
    for (auto& [id, link] : links)
        stop_and_join(link);
    return 0;
}

int Agent::send_to_printer(const std::string& dev_id, const std::string& json, int qos)
{
    LinkPtr link;
    {
        std::lock_guard<std::mutex> lock(m_links_mutex);
        const auto                  it = m_links.find(dev_id);
        if (it != m_links.end())
            link = it->second;
    }
    if (!link || !link->mqtt.publish("device/" + dev_id + "/request", json, qos)) {
        LOG_W("send_to_printer ", dev_id, ": not connected");
        return BAMBU_NETWORK_ERR_SEND_MSG_FAILED;
    }
    LOG_D("-> ", dev_id, " ", json);
    return 0;
}

bool Agent::is_printer_connected(const std::string& dev_id) const
{
    std::lock_guard<std::mutex> lock(m_links_mutex);
    const auto                  it = m_links.find(dev_id);
    return it != m_links.end() && it->second->mqtt.connected();
}

void Agent::add_report_watcher(const std::string& dev_id, ReportWatcher watcher)
{
    std::lock_guard<std::mutex> lock(m_watch_mutex);
    m_watchers.emplace(dev_id, std::move(watcher));
}

void Agent::dispatch_report(const std::string& dev_id, const std::string& payload)
{
    {
        std::lock_guard<std::mutex> lock(m_watch_mutex);
        auto range = m_watchers.equal_range(dev_id);
        for (auto it = range.first; it != range.second;) {
            bool keep = false;
            try {
                keep = it->second(payload);
            } catch (const std::exception& e) {
                LOG_E("report watcher threw: ", e.what());
            }
            it = keep ? std::next(it) : m_watchers.erase(it);
        }
    }
    invoke_safely("local_message", callbacks().on_local_message, dev_id, payload);
}

std::string Agent::next_sequence_id() { return std::to_string(m_sequence++); }

} // namespace bbl_lan
