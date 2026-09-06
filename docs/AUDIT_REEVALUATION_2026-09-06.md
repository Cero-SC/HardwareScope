# Audit re-evaluation at 5acc06d

Claude's pasted summary was reviewed against the native source. Its separate full
report was not attached; claims about its test runs remain attributed to Claude.

## Confirmed and addressed

- Setup omitted the external PawnIO runtime. Embedded module bytecode is not a
  replacement for the signed driver/library. Bundle the unmodified official installer,
  pin its hash, check installed runtime access, and test on a clean hosted runner.
- Control IPC used a synchronous ReadFile from SYSTEM to an unauthenticated,
  user-owned pipe. Explicit anonymous SQOS avoids lending the service token;
  verify the server executable, keep reads nonblocking, and expire abandoned requests.
- The producer disconnected immediately after WriteFile, discarding unread messages.
  Preserve the connection until the reader consumes it; test delayed delivery.
- A service crash could leave mapped stale values looking current indefinitely.
  Reject snapshots older than five seconds and reconnect.
- The updater rehashed after waiting for the parent, but closed the verified file
  before launching it. Retain a read-only sharing handle through installation.
- The native ETW implementation was not a production backend. Keep it only in its
  probe and correct architecture documentation. Static-library inclusion alone did
  not prove linked executable bloat; the earlier review overstated that claim.
- Changing the manifest URL to Cero-SC broke old clients' allowlist. Restore the
  verified legacy redirect per the existing release compatibility policy.

## Not established as defects

- The claimed --test-ms busy loop is contradicted by Sleep(wait_milliseconds).
- A tagged source tree containing the previous manifest is intentional. The bot
  publishes the next manifest only after release assets are available and verified.
- V1 WPF/C# findings do not establish defects in the native 2.x implementation.
- Live plausible sensor values on one PC do not prove accuracy across all hardware.
  Retain the multi-PC validation requirement and do not invent register mappings.
- Unsigned builds are known and signing is explicitly deferred by the owner.

## Limits

Clean hosted-runner installation verifies prerequisite deployment and driver access;
it does not emulate physical Ryzen, NVIDIA, DIMM, or motherboard sensors. Hardware
accuracy still needs measured comparisons on representative physical PCs.
The pipe executable-path check assumes an administrator-protected installation;
it is not a defense against an attacker who already controls that directory.
Short tests and compile success do not establish the absence of all concurrency
bugs, security defects, or long-term resource growth.
