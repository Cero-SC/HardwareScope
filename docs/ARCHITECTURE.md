# HardwareScope 2.0 architecture

## Non-negotiable rules

1. Hardware access never runs on the window thread.
2. Rendering never waits for hardware access, ETW, storage I/O, settings I/O, or networking.
3. Snapshot publication is bounded and cannot grow with runtime.
4. The UI paints only visible sensor rows and only after Windows asks it to paint.
5. Window movement and resizing are native non-client operations handled by DWM.
6. Expensive providers use independent polling schedules and cache static metadata.
7. The stable 1.x build remains available until native sensor and updater parity is verified.
8. The main telemetry window defaults to software Direct2D to avoid GPU-driver memory, thread, and handle overhead.

## Process model

The desktop application runs without elevation. A separate LocalSystem service
collects privileged sensors using the installed official PawnIO runtime and
embedded modules. The service starts PresentMon only for requested game capture.
The updater is a separate native executable that runs only during an update.

## Data flow

```text
Native providers -> polling coordinator -> local SensorSnapshot
                                             |
                                         SnapshotStore
                                             |
                         WM_APP_SNAPSHOT -> Direct2D renderer

Game detector -> control pipe -> service/PresentMon -> shared snapshot -> UI worker
```

The worker builds a complete snapshot locally and publishes it in one operation. A
short mutex protects only the bounded snapshot memory copy; it never encloses
hardware collection or I/O. The renderer copies the most recent snapshot and never
walks provider objects. This deliberately favors a small, provably coherent critical
section over a complex lock-free protocol.

## Provider boundaries

Providers expose discovery, static metadata, polling cadence, and value collection.
The experimental direct ETW engine is built only into HardwareScopeNativeFpsProbe;
it is not a runtime fallback. Production frame analysis uses PresentMon's CSV stream.

The control pipe retains its nonblocking connection between polls. The service
connects anonymously, checks the server executable against the installed sibling
HardwareScope.exe, and expires an abandoned FPS request after 15 seconds
(up to 32 seconds at the slowest hardware polling interval).
Snapshots older than five seconds are rejected so stopped collection cannot look live.
These are failure-handling boundaries, not process isolation for every provider:
a native provider crash can still terminate its hosting process.
