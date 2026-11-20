Native aarch64 LAN plugin for **Bambu Studio 2.9.x** and **OrcaSlicer 2.4.x**, tested with a Bambu Lab A1 in Bambu Studio.

Install: close the slicer, extract the tarball, then run:

- `./install.sh`: Bambu Studio flatpak
- `./install.sh --native`: Bambu Studio, non-flatpak
- `./install.sh --orca`: OrcaSlicer, non-flatpak
- `./install.sh --orca-flatpak`: OrcaSlicer flatpak

Add `--uninstall` to restore the original files.

`COMPAT` lists the exact slicer versions. See the README for supported printers and features. LAN only: no Bambu cloud, no SD-card file browser.
