# OrcaSlicer ↔ plugin contract (OrcaSlicer v2.4.2)

Audit of how OrcaSlicer 2.4.2 loads and calls the plugins, with source
references, as a companion to [slicer-contract.md](slicer-contract.md) (Bambu
Studio). `S/` is `src/slic3r/` in the OrcaSlicer v2.4.2 source tree.
Decisions taken from it: build against Orca's headers (`third_party/orca/`),
install the plugin as both `libbambu_networking_02.03.00.99.so` and
`…_02.03.00.62-lan.so`, link with `-z nodelete`, start SSDP discovery in
`start()`.

---------------------------------------------------------------------------------------------------
## 1. Unchecked plugin calls on LAN paths -> required symbols

### libbambu_networking: every `bambu_network_*` call is null-checked
- All pointers are resolved by `dlsym` in `S/Utils/BBLNetworkPlugin.cpp:542-642` (`load_all_function_pointers`).
- Every wrapper checks `if (func && agent)` before calling:
  - `S/Utils/BBLPrinterAgent.cpp:23-441` (send_message, connect/disconnect_printer, send_message_to_printer,
    check_cert, install_device_cert, start_discovery, ping_bind, bind_detect, bind, unbind, request_bind_ticket,
    set_server_callback, get/set_user_selected_machine, *_subscribe, start_print, start_local_print_with_record,
    start_send_gcode_to_sdcard, start_local_print, start_sdcard_print, all set_on_* callbacks).
  - `S/Utils/BBLCloudServiceAgent.cpp` (init_log, set_config_dir, set_cert_file, set_country_code, start, user/login,
    presets, tracking...): every `auto func = plugin.get_x(); if (func && agent)` (scripted check: no exception).
  - `BBLNetworkPlugin` itself: `get_version()` checks `m_check_debug_consistent`/`m_get_version` (`:224-244`),
    `create_agent` checks `m_create_agent` (`:250-261`), `destroy_agent` checks (`:263-271`).
  - Other direct users only test for null: `get_get_my_token()` in `S/GUI/GUI_App.cpp:2506` and `S/GUI/HttpServer.cpp:306`.
  - `NetworkAgent` (`S/Utils/NetworkAgent.cpp`) dispatches to the printer/cloud agent objects and checks
    `if (m_printer_agent)` (e.g. `:700-810`, `:819-900`).
- Unchecked paths:
  - **`ft_*` (file transfer)**: resolved in `S/Utils/FileTransferUtils.cpp:9-38` (`InitFTModule` is called from
    `BBLNetworkPlugin.cpp:160` on every load), then called without checks by `FileTransferTunnel`/`FileTransferJob`
    from `S/GUI/Jobs/PrintJob.cpp:219` (eMMC verify, `could_emmc_print`), `S/GUI/SendToPrinter.cpp:1720,1754,1858,1912`.
    The API is identical to Studio's (`third_party/bambu/FileTransferUtils.hpp.ref` differs only by an include).
  - **`Bambu_*` (libBambuSource)**: resolved in `S/GUI/Printer/PrinterFileSystem.cpp:1819-1854`
    (Create, Open, StartStream, StartStreamEx, GetStreamCount, GetStreamInfo, SendMessage, ReadSample, Close, Destroy,
    SetLogger, FreeLogMsg, Deinit). Only `Bambu_Create` gets a fake fallback; `gstbambusrc.c` checks only
    `_lib->Bambu_Open` (`S/GUI/Printer/gstbambusrc.c:408-414`) then calls SetLogger, Open, StartStream,
    GetStreamCount, GetStreamInfo, ReadSample, Close, Destroy, FreeLogMsg unconditionally.
- We export all of these (ft_abi_version, ft_free, ft_job_*, ft_tunnel_*; Bambu_Create/SetLogger/Open/StartStream/
  StartStreamEx/GetStreamCount/GetStreamInfo/ReadSample/SendMessage/Close/Destroy/FreeLogMsg/Deinit).

**Result: no required symbol is missing.** The list of "required but not exported" symbols is empty.

