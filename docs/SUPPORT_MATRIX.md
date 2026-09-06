# HardwareScope native support and qualification

This is an implementation inventory, not a certification of every model or driver.

| Area | Implemented boundary | Required validation before claiming a device supported |
|---|---|---|
| CPU temperature | AMD Zen provider and selected family/model CCD register paths | CPU/firmware-specific idle/load comparison; stable physical CCD IDs |
| Intel CPU temperature | No native temperature provider currently implemented | Remains unsupported; generic CPU usage is separate |
| NVIDIA GPU | NVAPI core telemetry, model-dependent supplemental thermal paths, PCI-matched NVML | Model/driver channel validity and multi-GPU identity; never infer adapter mapping from enumeration order |
| AMD GPU | Implemented vendor telemetry/PMLog paths | Model/driver scaling and reordered-adapter identity |
| Motherboard | Selected Nuvoton NCT6687/NCT6799 paths | Board-specific labels/registers, lock contention and fan/voltage validation |
| Memory temperature | Implemented DDR5 SPD/SMBus path | Controller/DIMM compatibility; missing readings remain unavailable |
| Storage | Implemented Windows storage/NVMe interfaces | Composite vs auxiliary sensors, firmware and failure behavior |
| Game FPS | PresentMon 2.4.1 application presentation intervals | Supported game/API, stream selection, severe stalls and capture transitions |
| OSD | Layered topmost Windows surfaces, including split FPS | Borderless/windowed support; exclusive fullscreen is not universally guaranteed |

Game-only mode uses recognized executable/library-path evidence, not window size
alone or any arbitrary `Games` folder. Unknown fullscreen applications can be
considered when game-only mode is disabled. Classification remains heuristic.

Unsupported or failed readings must not be replaced with guessed values. A retry
failure invalidates the affected provider cache; a cached value between scheduled
reads is intentionally held until the next measurement attempt.

FPS streams are selected using bounded recent present counts with hysteresis and
a cessation fallback. Switching streams resets statistics. Valid long intervals
up to the 60-second history length are retained; larger intervals are treated as
capture discontinuities. These are presentation FPS, not a promise of displayed
or generated frames. Buffered-output timestamp validation still needs qualification.

Keyboard shortcuts, larger text, high-contrast palettes and native settings
controls exist; full sensor-row UIA/Narrator support is not yet qualified.
Single-instance behavior is per Windows session; simultaneous user sessions need
an explicit ownership policy before claiming whole-computer enforcement.

The default Program Files installation has protected permissions. Alternate
directories and installer failure recovery need dedicated disposable-machine tests.
Do not treat the source audit or passing deterministic tests as those tests.
