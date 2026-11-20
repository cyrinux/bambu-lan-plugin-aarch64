# What Bambu Studio 02.09.01.70 expects from the plugins (LAN mode)

Source: research/bambustudio-src @ v02.09.01.70. DeviceManager lives in
`src/slic3r/GUI/DeviceCore/DevManager.cpp`.

## Loading
- `<data_dir>/plugins/libbambu_networking.so` dlopen'd; Linux has **no signature check**.
- `bambu_network_get_version()` first 8 chars must equal SLIC3R_VERSION's ("02.09.01").
- `<data_dir>/plugins/libBambuSource.so` must dlopen, else plugin marked incompatible.
- Every networking symbol is optional (null-checked wrapper), EXCEPT the ft_* family
  which is called unchecked from SendToPrinter (brtc), PrintJob eMMC check, MediaPlayCtrl
  device image → export ft_* stubs returning FT_EIO (-3) rather than omitting them.
- Flatpak data_dir: `~/.var/app/com.bambulab.BambuStudio/config/BambuStudio`.

## Discovery
- `start_discovery(true,false)` at startup: just listen for SSDP NOTIFY (UDP 2021/1990).
- `on_ssdp_msg_fn(json)`: flat JSON, all string values. Required: dev_name, dev_id (serial),
  dev_ip, dev_type (model code, A1 = "N2S"), dev_signal ("-45dBm"), connect_type ("lan"|"cloud"|"farm"),
  bind_state ("free"|"occupied"). Optional: sec_link, ssdp_version, connection_name.
- LAN printers listed need connect_type=lan, bind_state=free, access code set.
- Add-by-IP: `bind_detect(ip, "secure", detectResult&)` fills dev_id, dev_name, model_id, version,
  bind_state, connect_type. Return -3 → manual serial/model entry panel. Also used at startup
  to restore saved LAN printers (failure tolerated).

## Local MQTT
- `connect_printer(sn, ip, "bblp", access_code, use_ssl=true)` → TLS 8883.
- `on_local_connect_fn(status, sn, msg)`: 0 Ok, 1 Failed (msg "5" = wrong access code →
  "Incorrect password"), 2 Lost.
- `on_local_message_fn(sn, raw_payload)` for each device/<sn>/report message, unwrapped.
  Empty dev_id = control strings (don't use).
- `send_message_to_printer(sn, json, qos, flag)` → publish device/<sn>/request. flag always 0;
  slicer never signs. Return 0 on success.
- `disconnect_printer()` when switching. `is_user_login` → false. Cloud calls: stubs return 0/empty.
- Optional: on_local_message(sn, "device_cert_installed") enables is_security_control_ready.

## libBambuSource (camera + file browser)
- Header: GUI/Printer/BambuTunnel.h. Must export: Create, SetLogger, Open, StartStream,
  GetStreamInfo, ReadSample, SendMessage, Close, Destroy, FreeLogMsg (+ optional StartStreamEx,
  SetStreamInfoCallback, SetTrackReporter, GetSessionStat, Deinit, GetStreamCount).
- Logger strings passed to the callback are freed with Bambu_FreeLogMsg → allocate them.
- Linux live view: wxMediaCtrl3 + libavcodec. Open (0 ok) → StartStream(true) loop while 2 →
  GetStreamInfo(0): type VIDE, sub_type MJPG, width ≥ 320, frame_rate > 0 →
  ReadSample: one full JPEG per sample; 2 = would_block (poll 10 ms); other = end.
- URL: `bambu:///local/<ip>.?port=6000&user=bblp&passwd=<code>&device=<sn>&net_ver=..&dev_ver=..&cli_id=..&cli_ver=..[&play_start_ms=..]`
  (short form without the "." also used).
- File browser: same URL, StartStreamEx(t, 0x3001) / StartStream(t,false); SendMessage(t, 0x3001,
  json[+"\n\n"+bin]); ReadSample returns `{"result","sequence","reply"}` + "\n\n" + binary.
  cmdtype LIST_INFO=1, SUB_FILE=2, FILE_DEL=3, FILE_DOWNLOAD=4, FILE_UPLOAD=5, MEDIA_ABILITY=7.
  On A1 the wire protocol behind this is proprietary (port 6000 ctrl channel) → v1 unsupported.

## A1 camera wire protocol (port 6000, community-documented)
- TLS (no verify). Send 80-byte auth: u32le 0x40, u32le 0x3000, u32 0, u32 0,
  "bblp" padded to 32 bytes, access code padded to 32 bytes.
- Receive loop: 16-byte header (u32le payload size, u32 itrack, u32 flags, u32 0) + JPEG payload.

## Print jobs (PrintJob.cpp / SendJob.cpp)
- Access-code check first: `start_send_gcode_to_sdcard(params, nullptr, nullptr, nullptr)` with
  project_name="verify_job", filename=<resources>/check_access_code.txt. Must return 0 if FTPS login OK.
  If `fun2` bit0 (could_emmc_print) the slicer also tries `ft_tunnel_sync_connect` on
  `bambu:///local/<ip>?port=6000&user=bblp&passwd=<code>` — non-zero just falls back to the check above.
- LAN print: `start_local_print(params, update_fn, cancel_fn)` (no wait_fn).
  - filename = local temp 3mf with gcode; ftp_folder="sdcard/" (A1); ftp_file/dst_file NOT set →
    plugin picks remote name. project_name/task_name = project (≤100 chars). plate_index 1-based.
  - task_bed_leveling, task_flow_cali, task_vibration_cali, task_layer_inspect, task_record_timelapse,
    task_use_ams, task_bed_type, ams_mapping (JSON array of tray ids, -1 = external),
    ams_mapping2 ([{"ams_id","slot_id"}]), auto_bed_leveling/auto_flow_cali/auto_offset_cali (0/1/2).
- send-to-storage: `start_send_gcode_to_sdcard`; remote name = project_name ("<project>.gcode.3mf").
- update_fn(stage, code, info): stage SendingPrintJobStage; during Upload (1) code = percent 0..100;
  Finished (6) with info = seconds string (e.g. "3"); error: stage 7 + negative code.
- Return ≥0 success; -18 canceled (poll cancel_fn); -4020 LP upload failed; -4030 LP publish failed;
  -5010 SG upload failed.
- Printer ack: `print.command=="project_file"` with result "fail" → error. For local tasks the slicer
  derives plate number from `gcode_file`/param "Metadata/plate_<n>.gcode".

## MQTT project_file (cross-checked with ha-bambulab's pybambu/utils.py build_print_project_file_command)
{"print":{"sequence_id":"N","command":"project_file","param":"Metadata/plate_<n>.gcode",
 "project_id":"0","profile_id":"0","task_id":"0","subtask_id":"0","subtask_name":"<name>",
 "file":"<remote name>","url":"file:///sdcard/<remote name>" (X1/P1/A1; newer models "ftp:///<name>"),
 "md5":"<md5>","timelapse":bool,"bed_type":"auto","bed_leveling":bool,"auto_bed_leveling":0|1,
 "flow_cali":bool,"extrude_cali_flag":0|1,
 "vibration_cali":bool,"layer_inspect":bool,"use_ams":bool,"ams_mapping":[..],"ams_mapping2":[..]}}
