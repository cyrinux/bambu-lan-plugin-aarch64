#include "print_job.hpp"

#include "../common/log.hpp"
#include "ftps_upload.hpp"
#include "printer_models.hpp"

#include <nlohmann/json.hpp>
#include <openssl/evp.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <memory>
#include <mutex>

namespace bbl_lan {
namespace {

struct FileCloser {
    void operator()(FILE* f) const { std::fclose(f); }
};

using BBL::PrintParams;
using json = nlohmann::json;

constexpr int kAckTimeoutS = 15;

void report(const BBL::OnUpdateStatusFn& fn, int stage, int code, const std::string& info = {})
{
    if (fn)
        fn(stage, code, info);
}

bool is_cancelled(const BBL::WasCancelledFn& fn) { return fn && fn(); }

FtpsTarget target_from(const PrintParams& p)
{
    FtpsTarget t;
    t.host     = p.dev_ip;
    t.username = p.username.empty() ? "bblp" : p.username;
    t.password = p.password;
    return t;
}

// Parses a JSON array string coming from the slicer; null when absent/invalid.
json parse_array(const std::string& s)
{
    if (s.empty())
        return nullptr;
    json j = json::parse(s, nullptr, false);
    return j.is_array() ? j : json(nullptr);
}

std::string sanitize(const std::string& name)
{
    std::string out;
    for (const char c : name) {
        const bool bad = c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' ||
                         c == '>' || c == '|' || static_cast<unsigned char>(c) < 0x20;
        out.push_back(bad ? '_' : c);
    }
    while (!out.empty() && (out.front() == '.' || out.front() == ' '))
        out.erase(out.begin());
    if (out.empty())
        return "studio_print";
    // Truncate to at most 120 bytes without splitting a UTF-8 sequence.
    size_t cut = std::min<size_t>(out.size(), 120);
    while (cut > 0 && cut < out.size() && (static_cast<unsigned char>(out[cut]) & 0xC0) == 0x80)
        --cut;
    return out.substr(0, cut);
}

std::string str_field(const json& obj, const char* key)
{
    const auto it = obj.find(key);
    return it != obj.end() && it->is_string() ? it->get<std::string>() : std::string{};
}

bool ends_with(const std::string& s, const std::string& suffix)
{
    return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// Publishes the command and waits for the printer's project_file ack.
// Returns 0, or BAMBU_NETWORK_ERR_PRINT_LP_PUBLISH_MSG_FAILED.
int publish_and_wait_ack(Agent& agent, const PrintParams& params, const std::string& cmd, const std::string& seq,
                         const BBL::WasCancelledFn& cancel_fn)
{
    struct AckState {
        std::mutex              m;
        std::condition_variable cv;
        bool                    done      = false;
        bool                    abandoned = false;
        std::string             result;
        std::string             reason;
    };
    auto state = std::make_shared<AckState>();
    agent.add_report_watcher(params.dev_id, [state, seq](const std::string& payload) {
        {
            std::lock_guard<std::mutex> lock(state->m);
            if (state->abandoned)
                return false;
        }
        const json j = json::parse(payload, nullptr, false);
        if (!j.is_object())
            return true;
        const auto it = j.find("print");
        if (it == j.end() || !it->is_object())
            return true;
        if (str_field(*it, "command") != "project_file" || str_field(*it, "sequence_id") != seq)
            return true;
        std::lock_guard<std::mutex> lock(state->m);
        state->done   = true;
        state->result = str_field(*it, "result");
        state->reason = str_field(*it, "reason");
        state->cv.notify_all();
        return false;
    });
    const auto abandon = [&state] {
        std::lock_guard<std::mutex> lock(state->m);
        state->abandoned = true;
    };

    if (agent.send_to_printer(params.dev_id, cmd, 1) != 0) {
        abandon();
        return BAMBU_NETWORK_ERR_PRINT_LP_PUBLISH_MSG_FAILED;
    }

    std::unique_lock<std::mutex> lock(state->m);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(kAckTimeoutS);
    while (!state->done && std::chrono::steady_clock::now() < deadline && !is_cancelled(cancel_fn))
        state->cv.wait_for(lock, std::chrono::milliseconds(250));
    if (!state->done) {
        state->abandoned = true;
        if (is_cancelled(cancel_fn)) {
            // The command is already sent: the print may still start.
            LOG_W("print: cancelled while waiting for the printer's ack");
            return BAMBU_NETWORK_ERR_CANCELED;
        }
        // Older firmware does not always ack; the push status will show the job.
        LOG_W("print: no project_file ack within ", kAckTimeoutS, "s, assuming accepted");
        return 0;
    }
    if (state->result == "fail" || state->result == "failed") {
        LOG_E("print: printer rejected project_file: ", state->reason);
        return BAMBU_NETWORK_ERR_PRINT_LP_PUBLISH_MSG_FAILED;
    }
    LOG_I("print: printer accepted project_file (", state->result, ")");
    return 0;
}

int upload_with_progress(const PrintParams& params, const std::string& remote_name, const BBL::OnUpdateStatusFn& update_fn,
                         const BBL::WasCancelledFn& cancel_fn, int upload_error)
{
    report(update_fn, BBL::PrintingStageUpload, 0);
    const FtpsResult r = ftps_upload(
        target_from(params), params.filename, "/" + remote_name,
        [&](int pct) { report(update_fn, BBL::PrintingStageUpload, pct); }, [&] { return is_cancelled(cancel_fn); });
    switch (r.error) {
    case FtpsError::None: return 0;
    case FtpsError::Cancelled: return BAMBU_NETWORK_ERR_CANCELED;
    case FtpsError::FileNotFound: return BAMBU_NETWORK_ERR_FILE_NOT_EXIST;
    default:
        report(update_fn, BBL::PrintingStageERROR, upload_error, r.message);
        return upload_error;
    }
}

int start_print_from_storage(Agent& agent, const PrintParams& params, const std::string& remote_name, const std::string& md5,
                             const BBL::OnUpdateStatusFn& update_fn, const BBL::WasCancelledFn& cancel_fn)
{
    report(update_fn, BBL::PrintingStageSending, 0);
    const std::string seq = agent.next_sequence_id();
    const std::string cmd = build_project_file_command(params, remote_name, md5, seq);
    LOG_I("print: project_file ", remote_name, " plate ", params.plate_index, " seq ", seq);
    const int rc = publish_and_wait_ack(agent, params, cmd, seq, cancel_fn);
    if (rc != 0) {
        report(update_fn, BBL::PrintingStageERROR, rc, "printer did not accept the job");
        return rc;
    }
    report(update_fn, BBL::PrintingStageFinished, 0, "3");
    return 0;
}

} // namespace

std::string remote_file_name(const PrintParams& params)
{
    if (!params.ftp_file.empty())
        return sanitize(params.ftp_file);
    std::string base = sanitize(!params.project_name.empty() ? params.project_name : params.task_name);
    if (!ends_with(base, ".3mf"))
        base += ".gcode.3mf";
    return base;
}

std::string build_project_file_command(const PrintParams& params, const std::string& remote_name, const std::string& md5,
                                       const std::string& sequence_id)
{
    const int plate = params.plate_index > 0 ? params.plate_index : 1;
    json      p     = {
        {"sequence_id", sequence_id},
        {"command", "project_file"},
        {"param", "Metadata/plate_" + std::to_string(plate) + ".gcode"},
        {"url", project_file_url(params.dev_id, remote_name)},
        {"file", remote_name},
        {"md5", md5},
        {"subtask_name", params.task_name.empty() ? remote_name : params.task_name},
        {"project_id", "0"},
        {"profile_id", "0"},
        {"task_id", "0"},
        {"subtask_id", "0"},
        {"bed_type", params.task_bed_type.empty() ? "auto" : params.task_bed_type},
        {"timelapse", params.task_record_timelapse},
        {"bed_leveling", params.task_bed_leveling},
        {"auto_bed_leveling", params.auto_bed_leveling},
        {"flow_cali", params.task_flow_cali},
        {"extrude_cali_flag", params.auto_flow_cali},
        {"vibration_cali", params.task_vibration_cali},
        {"layer_inspect", params.task_layer_inspect},
        {"use_ams", params.task_use_ams},
    };
    const json ams  = parse_array(params.ams_mapping);
    const json ams2 = parse_array(params.ams_mapping2);
    if (!ams.is_null())
        p["ams_mapping"] = ams;
    if (!ams2.is_null())
        p["ams_mapping2"] = ams2;
    const json nozzle = parse_array(params.nozzle_mapping);
    if (!nozzle.is_null())
        p["nozzle_mapping"] = nozzle;
    return json{{"print", p}}.dump(-1, ' ', false, json::error_handler_t::replace);
}

std::string file_md5(const std::string& path)
{
    std::unique_ptr<FILE, FileCloser> f(std::fopen(path.c_str(), "rb"), FileCloser{});
    if (!f)
        return {};
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> ctx(EVP_MD_CTX_new(), &EVP_MD_CTX_free);
    if (!ctx || EVP_DigestInit_ex(ctx.get(), EVP_md5(), nullptr) != 1)
        return {};
    unsigned char buf[64 * 1024];
    size_t        n = 0;
    while ((n = std::fread(buf, 1, sizeof(buf), f.get())) > 0)
        EVP_DigestUpdate(ctx.get(), buf, n);
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int  len = 0;
    EVP_DigestFinal_ex(ctx.get(), digest, &len);
    static const char kHex[] = "0123456789abcdef";
    std::string       out;
    for (unsigned int i = 0; i < len; ++i) {
        out.push_back(kHex[digest[i] >> 4]);
        out.push_back(kHex[digest[i] & 0x0F]);
    }
    return out;
}

int run_local_print(Agent& agent, const PrintParams& params, const BBL::OnUpdateStatusFn& update_fn,
                    const BBL::WasCancelledFn& cancel_fn)
{
    LOG_I("print: start_local_print dev=", params.dev_id, " ip=", params.dev_ip, " file=", params.filename);
    report(update_fn, BBL::PrintingStageCreate, 0);
    if (!agent.is_printer_connected(params.dev_id))
        LOG_W("print: printer ", params.dev_id, " has no MQTT session; command will fail");

    const std::string md5 = file_md5(params.filename);
    if (md5.empty())
        return BAMBU_NETWORK_ERR_FILE_NOT_EXIST;
    const std::string remote = remote_file_name(params);
    const int         up     = upload_with_progress(params, remote, update_fn, cancel_fn, BAMBU_NETWORK_ERR_PRINT_LP_UPLOAD_FTP_FAILED);
    if (up != 0)
        return up;
    if (is_cancelled(cancel_fn))
        return BAMBU_NETWORK_ERR_CANCELED;
    return start_print_from_storage(agent, params, remote, md5, update_fn, cancel_fn);
}

int run_send_to_storage(Agent&, const PrintParams& params, const BBL::OnUpdateStatusFn& update_fn,
                        const BBL::WasCancelledFn& cancel_fn)
{
    // The slicer validates IP + access code with a "verify_job" before any
    // real transfer: an FTPS login is enough and leaves no file behind.
    if (params.project_name == "verify_job") {
        const FtpsResult r = ftps_check_login(target_from(params));
        LOG_I("print: access code check on ", params.dev_ip, ": ", r.ok() ? "ok" : r.message);
        return r.ok() ? 0 : BAMBU_NETWORK_ERR_PRINT_SG_UPLOAD_FTP_FAILED;
    }
    LOG_I("print: send to storage dev=", params.dev_id, " file=", params.filename);
    report(update_fn, BBL::PrintingStageCreate, 0);
    const std::string remote = remote_file_name(params);
    const int up = upload_with_progress(params, remote, update_fn, cancel_fn, BAMBU_NETWORK_ERR_PRINT_SG_UPLOAD_FTP_FAILED);
    if (up != 0)
        return up;
    report(update_fn, BBL::PrintingStageFinished, 0, "3");
    return 0;
}

int run_sdcard_print(Agent& agent, const PrintParams& params, const BBL::OnUpdateStatusFn& update_fn,
                     const BBL::WasCancelledFn& cancel_fn)
{
    std::string remote = !params.dst_file.empty() ? params.dst_file : params.ftp_file;
    while (!remote.empty() && remote.front() == '/')
        remote.erase(remote.begin());
    if (remote.rfind("sdcard/", 0) == 0)
        remote = remote.substr(7);
    if (remote.empty())
        return BAMBU_NETWORK_ERR_FILE_NOT_EXIST;
    LOG_I("print: start_sdcard_print dev=", params.dev_id, " file=", remote);
    report(update_fn, BBL::PrintingStageCreate, 0);
    return start_print_from_storage(agent, params, remote, params.ftp_file_md5, update_fn, cancel_fn);
}

} // namespace bbl_lan
