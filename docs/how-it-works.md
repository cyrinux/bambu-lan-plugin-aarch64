# How this plugin was built

This explains the process: how Bambu Studio's closed plugins were understood
and how the replacement was written and verified, without disassembling
Bambu's binaries.

## 1. What the "driver" actually is

There is no kernel driver for Bambu printers. Bambu Studio (GPL/AGPL) delegates
everything that talks to a printer or to Bambu's cloud to two closed-source
shared libraries that it downloads on first start into
`<data_dir>/plugins/`:

| Library | Role |
|---|---|
| `libbambu_networking.so` | discovery, MQTT status/commands, FTPS upload, print jobs, cloud login, presets sync, MakerWorld… (~150 exported `bambu_network_*` / `ft_*` functions) |
| `libBambuSource.so` | camera streams and the printer file browser (`Bambu_*` C API) |
| `liblive555.so`, `libagora*.so` | RTSP (X1 camera) and Agora WebRTC (cloud camera), used by the two above |

Bambu's plugin API (`api.bambulab.com/v1/iot-service/api/slicer/resource`)
only serves `linux` (x86-64), `windows`, `windows_arm` and macOS builds. For
any arm64 Linux OS type it returns an empty resource list. An aarch64
process cannot `dlopen()` an x86-64 library, so on arm64 Linux the Device tab
shows "Network Plug-in is not detected".

## 2. Reading the contract from the open-source side

The libraries are closed, but **the caller is open source**. Everything the
slicer expects is visible in Bambu Studio's repository, pinned here to tag
`v02.09.01.70` (the flatpak 2.9.1 build):

- `src/slic3r/Utils/NetworkAgent.hpp` declares a function-pointer typedef for
  each of the 129 `bambu_network_*` functions the slicer `dlsym()`s.
  `NetworkAgent.cpp` shows that every call is null-checked, so a plugin may
  leave out functions it doesn't support.
- `src/slic3r/Utils/bambu_networking.hpp` holds the types crossing the boundary:
  `PrintParams`, callback `std::function` typedefs, error codes, enums.
- `src/slic3r/GUI/Printer/BambuTunnel.h` holds the `Bambu_*` camera API and its
  structs.
- `src/slic3r/Utils/FileTransferUtils.hpp` holds the `ft_*` tunnel API. These calls
  are *not* null-checked, so stubs must exist.

The ABI is a C++ one: `extern "C"` names, but `std::string`, `std::function`,
`std::map` and structs passed by value. That only works if the plugin uses
the same libstdc++ ABI as the slicer. This is why the plugin is compiled
inside the same flatpak SDK the slicer's runtime is built from (section 6). To
catch mistakes at compile time, the typedefs are extracted verbatim into
`third_party/bambu/network_agent_abi.hpp`, and every export is checked with
`static_assert(std::is_same_v<decltype(&bambu_network_x), func_x>)`.

### Loading rules (GUI_App.cpp, NetworkAgent.cpp, CertificateVerify.cpp)

- On Linux, `SummarizeSelf()`/`SummarizeModule()` return `nullopt`, so **no signature
  check** is done. Any library with the right name loads.
- `check_networking_version()` accepts the plugin if the first 8 characters of
  `bambu_network_get_version()` equal `SLIC3R_VERSION`'s (`"02.09.01"`).
- `libBambuSource.so` must also load, otherwise the plugin is marked
  incompatible.
- The OTA updater only replaces the plugin with a *newer* version. Reporting
  `02.09.01.99` keeps Studio from "updating" us back to the x86-64 build.

### Tracing each feature

For each feature, the call sites in the GUI were followed to answer: *which
plugin functions are called, with which arguments, on which thread, and how
are results and callbacks interpreted?* The results, with file:line
references, are in [`slicer-contract.md`](slicer-contract.md). Highlights:

- **Discovery:** the plugin passes a flat JSON object of strings (`dev_id`,
  `dev_ip`, `dev_type`, `connect_type`, `bind_state`…) to the SSDP callback.
  "Add by IP" calls `bind_detect()`, and returning `-3` falls back to manual entry.
- **Connection:** `connect_printer(sn, ip, "bblp", access_code, true)` is asynchronous.
  The result goes through `on_local_connect(status, sn, msg)`, and `msg == "5"` is how
  the GUI recognises a wrong access code.
- **Reports:** raw MQTT payloads from `device/<sn>/report` go to
  `on_local_message(sn, payload)` unchanged. Commands come back via
  `send_message_to_printer` with flag 0 (the slicer never signs anything).
- **Printing:** the slicer first validates the access code with a `verify_job`
  call to `start_send_gcode_to_sdcard`, then calls `start_local_print` with a
  sliced `.gcode.3mf`. The **slicer never builds the MQTT `project_file`
  command**: that is entirely the plugin's job.
- **Camera:** Linux uses `wxMediaCtrl3` + libavcodec. It wants `Bambu_StartStream`
  to return `would_block` until ready, a stream info of type `VIDE`/`MJPG` with
  width ≥ 320 and frame rate > 0, then one complete JPEG per `Bambu_ReadSample`.

## 3. The wire protocols

What the closed plugin does *on the network* is documented by the community
(OpenBambuAPI, ha-bambulab, bambu-connect), and was cross-checked against a
working Home Assistant `ha-bambulab` install:

