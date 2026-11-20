#include "printer_models.hpp"

namespace bbl_lan {
namespace {

// X1/P1/A1 families take file:///sdcard/ URLs (ha-bambulab LEGACY_SDCARD_PRINTERS);
// newer models take ftp:///.
constexpr PrinterModel kModels[] = {
    {"00M", "BL-P001", "Bambu Lab X1 Carbon", true},
    {"00W", "BL-P002", "Bambu Lab X1", true},
    {"03W", "C13", "Bambu Lab X1E", true},
    {"01S", "C11", "Bambu Lab P1P", true},
    {"01P", "C12", "Bambu Lab P1S", true},
    {"030", "N1", "Bambu Lab A1 mini", true},
    {"039", "N2S", "Bambu Lab A1", true},
    {"22E", "N7", "Bambu Lab P2S", false},
    {"26A", "N9", "Bambu Lab A2L", false},
    {"20P", "N6", "Bambu Lab X2D", false},
    {"094", "O1D", "Bambu Lab H2D", false},
    {"239", "O1E", "Bambu Lab H2D Pro", false},
    {"093", "O1S", "Bambu Lab H2S", false},
    {"31B", "O1C", "Bambu Lab H2C", false},
    {"32X", "N10", "Bambu Lab H3C", false},
    // Listed by Studio 02.09.01 under its model id only (unreleased name).
    {"35F", "N8", "Bambu Lab N8", false},
    // "31B" is shared by O1C and O1C2 (both H2C); detection reports O1C.
};

} // namespace

std::optional<PrinterModel> model_from_serial(const std::string& serial)
{
    for (const auto& m : kModels) {
        if (serial.rfind(m.sn_prefix, 0) == 0)
            return m;
    }
    return std::nullopt;
}

std::string project_file_url(const std::string& serial, const std::string& remote_name)
{
    const auto model = model_from_serial(serial);
    const bool sdcard = !model || model->sdcard_url;
    return (sdcard ? "file:///sdcard/" : "ftp:///") + remote_name;
}

} // namespace bbl_lan