### Symbols Orca dlsyms that we don't export (all null-checked, cloud-only or optional)
bind, build_login_cmd, build_login_info, build_logout_cmd, change_user, check_user_task_report, delete_setting,
get_camera_url, get_design_staffpick, get_model_mall_detail_url, get_model_mall_home_url, get_model_mall_rating,
get_model_publish_url, get_mw_user_4ulist, get_mw_user_preference, get_my_message, get_my_token, get_oss_config,
get_printer_firmware, get_setting_list, get_setting_list2, get_slice_info, get_subtask, get_subtask_info,
get_task_plate_index, get_user_info, get_user_presets, get_user_print_info, get_user_tasks, modify_printer_name,
ping_bind, put_model_mall_rating, put_rating_picture_oss, put_setting, query_bind_status, request_bind_ticket,
request_setting_id, send_message, start_print, start_publish, unbind, **update_cert**.

Note: Orca loads `check_cert` from the symbol name **`bambu_network_update_cert`** (`BBLNetworkPlugin.cpp:576`),
not `bambu_network_check_cert`. Our `check_cert` export is never found by Orca. That's harmless, because
`BBLPrinterAgent::check_cert` null-checks it. Add an alias only if you want parity. `get_my_token` being absent
makes `get_bbl_client_version()` report "01.10.01.50" (`GUI_App.cpp:2504-2509`), which only matters for cloud.

---------------------------------------------------------------------------------------------------
## 2. Behaviour differences vs docs/slicer-contract.md

### 2.1 Loading / version check (BIGGEST DIFFERENCE)
- File: `<data_dir>/plugins/libbambu_networking_<network_plugin_version>.so`, `dlopen(RTLD_LAZY)`
  (`BBLNetworkPlugin.cpp:104-146`). No fallback to the unversioned name, except the legacy auto-copy when the
  version is exactly `01.10.01.01` (`:80-102`). `data_dir` on Linux is normally `~/.config/OrcaSlicer`.
- Right after dlopen: `InitFTModule`, all dlsyms, then **`get_version()` is called before any agent exists**
  (`:166-174`) to decide `m_use_legacy_network = (version == "01.10.01.01")`. For any other string the **modern
  `PrintParams` (Orca layout) and 5-arg `send_message[_to_printer]` are used** (`BBLPrinterAgent.cpp:29-33,66-70,277-347`).
- `NetworkAgent::get_version()` calls `check_debug_consistent(false)` first (release build), so it must return true
  (`BBLNetworkPlugin.cpp:224-244`).
- **Config resync** (`S/GUI/GUI_App.cpp:3394-3404`): after a successful load, if
  `extract_base_version(network_plugin_version)` (text before the first '-') `!=` the full string returned by
  `get_version()`, Orca **overwrites `network_plugin_version` with `get_version()`** and saves. Our current
  `BBL_ORCA_PLUGIN_VERSION "02.03.00.99"` with a file named `..._02.03.00.62-lan.so` would therefore rewrite the config
  to `02.03.00.99`, and the next start looks for `libbambu_networking_02.03.00.99.so`.
