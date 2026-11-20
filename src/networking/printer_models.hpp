// Serial-number prefix → model table, from BambuStudio resources/printers/*.json
// ("sn_prefix", "model_id", "display_name", "ftp_folder").
#pragma once

#include <optional>
#include <string>

namespace bbl_lan {

struct PrinterModel {
    const char* sn_prefix;
    const char* model_id;
    const char* display_name;
    bool        sdcard_url; // project_file url uses file:///sdcard/ (else ftp:///)
};

std::optional<PrinterModel> model_from_serial(const std::string& serial);

// "file:///sdcard/<name>" or "ftp:///<name>" depending on the model.
std::string project_file_url(const std::string& serial, const std::string& remote_name);

} // namespace bbl_lan
