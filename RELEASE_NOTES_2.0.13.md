# HardwareScope 2.0.13

## Reliability improvements

- Preserve physical CPU CCD sensor identities when a channel is unavailable.
- Discard failed motherboard and DDR5 measurement caches and fail closed when required hardware locks are unavailable.
- Omit unmatched NVIDIA supplemental readings instead of matching devices by enumeration order.
- Apply polling and FPS configuration changes without restarting the sensor worker; request suspend and shutdown without waiting inside window handlers.
- Save settings through a bounded background writer and move settings import/export file I/O off the window thread. Oversized imports are rejected without changing active settings.
- Preserve graph history across missing-reference sensors, mark missing-data gaps, enforce compatible units and age samples by their timestamps.
- Initialize Favorites categories when their default temperature sensors arrive later.

## FPS and updates

- Parse quoted PresentMon output and select one swap chain per history, retaining valid long frame intervals rather than discarding severe stalls.
- Synchronize 1% low calculation with history resets so a previous stream cannot overwrite the new stream's statistics.
- Tighten game-only detection and avoid fast publication when no usable FPS is available.
- Check release metadata first and download the installer only after Update now is selected.
- Fix updater launch-directory lifetime and prevent older release events from regressing the stable update manifest.

## Candidate status and limitations

This draft is awaiting release qualification under RELEASING.md. Automated tests
and code review do not establish physical sensor accuracy, real-game FPS accuracy,
installer failure recovery or aggregate production resource use. Stable promotion
requires evidence for these exact candidate assets; the existing stable update
feed remains unchanged while qualification is pending.

HardwareScope remains unsigned. Hardware/driver support is model-dependent;
native Intel CPU temperature support is not currently implemented. Game FPS uses
application presentation intervals, and exclusive-fullscreen OSD visibility is
not universally guaranteed. See docs/SUPPORT_MATRIX.md for current boundaries.
