// Drives the built plugins through dlopen/dlsym exactly like the slicer, for
// testing against a real printer without the GUI.
//
//   BBL_ACCESS_CODE=xxxx bbl_lan_probe <plugin_dir> detect  <ip>
//   BBL_ACCESS_CODE=xxxx bbl_lan_probe <plugin_dir> connect <ip> <serial>
//   BBL_ACCESS_CODE=xxxx bbl_lan_probe <plugin_dir> verify  <ip> <serial>
//   BBL_ACCESS_CODE=xxxx bbl_lan_probe <plugin_dir> camera  <ip> <serial> <out.jpg>
//   BBL_ACCESS_CODE=xxxx bbl_lan_probe <plugin_dir> camera_rtsp <ip>[:port] <out.h264>   (X1 and newer)
//   BBL_ACCESS_CODE=xxxx bbl_lan_probe <plugin_dir> sdprint <ip> <serial> <file on printer>  (STARTS A PRINT)
//   bbl_lan_probe <plugin_dir> version
#include "BambuTunnel.h"
#include "network_agent_abi.hpp"

#include <dlfcn.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>

using namespace bbl_abi;

namespace {

void* g_net = nullptr;
void* g_src = nullptr;

template <typename T> T sym(void* lib, const char* name)
{
    void* p = dlsym(lib, name);
    if (p == nullptr) {
        std::fprintf(stderr, "missing symbol %s\n", name);
        std::exit(2);
    }
    return reinterpret_cast<T>(p);
}

#define NET(name) sym<func_##name>(g_net, "bambu_network_" #name)

std::string access_code()
{
    const char* c = std::getenv("BBL_ACCESS_CODE");
    if (c == nullptr || *c == '\0') {
        std::fprintf(stderr, "set BBL_ACCESS_CODE\n");
        std::exit(2);
    }
    return c;
}

int cmd_detect(void* agent, const std::string& ip)
{
    BBL::detectResult r;
    const int rc = NET(bind_detect)(agent, ip, "secure", r);
    std::printf("bind_detect rc=%d dev_id=%s model=%s name=%s bind=%s type=%s\n", rc, r.dev_id.c_str(), r.model_id.c_str(),
                r.dev_name.c_str(), r.bind_state.c_str(), r.connect_type.c_str());
    return rc == 0 ? 0 : 1;
}

int cmd_connect(void* agent, const std::string& ip, const std::string& sn)
{
    std::mutex              m;
    std::condition_variable cv;
    int                     status   = -1;
    int                     messages = 0;
    std::string             first;
    NET(set_on_local_connect_fn)(agent, [&](int st, std::string dev, std::string msg) {
        std::lock_guard<std::mutex> l(m);
        status = st;
        std::printf("on_local_connect status=%d dev=%s msg=%s\n", st, dev.c_str(), msg.c_str());
        cv.notify_all();
    });
    NET(set_on_local_message_fn)(agent, [&](std::string, std::string msg) {
        std::lock_guard<std::mutex> l(m);
        if (messages++ == 0)
            first = msg.substr(0, 300);
        cv.notify_all();
    });
    NET(connect_printer)(agent, sn, ip, "bblp", access_code(), true);
    {
        std::unique_lock<std::mutex> l(m);
        cv.wait_for(l, std::chrono::seconds(15), [&] { return status >= 0; });
        if (status != 0)
            return 1;
    }
    const std::string pushall = R"({"pushing":{"sequence_id":"1","command":"pushall","version":1,"push_target":1}})";
    std::printf("send pushall rc=%d\n", NET(send_message_to_printer)(agent, sn, pushall, 0, 0));
    std::this_thread::sleep_for(std::chrono::seconds(6));
    std::lock_guard<std::mutex> l(m);
    std::printf("received %d report(s); first: %s...\n", messages, first.c_str());
    NET(disconnect_printer)(agent);
    return messages > 0 ? 0 : 1;
}

int cmd_verify(void* agent, const std::string& ip, const std::string& sn)
{
    BBL::PrintParams p{};
    p.dev_id          = sn;
    p.dev_ip          = ip;
    p.username        = "bblp";
    p.password        = access_code();
    p.project_name    = "verify_job";
    p.use_ssl_for_ftp = true;
    const int rc = NET(start_send_gcode_to_sdcard)(agent, p, nullptr, nullptr, nullptr);
    std::printf("verify_job rc=%d\n", rc);
    return rc == 0 ? 0 : 1;
}

// Prints a file already on the printer's storage (no upload): exercises the
// project_file command and the ack wait.
int cmd_sdprint(void* agent, const std::string& ip, const std::string& sn, const std::string& file)
{
    std::atomic<int> connected{-1};
    NET(set_on_local_connect_fn)(agent, [&](int st, std::string, std::string) { connected = st; });
    NET(set_on_local_message_fn)(agent, [](std::string, std::string) {});
    NET(connect_printer)(agent, sn, ip, "bblp", access_code(), true);
    for (int i = 0; i < 150 && connected < 0; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (connected != 0)
        return 1;
    BBL::PrintParams p{};
    p.dev_id      = sn;
    p.dev_ip      = ip;
    p.password    = access_code();
    p.dst_file    = file;
    p.task_name   = file;
    p.plate_index = 1;
    p.task_bed_type = "auto";
    const int rc = NET(start_sdcard_print)(agent, p, [](int stage, int code, std::string info) {
        std::printf("  update stage=%d code=%d info=%s\n", stage, code, info.c_str());
    }, [] { return false; });
    std::printf("start_sdcard_print rc=%d\n", rc);
    NET(disconnect_printer)(agent);
    return rc == 0 ? 0 : 1;
}

// Regression: disconnect while a connect is still in progress must neither
// crash nor block (the connect worker is cancelled within ~100 ms).
int cmd_cancel(void* agent, const std::string& ip)
{
    NET(set_on_local_connect_fn)(agent, [](int st, std::string, std::string msg) {
        std::printf("on_local_connect status=%d msg=%s\n", st, msg.c_str());
    });
    for (int i = 0; i < 3; ++i) {
        NET(connect_printer)(agent, "SERIAL", ip, "bblp", "code", true);
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        const auto t0 = std::chrono::steady_clock::now();
        NET(disconnect_printer)(agent);
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        std::printf("disconnect during connect took %lld ms\n", static_cast<long long>(ms));
        if (ms > 1000)
            return 1;
    }
    return 0;
}

int run_camera(const std::string& url, const std::string& out)
{
    using CreateFn = int (*)(Bambu_Tunnel*, const char*);
    using T1       = int (*)(Bambu_Tunnel);
    using StartFn  = int (*)(Bambu_Tunnel, bool);
    using InfoFn   = int (*)(Bambu_Tunnel, int, Bambu_StreamInfo*);
    using ReadFn   = int (*)(Bambu_Tunnel, Bambu_Sample*);
    using V1       = void (*)(Bambu_Tunnel);
    using LogSet   = void (*)(Bambu_Tunnel, Logger, void*);
    using FreeLog  = void (*)(tchar const*);

    static FreeLog free_log = sym<FreeLog>(g_src, "Bambu_FreeLogMsg");
    Bambu_Tunnel t = nullptr;
    if (sym<CreateFn>(g_src, "Bambu_Create")(&t, url.c_str()) != 0)
        return 1;
    sym<LogSet>(g_src, "Bambu_SetLogger")(t, [](void*, int level, tchar const* msg) {
        std::printf("  [bambu log %d] %s\n", level, msg);
        free_log(msg);
    }, nullptr);
    int rc = sym<T1>(g_src, "Bambu_Open")(t);
    std::printf("Bambu_Open rc=%d\n", rc);
    const auto start = std::chrono::steady_clock::now();
    if (rc == 0) {
        while ((rc = sym<StartFn>(g_src, "Bambu_StartStream")(t, true)) == Bambu_would_block &&
               std::chrono::steady_clock::now() - start < std::chrono::seconds(20))
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    std::printf("Bambu_StartStream rc=%d\n", rc);
    Bambu_StreamInfo info{};
    if (rc == 0 && (rc = sym<InfoFn>(g_src, "Bambu_GetStreamInfo")(t, 0, &info)) == 0)
        std::printf("stream: sub_type=%d %dx%d @%dfps\n", info.sub_type, info.format.video.width, info.format.video.height,
                    info.format.video.frame_rate);
    Bambu_Sample s{};
    int          frames = 0;
    const int want = info.sub_type == AVC1 ? 10 : 3;
    while (rc == 0 && frames < want) {
        const int r = sym<ReadFn>(g_src, "Bambu_ReadSample")(t, &s);
        if (r == Bambu_would_block) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        if (r != 0) {
            rc = r;
            break;
        }
        ++frames;
        std::printf("sample %d: %d bytes%s t=%llums\n", frames, s.size, (s.flags & f_sync) ? " sync" : "",
                    static_cast<unsigned long long>(s.decode_time / 10000));
        // JPEG: keep the last frame. H.264: append, so the file plays.
        if (FILE* f = std::fopen(out.c_str(), info.sub_type == AVC1 && frames > 1 ? "ab" : "wb")) {
            std::fwrite(s.buffer, 1, static_cast<size_t>(s.size), f);
            std::fclose(f);
        }
    }
    sym<V1>(g_src, "Bambu_Close")(t);
    sym<V1>(g_src, "Bambu_Destroy")(t);
    return frames > 0 ? 0 : 1;
}

// A1/P1 camera (TLS port 6000, JPEG).
int cmd_camera(const std::string& ip, const std::string& sn, const std::string& out)
{
    return run_camera("bambu:///local/" + ip + ".?port=6000&user=bblp&passwd=" + access_code() + "&device=" + sn +
                          "&net_ver=x&dev_ver=x&cli_id=probe&cli_ver=02.09.01.70",
                      out);
}

// X1 and newer (RTSPS, H.264). host may carry a port ("ip:8322"); default 322.
int cmd_camera_rtsp(const std::string& host, const std::string& out)
{
    // Same shape as MediaPlayCtrl builds it.
    return run_camera("bambu:///rtsps___bblp:" + access_code() + "@" + host +
                          "/streaming/live/1?proto=rtsps&device=probe&net_ver=x&dev_ver=x&cli_id=probe&cli_ver=02.09.01.70",
                      out);
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s <plugin_dir> version|detect|connect|verify|camera|camera_rtsp ...\n", argv[0]);
        return 2;
    }
    const std::string dir = argv[1];
    const std::string cmd = argv[2];
#ifdef BBL_TARGET_ORCA
    const char* net_lib = "/libbambu_networking_orca.so";
#else
    const char* net_lib = "/libbambu_networking.so";
#endif
    g_net = dlopen((dir + net_lib).c_str(), RTLD_LAZY);
    g_src = dlopen((dir + "/libBambuSource.so").c_str(), RTLD_LAZY);
    if (g_net == nullptr || g_src == nullptr) {
        std::fprintf(stderr, "dlopen failed: %s\n", dlerror());
        return 2;
    }
    std::printf("plugin version %s\n", NET(get_version)().c_str());
    if (cmd == "version")
        return 0;

    void* agent = NET(create_agent)(".");
    int   rc    = 2;
    if (cmd == "detect" && argc >= 4)
        rc = cmd_detect(agent, argv[3]);
    else if (cmd == "connect" && argc >= 5)
        rc = cmd_connect(agent, argv[3], argv[4]);
    else if (cmd == "verify" && argc >= 5)
        rc = cmd_verify(agent, argv[3], argv[4]);
    else if (cmd == "cancel" && argc >= 4)
        rc = cmd_cancel(agent, argv[3]);
    else if (cmd == "sdprint" && argc >= 6)
        rc = cmd_sdprint(agent, argv[3], argv[4], argv[5]);
    else if (cmd == "camera" && argc >= 6)
        rc = cmd_camera(argv[3], argv[4], argv[5]);
    else if (cmd == "camera_rtsp" && argc >= 5)
        rc = cmd_camera_rtsp(argv[3], argv[4]);
    NET(destroy_agent)(agent);
    std::printf("%s\n", rc == 0 ? "OK" : "FAILED");
    return rc;
}
