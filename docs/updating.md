# Updating for a new Bambu Studio or OrcaSlicer release

A guide for humans and LLM agents. Follow the steps in order; each ends with a
check. Stop and report if a check fails in a way this guide doesn't cover.

Background: [how-it-works.md](how-it-works.md) explains the design, and
[slicer-contract.md](slicer-contract.md) lists what the slicer expects, with
source references.

## When an update is needed

Studio accepts the plugin only if the first 8 characters of
`bambu_network_get_version()` equal its own `SLIC3R_VERSION` (for example
`02.09.01`). OrcaSlicer applies the same rule against the plugin version
selected in its preferences (step 5b). So:

| Studio change | Example | Action |
|---|---|---|
| Build number only | `02.09.01.70` → `02.09.01.80` | Usually nothing. Run steps 1, 3 and 5 to confirm the ABI didn't change |
| Patch/minor/major | `02.09.01` → `02.09.02` / `02.10.00` | Full procedure. Studio rejects the old plugin ("Network Plug-in is not detected") |
| Flatpak runtime bump | GNOME 51 → 52 | Step 2 (build SDK) |
| OrcaSlicer release | 2.4.2 → 2.5.x | Step 5b |

## 0. Prerequisites

```sh
flatpak install flathub com.bambulab.BambuStudio org.freedesktop.Sdk//26.08
git clone https://github.com/cyrinux/bambu-lan-plugin-aarch64 && cd bambu-lan-plugin-aarch64
```

For step 3 you also need a local Bambu Studio checkout of both tags:

```sh
git clone --filter=blob:none https://github.com/bambulab/BambuStudio.git research/bambustudio-src
```

## 1. Find the new version

```sh
scripts/upstream.sh check
```

This prints the pinned tag (`third_party/bambu/UPSTREAM`), the latest upstream
tag, the installed flatpak's `SLIC3R_VERSION`, its runtime, and whether that
runtime's glibc/libstdc++/OpenSSL/curl match the build SDK.

Pick the upstream tag whose `SLIC3R_VERSION` equals the installed flatpak's
(tags are `v<SLIC3R_VERSION>`, e.g. `v02.09.01.70`). If the flatpak isn't
updated yet, use the latest tag.

✅ Check: you have a tag `NEW` and know `OLD` (`sed -n 's/^tag=//p' third_party/bambu/UPSTREAM`).

## 2. Build SDK vs runtime

If `check` reports a mismatch, or the flatpak runtime changed (for example
`org.gnome.Platform//52`), find the freedesktop SDK that runtime is built on:
GNOME N ships on freedesktop SDK `YY.08`. Install it, then run
`BBL_SDK=org.freedesktop.Sdk//YY.08 scripts/upstream.sh check` until it reports a match.

Then update the SDK reference in:

- `scripts/build.sh` (`SDK=` default)
- `scripts/upstream.sh` (`SDK=` default)
- `tests/integration.sh` (fallback `BBL_TEST_APP`)
- `.github/workflows/build.yml` (`flatpak install … org.freedesktop.Sdk//…`, `BBL_TEST_APP`)
- `docs/how-it-works.md` §6 and the README build section

✅ Check: `scripts/upstream.sh check` prints "runtime and build SDK match".

Why it matters: the ABI passes `std::string` and `std::function` by value, so the
plugin must use the same libstdc++ as the slicer.

## 3. Sync the ABI headers and review what changed

```sh
scripts/upstream.sh sync NEW          # vendors headers, regenerates typedefs, bumps BBL_STUDIO_PLUGIN_VERSION
git diff third_party/                  # ABI changes at a glance
```

Then diff the parts of the slicer that drive the plugin between the two tags:

```sh
cd research/bambustudio-src && git fetch --tags -q
git diff OLD NEW --stat -- src/slic3r/Utils/NetworkAgent.* src/slic3r/Utils/bambu_networking.hpp \
  src/slic3r/Utils/FileTransferUtils.* src/slic3r/Utils/CertificateVerify.cpp \
  src/slic3r/GUI/GUI_App.cpp src/slic3r/GUI/DeviceCore/DevManager.cpp src/slic3r/GUI/DeviceManager.cpp \
  src/slic3r/GUI/Jobs/PrintJob.cpp src/slic3r/GUI/Jobs/SendJob.cpp src/slic3r/GUI/SelectMachine.cpp \
  src/slic3r/GUI/MediaPlayCtrl.cpp src/slic3r/GUI/wxMediaCtrl3.cpp src/slic3r/GUI/AVVideoDecoder.cpp \
  src/slic3r/GUI/Printer/ resources/printers/
```