- `check_networking_version()` (`GUI_App.cpp:1906-1934`): first 8 chars of `get_version()` vs first 8 chars of the
  configured version (or latest `02.03.00.62` when empty). It does NOT use SLIC3R_VERSION (Orca's is `02.06.00.51`, `version.inc:17`).
- Then `plugins/libBambuSource.so` (unversioned) must dlopen, otherwise the plugin is marked incompatible plus
  "need update" (`GUI_App.cpp:3406-3415`, `BBLNetworkPlugin.cpp:280-317`).
- `has_network_update_available()` (`GUI_App.cpp:1813-1823`) compares 8 chars with latest `02.03.00.62`, so it
  never prompts while we report `02.03.00.xx`.
- No signature/certificate check anywhere in Orca (no `SummarizeSelf`/`CertificateVerify` code at all; only a UI
  warning string for 02.00.02.50 in `bambu_networking.hpp:318`).

### 2.2 Startup / init sequence (`GUI_App::on_init_network`, `GUI_App.cpp:3350-3546`)
1. Requires `installed_networking=true` and a non-empty `network_plugin_version`, else no load
   (empty: `m_networking_need_update`, `:3376-3387`).
2. initialize -> get_version (as above) -> check_networking_version -> libBambuSource dlopen.
3. `create_agent_from_config` (`S/Utils/NetworkAgentFactory.cpp:156-190`): primary cloud agent is **OrcaCloudServiceAgent**,
   and BBL cloud is only added if `"bbl"` is in the app config's cloud providers. `create_cloud_agent(BBL)` is what calls
   `create_agent(log_dir)` (`NetworkAgentFactory.hpp:131-147`). The printer agent starts as **nullptr**.
4. `m_agent->set_config_dir`, `init_log`, `set_cert_file(resources/cert, slicer_base64.cer)`, `init_networking_callbacks()`,
   `set_country_code`, `start()`: these go to **all registered cloud agents** (`:3470-3490`).
5. Then, whenever the BBL plugin is loaded and up to date (`:3495-3514`): `plugin.create_agent(data_dir)` if not yet
   created, then directly `set_config_dir`, `init_log`, `set_cert_file`, `set_country_code`, **`track_enable(false)`,
   `track_remove_files()`**, `start()`.
   - If the BBL cloud provider is registered, set_config_dir/init_log/set_cert_file/set_country_code/start are
     called **twice**. Our implementations are trivial or idempotent (`exports_core.cpp:42-53`), which is fine.
   - Order differs from Studio: `create_agent` may come after the callbacks were stored (step 4), and
     `set_on_*` callbacks only reach the plugin when a **printer agent** is set.
6. Printer agent: `GUI_App::switch_printer_agent()` (`:3561-3617`) is triggered from `Tab::on_presets_changed()`
   (`S/GUI/Tab.cpp:2108-2118`) when presets load. **BBLPrinterAgent is used only if the selected printer preset is a
   Bambu vendor preset** (`is_bbl_vendor()`, `:3571-3574`). Otherwise the agent is Orca/Moonraker/... and our plugin
   is not used for printer comms. `set_printer_agent` re-applies all stored callbacks
   (`NetworkAgent.cpp:116-154`: ssdp, printer_connected, subscribe_failure, message, user_message, local_connect,
   local_message, queue_on_main, server_callback). When switching away, the old agent gets **empty
   `std::function`s** (`apply_printer_callbacks(old, {})`). Our `invoke_safely` already checks `if (!fn)` (`agent.cpp:32-35`).
7. `post_init` (`GUI_App.cpp:979-996`): `set_on_ssdp_msg_fn` then **`start_discovery(true,false)`** once. This is a
   no-op if the printer agent is not BBL/null at that moment (`NetworkAgent.cpp:805-810`). Switching to a Bambu preset
   later does **not** call start_discovery again. Suggestion: tolerate this by starting the SSDP listener when
   `set_on_ssdp_msg_fn` gets a non-empty fn, or in `start()`.
8. Startup does **not** call `bind_detect` for saved LAN printers: they are restored from AppConfig alone
   (`S/GUI/DeviceCore/DevManager.cpp:27-48`). Studio does probe them.

### 2.3 Callbacks
- `on_local_connect` (`GUI_App.cpp:2068-2136`): same semantics as Studio. 0 = Ok (request push_all + get_version);
  1 = Failed (ignored unless dev is the selected machine; `msg=="5"` gives "Incorrect password" and clears the access code;
  otherwise "Connect %s failed! [SN, code=msg]"); 2 = Lost. For non-LAN (cloud-mode) objects: 0->1, "5"->5, else -2.
- `on_local_message` (`:2187-2212`): `process_network_msg(dev_id,msg)` first. Empty dev_id is used for control strings
  (wait_info, update_studio, cert_expired..., `:5762+`). Then `obj->parse_json("lan", msg)`. Same as Studio.
  `device_cert_installed` exists (`DeviceManager.cpp:4379`) as in Studio.
- `on_message` (cloud) and `on_printer_connected` ("tunnel/" prefix) are cloud-only. Messages are never signed:
  the flag passed is 0 (no MSG_SIGN use anywhere besides the enum, `bambu_networking.hpp:384`).
- SSDP JSON parsing (`DevManager.cpp` ~`:175-265`) is identical to Studio: dev_name, dev_id, dev_ip, dev_type,
  dev_signal, connect_type, bind_state required, plus optional sec_link/ssdp_version/connection_name; "farm" is mapped to lan/free.

### 2.4 Connect via IP / bind_detect (`S/GUI/ReleaseNote.cpp:1911-1990`, InputIpAddressDialog)
- `bind_detect(ip, "secure", detect)`: -1 -> "Failed to connect to printer.", -2 -> "Failed to publish login request.",
  **-3 -> manual SN/model panel**. On macOS it's forced to -3. Success requires `bind_state != "occupied"` and
  `connect_type != "cloud"`. Then `insert_local_device(dev_name, ip, dev_id, model_id, connect_type, bind_state,
  version, access_code)` and `set_selected_machine`, which calls `connect_printer`. Same contract as Studio.
- `connect_printer(sn, ip, "bblp", access_code, use_ssl)` (`S/GUI/DeviceManager.cpp:2404-2416`). use_ssl =
  `obj->local_use_ssl` in Release builds (`BBL_RELEASE_TO_PUBLIC=$<CONFIG:Release>`, root CMakeLists:81-84). In a
  non-Release build it comes from app_config `enable_ssl_for_mqtt` (likely false), and we already force TLS (`agent.cpp:94-97`).
- Orca's `SendJob` "check mode" no longer performs a verify_job (Studio's verify block was removed;
  `m_is_check_mode` is set but unused, `S/GUI/Jobs/SendJob.cpp`/`.hpp:26,60`).

