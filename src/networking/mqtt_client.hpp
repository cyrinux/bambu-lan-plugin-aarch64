// Minimal MQTT 3.1.1 client over TLS, sufficient for a Bambu printer's local
// broker (port 8883, user "bblp", password = LAN access code).
//
// Threading: one owner thread calls connect(), run() and close(). Any thread
// may call subscribe(), publish() and stop(); stop() makes run() return
// within ~100 ms and aborts an in-progress connect().
#pragma once

#include "../common/tls_socket.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>

namespace bbl_lan {

struct MqttOptions {
    std::string host;
    uint16_t    port = 8883;
    std::string client_id;
    std::string username;
    std::string password;
    uint16_t    keepalive_s        = 30;
    int         connect_timeout_ms = 10000;
};

// connack_code is the MQTT CONNACK return code (0 accepted, 4 bad user or
// password, 5 not authorized) or -1 when no CONNACK was received.
struct MqttConnectResult {
    int         connack_code = -1;
    std::string error;
    bool        ok() const { return connack_code == 0; }
};

class MqttClient {
public:
    using MessageFn = std::function<void(const std::string& topic, const std::string& payload)>;

    MqttClient() = default;
    MqttClient(const MqttClient&)            = delete;
    MqttClient& operator=(const MqttClient&) = delete;

    MqttConnectResult connect(const MqttOptions& opts);
    // Reads until the connection drops or stop() is called. Returns the reason.
    std::string run(const MessageFn& on_message);
    void        close();

    bool subscribe(const std::string& topic, int qos);
    bool publish(const std::string& topic, const std::string& payload, int qos);
    void stop();
    bool connected() const { return m_connected.load(); }
    bool stopping() const { return m_sock.cancelled(); }

    // Bambu hybrid-mode payload encryption (printer not in LAN-only mode):
    // payloads on device/<serial>/request and /report are AES-128-CBC with a
    // key derived from the owner's access code. Modes:
    //   plain  - always plaintext (LAN-only behaviour)
    //   aes    - always encrypted
    //   auto   - send every publish both ways; the first inbound payload that
    //            parses decides (JSON -> plain, valid AES->JSON -> aes).
    enum class CipherMode { plain, aes, auto_ };
    void set_cipher(const std::string& key16, CipherMode mode)
    {
        m_cipher_key = key16;
        m_cipher_mode.store(mode);
    }
    CipherMode cipher_mode() const { return m_cipher_mode.load(); }

private:
    bool send_packet(uint8_t header, const std::string& body);
    bool read_packet(uint8_t* header, std::string* body, int first_byte_timeout_ms, bool* timed_out);
    void handle_publish(uint8_t header, const std::string& body, const MessageFn& on_message);
    uint16_t next_packet_id();

    MqttOptions             m_opts;
    TlsSocket               m_sock;
    std::mutex              m_write_mutex;
    std::atomic<bool>       m_connected{false};
    std::atomic<uint16_t>   m_packet_id{0};
    std::atomic<int64_t>    m_last_tx_ms{0};
    std::string             m_cipher_key; // 16 ASCII bytes, empty = no cipher
    std::atomic<CipherMode> m_cipher_mode{CipherMode::plain};
};

} // namespace bbl_lan
