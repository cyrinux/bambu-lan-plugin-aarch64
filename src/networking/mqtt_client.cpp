#include "mqtt_client.hpp"

#include "../common/aes_cbc.hpp"
#include "../common/log.hpp"

#include <chrono>
#include <exception>

namespace bbl_lan {
namespace {

constexpr uint8_t kConnect    = 0x10;
constexpr uint8_t kConnack    = 0x20;
constexpr uint8_t kPublish    = 0x30;
constexpr uint8_t kPuback     = 0x40;
constexpr uint8_t kSubscribe  = 0x82; // type 8, reserved flags 0b0010
constexpr uint8_t kPingreq    = 0xC0;
constexpr uint8_t kDisconnect = 0xE0;
constexpr size_t  kMaxPacket  = 8 * 1024 * 1024;

int64_t now_ms()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

void put_u16(std::string* out, uint16_t v)
{
    out->push_back(static_cast<char>(v >> 8));
    out->push_back(static_cast<char>(v & 0xFF));
}

void put_str(std::string* out, const std::string& s)
{
    put_u16(out, static_cast<uint16_t>(s.size()));
    out->append(s);
}

std::string encode_remaining_length(size_t len)
{
    std::string out;
    do {
        uint8_t byte = len % 128;
        len /= 128;
        if (len > 0)
            byte |= 0x80;
        out.push_back(static_cast<char>(byte));
    } while (len > 0);
    return out;
}

uint16_t get_u16(const std::string& s, size_t pos)
{
    return static_cast<uint16_t>((static_cast<uint8_t>(s[pos]) << 8) | static_cast<uint8_t>(s[pos + 1]));
}

} // namespace

uint16_t MqttClient::next_packet_id()
{
    uint16_t id = ++m_packet_id;
    if (id == 0)
        id = ++m_packet_id;
    return id;
}

bool MqttClient::send_packet(uint8_t header, const std::string& body)
{
    std::string pkt;
    pkt.reserve(body.size() + 5);
    pkt.push_back(static_cast<char>(header));
    pkt.append(encode_remaining_length(body.size()));
    pkt.append(body);
    std::lock_guard<std::mutex> lock(m_write_mutex);
    const bool ok = m_sock.write_all(pkt.data(), pkt.size()) == IoResult::Ok;
    if (ok)
        m_last_tx_ms = now_ms();
    return ok;
}

// Reads one packet. *timed_out is set when no byte at all arrived within
// first_byte_timeout_ms (not an error: used to send keepalive pings).
bool MqttClient::read_packet(uint8_t* header, std::string* body, int first_byte_timeout_ms, bool* timed_out)
{
    *timed_out       = false;
    const IoResult r = m_sock.read_exact(header, 1, first_byte_timeout_ms);
    if (r == IoResult::Timeout) {
        *timed_out = true;
        return false;
    }
    if (r != IoResult::Ok)
        return false;
    size_t len = 0;
    for (int shift = 0, i = 0;; ++i, shift += 7) {
        if (i == 4)
            return false; // malformed remaining length
        uint8_t b = 0;
        if (m_sock.read_exact(&b, 1, 5000) != IoResult::Ok)
            return false;
        len |= static_cast<size_t>(b & 0x7F) << shift;
        if ((b & 0x80) == 0)
            break;
    }
    if (len > kMaxPacket)
        return false;
    body->assign(len, '\0');
    return len == 0 || m_sock.read_exact(body->data(), len, 10000) == IoResult::Ok;
}

MqttConnectResult MqttClient::connect(const MqttOptions& opts)
{
    m_opts = opts;
    MqttConnectResult result;
    result.error = m_sock.connect(opts.host, opts.port, opts.connect_timeout_ms);
    if (!result.error.empty())
        return result;

    std::string body;
    put_str(&body, "MQTT");
    body.push_back(4);    // protocol level 3.1.1
    uint8_t flags = 0x02; // clean session
    if (!opts.username.empty()) flags |= 0x80;
    if (!opts.password.empty()) flags |= 0x40;
    body.push_back(static_cast<char>(flags));
    put_u16(&body, opts.keepalive_s);
    put_str(&body, opts.client_id);
    if (!opts.username.empty()) put_str(&body, opts.username);
    if (!opts.password.empty()) put_str(&body, opts.password);

    if (!send_packet(kConnect, body)) {
        result.error = "failed to send CONNECT";
        m_sock.close();
        return result;
    }
    uint8_t     header    = 0;
    std::string ack;
    bool        timed_out = false;
    if (!read_packet(&header, &ack, opts.connect_timeout_ms, &timed_out) || (header & 0xF0) != kConnack || ack.size() < 2) {
        result.error = m_sock.cancelled() ? "cancelled" : "no CONNACK from broker";
        m_sock.close();
        return result;
    }
    result.connack_code = static_cast<uint8_t>(ack[1]);
    if (!result.ok()) {
        result.error = "broker refused connection, code " + std::to_string(result.connack_code);
        m_sock.close();
        return result;
    }
    m_connected = true;
    return result;
}

bool MqttClient::subscribe(const std::string& topic, int qos)
{
    std::string body;
    put_u16(&body, next_packet_id());
    put_str(&body, topic);
    body.push_back(static_cast<char>(qos & 0x03));
    return m_connected && send_packet(kSubscribe, body);
}

bool MqttClient::publish(const std::string& topic, const std::string& payload, int qos)
{
    if (!m_connected)
        return false;
    const int   q = qos > 0 ? 1 : 0;
    const auto  mode        = m_cipher_mode.load();
    const bool  have_cipher = !m_cipher_key.empty();
    // auto sends both copies; the printer processes only the one matching its
    // mode (a hybrid printer ignores plaintext, a LAN-only printer cannot
    // parse base64 JSON), and the first answer settles the mode.
    const bool send_plain = mode != CipherMode::aes && (mode != CipherMode::auto_ || have_cipher);
    const bool send_aes   = have_cipher && mode != CipherMode::plain;

    bool ok = true;
    if (send_plain) {
        std::string body;
        put_str(&body, topic);
        if (q == 1)
            put_u16(&body, next_packet_id());
        body.append(payload);
        ok = send_packet(static_cast<uint8_t>(kPublish | (q << 1)), body) && ok;
    }
    if (send_aes) {
        const std::string enc = aes128_cbc_encrypt_b64(payload, m_cipher_key);
        if (enc.empty()) {
            LOG_E("mqtt: AES encrypt failed");
            ok = false;
        } else {
            std::string body;
            put_str(&body, topic);
            if (q == 1) {
                // Same packet id as the plain copy when both go out: they are
                // one logical message to the printer, which acks only one.
                const uint16_t pid = send_plain ? static_cast<uint16_t>(m_packet_id.load()) : next_packet_id();
                put_u16(&body, pid);
            }
            body.append(enc);
            ok = send_packet(static_cast<uint8_t>(kPublish | (q << 1)), body) && ok;
        }
    }
    return ok;
}

void MqttClient::handle_publish(uint8_t header, const std::string& body, const MessageFn& on_message)
{
    if (body.size() < 2)
        return;
    const uint16_t tlen = get_u16(body, 0);
    size_t         pos  = 2 + tlen;
    if (pos > body.size())
        return;
    const std::string topic = body.substr(2, tlen);
    if (((header >> 1) & 0x03) > 0) {
        if (pos + 2 > body.size())
            return;
        std::string ack;
        put_u16(&ack, get_u16(body, pos));
        pos += 2;
        send_packet(kPuback, ack);
    }
    if (!on_message)
        return;
    const std::string raw = body.substr(pos);

    // Hybrid-mode payload handling (see set_cipher): the first payload that
    // parses decides the mode; afterwards one form only. A payload of the
    // other form (printer toggled LAN-only mid-session) re-decides.
    std::string payload  = raw;
    const auto  mode     = m_cipher_mode.load();
    if (!m_cipher_key.empty() && mode != CipherMode::plain) {
        if (!raw.empty() && (raw.front() == '{' || raw.front() == '[')) {
            if (mode == CipherMode::aes)
                LOG_I("mqtt: printer switched to plaintext payloads (LAN-only mode?)");
            m_cipher_mode.store(CipherMode::plain);
        } else {
            const auto dec = aes128_cbc_decrypt_b64(raw, m_cipher_key);
            if (!dec) {
                LOG_D("mqtt: payload neither JSON nor AES-for-this-key, ignored");
                return; // puback already sent
            }
            if (mode != CipherMode::aes)
                LOG_I("mqtt: hybrid mode detected, payloads are AES-encrypted");
            m_cipher_mode.store(CipherMode::aes);
            payload = *dec;
        }
    }
    try {
        on_message(topic, payload);
    } catch (const std::exception& e) {
        LOG_E("mqtt: message handler threw: ", e.what());
    } catch (...) {
        LOG_E("mqtt: message handler threw an unknown exception");
    }
}

std::string MqttClient::run(const MessageFn& on_message)
{
    const int64_t ping_every_ms = static_cast<int64_t>(m_opts.keepalive_s) * 1000 / 2;
    int64_t       last_rx_ms    = now_ms();
    std::string   reason        = "connection closed by printer";

    while (!m_sock.cancelled()) {
        uint8_t     header    = 0;
        std::string body;
        bool        timed_out = false;
        if (!read_packet(&header, &body, 1000, &timed_out)) {
            if (!timed_out)
                break;
            const int64_t now = now_ms();
            if (now - m_last_tx_ms >= ping_every_ms)
                send_packet(kPingreq, {});
            if (now - last_rx_ms > static_cast<int64_t>(m_opts.keepalive_s) * 3000) {
                reason = "keepalive timeout";
                break;
            }
            continue;
        }
        last_rx_ms = now_ms();
        if ((header & 0xF0) == kPublish)
            handle_publish(header, body, on_message);
        // PINGRESP, SUBACK, PUBACK need no action.
    }
    m_connected = false;
    return m_sock.cancelled() ? std::string("stopped") : reason;
}

void MqttClient::stop() { m_sock.cancel(); }

void MqttClient::close()
{
    if (m_connected.exchange(false) && !m_sock.cancelled())
        send_packet(kDisconnect, {});
    m_sock.close();
}

} // namespace bbl_lan
