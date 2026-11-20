#include "tls_socket.hpp"

#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>

namespace bbl_lan {
namespace {

using Clock = std::chrono::steady_clock;

constexpr int kSliceMs = 100; // cancel latency for blocking waits

std::string ssl_error_string()
{
    unsigned long code = 0;
    unsigned long last = 0;
    while ((code = ERR_get_error()) != 0) // drain the queue, keep the last one
        last = code;
    if (last == 0)
        return std::strerror(errno);
    char buf[256];
    ERR_error_string_n(last, buf, sizeof(buf));
    return buf;
}

int ms_left(Clock::time_point deadline)
{
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
    return static_cast<int>(std::max<long long>(0, left));
}

// Waits for `events` in short slices so cancel() takes effect quickly.
IoResult wait_fd(int fd, short events, int timeout_ms, const std::atomic<bool>& cancel)
{
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
    for (;;) {
        if (cancel)
            return IoResult::Closed;
        const int left = ms_left(deadline);
        pollfd    pfd{fd, events, 0};
        const int rc = ::poll(&pfd, 1, std::min(left, kSliceMs));
        if (rc > 0)
            return (pfd.revents & (POLLERR | POLLNVAL)) ? IoResult::Error : IoResult::Ok;
        if (rc < 0 && errno != EINTR)
            return IoResult::Error;
        if (left == 0)
            return IoResult::Timeout;
    }
}

} // namespace

TlsSocket::~TlsSocket() { close(); }

int TlsSocket::connect_tcp(const std::string& host, uint16_t port, int timeout_ms, std::string* err)
{
    addrinfo hints{};
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo*         res = nullptr;
    const std::string port_str = std::to_string(port);
    const int         rc = getaddrinfo(host.c_str(), port_str.c_str(), &hints, &res);
    if (rc != 0) {
        *err = std::string("resolve failed: ") + gai_strerror(rc);
        return -1;
    }
    int fd = -1;
    for (addrinfo* ai = res; ai != nullptr && !m_cancel; ai = ai->ai_next) {
        fd = ::socket(ai->ai_family, ai->ai_socktype | SOCK_CLOEXEC | SOCK_NONBLOCK, ai->ai_protocol);
        if (fd < 0)
            continue;
        if (::connect(fd, ai->ai_addr, ai->ai_addrlen) == 0)
            break;
        if (errno == EINPROGRESS) {
            const IoResult w = wait_fd(fd, POLLOUT, timeout_ms, m_cancel);
            if (w == IoResult::Ok || w == IoResult::Error) {
                int       so_err = 0;
                socklen_t len    = sizeof(so_err);
                ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_err, &len);
                if (so_err == 0)
                    break;
                errno = so_err;
            } else {
                errno = w == IoResult::Timeout ? ETIMEDOUT : ECANCELED;
            }
        }
        *err = std::string("connect failed: ") + std::strerror(errno);
        ::close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd >= 0) {
        const int one = 1;
        ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        ::setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
    } else if (m_cancel) {
        *err = "cancelled";
    }
    return fd;
}

std::string TlsSocket::connect(const std::string& host, uint16_t port, int timeout_ms, const std::string& ca_file)
{
    close();
    if (m_cancel)
        return "cancelled";
    std::string err;
    const int   fd = connect_tcp(host, port, timeout_ms, &err);
    if (fd < 0)
        return err;

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_fd = fd;
        if (m_cancel) { // cancel() may have run before m_fd was published
            close_locked();
            return "cancelled";
        }
        ERR_clear_error();
        m_ctx = SSL_CTX_new(TLS_client_method());
        if (m_ctx == nullptr) {
            const std::string e = ssl_error_string();
            close_locked();
            return "SSL_CTX_new failed: " + e;
        }
        if (!ca_file.empty() && SSL_CTX_load_verify_locations(m_ctx, ca_file.c_str(), nullptr) == 1)
            SSL_CTX_set_verify(m_ctx, SSL_VERIFY_PEER, nullptr);
        else
            SSL_CTX_set_verify(m_ctx, SSL_VERIFY_NONE, nullptr);
        m_ssl = SSL_new(m_ctx);
        SSL_set_fd(m_ssl, m_fd);
        SSL_set_tlsext_host_name(m_ssl, host.c_str());
    }

    const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
    for (;;) {
        int rc = 0;
        int e  = SSL_ERROR_NONE;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            ERR_clear_error();
            rc = SSL_connect(m_ssl);
            if (rc != 1)
                e = SSL_get_error(m_ssl, rc);
        }
        if (rc == 1)
            return {};
        const short events = e == SSL_ERROR_WANT_READ ? POLLIN : e == SSL_ERROR_WANT_WRITE ? POLLOUT : 0;
        IoResult    w      = IoResult::Error;
        if (events != 0 && ms_left(deadline) > 0)
            w = wait_fd(fd, events, ms_left(deadline), m_cancel);
        if (w != IoResult::Ok) {
            const std::string msg = m_cancel ? "cancelled" : events == 0 ? ssl_error_string() : "timeout";
            close();
            return "TLS handshake failed: " + msg;
        }
    }
}

