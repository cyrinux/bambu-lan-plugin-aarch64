// Dependency-free unit tests for the protocol helpers.
#include "networking/print_job.hpp"
#include "networking/printer_models.hpp"
#include "networking/ftps_upload.hpp"
#include "networking/ssdp_listener.hpp"
#include "source/bambu_url.hpp"
#include "source/camera_stream.hpp"

#include <nlohmann/json.hpp>

#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace {

int g_failures = 0;
int g_checks   = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        ++g_checks;                                                              \
        if (!(cond)) {                                                           \
            ++g_failures;                                                        \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                        \
    } while (0)

#define CHECK_EQ(a, b) CHECK((a) == (b))

using json = nlohmann::json;
using namespace bbl_lan;

void test_url_parsing()
{
    const auto u = parse_local_camera_url(
        "bambu:///local/192.168.1.50.?port=6000&user=bblp&passwd=ab%2Fcd12&device=03900A000000001&net_ver=02.09.01.99"
        "&dev_ver=01.06.00.00&cli_id=x&cli_ver=02.09.01.70&play_start_ms=1");
    CHECK(u.has_value());
    CHECK_EQ(u->host, "192.168.1.50");
    CHECK_EQ(u->port, 6000);
    CHECK_EQ(u->user, "bblp");
    CHECK_EQ(u->password, "ab/cd12");
    CHECK_EQ(u->device, "03900A000000001");

    const auto short_form = parse_local_camera_url("bambu:///local/10.0.0.5?port=6000&user=bblp&passwd=1234");
    CHECK(short_form.has_value());
    CHECK_EQ(short_form->host, "10.0.0.5");
    CHECK_EQ(short_form->password, "1234");

    CHECK(!parse_local_camera_url("bambu:///tutk?uid=abc").has_value());
    CHECK(!parse_local_camera_url("bambu:///rtsps___bblp:x@1.2.3.4/streaming/live/1?proto=rtsps").has_value());
    CHECK(!parse_local_camera_url("bambu:///local/1.2.3.4?port=99999").has_value());
}

void test_camera_auth_packet()
{
    const auto a = build_camera_auth("bblp", "12345678");
    CHECK_EQ(a.size(), 80u);
    CHECK_EQ(a[0], 0x40);
    CHECK_EQ(a[4], 0x00);
    CHECK_EQ(a[5], 0x30);
    CHECK_EQ(a[16], 'b');
    CHECK_EQ(a[19], 'p');
    CHECK_EQ(a[20], 0);
    CHECK_EQ(a[48], '1');
    CHECK_EQ(a[55], '8');
    CHECK_EQ(a[56], 0);
}

void test_jpeg_size()
{
    // SOI, APP0 (len 16), SOF0 with 1280x720.
    std::vector<uint8_t> j = {0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x10};
    j.insert(j.end(), 14, 0x00);
    const uint8_t sof[] = {0xFF, 0xC0, 0x00, 0x11, 0x08, 0x02, 0xD0, 0x05, 0x00, 0x03};
    j.insert(j.end(), std::begin(sof), std::end(sof));
    j.insert(j.end(), 12, 0x00);
    const JpegSize s = parse_jpeg_size(j.data(), j.size());
    CHECK_EQ(s.width, 1280);
    CHECK_EQ(s.height, 720);

    const uint8_t junk[] = {0x00, 0x01, 0x02};
    CHECK_EQ(parse_jpeg_size(junk, sizeof(junk)).width, 0);
}

void test_ssdp()
{
    const std::string dgram =
        "NOTIFY * HTTP/1.1\r\nHOST: 239.255.255.250:1990\r\nServer: UPnP/1.0\r\nLocation: 192.168.1.50\r\n"
        "NT: urn:bambulab-com:device:3dprinter:1\r\nUSN: 03900A000000001\r\nCache-Control: max-age=1800\r\n"
        "DevModel.bambu.com: N2S\r\nDevName.bambu.com: A1\r\nDevSignal.bambu.com: -44\r\n"
        "DevConnect.bambu.com: lan\r\nDevBind.bambu.com: free\r\nDevseclink.bambu.com: secure\r\n\r\n";
    const json j = json::parse(ssdp_to_device_json(parse_ssdp_headers(dgram), "192.168.1.50"));
    CHECK_EQ(j["dev_id"], "03900A000000001");
    CHECK_EQ(j["dev_ip"], "192.168.1.50");
    CHECK_EQ(j["dev_type"], "N2S");
    CHECK_EQ(j["dev_name"], "A1");
    CHECK_EQ(j["dev_signal"], "-44dBm");
    CHECK_EQ(j["connect_type"], "lan");
    CHECK_EQ(j["bind_state"], "free");
    CHECK_EQ(j["sec_link"], "secure");
    CHECK(!j.contains("connection_name"));
    for (const auto& [k, v] : j.items())
        CHECK(v.is_string());

    CHECK(ssdp_to_device_json(parse_ssdp_headers("NOTIFY * HTTP/1.1\r\nNT: upnp:rootdevice\r\nUSN: x\r\n"), "1.1.1.1").empty());
}

