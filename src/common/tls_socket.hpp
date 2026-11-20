// Blocking TLS client socket (OpenSSL) with timeouts and cross-thread cancel.
//
// Threading model: one *owner* thread calls connect()/read*/close(). Any
// thread may call write_all() (serialised with reads by an internal mutex) and
// cancel(). cancel() is sticky: it aborts an in-progress connect, wakes up
// blocked reads, and makes every later connect fail.
//
// Bambu printers present a certificate signed by Bambu's private CA with the
// serial number as CN. LAN tools conventionally skip verification; we do the
// same unless a CA bundle path is supplied.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>

typedef struct ssl_st     SSL;
typedef struct ssl_ctx_st SSL_CTX;

namespace bbl_lan {

enum class IoResult { Ok, Timeout, Closed, Error };

class TlsSocket {
public:
    TlsSocket() = default;
    ~TlsSocket();
    TlsSocket(const TlsSocket&)            = delete;
    TlsSocket& operator=(const TlsSocket&) = delete;

    // Owner thread. Returns an empty string on success, otherwise an error.
    std::string connect(const std::string& host, uint16_t port, int timeout_ms, const std::string& ca_file = {});

    // Any thread. Writes the whole buffer or fails.
    IoResult write_all(const void* data, size_t len);

    // Owner thread. Reads exactly len bytes, waiting at most timeout_ms per chunk.
    IoResult read_exact(void* data, size_t len, int timeout_ms);
    // Owner thread. Reads up to len bytes; *got receives the count.
    IoResult read_some(void* data, size_t len, size_t* got, int timeout_ms);

    // Common Name of the server certificate (Bambu printers: the serial).
    std::string peer_common_name();

    // Any thread: aborts connect/reads now and forever.
    void cancel();
    bool cancelled() const { return m_cancel.load(); }

    // Owner thread: releases the connection (cancel() is not reset).
    void close();

private:
    int  connect_tcp(const std::string& host, uint16_t port, int timeout_ms, std::string* err);
    void close_locked();

    std::atomic<bool> m_cancel{false};
    // Guards m_fd/m_ctx/m_ssl and every call into OpenSSL (SSL objects are
    // not safe for concurrent SSL_read/SSL_write). Waiting happens outside it.
    std::mutex m_mutex;
    int        m_fd  = -1;
    SSL_CTX*   m_ctx = nullptr;
    SSL*       m_ssl = nullptr;
};

} // namespace bbl_lan