IoResult TlsSocket::write_all(const void* data, size_t len)
{
    const auto* p = static_cast<const unsigned char*>(data);
    while (len > 0) {
        int rc = 0;
        int e  = SSL_ERROR_NONE;
        int fd = -1;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_ssl == nullptr || m_cancel)
                return IoResult::Closed;
            fd = m_fd;
            ERR_clear_error();
            rc = SSL_write(m_ssl, p, static_cast<int>(len));
            if (rc <= 0)
                e = SSL_get_error(m_ssl, rc);
        }
        if (rc > 0) {
            p += rc;
            len -= static_cast<size_t>(rc);
            continue;
        }
        if (e == SSL_ERROR_WANT_WRITE || e == SSL_ERROR_WANT_READ) {
            const IoResult r = wait_fd(fd, e == SSL_ERROR_WANT_WRITE ? POLLOUT : POLLIN, 3000, m_cancel);
            if (r != IoResult::Ok)
                return r;
            continue;
        }
        return e == SSL_ERROR_ZERO_RETURN ? IoResult::Closed : IoResult::Error;
    }
    return IoResult::Ok;
}

IoResult TlsSocket::read_some(void* data, size_t len, size_t* got, int timeout_ms)
{
    *got = 0;
    for (;;) {
        int rc = 0;
        int e  = SSL_ERROR_NONE;
        int fd = -1;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_ssl == nullptr || m_cancel)
                return IoResult::Closed;
            fd = m_fd;
            ERR_clear_error();
            rc = SSL_read(m_ssl, data, static_cast<int>(len));
            if (rc <= 0)
                e = SSL_get_error(m_ssl, rc);
        }
        if (rc > 0) {
            *got = static_cast<size_t>(rc);
            return IoResult::Ok;
        }
        if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE) {
            const IoResult r = wait_fd(fd, e == SSL_ERROR_WANT_READ ? POLLIN : POLLOUT, timeout_ms, m_cancel);
            if (r != IoResult::Ok)
                return r;
            continue;
        }
        if (e == SSL_ERROR_ZERO_RETURN || (e == SSL_ERROR_SYSCALL && ERR_peek_error() == 0))
            return IoResult::Closed;
        return IoResult::Error;
    }
}

IoResult TlsSocket::read_exact(void* data, size_t len, int timeout_ms)
{
    auto* p = static_cast<unsigned char*>(data);
    while (len > 0) {
        size_t         got = 0;
        const IoResult r   = read_some(p, len, &got, timeout_ms);
        if (r != IoResult::Ok)
            return r;
        p += got;
        len -= got;
    }
    return IoResult::Ok;
}

std::string TlsSocket::peer_common_name()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_ssl == nullptr)
        return {};
    X509* cert = SSL_get1_peer_certificate(m_ssl);
    if (cert == nullptr)
        return {};
    char cn[256] = {0};
    X509_NAME_get_text_by_NID(X509_get_subject_name(cert), NID_commonName, cn, sizeof(cn));
    X509_free(cert);
    return cn;
}

void TlsSocket::cancel()
{
    m_cancel = true;
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_fd >= 0)
        ::shutdown(m_fd, SHUT_RDWR); // wakes up the owner's poll()
}

void TlsSocket::close()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    close_locked();
}

void TlsSocket::close_locked()
{
    if (m_ssl != nullptr) {
        SSL_free(m_ssl);
        m_ssl = nullptr;
    }
    if (m_ctx != nullptr) {
        SSL_CTX_free(m_ctx);
        m_ctx = nullptr;
    }
    if (m_fd >= 0) {
        ::close(m_fd);
        m_fd = -1;
    }
}

} // namespace bbl_lan
