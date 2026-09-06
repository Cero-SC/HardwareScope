# HardwareScope 2.0.12

- Setup now includes the official PawnIO 2.2.0 installer, verifies its checksum,
  checks the driver can open, and preserves it when uninstalling HardwareScope.
- Portable packages include the prerequisite and clear setup instructions.
- Fixed blocking FPS/polling control reads and discarded settings messages;
  the service now rejects unexpected pipe owners and connects anonymously.
- Expire abandoned FPS requests and reject stale sensor snapshots after service failure.
- Hold verified update files against writes/deletion until installation completes.
- Keep the experimental direct ETW backend only in its diagnostic probe;
  production FPS continues using PresentMon.
- Restore legacy installer URLs so older versions can upgrade after the GitHub rename.
- Add a release gate for installation without any prior PawnIO, repeat installation,
  real driver access, and uninstall on a disposable Windows runner.

HardwareScope remains unsigned. Sensor support still depends on hardware;
successful driver installation does not imply support for every CPU or motherboard.
