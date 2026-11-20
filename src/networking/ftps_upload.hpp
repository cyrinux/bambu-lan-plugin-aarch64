// Implicit-TLS FTP (FTPS, port 990) upload to a Bambu printer via libcurl.
// curl reuses the control channel's TLS session for the data channel, which
// the printer's vsftpd requires.
#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace bbl_lan {

struct FtpsTarget {
    std::string host;
    uint16_t    port = 990;
    std::string username = "bblp";
    std::string password;
};

enum class FtpsError { None, FileNotFound, Login, Upload, Cancelled };

struct FtpsResult {
    FtpsError   error = FtpsError::None;
    std::string message;
    bool        ok() const { return error == FtpsError::None; }
};

// percent is 0..100. Return true from cancelled() to abort the transfer.
using FtpsProgressFn = std::function<void(int percent)>;
using FtpsCancelFn   = std::function<bool()>;

// Uploads local_path to remote_path ("/<name>" or "/sdcard/<name>"; the A1
// FTP root is the SD card).
FtpsResult ftps_upload(const FtpsTarget& target, const std::string& local_path, const std::string& remote_path,
                       const FtpsProgressFn& progress, const FtpsCancelFn& cancelled);

// Logs in and lists the root directory: used to validate an access code.
FtpsResult ftps_check_login(const FtpsTarget& target);

// URL-encodes each path segment (keeps '/').
std::string ftp_escape_path(const std::string& path);

} // namespace bbl_lan
