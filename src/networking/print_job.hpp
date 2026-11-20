// LAN print jobs: FTPS upload + MQTT "project_file" command.
#pragma once

#include "agent.hpp"
#include "bambu_networking.hpp"

#include <string>

namespace bbl_lan {

// Remote file name for an upload: sanitised project name + ".gcode.3mf".
std::string remote_file_name(const BBL::PrintParams& params);

// Builds the {"print":{"command":"project_file",...}} JSON.
std::string build_project_file_command(const BBL::PrintParams& params, const std::string& remote_name,
                                       const std::string& md5, const std::string& sequence_id);

// MD5 hex digest of a file, empty on error.
std::string file_md5(const std::string& path);

int run_local_print(Agent& agent, const BBL::PrintParams& params, const BBL::OnUpdateStatusFn& update_fn,
                    const BBL::WasCancelledFn& cancel_fn);
int run_send_to_storage(Agent& agent, const BBL::PrintParams& params, const BBL::OnUpdateStatusFn& update_fn,
                        const BBL::WasCancelledFn& cancel_fn);
int run_sdcard_print(Agent& agent, const BBL::PrintParams& params, const BBL::OnUpdateStatusFn& update_fn,
                     const BBL::WasCancelledFn& cancel_fn);

} // namespace bbl_lan
