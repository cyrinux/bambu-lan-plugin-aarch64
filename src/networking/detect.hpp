// "Add printer by IP" support: identifies a printer from the serial number
// in its MQTT TLS certificate, without needing the access code.
#pragma once

#include "bambu_networking.hpp"

#include <string>

namespace bbl_lan {

// Returns 0 and fills `out`, -1 when the printer is unreachable, or -3 when
// the model cannot be determined (the slicer then asks for serial + model).
int detect_printer(const std::string& ip, BBL::detectResult& out);

} // namespace bbl_lan
