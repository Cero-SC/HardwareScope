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
