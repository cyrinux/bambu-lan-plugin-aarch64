#include "ftps_upload.hpp"

#include "../common/log.hpp"

#include <curl/curl.h>

#include <cstdio>
#include <memory>
#include <mutex>
#include <sys/stat.h>

namespace bbl_lan {
namespace {

struct FileCloser {
    void operator()(FILE* f) const { std::fclose(f); }
};

struct CurlGlobal {
    CurlGlobal() { curl_global_init(CURL_GLOBAL_DEFAULT); }
};

void ensure_curl_init()
{
    static std::once_flag once;
    std::call_once(once, [] { static CurlGlobal g; });
}

struct ProgressCtx {
    const FtpsProgressFn* progress;
    const FtpsCancelFn*   cancelled;
    int                   last_percent = -1;
};

int on_progress(void* user, curl_off_t, curl_off_t, curl_off_t ultotal, curl_off_t ulnow)
{
    auto* ctx = static_cast<ProgressCtx*>(user);
    if (ctx->cancelled != nullptr && *ctx->cancelled && (*ctx->cancelled)())
        return 1; // abort
    if (ultotal > 0 && ctx->progress != nullptr && *ctx->progress) {
        const int pct = static_cast<int>(ulnow * 100 / ultotal);
        if (pct != ctx->last_percent) {
            ctx->last_percent = pct;
            (*ctx->progress)(pct);
        }
    }
    return 0;
}

size_t discard(char*, size_t size, size_t nmemb, void*) { return size * nmemb; }

using CurlPtr = std::unique_ptr<CURL, decltype(&curl_easy_cleanup)>;

CurlPtr make_handle(const FtpsTarget& t, const std::string& remote_path)
{
    ensure_curl_init();
    CurlPtr h(curl_easy_init(), &curl_easy_cleanup);
    if (!h)
        return h;
    const std::string url = "ftps://" + t.host + ":" + std::to_string(t.port) + ftp_escape_path(remote_path);
    curl_easy_setopt(h.get(), CURLOPT_URL, url.c_str());
    curl_easy_setopt(h.get(), CURLOPT_USERNAME, t.username.c_str());
    curl_easy_setopt(h.get(), CURLOPT_PASSWORD, t.password.c_str());
    curl_easy_setopt(h.get(), CURLOPT_USE_SSL, static_cast<long>(CURLUSESSL_ALL));
    // Printer certificates are signed by Bambu's private CA.
    curl_easy_setopt(h.get(), CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(h.get(), CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(h.get(), CURLOPT_FTP_USE_EPSV, 0L); // printer only does PASV
    curl_easy_setopt(h.get(), CURLOPT_FTP_SKIP_PASV_IP, 1L);
    curl_easy_setopt(h.get(), CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(h.get(), CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(h.get(), CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(h.get(), CURLOPT_LOW_SPEED_TIME, 60L);
    return h;
}

FtpsError classify(CURLcode rc)
{
    switch (rc) {
    case CURLE_LOGIN_DENIED:
    case CURLE_REMOTE_ACCESS_DENIED: return FtpsError::Login;
    case CURLE_ABORTED_BY_CALLBACK: return FtpsError::Cancelled;
    default: return FtpsError::Upload;
    }
}

} // namespace

std::string ftp_escape_path(const std::string& path)
{
    static const char kHex[] = "0123456789ABCDEF";
    std::string       out;
    for (const unsigned char c : path) {
        if (std::isalnum(c) || c == '/' || c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(kHex[c >> 4]);
            out.push_back(kHex[c & 0x0F]);
        }
    }
    return out;
}

FtpsResult ftps_upload(const FtpsTarget& target, const std::string& local_path, const std::string& remote_path,
                       const FtpsProgressFn& progress, const FtpsCancelFn& cancelled)
{
    struct stat st{};
    if (::stat(local_path.c_str(), &st) != 0)
        return {FtpsError::FileNotFound, "file not found: " + local_path};
    std::unique_ptr<FILE, FileCloser> file(std::fopen(local_path.c_str(), "rb"), FileCloser{});
    if (!file)
        return {FtpsError::FileNotFound, "cannot open: " + local_path};

    CurlPtr h = make_handle(target, remote_path);
    if (!h)
        return {FtpsError::Upload, "curl_easy_init failed"};

    char        errbuf[CURL_ERROR_SIZE] = {0};
    ProgressCtx ctx{&progress, &cancelled};
    curl_easy_setopt(h.get(), CURLOPT_UPLOAD, 1L);
    curl_easy_setopt(h.get(), CURLOPT_READDATA, file.get());
    curl_easy_setopt(h.get(), CURLOPT_INFILESIZE_LARGE, static_cast<curl_off_t>(st.st_size));
    curl_easy_setopt(h.get(), CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(h.get(), CURLOPT_XFERINFOFUNCTION, &on_progress);
    curl_easy_setopt(h.get(), CURLOPT_XFERINFODATA, &ctx);
    curl_easy_setopt(h.get(), CURLOPT_ERRORBUFFER, errbuf);

    LOG_I("ftps: uploading ", local_path, " (", st.st_size, " bytes) to ", target.host, remote_path);
    const CURLcode rc = curl_easy_perform(h.get());
    if (rc != CURLE_OK) {
        const std::string msg = errbuf[0] ? errbuf : curl_easy_strerror(rc);
        LOG_E("ftps: upload failed: ", msg);
        return {classify(rc), msg};
    }
    if (progress)
        progress(100);
    return {};
}

FtpsResult ftps_check_login(const FtpsTarget& target)
{
    CurlPtr h = make_handle(target, "/");
    if (!h)
        return {FtpsError::Upload, "curl_easy_init failed"};
    char errbuf[CURL_ERROR_SIZE] = {0};
    curl_easy_setopt(h.get(), CURLOPT_DIRLISTONLY, 1L);
    curl_easy_setopt(h.get(), CURLOPT_WRITEFUNCTION, &discard);
    curl_easy_setopt(h.get(), CURLOPT_ERRORBUFFER, errbuf);
    curl_easy_setopt(h.get(), CURLOPT_TIMEOUT, 20L);
    const CURLcode rc = curl_easy_perform(h.get());
    if (rc != CURLE_OK)
        return {classify(rc), errbuf[0] ? errbuf : curl_easy_strerror(rc)};
    return {};
}

} // namespace bbl_lan