void test_models()
{
    CHECK_EQ(std::string(model_from_serial("03900A000000001")->model_id), "N2S");
    CHECK_EQ(std::string(model_from_serial("0300AA")->model_id), "N1");
    CHECK(!model_from_serial("ZZZ").has_value());
    CHECK_EQ(project_file_url("03900A000000001", "a.gcode.3mf"), "file:///sdcard/a.gcode.3mf");
    CHECK_EQ(project_file_url("22E000", "a.gcode.3mf"), "ftp:///a.gcode.3mf");
}

void test_project_file()
{
    BBL::PrintParams p{};
    p.dev_id                = "03900A000000001";
    p.project_name          = "Benchy / test";
    p.task_name             = "Benchy / test";
    p.plate_index           = 2;
    p.task_bed_leveling     = true;
    p.task_flow_cali        = false;
    p.task_vibration_cali   = false;
    p.task_layer_inspect    = true;
    p.task_record_timelapse = false;
    p.task_use_ams          = true;
    p.task_bed_type         = "textured_plate";
    p.auto_bed_leveling     = 2;
    p.auto_flow_cali        = 0;
    p.ams_mapping           = "[0,-1,3]";
    p.ams_mapping2          = "[{\"ams_id\":0,\"slot_id\":0}]";

    const std::string name = remote_file_name(p);
    CHECK_EQ(name, "Benchy _ test.gcode.3mf");

    const json j = json::parse(build_project_file_command(p, name, "abc", "20001"));
    const json& c = j["print"];
    CHECK_EQ(c["command"], "project_file");
    CHECK_EQ(c["sequence_id"], "20001");
    CHECK_EQ(c["param"], "Metadata/plate_2.gcode");
    CHECK_EQ(c["url"], "file:///sdcard/Benchy _ test.gcode.3mf");
    CHECK_EQ(c["md5"], "abc");
    CHECK_EQ(c["bed_leveling"], true);
    CHECK_EQ(c["auto_bed_leveling"], 2);
    CHECK_EQ(c["use_ams"], true);
    CHECK_EQ(c["ams_mapping"], json::parse("[0,-1,3]"));
    CHECK_EQ(c["ams_mapping2"][0]["slot_id"], 0);
    CHECK_EQ(c["bed_type"], "textured_plate");
    CHECK_EQ(c["subtask_id"], "0");

    p.ams_mapping = "not json";
    const json j2 = json::parse(build_project_file_command(p, name, "", "1"));
    CHECK(!j2["print"].contains("ams_mapping"));

    BBL::PrintParams q{};
    q.project_name = "x.gcode.3mf";
    CHECK_EQ(remote_file_name(q), "x.gcode.3mf");
}

void test_ftp_escape()
{
    CHECK_EQ(ftp_escape_path("/Benchy test.gcode.3mf"), "/Benchy%20test.gcode.3mf");
    CHECK_EQ(ftp_escape_path("/a#b"), "/a%23b");
}

} // namespace

int main()
{
    const std::vector<std::pair<const char*, std::function<void()>>> tests = {
        {"url_parsing", test_url_parsing}, {"camera_auth_packet", test_camera_auth_packet},
        {"jpeg_size", test_jpeg_size},     {"ssdp", test_ssdp},
        {"models", test_models},           {"project_file", test_project_file},
        {"ftp_escape", test_ftp_escape},
    };
    for (const auto& [name, fn] : tests) {
        const int before = g_failures;
        fn();
        std::printf("[%s] %s\n", g_failures == before ? " OK " : "FAIL", name);
    }
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
