<p align="center">
  <img src="assets/hardwarescope-windows-icon-master.png" alt="HardwareScope logo" width="160">
</p>

# HardwareScope

[![Windows CI](https://github.com/Cero-SC/HardwareScope/actions/workflows/ci.yml/badge.svg)](https://github.com/Cero-SC/HardwareScope/actions/workflows/ci.yml)
[![Latest release](https://img.shields.io/github/v/release/Cero-SC/HardwareScope)](https://github.com/Cero-SC/HardwareScope/releases/latest)

HardwareScope is a lightweight, read-only Windows hardware monitor written in
C++23 with Win32, Direct2D, and DirectWrite. It provides live hardware sensors,
a configurable on-screen display, and game-only FPS monitoring without a web
runtime or managed UI framework.

## Download

Open the [latest release](https://github.com/Cero-SC/HardwareScope/releases/latest)
and download `HardwareScope-Setup-<version>-x64.exe` under **Assets**.

HardwareScope is currently unsigned. Windows may show an **Unknown publisher**
or SmartScreen warning. Every release includes `SHA256SUMS.txt` so the installer
can be verified before it is opened.

## Code signing policy

HardwareScope is preparing to use sponsored open-source signing. Free code
signing provided by [SignPath.io](https://signpath.io/), certificate by
[SignPath Foundation](https://signpath.org/). Until Windows shows a valid
SignPath Foundation signature for a release, that release remains unsigned.

See the complete [code signing policy](CODE_SIGNING_POLICY.md), including team
roles and release controls, and the [privacy policy](PRIVACY.md).

## Highlights

- CPU, GPU, storage, memory, motherboard, fan, power, voltage, usage, and clock
  telemetry when supported by the installed hardware
- Searchable sensor table organized into collapsible sections
- Dark and light themes with configurable accent/text colors
- Compact OSD with corner placement, vertical or horizontal layout, spacing,
  opacity, and scaling controls
- A simple Favorites view with one-click stars and automatic CPU/GPU temperature defaults
- Portable settings import and export with format validation
- A lightweight first-run setup with recommended or customizable configuration
- Standard, large, and extra-large interface text plus an optional high-contrast palette
- Keyboard shortcuts: `Ctrl+F` searches sensors, `Ctrl+,` opens Settings, and `Esc` clears search
- Independent, game-only FPS capture and display settings
- Rolling 1% low FPS calculated from the slowest recent PresentMon frame times
- HWiNFO-style live graphs in the OSD or a detachable resizable window, with up
  to four same-unit sensor lines, fixed/adaptive/custom scales, grid and value
  labels, 5-second to 5-minute history, independent refresh, colors, thickness,
  pause/reset, zoom, and saved window placement
- Minimize-to-tray behavior, startup options, and single-instance enforcement
- Verified GitHub updates with installer size and SHA-256 validation
- Statically linked MSVC runtime; no separate Visual C++ redistributable needed

## Sensor access and safety

HardwareScope reads telemetry only. It does not change voltages, fan curves,
clock speeds, memory timings, or power limits. Its Windows service performs the
small amount of privileged read-only sensor access that cannot be done from the
normal desktop process.

Unsupported or ambiguous readings are omitted instead of being guessed. Sensor
coverage depends on the CPU, GPU, motherboard controller, DIMMs, storage
firmware, and available vendor interfaces.

## Requirements

- Windows 10 or Windows 11
- 64-bit processor
- Administrator approval during installation

Setup includes the official PawnIO 2.2.0 installer for supported CPU,
motherboard and DDR5 temperature access. It checks existing installations and
does not remove the shared driver when HardwareScope is uninstalled.
Some hardware remains unsupported; installing the driver does not add sensor
support for every CPU or motherboard.

The portable ZIP includes PawnIO Setup under `prerequisites/`. It requires
manual prerequisite installation and elevation for direct privileged sensors;
the full installer is recommended for service-backed monitoring and FPS.

## Build from source

Prerequisites:

- Visual Studio 2022 or newer with Desktop development with C++
- CMake 3.28 or newer
- Inno Setup 6 to create the installer

```powershell
cmake --preset windows-x64-release
cmake --build --preset windows-x64-release
ctest --preset windows-x64-release
```

The production application is written to
`out/build/x64-release/Release/HardwareScopeNative.exe`. The project also builds
separate test/probe executables; test-only window hooks are not compiled into
the packaged application.

## Project layout

- `src/` and `include/` — application, OSD, sensor providers, updater, and service
- `resources/` — application assets and embedded read-only PawnIO modules
- `installer/` — Inno Setup template
- `tests/` — deterministic core, UI, updater, hardware, and soak probes
- `docs/` — architecture, performance budgets, and second-PC validation
- `third_party/` — pinned runtime dependencies included with attribution

See [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) for dependency and license
details, [PRIVACY.md](PRIVACY.md) for network behavior, and
[RELEASING.md](RELEASING.md) for the verified release process.

## License

HardwareScope is released under the terms in [LICENSE.txt](LICENSE.txt).
