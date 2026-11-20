// bambu_network_* entry points used for LAN operation. Signatures are checked
// against the slicer's function-pointer typedefs at compile time.
#include "abi_check.hpp"
#include "agent.hpp"
#include "detect.hpp"
#include "print_job.hpp"
#include "version.hpp"

#include "../common/log.hpp"

using namespace BBL;
using bbl_lan::Agent;

namespace {
Agent* as_agent(void* agent) { return static_cast<Agent*>(agent); }
} // namespace

#define WITH_AGENT(agent, fail_value)       \
    Agent* a = as_agent(agent);             \
    if (a == nullptr)                       \
        return fail_value;

extern "C" {

BBL_EXPORT bool bambu_network_check_debug_consistent(bool) { return true; }

BBL_EXPORT std::string bambu_network_get_version(void) { return BBL_LAN_PLUGIN_VERSION; }

BBL_EXPORT void* bambu_network_create_agent(std::string log_dir)
{
    bbl_lan::log_open(log_dir, "bambu_lan_plugin.log");
    LOG_I("create_agent: bambu-lan-plugin ", BBL_LAN_PLUGIN_VERSION, " (", BBL_LAN_PLUGIN_GIT, ")");
    return bbl_lan::guarded<void*>(__func__, nullptr, [&] { return new Agent(std::move(log_dir)); });
}

BBL_EXPORT int bambu_network_destroy_agent(void* agent)
{
    delete as_agent(agent);
    return 0;
}

BBL_EXPORT int bambu_network_init_log(void*) { return 0; }

BBL_EXPORT int bambu_network_set_config_dir(void* agent, std::string config_dir)
{
    WITH_AGENT(agent, BAMBU_NETWORK_ERR_INVALID_HANDLE);
    a->set_config_dir(config_dir);
    return 0;
}

BBL_EXPORT int bambu_network_set_cert_file(void*, std::string, std::string) { return 0; }
BBL_EXPORT int bambu_network_set_country_code(void*, std::string) { return 0; }
BBL_EXPORT int bambu_network_start(void* agent)
{
#ifdef BBL_TARGET_ORCA
    // Orca calls start_discovery only once, at start-up, and only if the
    // selected preset is a Bambu printer at that moment. Listen from start()
    // so switching to a Bambu preset later still finds printers.
    WITH_AGENT(agent, BAMBU_NETWORK_ERR_INVALID_HANDLE);
    bbl_lan::guarded(__func__, false, [&] { return a->start_discovery(true); });
#endif
    return 0;
}

#define DEFINE_SETTER(fn_name, type, field)                               \
    BBL_EXPORT int bambu_network_##fn_name(void* agent, type fn)          \
    {                                                                     \
        WITH_AGENT(agent, BAMBU_NETWORK_ERR_INVALID_HANDLE);              \
        a->update_callbacks([&](bbl_lan::Callbacks& c) { c.field = fn; }); \
        return 0;                                                         \
    }

DEFINE_SETTER(set_on_ssdp_msg_fn, OnMsgArrivedFn, on_ssdp_msg)
#ifndef BBL_TARGET_ORCA  // not in Orca 2.4 ABI
DEFINE_SETTER(set_on_user_login_fn, OnUserLoginFn, on_user_login)
#endif
DEFINE_SETTER(set_on_printer_connected_fn, OnPrinterConnectedFn, on_printer_connected)
DEFINE_SETTER(set_on_server_connected_fn, OnServerConnectedFn, on_server_connected)
DEFINE_SETTER(set_on_http_error_fn, OnHttpErrorFn, on_http_error)
DEFINE_SETTER(set_get_country_code_fn, GetCountryCodeFn, get_country_code)
DEFINE_SETTER(set_on_subscribe_failure_fn, GetSubscribeFailureFn, on_subscribe_failure)
DEFINE_SETTER(set_on_message_fn, OnMessageFn, on_message)
DEFINE_SETTER(set_on_user_message_fn, OnMessageFn, on_user_message)
DEFINE_SETTER(set_on_local_connect_fn, OnLocalConnectedFn, on_local_connect)
DEFINE_SETTER(set_on_local_message_fn, OnMessageFn, on_local_message)
DEFINE_SETTER(set_queue_on_main_fn, QueueOnMainFn, queue_on_main)
DEFINE_SETTER(set_server_callback, OnServerErrFn, on_server_err)

BBL_EXPORT bool bambu_network_start_discovery(void* agent, bool start, bool)
{
    WITH_AGENT(agent, false);
    return bbl_lan::guarded(__func__, false, [&] { return a->start_discovery(start); });
}

BBL_EXPORT int bambu_network_connect_printer(void* agent, std::string dev_id, std::string dev_ip, std::string username,
                                             std::string password, bool use_ssl)
{
    WITH_AGENT(agent, BAMBU_NETWORK_ERR_INVALID_HANDLE);
    return bbl_lan::guarded(__func__, BAMBU_NETWORK_ERR_CONNECT_FAILED,
                            [&] { return a->connect_printer(dev_id, dev_ip, username, password, use_ssl); });
}

BBL_EXPORT int bambu_network_disconnect_printer(void* agent)
{
    WITH_AGENT(agent, BAMBU_NETWORK_ERR_INVALID_HANDLE);
    return bbl_lan::guarded(__func__, BAMBU_NETWORK_ERR_DISCONNECT_FAILED, [&] { return a->disconnect_printers(); });
}

BBL_EXPORT int bambu_network_send_message_to_printer(void* agent, std::string dev_id, std::string json_str, int qos, int)
{
    WITH_AGENT(agent, BAMBU_NETWORK_ERR_INVALID_HANDLE);
    return bbl_lan::guarded(__func__, BAMBU_NETWORK_ERR_SEND_MSG_FAILED, [&] { return a->send_to_printer(dev_id, json_str, qos); });
}

BBL_EXPORT int bambu_network_bind_detect(void*, std::string dev_ip, std::string, detectResult& detect)
{
    return bbl_lan::guarded(__func__, -1, [&] { return bbl_lan::detect_printer(dev_ip, detect); });
}

BBL_EXPORT int bambu_network_start_local_print(void* agent, PrintParams params, OnUpdateStatusFn update_fn,
                                               WasCancelledFn cancel_fn)
{
    WITH_AGENT(agent, BAMBU_NETWORK_ERR_INVALID_HANDLE);
    return bbl_lan::guarded(__func__, BAMBU_NETWORK_ERR_PRINT_LP_UPLOAD_FTP_FAILED, [&] { return bbl_lan::run_local_print(*a, params, update_fn, cancel_fn); });
}

// "With record" also creates a cloud task record; LAN-only prints locally.
BBL_EXPORT int bambu_network_start_local_print_with_record(void* agent, PrintParams params, OnUpdateStatusFn update_fn,
                                                           WasCancelledFn cancel_fn, OnWaitFn)
{
    WITH_AGENT(agent, BAMBU_NETWORK_ERR_INVALID_HANDLE);
    return bbl_lan::guarded(__func__, BAMBU_NETWORK_ERR_PRINT_LP_UPLOAD_FTP_FAILED, [&] { return bbl_lan::run_local_print(*a, params, update_fn, cancel_fn); });
}

BBL_EXPORT int bambu_network_start_send_gcode_to_sdcard(void* agent, PrintParams params, OnUpdateStatusFn update_fn,
                                                        WasCancelledFn cancel_fn, OnWaitFn)
{
    WITH_AGENT(agent, BAMBU_NETWORK_ERR_INVALID_HANDLE);
    return bbl_lan::guarded(__func__, BAMBU_NETWORK_ERR_PRINT_SG_UPLOAD_FTP_FAILED, [&] { return bbl_lan::run_send_to_storage(*a, params, update_fn, cancel_fn); });
}

BBL_EXPORT int bambu_network_start_sdcard_print(void* agent, PrintParams params, OnUpdateStatusFn update_fn,
                                                WasCancelledFn cancel_fn)
{
    WITH_AGENT(agent, BAMBU_NETWORK_ERR_INVALID_HANDLE);
    return bbl_lan::guarded(__func__, BAMBU_NETWORK_ERR_PRINT_LP_PUBLISH_MSG_FAILED, [&] { return bbl_lan::run_sdcard_print(*a, params, update_fn, cancel_fn); });
}

} // extern "C"

