# bambu-lan-plugin-aarch64: LAN network plugin for Bambu Studio & OrcaSlicer on arm64 Linux

[![build](https://github.com/cyrinux/bambu-lan-plugin-aarch64/actions/workflows/build.yml/badge.svg)](https://github.com/cyrinux/bambu-lan-plugin-aarch64/actions/workflows/build.yml)

> **Unofficial community project.** Not affiliated with, endorsed by or
> supported by Bambu Lab. Use at your own risk; see [Legal notice](#legal-notice).

Bambu Studio and OrcaSlicer talk to Bambu printers through two closed-source
plugins that Bambu only ships for x86-64 (`libbambu_networking.so`,
`libBambuSource.so`). On an aarch64 machine the slicer cannot load them, so the
Device tab, sending prints and the camera don't work.

This project is an open-source, native aarch64 reimplementation of the **LAN**
part of those plugins, written against the plugin ABI published in the slicers'
own source. How it was done: [docs/how-it-works.md](docs/how-it-works.md).
Updating for a new slicer release: [docs/updating.md](docs/updating.md).

| Slicer | Built against | Plugin file | Tested |
|---|---|---|---|
| Bambu Studio 2.9.x (flatpak or native) | `bambulab/BambuStudio` v02.09.01.70 | `libbambu_networking.so` | 2.9.1 flatpak with a **Bambu Lab A1** |
| OrcaSlicer 2.4.x (native, e.g. nixpkgs, or flatpak) | `OrcaSlicer/OrcaSlicer` v2.4.2 (Bambu plugin series 02.03.00) | `libbambu_networking_02.03.00.62-lan.so` | plugin ABI and protocol tests in a host (Nix) OrcaSlicer environment |

Both share the same `libBambuSource.so` (camera). Native builds need glibc ≥ 2.38,
libstdc++ from GCC ≥ 13, OpenSSL 3 and libcurl 4 (any 2024+ distribution).

## Printer compatibility

All Bambu printers use the same LAN protocols for discovery, MQTT and FTPS, so
status, control and printing should work beyond the A1. The camera is the
main difference: A1/P1 send JPEG frames on port 6000, the X1 and newer models
stream H.264 over RTSPS on port 322. The plugin handles both.

| Printer | Status / control / print | Camera | Notes |
|---|---|---|---|
| A1 | ✅ tested | ✅ tested | |
| A1 mini, P1P, P1S | ✅ expected | ✅ expected | same port-6000 JPEG camera and `file:///sdcard/` print URL as the A1 |
| X1, X1 Carbon, X1E | ✅ expected | ⚠️ untested | RTSPS camera (port 322, H.264), tested against a simulated printer only; enable "LAN Only Liveview" on the printer |
| P2S, H2D, H2D Pro, H2S, H2C, X2D, A2L, H3C, N8 | ⚠️ untested | ⚠️ untested | `ftp:///` print URL; RTSPS camera when the printer reports one; may need the `ft_*` tunnel (eMMC) or signed commands on newer firmware |

The slicer picks the camera protocol from the printer's own report
(`ipcam.rtsp_url`), so the plugin follows whatever the printer announces.

Whatever the model, the printer must be in **LAN-only mode with Developer Mode**
enabled on recent firmware, because this plugin cannot sign commands.

## What works

| Feature | How |
|---|---|
| Discovery | SSDP NOTIFY listener on UDP 2021/1990 + periodic M-SEARCH |
| Add printer by IP | `bind_detect`: reads the serial from the printer's TLS certificate CN, model from the serial prefix |
| Status / control (Device tab) | MQTT 3.1.1 over TLS 8883 (`bblp` + access code), `device/<sn>/report` / `request` |
| Wrong access code | reported as CONNACK "5" → Studio's "Incorrect password" prompt |
| Print (LAN) | FTPS upload (libcurl, implicit TLS 990) + MQTT `project_file`, waits for the printer's ack |
| Send to printer storage | FTPS upload |
| Print from printer storage | MQTT `project_file` |
| Live camera (A1/P1) | TLS port 6000, 80-byte auth, JPEG frames → MJPEG stream for Studio's decoder |
| Live camera (X1 and newer) | RTSP over TLS port 322 (Basic/Digest auth), RTP/H.264 interleaved on TCP → Annex-B access units (SPS/PPS before every keyframe) for libavcodec (Studio) or GStreamer (Orca) |

## Not supported (by design)

- Bambu cloud: login, cloud printing, MakerWorld, presets sync, firmware/OTA via cloud.
  The plugin reports "not logged in"; cloud calls fail cleanly.
- Printer file browser (timelapse/model list on the SD card): proprietary protocol on the port-6000
  control channel. Studio shows its normal "unavailable" state.
- Plain (non-TLS) `rtsp://` cameras: no current printer is known to announce one.
- The `ft_*` file-transfer tunnel (eMMC printers, brtc). Studio falls back to FTPS.
- Message signing. Recent firmware requires **LAN-only mode + Developer Mode** on the printer
  for unsigned third-party control (Settings → LAN Only Mode → Developer Mode on the printer screen).

## Install from a release

Download `bambu-lan-plugin-*-aarch64.tar.gz` from the
[releases](https://github.com/cyrinux/bambu-lan-plugin-aarch64/releases), close Bambu Studio, then:

```sh
tar xzf bambu-lan-plugin-*-aarch64.tar.gz && cd bambu-lan-plugin-*-aarch64
./install.sh                  # Bambu Studio flatpak (~/.var/app/com.bambulab.BambuStudio/...)
./install.sh --native         # Bambu Studio, non-flatpak (~/.config/BambuStudio)
./install.sh --orca           # OrcaSlicer, non-flatpak (~/.config/OrcaSlicer)
./install.sh --orca-flatpak   # OrcaSlicer flatpak
./install.sh --orca --uninstall   # restore the original files (any target)
```

In Studio, **don't click "Install Network Plug-in"**: that downloads Bambu's
x86-64 build again.

### OrcaSlicer

OrcaSlicer loads `plugins/libbambu_networking_<version>.so`, `<version>` being
`app.network_plugin_version` in `OrcaSlicer.conf`. After loading a plugin it
saves the plugin's own `get_version()` there. `install.sh --orca` therefore
installs the same library twice:

- `libbambu_networking_02.03.00.99.so`: what Orca loads (`02.03.00.99` is our version).
- `libbambu_networking_02.03.00.62-lan.so`: listed in Preferences → Network
  plug-in version as `02.03.00.62-lan`.

It then sets `network_plugin_version = 02.03.00.99` and `installed_networking = true`.
The previous values are restored by `--uninstall`; `--no-config` leaves the config alone.

- Restart Orca after installing. **Don't pick another plug-in version or click
  "download"/"update"**: Orca would fetch Bambu's x86-64 build over ours.
- The Bambu plugin is only used while a Bambu printer preset is selected.
- Live camera: Orca plays it through GStreamer and refuses to start unless
  `h264parse` and an H.264 decoder are installed (even though this camera is
  MJPEG), plus `jpegdec` (gst-plugins-bad/-good, libav).
- Contract details: [docs/orca-contract.md](docs/orca-contract.md).

## Build from source

Built inside the freedesktop 26.08 SDK, the base of Studio's GNOME 51 runtime, so
glibc/libstdc++/OpenSSL/libcurl match the Studio flatpak exactly (and are old
enough for any current distribution):

```sh
flatpak install flathub org.freedesktop.Sdk//26.08   # once
scripts/build.sh                                      # → dist/*.so (Studio + Orca), runs unit tests
tests/integration.sh                                  # fake printer, Studio plugin, in Studio's runtime
BBL_TEST_TARGET=orca tests/integration.sh             # same for the Orca plugin
BBL_TEST_TARGET=orca BBL_TEST_RUNNER=native tests/integration.sh   # loaded by a host-built process
scripts/package.sh v0.1.0                             # → dist/bambu-lan-plugin-v0.1.0-aarch64.tar.gz
```

On NixOS, run the native test under `nix-shell -p gcc pkg-config openssl curl --run ...`.

Install from the source tree with `scripts/install.sh` (same options).

## Usage

The original x86-64 files are kept in `plugins/x86_64-original/`. The plugin
reports version `02.09.01.99` (Studio) / `02.03.00.99` (Orca), so the slicer
accepts it (same first 8 characters) and does not offer to "update" it back to
Bambu's x86-64 build.

Then in the slicer: Device tab → add the printer (it appears by itself if the
broadcasts reach this machine, otherwise use "connect via IP") → enter the
access code from the printer screen.

Plugin log: `bambu_lan_plugin.log` in the log directory the slicer passes to the
plugin (its `log/` folder). `BBL_LAN_DEBUG=1` also logs to stderr,
including MQTT traffic.

## Troubleshooting

- **Device tab says the network plugin is missing / needs an update after a slicer upgrade**:
  the new version expects a different plugin version, or its OTA updater copied
  Bambu's x86-64 build over ours. Follow [docs/updating.md](docs/updating.md), then
  re-run `install.sh`.
- **OrcaSlicer asks to download the network plug-in**: `network_plugin_version` points
  to a file that isn't ours (for example after picking another version in Preferences).
  Close Orca and re-run `install.sh --orca`.
- **"Incorrect password"**: the access code changed on the printer, or LAN-only mode is off.
- **Connects, but commands are ignored**: enable Developer Mode on the printer (unsigned control).
- **Printer not discovered**: SSDP broadcasts don't cross VLANs/VPNs; add it by IP.
- **X1/H2 camera stays black**: enable LAN live view on the printer ("LAN Only Liveview"),
  and for OrcaSlicer make sure GStreamer has an H.264 decoder (`openh264dec` or `avdec_h264`).
  The plugin log (`bambu_lan_plugin.log`) names the RTSP step that failed.

## Testing against a real printer without the GUI

```sh
export BBL_ACCESS_CODE=xxxxxxxx   # from the printer screen
P=build/bbl_lan_probe
$P dist detect  192.168.1.50
$P dist connect 192.168.1.50 <serial>
$P dist verify  192.168.1.50 <serial>          # FTPS login check (Studio's "verify_job")
$P dist camera  192.168.1.50 <serial> frame.jpg  # A1/P1
$P dist camera_rtsp 192.168.1.50 live.h264        # X1 and newer; play with: ffplay live.h264
```

Run them inside Studio's runtime to test the exact environment:
`flatpak run --filesystem=$PWD --command=sh com.bambulab.BambuStudio -c "cd $PWD && ..."`.

## Releasing

CI (`.github/workflows/build.yml`, native arm64 runner) builds, tests and
uploads the tarball for every push and PR. Pushing a `v*` tag also publishes
it as a GitHub release:

```sh
git tag -a v0.2.0 -m v0.2.0 && git push --follow-tags
```

If a release with that tag already exists (e.g. uploaded by hand), CI only adds
missing assets and never replaces them, so a published hash stays valid for
packagers (e.g. a Nix `fetchurl`).

## Layout

```
src/common/       logger, OpenSSL TLS socket
src/networking/   libbambu_networking*.so: agent, MQTT, SSDP, FTPS, print jobs, C exports
src/source/       libBambuSource.so: camera URL parsing, A1/P1 JPEG stream, RTSPS/H.264 stream, Bambu_* exports
third_party/      nlohmann/json 3.11.3; ABI headers of Bambu Studio (bambu/) and OrcaSlicer (orca/), AGPL-3.0
scripts/          build, package, install, upstream (track new slicer releases)
tests/            unit tests, probe tool, fake printer (MQTT, both cameras) + integration test
docs/             how it works, Studio/Orca ↔ plugin contracts, updating guide
```

## License

AGPL-3.0, like the Bambu Studio and OrcaSlicer headers it builds against
(`third_party/bambu/`, `third_party/orca/`, copied from those AGPL-3.0
projects with their origin recorded in `UPSTREAM`).

## Legal notice

- This is an independent, unofficial project. It is **not affiliated with,
  endorsed by or supported by Bambu Lab** (Shenzhen Tuozhu Technology Co., Ltd.),
  OrcaSlicer or their authors. "Bambu Lab", "Bambu Studio", "A1", "P1", "X1" and
  related names are trademarks of their respective owners, used here only to
  describe compatibility. No logos or other branding are used.
- Its sole purpose is **interoperability**: letting the open-source Bambu Studio
  and OrcaSlicer applications use printers you own, over your own local
  network, on a CPU architecture for which no official plugin exists.
- It contains **no code, binaries, keys or certificates from Bambu Lab's
  closed-source software**, and was not made by decompiling or disassembling it.
  It was written from the slicers' open-source (AGPL-3.0) code, which declares the
  plugin interface, and from publicly documented printer protocols.
- It does **not** circumvent any access control: it uses the access code the
  owner reads from the printer's own screen, does not sign commands, does not
  bypass firmware authorization, and does not talk to Bambu Lab's cloud
  services. Contributions adding extracted keys, signing or cloud
  authentication bypasses will be refused.
- Provided "as is", without warranty of any kind (see LICENSE). Using third-party
  software with your printer may affect its warranty or support; that is your
  decision.
- Rights holders with concerns can open an issue or contact the maintainer
  through GitHub; concerns will be addressed promptly.