Review each changed area against this table. The "what to look for" column is
what would break us.

| Area | Files | What to look for | Where to adapt |
|---|---|---|---|
| Plugin loading | `GUI_App.cpp` (`check_networking_version`, `on_init_network`), `NetworkAgent.cpp` (`initialize_network_module`), `CertificateVerify.cpp` | version rule changed; **Linux signature check added** (the `#else` branch of `SummarizeSelf` no longer `nullopt`); new mandatory symbol not null-checked | `version.hpp`; if signatures become mandatory on Linux, the project needs a new approach: stop and report |
| Exported functions | `NetworkAgent.hpp` typedefs | changed parameter lists of functions we export | the build fails with `ABI mismatch for bambu_network_X`: fix the signature in `src/networking/exports_*.cpp` |
| New functions | `NetworkAgent.hpp` / `.cpp` | new `func_*` used in LAN flows **without** a null check | add a stub or an implementation in `exports_cloud_stubs.cpp` / `exports_core.cpp` |
| Structs | `bambu_networking.hpp` (`PrintParams`, `detectResult`) | renamed or removed fields | the build fails where we use them: `print_job.cpp`, `detect.cpp` |
| Discovery | `DevManager.cpp` `on_machine_alive` | JSON keys read from the SSDP callback | `ssdp_listener.cpp` `ssdp_to_device_json` |
| Connection | `GUI_App.cpp` local-connect lambda | meaning of `status` / `msg` (e.g. the `"5"` code) | `agent.cpp` `connect_failure_msg`, `run_link` |
| Printing | `PrintJob.cpp`, `SendJob.cpp` | which `start_*` function is called, `PrintParams` fields set, stage/progress meaning, `verify_job` | `print_job.cpp` |
| `ft_*` tunnel | `FileTransferUtils.*` and its callers | new unchecked calls, or LAN flows that now *require* the tunnel | `exports_ft.cpp` (compare with `third_party/bambu/FileTransferUtils.hpp.ref`) |
| Camera | `MediaPlayCtrl.cpp` (URL), `wxMediaCtrl3.cpp` (call sequence), `Printer/BambuTunnel.h` | URL formats `bambu:///local/...` and `bambu:///rtsps___...`, required `Bambu_*` symbols, `Bambu_StreamInfo` / `Bambu_Sample` layout, frame-rate/size rules | `src/source/` |
| Models | `resources/printers/*.json` | new models (`sn_prefix`, `model_id`) | `printer_models.cpp` table + unit test |

When behaviour changes, update `docs/slicer-contract.md` (keep the file:line
references current) and add or adjust a unit or integration test that
captures it.

✅ Check: every row was either "unchanged" or adapted.

## 4. Build and test

```sh
scripts/build.sh            # compile (ABI static_asserts) + unit tests
tests/integration.sh        # fake printer, run inside the installed Studio runtime
```

✅ Check: build OK, `0 failures`, all integration lines `PASS`.

## 5. Test in Bambu Studio

```sh
scripts/install.sh          # Studio closed
```

Then, in Studio:
- [ ] no "Network Plug-in is not detected" notice
- [ ] Device tab: the printer connects (add by IP if needed) and status updates
- [ ] a wrong access code shows "Incorrect password"
- [ ] live camera shows frames (A1/P1 JPEG; X1 and newer RTSPS/H.264)
- [ ] send a small print, which starts on the printer
- [ ] "Send to printer storage" uploads a file
- [ ] `log/bambu_lan_plugin.log` has no `ERROR` lines

Without the GUI, against the real printer:
`BBL_ACCESS_CODE=… build/bbl_lan_probe dist connect|verify|camera <ip> <serial> …`,
or `… camera_rtsp <ip> out.h264` for an RTSPS camera (`ffplay out.h264` to watch it).

## 5b. OrcaSlicer

