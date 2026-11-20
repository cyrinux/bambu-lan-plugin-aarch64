#include "detect.hpp"

#include "../common/log.hpp"
#include "../common/tls_socket.hpp"
#include "printer_models.hpp"

namespace bbl_lan {

int detect_printer(const std::string& ip, BBL::detectResult& out)
{
    TlsSocket         sock;
    const std::string err = sock.connect(ip, 8883, 5000);
    if (!err.empty()) {
        LOG_W("bind_detect ", ip, ": ", err);
        return -1;
    }
    const std::string serial = sock.peer_common_name();
    sock.close();
    const auto model = model_from_serial(serial);
    if (serial.empty() || !model) {
        LOG_W("bind_detect ", ip, ": unknown serial '", serial, "', falling back to manual entry");
        return -3;
    }
    out.result_msg   = "success";
    out.command      = "bind_detect";
    out.dev_id       = serial;
    out.model_id     = model->model_id;
    out.dev_name     = std::string(model->display_name) + " " + serial.substr(serial.size() > 4 ? serial.size() - 4 : 0);
    out.version      = "";
    out.bind_state   = "free";
    out.connect_type = "lan";
    LOG_I("bind_detect ", ip, ": ", serial, " (", model->model_id, ")");
    return 0;
}

} // namespace bbl_lan
