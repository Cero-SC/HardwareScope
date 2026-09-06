# PawnIO prerequisite (official signed edition)

Pinned version: 2.2.0. The installer is redistributed unmodified.

Source: https://github.com/namazso/PawnIO.Setup/releases/tag/2.2.0

SHA-256: `1F519A22E47187F70A1379A48CA604981C4FCF694F4E65B734AAA74A9FBA3032`

The publisher's integration guide permits redistribution of the official installer:
https://github.com/namazso/PawnIO.Modules/wiki/Using-PawnIO-Modules
The official binary edition has its own license; it is not covered by HardwareScope's license.
Use the official signed edition, never the unrestricted edition.

HardwareScope Setup installs this prerequisite when no compatible installation exists.
It is shared with other hardware utilities and is deliberately retained on uninstall.

For the portable ZIP: run PawnIO_setup.exe once as administrator, then run
HardwareScope.exe as administrator for direct supported CPU/board/DIMM access.
Portable mode does not install the HardwareScope sensor service and does not provide
service-backed FPS capture. Use HardwareScope Setup for the full feature set.

Administrative silent installation: `PawnIO_setup.exe -install -silent`.
Exit 3010 means installation succeeded and Windows requires a restart.