### 2.5 Local print (`S/GUI/Jobs/PrintJob.cpp`)
- verify_job (`:210-249`), when connection_type=="lan" and print type "from_normal":
  - if `could_emmc_print`: `FileTransferTunnel(module(), "bambu:///local/<ip>?port=6000&user=bblp&passwd=<code>")`
    then `sync_start_connect()` (exceptions are caught).
  - **always also** `start_send_gcode_to_sdcard(params, nullptr, nullptr, nullptr)` with project_name="verify_job",
    filename=`<resources>/check_access_code.txt`, dev_id, connection_type, dev_ip, username "bblp", password,
    use_ssl_for_ftp/mqtt. The check passes if `emmc_ok || ftp_ok (ret==0)`. update/cancel/wait fns are empty
    std::functions, so they must be null-checked.
- LAN print uses **`start_local_print(params, update_fn, cancel_fn)`** (no wait_fn) (`:605-633`), gated by
  sdcard_state, or directly when `could_emmc_print`. "from_sdcard_view" uses `start_sdcard_print` (`:540-542`).
  Non-LAN connection types use `start_local_print_with_record` / `start_print` (cloud).
- PrintParams is the **modern Orca struct** (`bambu_networking.hpp:233-280`; no task_timelapse_use_internal,
  extruder_cali_manual_mode, svc_context, slicer_uid, queue_plate_id). Fields filled (`:201-379`): dev_ip,
  use_ssl_for_ftp, use_ssl_for_mqtt, username "bblp", password, dev_id, ftp_folder (`obj->get_ftp_folder()`,
  `SelectMachine.cpp:2473`), filename (3mf), config_filename, plate_index (1-based), task_bed_leveling, task_flow_cali,
  task_vibration_cali, task_layer_inspect, task_record_timelapse, nozzle_mapping, ams_mapping, ams_mapping2,
  ams_mapping_info, nozzles_info, connection_type, task_use_ams, task_bed_type, print_type, auto_bed_leveling,
  auto_flow_cali, auto_offset_cali, task_ext_change_assist, **try_emmc_print = could_emmc_print && !disable_emmc**
  (`disable_emmc_print` defaults to true, so it's false unless the user opts in) (`:279-288`), dst_file (sdcard view only),
  origin_profile_id/origin_model_id/preset_name/stl_design_id, project_name (MakerWorld name truncated to 100, or
  m_project_name). ftp_file and ftp_file_md5 are not set. Same as Studio.
- update_fn: Upload stage with code 0..100 and a non-empty info gets `"(info)"` appended. Finished stage: info =
  seconds. Errors: code<0 or >100 or stage 7. Return codes are handled as in Studio (-18 canceled, LP/SG FTP failed...).
- Send to storage (`SendJob.cpp:164-186, 262-320`): `start_send_gcode_to_sdcard(params, update_fn, cancel_fn, nullptr)`,
  project_name = `<project>.gcode.3mf`, filename = 3mf, ams_mapping, task_use_ams, dev_ip/creds. No verify_job.

### 2.6 Own agents for Bambu printers?
- For Bambu vendor presets: always `BBLPrinterAgent` -> our plugin (`GUI_App.cpp:3571-3574`; `select_machine` is a
  no-op for BBL, `:3621-3625`). OrcaPrinterAgent, Moonraker, Qidi, Snapmaker and Creality agents serve non-BBL presets
  (`NetworkAgentFactory.cpp:132-154`). The primary **cloud** agent is always OrcaCloudServiceAgent, which is unrelated to Bambu.

### 2.7 Things that can overwrite / delete / bypass our file
- `remove_legacy_library()` is never called (dead code). `remove_old_networking_plugins()` call is commented out (`GUI_App.cpp:2864`).
- **Download / install** (user clicks Download in the startup dialog, Preferences version switch to a missing
  version, or `installed_networking` toggle on Windows only): `download_plugin` sends `X-BBL-OS-Type: linux` even on
  aarch64 (`GUI_App.cpp:1208-1218`), so it fetches the **x86-64** build. `install_plugin` (`:1354-1596`) extracts it,
  **renames to `libbambu_networking_<config_version>.so`, removing any existing file with that name** (`:1516-1540`),
  overwrites `plugins/libBambuSource.so`, and copies all of plugins/ into plugins/backup.
- **PresetUpdater OTA** (`S/Utils/PresetUpdater.cpp:833-976`, called from post_init `GUI_App.cpp:932-941` unless
  stealth mode): `sync_resources` downloads to `<data_dir>/ota/` only if the online version has the same
  maj.min and patch/100, and `> get_version()` (`:575-587`). With `force_update` it sets `update_network_plugin=true`,
  and on the next start `copy_network_if_available()` (`GUI_App.cpp:3236-3348`) copies `ota/libbambu_networking.so` to
  `plugins/libbambu_networking_<cached>.so`, **overwrites `plugins/libBambuSource.so`**, and sets
  `network_plugin_version=<cached>`. Reporting a high build number (`02.03.00.99`) avoids this. Stealth mode disables it.
- **Missing configured file**: dlopen fails, `set_load_error`, `m_networking_need_update=true`, no BBL agent,
  then at post_init `show_network_plugin_download_dialog(false)` in "CorruptedPlugin" mode (`GUI_App.cpp:909-911,
  1825-1878`). Nothing is downloaded automatically; the user must pick Download/Skip/Remind/Don't ask.
- **Hot reload** (Preferences version change or "reload" button: `hot_reload_network_plugin`, `GUI_App.cpp:1707-1800`):
  clears callbacks, `start_discovery(false)`, `disconnect_printer()`, deletes NetworkAgent, then
  **`dlclose()` without `destroy_agent`**. `BBLNetworkPlugin::unload` (`:188-217`) keeps `m_agent`, and
  `create_agent` returns the stale pointer after reload (`:250-261`). With threads or vtables in our .so this
  could crash. **Recommendation: link both libs with `-Wl,-z,nodelete`** (or make sure the reloaded image reuses the same
  Agent safely). Also, after a hot reload the new NetworkAgent has no printer agent until a preset change, so
  discovery/callbacks may not resume (Orca issue). Advise users to restart Orca after switching.

---------------------------------------------------------------------------------------------------
## 3. Camera (libBambuSource)
- Loaded from **`<data_dir>/plugins/libBambuSource.so`** (unversioned), only after the networking lib is loaded
  (`BBLNetworkPlugin.cpp:280-317`).
- `S/GUI/Printer/BambuTunnel.h` is a subset of Studio's (no PlayerEventC/TrackReporter, StreamInfoCallback,
  Bambu_SessionStat, SetStreamInfoCallback, SetTrackReporter, GetSessionStat). Structs Bambu_StreamInfo/Bambu_Sample
  and all common signatures are identical, so the extra exports are harmless.
