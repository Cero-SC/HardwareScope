# Performance and release budgets

These are candidate qualification targets. Historical measurements below are not
evidence for a newly built candidate; record fresh results before stable promotion.

| Area | Release target |
|---|---:|
| Desktop idle total CPU | <= 0.10% on the 32-thread reference PC |
| Working set after five minutes | <= 80 MB |
| Private memory after five minutes | <= 60 MB |
| UI p95 paint duration | <= 4 ms at 1920x1080 |
| Window drag/resize | no sensor-caused frame stalls |
| Snapshot handoff | bounded, no unbounded queues |
| Handle growth over five minutes | zero sustained growth |
| Sensor provider crash | may terminate its host; UI/service recovery must be tested |
| FPS capture on desktop | stopped |
| Settings/update network work | never blocks the UI thread |

## Native shell baseline (2026-08-24)

Measured over 20 seconds on the 32-thread reference PC with a 500 ms synthetic
sensor cadence and the window visible:

| Renderer | Total CPU | Working set | Private memory | Threads | Handles |
|---|---:|---:|---:|---:|---:|
| Direct2D hardware/default | 0.0096% | 49.8 MB | 65.0 MB | 43 | 501 |
| Direct2D software | 0.0168% | 32.4 MB | 23.0-25.0 MB | 19-20 | 183-184 |

Software Direct2D is the default. Its negligible CPU increase is preferable to
loading the GPU driver stack for a low-frequency telemetry surface. The final
path uses immediate DWM presentation, no unnecessary retained backbuffer, and
aliased geometry while preserving antialiased text. A later hardware-renderer
experiment reduced paint latency but reached 87 MB working set and 94 MB private
memory, so it was rejected rather than weakening the memory budgets.

## Integrated native 2.0 profile (2026-08-24)

Measured for 20 seconds after UI/settings warmup with hardware at 750 ms and
game-only FPS enabled but inactive on the desktop:

| Total CPU | One-core CPU | Working set | Private memory | Threads | Handles | UI p95 paint |
|---:|---:|---:|---:|---:|---:|---:|
| 0.0195% | 0.625% | 65.6 MB | 46.8 MB | 18 | 287 | 2.91 ms |

The FPS refresh cadence switches to 100 ms only after a real game frame sample
exists. On the desktop the entire UI/OSD remains on the 750 ms hardware cadence.

## Combined UI and privileged collector profile (2026-08-24)

Measured for 20 seconds after warmup with the normal UI and the elevated,
read-only sensor collector running together on the 32-thread reference PC:

| Total CPU | One-core CPU | Working set | Private memory | Threads | Handles |
|---:|---:|---:|---:|---:|---:|
| 0.0537% | 1.719% | 56.5 MB | 34.4 MB | 16 | 506 |

This measurement includes the process that reads AMD Zen, Nuvoton, and DDR5
telemetry and publishes it through the bounded shared-memory bridge. It remains
inside the desktop CPU and memory budgets.

## Native FPS validation (2026-08-24)

The final static-runtime Direct3D 11 flip-model fixture reports 238 FPS through the game-only ETW
path. FPS publishes independently near 100 ms after the first frame sample,
while hardware remains at 750 ms. Capture uses raw QPC timestamps and handles
DXGI Present, DXGI multiplane Present, D3D9 Present, and the filtered DxgKrnl
Present markers used as the compatibility path. Exact Present keyword matching
rejects unrelated kernel events before they reach the app callback. The final
3.5-second probe was below `GetProcessTimes` CPU resolution and stopped the ETW
session in 2.1 ms; desktop capture remains completely stopped when no game is
detected.

## Automated soak checkpoints (2026-08-24)

The native soak probe waits for two live sensor snapshots and warms lazy UI
resources before its baseline, then
checks one-second responsiveness, total CPU, final/max memory, process handles,
GDI/USER objects, and steady-state paint p95.

| Workload | Duration | Total CPU | Working set | Private memory | Handle growth | GDI/USER growth | Paint p95 |
|---|---:|---:|---:|---:|---:|---:|---:|
| Visible UI + repeated Settings | 60.8 s | 0.0201% | 63 MB | 43 MB | +1 | 0 / 0 | 1.60 ms |
| Minimized-to-tray monitoring | 300.2 s | 0.0111% | 58 MB | 41 MB | -2 | 0 / 0 | 1.62 ms |
| Final low-memory UI, fresh process | 60.5 s | 0.0291% | 70 MB | 40 MB | -33 | 0 / 0 | 1.89 ms |
| Final low-memory UI + Settings stress | 60.3 s | 0.0551% | 73 MB | 43 MB | +3 | 0 / 0 final | 1.95 ms |
| Static-runtime UI, fresh process | 60.5 s | 0.0170% | 62 MB | 36 MB | -31 | 0 / 0 | 0.72 ms |
| Static-runtime UI + Settings stress | 60.1 s | 0.0463% | 77 MB | 46 MB | -31 | 0 / -1 | 2.00 ms |
| Detached-soak harness control | 10.1 s | 0.0145% | 54 MB | 34 MB | 0 | 0 / 0 | 0.75 ms |
| Exact hook-free candidate, strict tray gate | 300.2 s | 0.0115% | 59 MB | 35 MB | -49 | 0 / 0 | n/a |

All runs stayed responsive and inside every automated budget. A five-minute
exact-candidate tray run is the required release resource-growth gate; a
prolonged unattended soak is not required for each release. The probe
caps its duration at 300 seconds and applies the zero-final-growth rule at that
boundary. The hook-free production mode includes a 36-second startup/update
stabilization period outside the measured interval and cannot query test-only
paint telemetry because the packaged app exposes no test messages.

## Accuracy gates

- Primary CPU temperature, GPU core temperature, GPU memory junction, drive composite temperature, utilization, clocks, fan speed, power, and voltage are compared with the stable app and HWiNFO.
- A reading is not shipped merely because it is available; its unit, source, scaling, and label must be validated.
- Unknown, invalid, sentinel, and critical-limit values are never presented as live temperatures.

## Publication gate

No v2 GitHub release or updater manifest is created until functional parity, clean install/update, multi-PC sensor audits, and performance soak tests pass.
