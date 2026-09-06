# Third-party notices

HardwareScope 2.0's sensor implementations are informed by these audited upstream projects and pinned revisions.

## LibreHardwareMonitor 0.9.6

- Project: https://github.com/LibreHardwareMonitor/LibreHardwareMonitor
- Revision: `3d331e3370efb858411f19511373eff65a218701`
- License: Mozilla Public License 2.0
- Used for verified AMD ADL, AMD Family 17h, and Nuvoton register/structure definitions.

## RAMSPDToolkit

- Project: https://github.com/Blacktempel/RAMSPDToolkit
- Revision: `3b47b960e0830fef344624ad5e389675d5f0a1ce`
- License: Mozilla Public License 2.0
- Used for verified PawnIO PIIX4 SMBus protocol and DDR5 temperature-register interpretation.
- Embedded `SmbusPIIX4.bin` SHA-256: `3CAAB1F324C402D8C22E6E82CE98A363092262EDF38E2CE32A8B60F01C1E3703`

HardwareScope's DIMM provider is intentionally read-only. It does not use the upstream SPD page-write paths and does not expose thermal warning or critical-limit registers as live temperatures.

## PawnIO modules

The full license text accompanying the embedded PawnIO modules is retained in `resources/PawnIO-Modules-COPYING.txt`.
It is also included in installed and portable packages as `PawnIO-Modules-COPYING.txt`.

## PawnIO official installer 2.2.0

- Unmodified official signed binary from https://github.com/namazso/PawnIO.Setup/releases/tag/2.2.0
- SHA-256: `1F519A22E47187F70A1379A48CA604981C4FCF694F4E65B734AAA74A9FBA3032`
- Official binary edition is proprietary; installer redistribution is permitted by
  https://github.com/namazso/PawnIO.Modules/wiki/Using-PawnIO-Modules .
- The official installer retains its own terms and installs the shared runtime.
  HardwareScope's license does not relicense that runtime.

## PresentMon 2.4.1

- Project: https://github.com/GameTechDev/PresentMon
- License: MIT
- The official x64 console runtime is packaged as HardwareScope's privileged,
  game-only frame-event analysis backend. HardwareScope consumes only its
  real-time CSV stream and starts it only while a detected game is running.

Copyright (C) 2017-2024 Intel Corporation

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"),
to deal in the Software without restriction, including without limitation
the rights to use, copy, modify, merge, publish, distribute, sublicense,
and/or sell copies of the Software, and to permit persons to whom
the Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included
in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES
OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE
OR OTHER DEALINGS IN THE SOFTWARE.
