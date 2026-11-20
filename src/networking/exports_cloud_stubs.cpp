// Cloud / account / telemetry entry points. This plugin is LAN-only: the
// slicer must see "not logged in, no server" and never block on these.
// Functions not exported at all are skipped by the slicer's null-checked
// wrappers; the ones below are exported because their default would be
// misleading or because they are called at start-up.
#include "abi_check.hpp"

#include "../common/log.hpp"

using namespace BBL;

extern "C" {

BBL_EXPORT int  bambu_network_connect_server(void*) { return BAMBU_NETWORK_ERR_CONNECTION_TO_SERVER_FAILED; }
BBL_EXPORT bool bambu_network_is_server_connected(void*) { return false; }
BBL_EXPORT int  bambu_network_refresh_connection(void*) { return 0; }
BBL_EXPORT int  bambu_network_start_subscribe(void*, std::string) { return 0; }
BBL_EXPORT int  bambu_network_stop_subscribe(void*, std::string) { return 0; }
BBL_EXPORT int  bambu_network_add_subscribe(void*, std::vector<std::string>) { return 0; }
BBL_EXPORT int  bambu_network_del_subscribe(void*, std::vector<std::string>) { return 0; }
BBL_EXPORT void bambu_network_enable_multi_machine(void*, bool) {}
BBL_EXPORT int  bambu_network_check_cert(void*) { return 0; }
BBL_EXPORT void bambu_network_install_device_cert(void*, std::string, bool) {}

BBL_EXPORT bool        bambu_network_is_user_login(void*) { return false; }
BBL_EXPORT int         bambu_network_user_logout(void*, bool) { return 0; }
BBL_EXPORT std::string bambu_network_get_user_id(void*) { return {}; }
BBL_EXPORT std::string bambu_network_get_user_name(void*) { return {}; }
BBL_EXPORT std::string bambu_network_get_user_avatar(void*) { return {}; }
BBL_EXPORT std::string bambu_network_get_user_nickanme(void*) { return {}; }
BBL_EXPORT int         bambu_network_report_consent(void*, std::string) { return 0; }
BBL_EXPORT std::string bambu_network_get_bambulab_host(void*) { return "https://bambulab.com"; }
BBL_EXPORT std::string bambu_network_get_user_selected_machine(void*) { return {}; }
BBL_EXPORT int         bambu_network_set_user_selected_machine(void*, std::string) { return 0; }
BBL_EXPORT std::string bambu_network_get_studio_info_url(void*) { return {}; }
BBL_EXPORT int         bambu_network_set_extra_http_header(void*, std::map<std::string, std::string>) { return 0; }

BBL_EXPORT int bambu_network_get_my_profile(void*, std::string, unsigned int* http_code, std::string* http_body)
{
    if (http_code != nullptr)
        *http_code = 401;
    if (http_body != nullptr)
        http_body->clear();
    return BAMBU_NETWORK_ERR_CONNECTION_TO_SERVER_FAILED;
}

BBL_EXPORT int bambu_network_post_device_region(void*, DeviceRegionParams, std::string* http_body)
{
    if (http_body != nullptr)
        http_body->clear();
    return 0;
}

BBL_EXPORT int bambu_network_get_minors_decisions(void*, const std::vector<std::string>& scene_codes,
                                                  std::vector<MinorsDecisionResult>* decisions)
{
    // No account: nothing to restrict. Report "allow" so local features work.
    if (decisions != nullptr) {
        decisions->clear();
        for (const auto& scene : scene_codes) {
            MinorsDecisionResult r;
            r.scene_code = scene;
            r.decision   = MinorDecision::Allow;
            decisions->push_back(r);
        }
    }
    return 0;
}

// Telemetry: accepted and dropped.
BBL_EXPORT int bambu_network_track_enable(void*, bool) { return 0; }
BBL_EXPORT int bambu_network_track_remove_files(void*) { return 0; }
BBL_EXPORT int bambu_network_track_event(void*, std::string, std::string) { return 0; }
BBL_EXPORT int bambu_network_track_header(void*, std::string) { return 0; }
BBL_EXPORT int bambu_network_track_update_property(void*, std::string, std::string, std::string) { return 0; }
BBL_EXPORT int bambu_network_track_get_property(void*, std::string, std::string& value, std::string)
{
    value.clear();
    return 0;
}

} // extern "C"

BBL_CHECK(connect_server)
BBL_CHECK(is_server_connected)
BBL_CHECK(refresh_connection)
BBL_CHECK(start_subscribe)
BBL_CHECK(stop_subscribe)
BBL_CHECK(add_subscribe)
BBL_CHECK(del_subscribe)
BBL_CHECK(enable_multi_machine)
BBL_CHECK(check_cert)
BBL_CHECK(install_device_cert)
BBL_CHECK(is_user_login)
BBL_CHECK(user_logout)
BBL_CHECK(get_user_id)
BBL_CHECK(get_user_name)
BBL_CHECK(get_user_avatar)
BBL_CHECK(get_user_nickanme)
BBL_CHECK(report_consent)
BBL_CHECK(get_bambulab_host)
BBL_CHECK(get_user_selected_machine)
BBL_CHECK(set_user_selected_machine)
BBL_CHECK(get_studio_info_url)
BBL_CHECK(set_extra_http_header)
BBL_CHECK(get_my_profile)
BBL_CHECK(post_device_region)
BBL_CHECK(get_minors_decisions)
BBL_CHECK(track_enable)
BBL_CHECK(track_remove_files)
BBL_CHECK(track_event)
BBL_CHECK(track_header)
BBL_CHECK(track_update_property)
BBL_CHECK(track_get_property)