| Channel | Protocol |
|---|---|
| Discovery | SSDP-like `NOTIFY` on UDP 2021/1990 with `DevModel.bambu.com`, `DevName…`, `DevConnect…`, `DevBind…` headers. `USN` is the serial |
| Status/control | MQTT 3.1.1 over TLS on 8883, user `bblp`, password = LAN access code, topics `device/<sn>/report` and `device/<sn>/request` |
| Files | implicit-TLS FTP (FTPS) on 990. The printer requires TLS session reuse on the data channel, which libcurl does |
| Start print | MQTT `{"print":{"command":"project_file","param":"Metadata/plate_N.gcode","url":"file:///sdcard/<file>",…}}`. X1/P1/A1 take `file:///sdcard/`, newer models `ftp:///` |
| A1/P1 camera | TLS on 6000, 80-byte auth (`0x40`, `0x3000`, 0, 0, user[32], code[32]), then frames of a 16-byte header (u32le size, …) + one JPEG |
| Identity | the printer's TLS certificate CN is its serial. `bind_detect` reads it and maps the serial prefix to a model with Studio's own `resources/printers/*.json` (`sn_prefix`) |

## 4. Implementation

Small C++17 libraries with only OpenSSL and libcurl as dependencies, both
present in the flatpak runtime:

- `src/common/tls_socket.*`: non-blocking OpenSSL client. One owner thread does
  connect and reads; any thread may write or `cancel()`. Every wait is sliced at
  100 ms so cancellation is fast.
- `src/networking/mqtt_client.*`: minimal MQTT 3.1.1 client: CONNECT/CONNACK, SUBSCRIBE,
  QoS0/1 PUBLISH, PUBACK, keepalive ping.
- `src/networking/agent.*`: the `void* agent` handle. One thread per printer owns its MQTT
  session; callbacks are invoked without holding locks; disconnects never
  block the GUI thread for more than ~100 ms.
- `src/networking/print_job.*`: upload with progress mapped to the slicer's
  `OnUpdateStatusFn` stages, `project_file`, then wait for the printer's ack
  (matched on `sequence_id`).
- `src/networking/exports_*.cpp`: the C entry points. Cloud functions report "not
  logged in"; `ft_*` stubs fail cleanly so Studio falls back to FTPS. No
  exception can cross into the slicer.
- `src/source/*`: `bambu:///local/<ip>.?port=6000&…` URL parser, camera reader
  thread, `Bambu_*` exports.

## 5. Verification

- **Compile time:** every exported signature is `static_assert`ed against the slicer's
  typedefs.
- **Unit tests** (`tests/unit_tests.cpp`): URL parsing, SSDP → JSON, model table,
  `project_file` JSON, JPEG size parsing, camera auth packet.
- **Integration** (`tests/integration.sh`): `tests/fake_printer.py` implements the printer side
  (TLS MQTT with CONNACK 5 on a bad code, report pushes, `project_file` ack,
  port-6000 camera, certificate CN = serial). `bbl_lan_probe` loads the built
  libraries with `dlopen`/`dlsym` exactly like the slicer and drives detect,
  connect, bad code, print-from-storage, camera, and disconnect-during-connect.
  It runs inside Bambu Studio's own flatpak runtime.
- **Review:** an independent concurrency/lifetime review led to the
  single-owner-thread design above.
- **Real hardware:** Bambu Studio 2.9.1 flatpak on aarch64 with an A1.

## 6. Why the build runs in the flatpak SDK

Bambu Studio's flatpak runs on `org.gnome.Platform//51`, which is built on
`org.freedesktop.Platform//26.08`. Building inside
`org.freedesktop.Sdk//26.08` gives exactly the runtime's glibc, libstdc++,
OpenSSL 3 and libcurl 4, and therefore the same `std::string` /
`std::function` ABI as the slicer. The resulting libraries depend only on
`libssl.so.3`, `libcrypto.so.3`, `libcurl.so.4`, `libstdc++.so.6` and `libc`.

## 7. OrcaSlicer

OrcaSlicer vendors Bambu's plugin API with its own changes (details and
references: [orca-contract.md](orca-contract.md)):

- **Different ABI.** Orca 2.4 targets Bambu's 02.03.00 plugin series: its
  `PrintParams` lacks five fields Studio 2.9 has (so the struct layout differs),
  `bind` has one parameter less, and a few Studio functions don't exist. The
  networking plugin is therefore compiled twice from the same sources, once
  against `third_party/bambu/` (Studio) and once against `third_party/orca/`
  (`-DBBL_TARGET_ORCA`), and both are `static_assert`ed against their slicer's
  typedefs. `libBambuSource.so` is shared: Orca's `BambuTunnel.h` is a subset of
  Studio's with identical structs.
- **Versioned file name.** Orca loads `libbambu_networking_<configured version>.so`
  and, after loading, saves the plugin's own `get_version()` as the configured
  version. We report `02.03.00.99` (same 8-character prefix as `02.03.00.62`,
  higher than any Bambu build so Orca's updater leaves it alone) and install
  under that name, plus `…_02.03.00.62-lan.so` so it is listed in Preferences.
- **Lifecycle.** Orca can `dlclose()` the plugin on a hot reload without
  destroying the agent, so both libraries are linked with `-z nodelete`. It
  starts discovery only if a Bambu preset is selected at start-up, so the Orca
  build starts listening in `start()`.
- **Camera.** Orca plays the stream through a GStreamer `bambusrc` element
  instead of `wxMediaCtrl3`; it uses the same `Bambu_*` calls and gets one JPEG
  per sample, which GStreamer detects as `image/jpeg`.
- **Environment.** A native Orca (e.g. nixpkgs) has its own glibc, libstdc++,
  OpenSSL and libcurl. The SDK-built libraries only need glibc ≥ 2.38 and
  GCC ≥ 13's libstdc++, and resolve `libssl.so.3`/`libcurl.so.4` against the
  copies Orca has already loaded. `tests/integration.sh` with
  `BBL_TEST_RUNNER=native` checks exactly that.