Orca vendors Bambu's plugin API with its own changes and loads
`plugins/libbambu_networking_<version>.so`, `<version>` being
`app.network_plugin_version`. Its table `AVAILABLE_NETWORK_VERSIONS`
(`src/slic3r/Utils/bambu_networking.hpp`) lists the Bambu plugin series it
supports, each with an ABI (`NetworkAbi::Current`, `V0203`, `Legacy` from
2.5 on). We build `libbambu_networking_orca.so` against Orca's headers and
install it as `libbambu_networking_<our version>.so` (Orca saves the loaded
plugin's version as the configured one) and `…_<base>-lan.so` (listed in
Preferences). Read [orca-contract.md](orca-contract.md) first.

1. `scripts/upstream.sh check` shows the pinned and latest Orca tags.
2. `scripts/upstream.sh sync-orca vX.Y.Z` vendors `third_party/orca/`
   (`bambu_networking.hpp` adapted, typedefs from `BBLNetworkPlugin.hpp`),
   records the `is_latest` base in `third_party/orca/UPSTREAM` and sets
   `BBL_ORCA_PLUGIN_VERSION`.
3. Review in the Orca source between the two tags, with the same table as
   step 3 but these files: `Utils/BBLNetworkPlugin.*` (symbols loaded, how
   the version is chosen, ABI selection per series), `Utils/BBLPrinterAgent.*`
   and `Utils/BBLCloudServiceAgent.*` (which calls are null-checked),
   `Utils/NetworkAgent*.cpp`, `GUI/GUI_App.cpp` (`check_networking_version`,
   `on_init_network` config sync, plugin download/OTA, hot reload),
   `libslic3r/AppConfig.cpp` (config key migration), `GUI/Printer/gstbambusrc.c`
   and `GUI/MediaPlayCtrl.cpp` (camera).
   Check especially:
   - **the ABI used for our base**: if Orca maps the series we build for to a
     converted struct (2.5 has `PrintParams_0203` for the `02.03.00` series and
     the Studio layout for `02.08.04`), build against the matching struct. If the
     newest series uses the Studio layout, Orca may accept the Studio build.
   - **the base names**: 2.4 lists full versions (`02.03.00.62`), 2.5 lists
     8-character series (`02.03.00`). The suffix file must be
     `<an entry of the table>-lan`.
   - Studio-only exports guarded by `#ifndef BBL_TARGET_ORCA` in
     `src/networking/exports_*.cpp`: the build fails on any symbol missing from
     Orca's typedefs; add or remove guards accordingly.
4. Build and test:
   ```sh
   scripts/build.sh
   BBL_TEST_TARGET=orca tests/integration.sh
   BBL_TEST_TARGET=orca BBL_TEST_RUNNER=native tests/integration.sh   # NixOS: inside nix-shell -p gcc pkg-config openssl curl
   ```
5. In Orca: `install.sh --orca` (or `--orca-flatpak`), restart Orca twice (the
   second start loads the version Orca saved), then run through the step 5
   checklist with a Bambu printer preset selected.
6. Update `docs/orca-contract.md`.

## 6. Release

```sh
git commit -am "feat: support Bambu Studio <x.y.z>"   # or OrcaSlicer
git tag -a vX.Y.Z -m vX.Y.Z
git push --follow-tags
```

CI builds, tests and publishes `bambu-lan-plugin-vX.Y.Z-aarch64.tar.gz` (+
`.sha256`) as the GitHub release (notes from `docs/release-notes.md`: update it
first). Bump the minor version when a supported slicer version changes. The
tarball's `COMPAT` file names the slicer tags and plugin versions.

Packagers (e.g. Nix): update `version`, `orcaPluginBase` (if changed) and the hash
(`nix store prefetch-file --json <release url> | jq -r .hash`).

## Notes for LLM agents

- Read `docs/how-it-works.md` and `docs/slicer-contract.md` first. Read
  the upstream source to find behaviour; don't infer it from function names.
- Never weaken an `ABI mismatch` static_assert to get a build through. Fix
  the exported signature instead.
- Keep the plugin LAN-only. Never embed Bambu credentials, certificates, or
  binaries.
- Integration tests use `tests/fake_printer.py`. If upstream changes the wire
  protocol, update the fake printer *and* say so in the PR, because it then no
  longer proves compatibility with real firmware.
- Report clearly what could not be verified (no printer, no GUI).
