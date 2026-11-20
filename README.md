# bambulab-a1-aarch64: Bambu Studio LAN plugin for arm64 Linux

[![build](https://github.com/cyrinux/bambulab-a1-aarch64/actions/workflows/build.yml/badge.svg)](https://github.com/cyrinux/bambulab-a1-aarch64/actions/workflows/build.yml)

Bambu Studio talks to printers through two closed-source plugins that Bambu only
ships for x86-64 (`libbambu_networking.so`, `libBambuSource.so`). On an aarch64
machine the slicer cannot load them, so the Device tab, sending prints and the
camera don't work.

This project is an open-source, native aarch64 reimplementation of the **LAN**
part of those plugins, written against the plugin ABI published in Bambu
Studio's source (`src/slic3r/Utils/NetworkAgent.hpp`, `bambu_networking.hpp`,
`GUI/Printer/BambuTunnel.h`, pinned to **v02.09.01.70**). How it was done:
[docs/how-it-works.md](docs/how-it-works.md).

Tested: Bambu Studio 2.9.1 flatpak on aarch64 with a **Bambu Lab A1**.

## Printer compatibility

All Bambu printers use the same LAN protocols for discovery, MQTT and FTPS, so
status, control and printing should work beyond the A1. The camera is the
main difference.

| Printer | Status / control / print | Camera | Notes |
|---|---|---|---|
| A1 | ✅ tested | ✅ tested | |
| A1 mini, P1P, P1S | ✅ expected | ✅ expected | same port-6000 JPEG camera and `file:///sdcard/` print URL as the A1 |
| X1, X1 Carbon, X1E | ✅ expected | ❌ | camera is RTSPS (port 322, H.264), not implemented |
| P2S, H2D, H2D Pro, H2S, H2C, X2D, A2L, H3C | ⚠️ untested | ❌ | `ftp:///` print URL; may need the `ft_*` tunnel (eMMC) or signed commands on newer firmware |

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

## Not supported (by design)

- Bambu cloud: login, cloud printing, MakerWorld, presets sync, firmware/OTA via cloud.
  The plugin reports "not logged in"; cloud calls fail cleanly.
- Printer file browser (timelapse/model list on the SD card): proprietary protocol on the port-6000
  control channel. Studio shows its normal "unavailable" state.
- The `ft_*` file-transfer tunnel (eMMC printers, brtc). Studio falls back to FTPS.
- Message signing. Recent firmware requires **LAN-only mode + Developer Mode** on the printer
  for unsigned third-party control (Settings → LAN Only Mode → Developer Mode on the printer screen).

## Install from a release

Download `bambu-lan-plugin-*-aarch64.tar.gz` from the
[releases](https://github.com/cyrinux/bambulab-a1-aarch64/releases), close Bambu Studio, then:

```sh
tar xzf bambu-lan-plugin-*-aarch64.tar.gz && cd bambu-lan-plugin-*-aarch64
./install.sh               # flatpak Studio (~/.var/app/com.bambulab.BambuStudio/...)
./install.sh --native      # non-flatpak Studio (~/.config/BambuStudio)
./install.sh --uninstall   # restore Bambu's original files
```

In Studio, **don't click "Install Network Plug-in"**: that downloads Bambu's
x86-64 build again.

## Build from source

Built inside the freedesktop 26.08 SDK, the base of Studio's GNOME 51 runtime, so
glibc/libstdc++/OpenSSL/libcurl match exactly:

```sh
flatpak install flathub org.freedesktop.Sdk//26.08   # once
scripts/build.sh                                      # → dist/*.so, runs unit tests
tests/integration.sh                                  # fake printer, run in Studio's runtime
scripts/package.sh v0.1.0                             # → dist/bambu-lan-plugin-*.tar.gz
```

Install from the source tree with `scripts/install.sh` (same options).

## Usage

The original x86-64 files are kept in `plugins/x86_64-original/`. The plugin
reports version `02.09.01.99` so Studio accepts it (prefix `02.09.01`) and does
not offer to "update" it back to Bambu's x86-64 build.

Then in Studio: Device tab → add the printer (it appears by itself if the
broadcasts reach this machine, otherwise use "connect via IP") → enter the
access code from the printer screen.

Plugin log: `bambu_lan_plugin.log` in the log directory Studio passes to the
plugin (Studio's `log/` folder). `BBL_LAN_DEBUG=1` also logs to stderr,
including MQTT traffic.

## Troubleshooting

- **Device tab says the network plugin is missing / needs an update after a Studio upgrade**:
  the new Studio version has a different `02.xx.yy` prefix, or its OTA updater copied
  Bambu's x86-64 build from `ota/plugins/` over ours. Rebuild against the matching tag
  (update `third_party/bambu/*` and `BBL_LAN_PLUGIN_VERSION`), then re-run `scripts/install.sh`.
- **"Incorrect password"**: the access code changed on the printer, or LAN-only mode is off.
- **Connects, but commands are ignored**: enable Developer Mode on the printer (unsigned control).
- **Printer not discovered**: SSDP broadcasts don't cross VLANs/VPNs; add it by IP.

## Testing against a real printer without the GUI

```sh
export BBL_ACCESS_CODE=xxxxxxxx   # from the printer screen
P=build/bbl_lan_probe
$P dist detect  192.168.1.50
$P dist connect 192.168.1.50 <serial>
$P dist verify  192.168.1.50 <serial>          # FTPS login check (Studio's "verify_job")
$P dist camera  192.168.1.50 <serial> frame.jpg
```

Run them inside Studio's runtime to test the exact environment:
`flatpak run --filesystem=$PWD --command=sh com.bambulab.BambuStudio -c "cd $PWD && ..."`.

## Releasing

Release tarballs are built and tested locally, then uploaded as-is, so their
hash is stable for packagers (e.g. a Nix `fetchurl`). CI builds and tests every push.

```sh
git tag v0.1.0 && scripts/package.sh v0.1.0 && tests/integration.sh
gh release create v0.1.0 dist/bambu-lan-plugin-v0.1.0-*.tar.gz* --notes-file docs/release-notes.md
```

## Layout

```
src/common/       logger, OpenSSL TLS socket
src/networking/   libbambu_networking.so: agent, MQTT, SSDP, FTPS, print jobs, C exports
src/source/       libBambuSource.so: bambu:///local URL parser, A1 camera stream, Bambu_* exports
third_party/      nlohmann/json 3.11.3, Bambu Studio ABI headers (AGPL-3.0)
tests/            unit tests, probe tool, fake printer + integration test
docs/             how it works, slicer↔plugin contract (with source line references)
```

## License

AGPL-3.0, like the Bambu Studio headers it builds against.
