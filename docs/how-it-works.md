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
  to return `would_block` until ready, a stream info of type `VIDE` with the
  size and a frame rate > 0, then one complete frame per `Bambu_ReadSample`:
  a JPEG (`MJPG`) or an H.264 access unit (`AVC1`). The decoder is opened
  without extradata, so H.264 samples must carry SPS/PPS in-band before each
  keyframe (`f_sync`).

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
| X1 and newer camera | standard RTSPS on 322 (`rtsps://bblp:<code>@<ip>:322/streaming/live/1`, what ha-bambulab and go2rtc use): DESCRIBE/SETUP/PLAY, H.264 in RTP (RFC 6184) interleaved on the TCP connection. The slicer passes it as `bambu:///rtsps___bblp:<code>@<ip>/streaming/live/1?proto=rtsps&…` when the printer reports an `rtsps` `ipcam.rtsp_url` |
| Identity | the printer's TLS certificate CN is its serial. `bind_detect` reads it and maps the serial prefix to a model with Studio's own `resources/printers/*.json` (`sn_prefix`) |

### MQTT cipher modes

Bambu uses two wire formats on the same MQTT topics:

- **LAN-only mode** (Developer Mode on recent firmware): payloads are plain JSON.
- **Hybrid mode** (older firmware kept in LAN + cloud mode, no Developer Mode):
  payloads on `request`/`report` are AES-128-CBC + PKCS7, base64-encoded, with
  key **and** IV = the LAN access code as ASCII, right-padded with `0` to 16
  characters (an 8-digit code). The TLS connection, `bblp` auth and topics are
  identical, so the plugin can probe both.

`MqttClient` has `CipherMode { plain, aes, auto }`. In `auto` (the default) every
publish is sent in both forms when an access code is known — the printer only
parses the one it expects and ignores the other (verified against the
`--hybrid` mode of `tests/fake_printer.py`; **not yet verified against a real
hybrid printer**). The first inbound payload that decodes settles the mode:
payload starting with `{`/`[` → plain, a valid base64/AES/PKCS7/JSON decode →
aes; anything else is dropped. Force a mode with
`BBL_LAN_MQTT_CIPHER=plain|aes|auto`; `BBL_LAN_DEBUG=1` logs the settled mode.

The derivation is documented by community projects (bambu_mqtt_control) and is
the same scheme the cloud protocol uses with per-device keys. It implements no
extracted secret: the key material is the user's own access code, which the
slicer already requires.

**Verification status.** Both the plaintext and encrypted paths are covered by
the fake printer (`tests/integration.sh`, `--hybrid`) for both ABIs. The fake
implements this document, so it proves the C++ and reference implementations
agree, *not* that they match a real hybrid printer. Field-testing one is the
open item; if a real printer never answers, run with
`BBL_LAN_MQTT_CIPHER=aes BBL_LAN_DEBUG=1` and compare against the log.

## 4. Implementation

Small C++17 libraries with only OpenSSL and libcurl as dependencies, both
present in the flatpak runtime:

- `src/common/tls_socket.*`: non-blocking OpenSSL client. One owner thread does
  connect and reads; any thread may write or `cancel()`. Every wait is sliced at
  100 ms so cancellation is fast.
- `src/networking/mqtt_client.*`: minimal MQTT 3.1.1 client: CONNECT/CONNACK, SUBSCRIBE,
  QoS0/1 PUBLISH, PUBACK, keepalive ping. With a cipher key set it can send
  each publish plain, AES-encrypted or both (`auto`), and settles the mode on
  the first decodable inbound payload — see [MQTT cipher modes](#mqtt-cipher-modes).
- `src/networking/agent.*`: the `void* agent` handle. One thread per printer owns its MQTT
  session; callbacks are invoked without holding locks; disconnects never
  block the GUI thread for more than ~100 ms.
- `src/networking/print_job.*`: upload with progress mapped to the slicer's
  `OnUpdateStatusFn` stages, `project_file`, then wait for the printer's ack
  (matched on `sequence_id`).
- `src/networking/exports_*.cpp`: the C entry points. Cloud functions report "not
  logged in"; `ft_*` stubs fail cleanly so Studio falls back to FTPS. No
  exception can cross into the slicer.
- `src/source/*`: the `Bambu_*` exports pick a `VideoSource` from the URL:
  `CameraStream` (`bambu:///local/…`, JPEG) or `RtspStream`
  (`bambu:///rtsps___…`). `rtsp.*` holds the I/O-free RTSP/SDP/Digest
  helpers, `h264.*` the RTP depacketiser (single NAL, STAP-A, FU-A; drops to
  the next keyframe after packet loss) and the SPS size parser.

## 5. Verification

- **Compile time:** every exported signature is `static_assert`ed against the slicer's
  typedefs.
- **Unit tests** (`tests/unit_tests.cpp`): URL parsing, SSDP → JSON, model table,
  `project_file` JSON, JPEG size parsing, camera auth packet.
  `tests/unit_tests_rtsp.cpp`: RTSP URL/messages, Digest (RFC 2617 vector),
  SDP, SPS cropping and depacketisation of a real x264 stream
  (`tests/fixtures/camera_320x180.h264`), including packet loss.
- **Integration** (`tests/integration.sh`): `tests/fake_printer.py` implements the printer side
  (TLS MQTT with CONNACK 5 on a bad code, report pushes, `project_file` ack,
  port-6000 camera, an RTSPS camera with Digest auth streaming the fixture,
  certificate CN = serial). `bbl_lan_probe` loads the built
  libraries with `dlopen`/`dlsym` exactly like the slicer and drives detect,
  connect, bad code, print-from-storage, both cameras (good and bad code), and
  disconnect-during-connect. The H.264 samples it writes decode with ffmpeg
  and with GStreamer `h264parse ! openh264dec`.
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
  or H.264 access unit per sample, which GStreamer typefinds as `image/jpeg` or
  `video/x-h264` (byte-stream). H.264 needs `h264parse` and a decoder
  (`openh264dec`, `avdec_h264`…) in Orca's GStreamer.
- **Environment.** A native Orca (e.g. nixpkgs) has its own glibc, libstdc++,
  OpenSSL and libcurl. The SDK-built libraries only need glibc ≥ 2.38 and
  GCC ≥ 13's libstdc++, and resolve `libssl.so.3`/`libcurl.so.4` against the
  copies Orca has already loaded. `tests/integration.sh` with
  `BBL_TEST_RUNNER=native` checks exactly that.
