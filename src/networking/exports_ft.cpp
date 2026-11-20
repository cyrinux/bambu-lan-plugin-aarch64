// ft_* "file transfer tunnel" API (BambuStudio FileTransferUtils.hpp). The
// slicer calls most of these without null checks, so they must exist. This
// LAN plugin has no tunnel implementation: creation fails with FT_EIO, which
// makes the slicer fall back to FTPS (start_send_gcode_to_sdcard) paths.
#include "abi_check.hpp"

#include <cstdint>
#include <cstdlib>

extern "C" {

struct ft_job_result {
    int         ec;
    int         resp_ec;
    const char* json;
    const void* bin;
    uint32_t    bin_size;
};
struct ft_job_msg {
    int         kind;
    const char* json;
};
struct FT_TunnelHandle;
struct FT_JobHandle;
typedef enum { FT_OK = 0, FT_EINVAL = -1, FT_ESTATE = -2, FT_EIO = -3, FT_ETIMEOUT = -4, FT_ECANCELLED = -5 } ft_err;

typedef void (*ft_connect_cb)(void* user, int ok, int err, const char* msg);
typedef void (*ft_status_cb)(void* user, int old_status, int new_status, int err, const char* msg);
typedef void (*ft_result_cb)(void* user, ft_job_result result);
typedef void (*ft_msg_cb)(void* user, ft_job_msg msg);

BBL_EXPORT int  ft_abi_version() { return 1; }
BBL_EXPORT void ft_free(void* p) { std::free(p); }
BBL_EXPORT void ft_job_result_destroy(ft_job_result*) {}
BBL_EXPORT void ft_job_msg_destroy(ft_job_msg*) {}

BBL_EXPORT ft_err ft_tunnel_create(const char*, FT_TunnelHandle** out)
{
    if (out != nullptr)
        *out = nullptr;
    return FT_EIO;
}
BBL_EXPORT void   ft_tunnel_retain(FT_TunnelHandle*) {}
BBL_EXPORT void   ft_tunnel_release(FT_TunnelHandle*) {}
BBL_EXPORT ft_err ft_tunnel_start_connect(FT_TunnelHandle*, ft_connect_cb cb, void* user)
{
    if (cb != nullptr)
        cb(user, -1, FT_EIO, "file transfer tunnel not supported by LAN plugin");
    return FT_EIO;
}
BBL_EXPORT ft_err ft_tunnel_sync_connect(FT_TunnelHandle*) { return FT_EIO; }
BBL_EXPORT ft_err ft_tunnel_set_status_cb(FT_TunnelHandle*, ft_status_cb, void*) { return FT_EIO; }
BBL_EXPORT ft_err ft_tunnel_shutdown(FT_TunnelHandle*) { return FT_OK; }

BBL_EXPORT ft_err ft_job_create(const char*, FT_JobHandle** out)
{
    if (out != nullptr)
        *out = nullptr;
    return FT_EIO;
}
BBL_EXPORT void   ft_job_retain(FT_JobHandle*) {}
BBL_EXPORT void   ft_job_release(FT_JobHandle*) {}
BBL_EXPORT ft_err ft_job_set_result_cb(FT_JobHandle*, ft_result_cb, void*) { return FT_EIO; }
BBL_EXPORT ft_err ft_job_get_result(FT_JobHandle*, uint32_t, ft_job_result* out)
{
    if (out != nullptr)
        *out = ft_job_result{FT_EIO, 0, nullptr, nullptr, 0};
    return FT_EIO;
}
BBL_EXPORT ft_err ft_tunnel_start_job(FT_TunnelHandle*, FT_JobHandle*) { return FT_EIO; }
BBL_EXPORT ft_err ft_job_cancel(FT_JobHandle*) { return FT_OK; }
BBL_EXPORT ft_err ft_job_set_msg_cb(FT_JobHandle*, ft_msg_cb, void*) { return FT_EIO; }
BBL_EXPORT ft_err ft_job_try_get_msg(FT_JobHandle*, ft_job_msg*) { return FT_EIO; }
BBL_EXPORT ft_err ft_job_get_msg(FT_JobHandle*, uint32_t, ft_job_msg*) { return FT_EIO; }

} // extern "C"
