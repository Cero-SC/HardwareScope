# Qualification progress - September 6, 2026

This is a scoped evidence record, not stable-release approval. Source `main`
contains changes newer than the v2.0.14 draft. The runtime fixes below are **not
in that draft installer** and are not installed on the developer's PC. A new
candidate must be built and its exact bytes requalified before promotion.

## Implemented and locally checked

- Graph history advances when the producer stops delivering snapshots. Stale
  series become unavailable and old points expire without invented samples.
  Explicit pause freezes history; recovery has a gap. Visible graph surfaces
  use a one-second aging timer; hidden OSD surfaces stop that timer. This does
  not establish freshness behavior for every non-graph sensor label.
- FPS discovery stays responsive at slow hardware polling settings: while FPS
  is enabled but unavailable, snapshot publication is capped at 500 ms. Full
  hardware collection remains on its own configured deadline. This adds at
  most two discovery publications per second; aggregate cost is not yet measured.
- FPS freshness uses the actual presentation QPC timestamp. Buffered rows over
  2.5 seconds old, future timestamps and malformed timestamps are rejected.
  Selected-stream stale gaps reset statistics on fresh recovery; stale foreign
  streams do not. Raw QPC orders frames, avoiding coarse clock rounding errors.
  Real long presentation intervals up to the 60-second history are retained.
- Production and instrumented targets build. All three CTest suites pass:
  Core, UpdaterRecovery and ReleaseEvidence. These include outage, polling and
  timestamp regressions. UpdaterRecovery covers failed handoff/relaunch, **not
  failure after installer file replacement**.
- Isolated synthetic UI smoke passes. Observed paint p95 was 1.048 ms; GDI
  handles stayed 47 to 47 and USER handles 25 to 25. This is not a production
  game-capture benchmark or an exact-byte packaged soak test.

## Exact-byte historical installer upgrade