- **Linux does NOT use wxMediaCtrl3** (the file doesn't exist in Orca). It uses `wxMediaCtrl2` = wxMediaCtrl GStreamer
  backend, or a native Wayland `playbin` + `gtksink` (`S/GUI/wxMediaCtrl2.cpp:100-200, 323-505`), with the in-process
  GStreamer element **`bambusrc`** (`S/GUI/Printer/gstbambusrc.c`) registered for the `bambu://` URI scheme.
  Call sequence (`gst_bambusrc_start`, `:396-462`): Bambu_Create(location=full URL), SetLogger, Open (!=0 fails),
  `while (StartStream(t, 1) == would_block) usleep(100 ms)`, **GetStreamCount** then GetStreamInfo(i) for each
  (only type==VIDE is used: sub_type and frame_rate are stored, no width check). Then `create()` (`:264-300`):
  `while (ReadSample == would_block) usleep(33 ms)`, stream_end means EOS, anything else is an error. Each sample is
  wrapped in a GstBuffer with synthesized PTS. **The src pad caps are ANY**, so playbin/decodebin typefinds the
  bytes: each sample must be a complete JPEG (FFD8...FFD9) so typefind detects `image/jpeg` and plugs jpegdec. Logger
  strings are freed with Bambu_FreeLogMsg (`:389-393`).
- `start()` runs synchronously inside `gst_element_set_state(PAUSED)` from `wxMediaCtrl2::Load` on the **GUI thread**,
  so Open+StartStream polling blocks the UI until the first connection succeeds. Keep the connect/auth timeout short.
- `wxMediaCtrl2::Load` on GTK3 refuses to play unless GStreamer has `h264parse` **and** an H.264 decoder
  (openh264dec/avdec_h264/vaapi/va/nv/v4l2), even for MJPEG (`wxMediaCtrl2.cpp:427-471`). Users also need
  jpegdec (gst-plugins-good). The native Wayland path needs gtksink.
- URL (`S/GUI/MediaPlayCtrl.cpp:289-305`), same as Studio:
  `bambu:///local/<ip>.?port=6000&user=bblp&passwd=<code>&device=<sn>&net_ver=<get_version()>&dev_ver=<fw>&cli_id=<uuid>&cli_ver=02.06.00.51`
  (only when `liveview_local == LVL_Local`; rtsps/rtsp variants for X1). There's no `play_start_ms`. The virtual-camera
  variant writes the URL to cameratools/url.txt (`:534-545`).
- File browser (`PrinterFileSystem`) uses the same Bambu_* API as Studio, including StartStreamEx and SendMessage.

---------------------------------------------------------------------------------------------------
## 4. Selecting our plugin in Orca
- Config: `OrcaSlicer.conf`, section `[app]`: `installed_networking = true` (Preferences > Online > "Enable network
  plug-in", `S/GUI/Preferences.cpp:1897`) and `network_plugin_version = <ver>` (`AppConfig.hpp:27`), which maps to
  `<data_dir>/plugins/libbambu_networking_<ver>.so`, plus `<data_dir>/plugins/libBambuSource.so`.
- The UI combobox (`Preferences.cpp:1206-1300`) lists AVAILABLE_NETWORK_VERSIONS (02.03.00.62, 02.01.01.52,
  02.00.02.50, 01.10.01.01) plus discovered **suffixed** files whose base is in that list, e.g.
  `libbambu_networking_02.03.00.62-lan.so` shows as "└ 02.03.00.62-lan" (`BBLNetworkPlugin.cpp:428-472, 746-804`).
  Unsuffixed unknown versions (e.g. `02.03.00.99`) are **not listed**. Selecting an entry that exists on disk triggers
  a hot reload. For a missing one, it offers a download, which would fetch x86-64.
- Recommended scheme, given the config resync (2.1) and OTA (2.7):
  - Keep `get_version()` = `"02.03.00.99"` (OTA-proof, 8-char match with 02.03.00.62).
  - Install the library as **`plugins/libbambu_networking_02.03.00.99.so`** and set
    **`network_plugin_version = 02.03.00.99`** directly (edit the conf with Orca closed). That is stable: base == loaded, so
    no rewrite. The combobox will display index 0 ("02.03.00.62 (Latest)") because .99 isn't listed, and users must not
    touch it.
  - Optional, for UI selectability: also install it as (or symlink it to) `libbambu_networking_02.03.00.62-lan.so`. A user
    who picks "└ 02.03.00.62-lan" gets it loaded, and Orca then rewrites the config to `02.03.00.99`, which must
    therefore exist too (`scan_plugin_versions` uses `is_regular_file(status())`, so symlinks count).
  - Alternative: `get_version()` = `"02.03.00.62"` + file `..._02.03.00.62-lan.so` (no rewrite, nicely listed), but then
    any Bambu linux OTA > 02.03.00.62 could be downloaded and force-installed over libBambuSource.so (2.7). Not recommended.
  - Recommend Orca's stealth mode for LAN-only users (disables PresetUpdater sync).