BBL_CHECK(check_debug_consistent)
BBL_CHECK(get_version)
BBL_CHECK(create_agent)
BBL_CHECK(destroy_agent)
BBL_CHECK(init_log)
BBL_CHECK(set_config_dir)
BBL_CHECK(set_cert_file)
BBL_CHECK(set_country_code)
BBL_CHECK(start)
BBL_CHECK(set_on_ssdp_msg_fn)
#ifndef BBL_TARGET_ORCA
BBL_CHECK(set_on_user_login_fn)
#endif
BBL_CHECK(set_on_printer_connected_fn)
BBL_CHECK(set_on_server_connected_fn)
BBL_CHECK(set_on_http_error_fn)
BBL_CHECK(set_get_country_code_fn)
BBL_CHECK(set_on_subscribe_failure_fn)
BBL_CHECK(set_on_message_fn)
BBL_CHECK(set_on_user_message_fn)
BBL_CHECK(set_on_local_connect_fn)
BBL_CHECK(set_on_local_message_fn)
BBL_CHECK(set_queue_on_main_fn)
BBL_CHECK(set_server_callback)
BBL_CHECK(start_discovery)
BBL_CHECK(connect_printer)
BBL_CHECK(disconnect_printer)
BBL_CHECK(send_message_to_printer)
BBL_CHECK(bind_detect)
BBL_CHECK(start_local_print)
BBL_CHECK(start_local_print_with_record)
BBL_CHECK(start_send_gcode_to_sdcard)
BBL_CHECK(start_sdcard_print)