[Hosted Windows run 34057470013](https://github.com/Cero-SC/HardwareScope/actions/runs/34057470013)
passed clean 2.0.12 installation, upgrade to 2.0.14, runtime access, automatic
running service, installed app/service/updater version checks, preservation of
seeded user settings and uninstall while preserving shared PawnIO.

| Installer | SHA-256 |
|---|---|
| 2.0.12 baseline | A3B3C0E13C84C35590C69C9416E5D7A3171AA5BE07A4E7C8F235A3FEF0FDD5C9 |
| 2.0.14 draft | EA098084F5C73B7B3C5C6D81D3A78EFF392B14B58860FE76311B278A3CDA138C |

Candidate bytes came from the successful tagged build artifact and matched the
separately verified draft installer. The workflow has read-only permissions;
it neither rebuilds the installer nor publishes a release. Package tests refuse
to run outside a clean disposable GitHub-hosted Windows runner.

[Follow-up run 34058199398](https://github.com/Cero-SC/HardwareScope/actions/runs/34058199398)
repeated those checks on the same installer bytes and passed the external
production UI boundary probe: the packaged main window and Settings expose no
internal test messages, and Settings opens and closes. The probe was built from
`a39478e`; the application under test was the unchanged v2.0.14 installer.
This closes that narrow hook-boundary gate for 2.0.14, not full visual QA.

## Bundled PresentMon output contract

Offline replay of the official
[v2.4.1 test_case_2.etl](https://github.com/GameTechDev/PresentMon/blob/v2.4.1/Tests/Gold/test_case_2.etl)
through the bundled 2.4.1 executable produced 204 rows, including 104 positive
presentation intervals, with the required production CSV fields. The fixture
contains Presenter.exe and dwm.exe, **not CS2**. Its SHA-256 is
`71194538CF20CCE3ADA1FEBCE98DD12E978920DB00ADEE7FDD1AF2886983FAF3`.

After downloading that pinned upstream fixture into an ignored output directory:

```powershell
./tests/validate_presentmon_replay.ps1 -TracePath ./out/qualification/presentmon-v2.4.1-test-case-2.etl -OutputDirectory ./out/qualification/verified-replay
```

The script verifies the trace hash, uses offline `--etl_file` mode with the
production metric flags and raw `--qpc_time`, bounds execution to 30 seconds,
and retains CSV/stderr. It never starts or stops a live ETW session. This checks
the actual binary's output format; synthetic C++ tests separately exercise
parser freshness. It is not an end-to-end live-game parser accuracy test.

The upstream [CSV implementation](https://raw.githubusercontent.com/GameTechDev/PresentMon/v2.4.1/PresentMon/CsvOutput.cpp)
and [metrics implementation](https://raw.githubusercontent.com/GameTechDev/PresentMon/v2.4.1/PresentMon/OutputThread.cpp)
establish that `TimeInQPC` represents presentation start. `CPUStartQPC` is not
used for freshness because CPU work for a genuine long frame can start earlier.

## Remaining qualification

### Live CS2 follow-up: issue found, not fully passed

The September 6 22:36 local-time isolated 90-second run used the latest
capture/parser/statistics and OSD renderer in `HardwareScopeLiveFpsOsdProbe`.
The installed app and sensor service were stopped. The probe uses a private
ETW session and is not packaged; service IPC/worker scheduling were bypassed.
CS2 was visually confirmed at its main menu, with the temporary FPS and 1% low
overlay visible and no installed monitoring overlay. No game settings changed.

- 824 samples; FPS available in 795, lows in 755; zero OSD text/value mismatches.
- First FPS at 2.187 seconds; first 1% low at 4.359 seconds. Low warm-up is
  intentional until at least 100 accepted intervals exist.
- Nine unavailable samples after startup, in three brief episodes around
  9.7, 28.1 and 71.6 seconds. Raw events continued: maximum presentation gap
  was 83.229 ms. Thus these were not multi-second rendering pauses in CS2.
- Latest frame-time values changed about once per second (mean 1008 ms,
  observed 907-1109 ms), despite roughly 100 ms probe snapshot polling.
- Final smoothed FPS independently recalculated to 136, matching the probe.
  Final displayed low was 45; raw 60-second-history calculation gave 39.
  History resets after freshness gaps mean these populations differ; this is
  not yet evidence of an arithmetic error. Delivery/freshness interaction needs
  investigation before declaring low statistics reliable through those gaps.
- Probe and its PresentMon child exited; empty OSD cleared. Installed monitoring
  was left stopped at the user's request. CS2 remained running.

The original probe printed a component-path PASS because it checked availability
counts, output formatting and cleanup, not continuity. Manual evidence review
overrides that as a **full FPS qualification failure**. The harness now counts
post-start unavailability as failure for continuous-render tests; raw-event
analysis remains necessary to distinguish actual game pauses. No shipping FPS
behavior was changed to conceal these findings.

Local evidence: `out/qualification/cs2-isolated-20260906-223622/`.
Readings SHA-256: `E35A6B7890CC167F34FCE95ED6214DB4BF1F0B8D5FB97B4AC09E840B147B9DA2`.
Raw CSV SHA-256: `3902334023BF11342A6A81C5A6884E66904288A075E6F372E64FFD5CC09A4E84`.

### Delivery correction and repeat test: passed within scope

The subsequent 22:43 isolated CS2 run fixed the delivery problem without
weakening the 2.5-second stale-frame guard or substituting receipt time for
presentation time. `SetTarget`, already called by the service's collection
loop, now flushes only its own live ETW session at most once per 100 ms. There
is no additional worker thread; failed non-startup flush requests back off to
five seconds. No flush occurs with FPS disabled or a stopped child process.

Why: the bundled [PresentMon output code](https://raw.githubusercontent.com/GameTechDev/PresentMon/v2.4.1/PresentMon/CsvOutput.cpp)
already flushes stdout per row; its [trace configuration](https://raw.githubusercontent.com/GameTechDev/PresentMon/v2.4.1/PresentData/PresentMonTraceSession.cpp)
does not request subsecond buffer delivery. The correction uses Microsoft's
documented [session flush operation](https://learn.microsoft.com/en-us/windows/win32/api/evntrace/nf-evntrace-controltracew).
The measured latency improvement supports trace buffering as the cause of the
observed freshness failures; the PresentMon executable itself is unchanged.

- 90 seconds, 4,076 valid raw presentation intervals, 821 available FPS samples.
- Zero post-start dropouts and zero OSD text/value mismatches.
- All 821 smoothed FPS comparisons and 81 distinct cached low calculations
  matched independent raw-frame calculations using recorded QPC boundaries and
  actual low-history counts. The once-per-second low cache is respected.
- Average new-frame update spacing 114.47 ms (previously 1007.54 ms); maximum
  235 ms. Average event age 245.41 ms; maximum 546.69 ms.
- First available FPS at 422 ms rather than 2187 ms in the failing run.
- Probe stopped cleanly, cleared its OSD and left no PresentMon child running.
- The installed service remained stopped. This was CS2 menu/background rendering,
  not a played match, full service IPC integration or aggregate resource benchmark.

Evidence: `out/qualification/cs2-flush-20260906-224358/`. The repeatable validator is
`tests/validate_live_fps_capture.ps1` (PowerShell 7); when analyzing on another host,
pass the capture host's QPC frequency with `-QpcFrequency` (10,000,000 here).
The probe records the presentation/low-cache boundaries, not just rounded text,
so comparisons do not accidentally mix different rolling windows.

### Other open checks

- Real-game capture (including CS2), swap-chain transitions, stalls and FPS/1%
  low agreement against recorded intervals; live ETW permissions and recovery.
- Model-specific physical sensor/reference checks and multi-GPU identity.
- Installer failure after replacement, service-start failure recovery and
  alternate install-directory permissions on disposable Windows.
- Repeat exact-byte installation and production UI boundary checks for the next
  candidate containing the source fixes above.
- Aggregate production app + service + FPS process CPU/memory/handle measurements
  with a short idle/tray/game run on the new candidate.
- Narrator/sensor-row accessibility and simultaneous Windows-session ownership.

Signing remains deferred by choice. Public stable promotion, public asset
availability and updating the installed application are separate operations.
