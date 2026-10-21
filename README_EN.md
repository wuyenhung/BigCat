# BigCat — SOEM EtherCAT Master for Windows

**BigCat** is a Windows EtherCAT master application built on [SOEM (Simple Open EtherCAT Master)](https://github.com/OpenEtherCATsociety/SOEM), designed for **6-DoF (six degrees of freedom) motion platform control**. It provides a complete Win32 GUI, embedded HTTP/SSE server, Stewart platform inverse kinematics, PDO filtering, and license management.

> **Author: wuyenhung (Yen-Hung Wu)** · 📧 wuyenhung@gmail.com · 🏢 Li-Yang Technology Co.

---

## Table of Contents

- [Key Features](#key-features)
- [System Requirements](#system-requirements)
- [Project Structure](#project-structure)
- [Quick Build](#quick-build)
- [Running & Startup Modes](#running--startup-modes)
- [Web UI](#web-ui)
- [License Management](#license-management)
- [Advanced Features](#advanced-features)
- [Common Operations & Tips](#common-operations--tips)
- [CMake Options](#cmake-options)
- [References](#references)
- [Version History](#version-history)

---

## Key Features

### EtherCAT Communication
- **PDO (Process Data Object)**: Cyclic process data exchange, supporting CSP (Cyclic Synchronous Position) mode
- **SDO (Service Data Object)**: Service data read/write and validation
- **Mailbox**: Mailbox send/receive

### Win32 GUI
- Slave list, status monitoring, SDO editor
- Motion control panel: Power On, Homing, Smooth Stop, Emergency Stop, Jog
- Status log and history browsing
- System tray minimization

### Motion Control
- **MotionController**: Batch power on / homing / emergency stop / smooth stop
- **PDOFilter**: Velocity, acceleration, deceleration, and hard-limit filtering protection
- **Soft-Start**: Gradual startup to prevent impact
- **Jog**: Forward/reverse manual movement

### Stewart Platform Kinematics
- **StewartIK**: 6-DoF Stewart platform inverse kinematics calculation
- **ForwardKinematics**: Forward kinematics pose solver
- Supports symmetric hexagonal platform and base parameter configuration

### Inter-Process Communication
- **MMFReader / MMFWriter**: Memory-Mapped File read/write for external 6-axis floating-point data input

### HTTP Server
- Embedded HTTP server (default port 8080)
- SSE (Server-Sent Events) real-time status push
- RESTful API: Slave info, SDO read/write, status queries
- Login/PIN verification and session management

### Security & Licensing
- TPM / hardware fingerprint license verification
- Automatic `license.txt` support
- Singleton protection: Prevents multiple instances

### Crash Handling
- **CrashHandler**: Automatic minidump file generation
- Automated crash analysis scripts

---

## System Requirements

For full tool installation and developer-environment recommendations, read [docs/ENVIRONMENT_SETUP.md](docs/ENVIRONMENT_SETUP.md) first. Missing common tools such as Node.js, CMake, or Git will slow down Web UI validation, builds, and smoke tests.

| Item | Requirement |
|------|-------------|
| OS | Windows 10/11 (64-bit) |
| Compiler | Visual Studio 2022+ (with C++ Desktop workload) |
| CMake | 3.25+ (4.1.0 validated in this workspace) |
| Network Driver | Npcap (recommended) or WinPcap |
| Permissions | Administrator privileges (packet capture/injection) |

---

## Project Structure

WuBigCat is organized by responsibility across core C++ code, Web UI assets, scripts, tests, configuration files, and developer tooling. For a folder-by-folder explanation, see [docs/PROJECT_STRUCTURE_GUIDE.md](docs/PROJECT_STRUCTURE_GUIDE.md).

High-level summary:

- `include/`, `src/`: core interfaces and implementations.
- `resources/web/`: static Web UI pages and front-end assets.
- `scripts/`: build/test wrappers, crash analysis, AI request, and HTTP sharing helpers.
- `docs/`: architecture, protection, and Copilot/Agent documentation.
- `tests/`: unit and behavior tests.
- `Par/`, `esi/`: motion parameter files and EtherCAT ESI definitions.
- `.github/`, `.vscode/`: Copilot/Agent settings, workspace tasks, and crash automation artifacts.

AI automation model defaults, override parameters, and legacy script limitations are documented in [scripts/README.md](scripts/README.md).

```
WuBigCat/
├── .github/
├── .vscode/
├── CMakeLists.txt
├── include/          # Core headers
├── src/              # Source files
├── resources/        # Web UI static resources
├── lib/              # Third-party libraries (SOEM, Npcap SDK, tinyxml2)
├── scripts/          # Build and analysis scripts
├── tests/            # Unit tests
├── docs/             # Documentation
├── esi/              # ESI XML configuration files
└── Par/              # Motion parameter files
```

---

## Quick Build

### Using PowerShell (Recommended)

1. Navigate to the project directory and create a build directory:
   ```powershell
   cd D:\Yenhung\WORK\Wu6Dof\PROG\WuBigCat
   New-Item -ItemType Directory -Path .\build -Force
   ```

2. Configure with the workspace preset:
   ```powershell
   cmake --preset default
   ```

3. Build Release version:
   ```powershell
   cmake --build --preset default
   ```

### Output Location
- Executables are typically located in `build\Release\bin\`:
  - `BigCat.exe` (main executable)
  - Other tools and test executables as configured

---

## Running & Startup Modes

### Run as Administrator

```powershell
Start-Process .\build\Release\bin\main\BigCat.exe -Verb RunAs
```

### Startup Modes

| Mode | CLI Argument | Behavior |
|------|-------------|----------|
| **Normal** | (none) | Main window minimizes to system tray; HTTP server controlled by `BigCat.ini` setting `[Server] EnableLocalHttpServer` (default: off) |
| **Debug** | `Debug` | Main window displayed normally for development |

### Singleton Protection

The application uses a named Mutex to prevent multiple instances. If BigCat is already running, new instances will automatically exit.

## Configuration (INI)

BigCat reads a same-name `.ini` file from the executable directory (e.g., `BigCat.ini`).

Common settings (section / key / description / default):
- `[Server] EnableLocalHttpServer` — Enable built-in HTTP server (0/1) — Default: 0 (off)
- `[Startup] ShowMainWindow` — Show main window on startup (0/1) — Default: 0 (off)
- `[Startup] AutoTrigger` — Auto-trigger startup sequence (0/1) — Default: 1 (on)
- `[Startup] TriggerTestCrash` — Trigger test crash on startup (0/1) — Default: 0 (off)
- `[Compat] ForceWin10Mode` — Force the Windows 10 cyclic wait strategy (0/1) — Default: 0 (off, auto-detect OS)
- `[Logging] EnableStatusLog` — Write status messages to `logs/status.log` (0/1) — Default: 1 (on)

Example `BigCat.ini`:
```ini
[Server]
; Enable local HTTP server (SSE / REST)
EnableLocalHttpServer=0

[Startup]
; Show main window on startup (otherwise minimize to tray)
ShowMainWindow=0
; Auto-trigger startup Auto sequence
AutoTrigger=1
; Test: trigger a test crash on startup
TriggerTestCrash=0

[Logging]
; Write to logs/status.log
EnableStatusLog=1
```

---

## Web UI

### Access

After starting in normal mode, the HTTP server starts on port 8080:
- `http://localhost:8080/auto.html` — Auto operation main page

### Pages

| Page | Description |
|------|-------------|
| `auto.html` | Auto operation main page |
| `status.html` | Status monitoring |
| `sdo.html` | SDO read/write |

### Authentication

- All pages use a shared sidebar menu (`sidebar-menu.js`) and login module (`auth.js`)
- Interactive controls are disabled when not logged in
- Login state stored in `localStorage` (key: `wusoem_logged_in_v1`)

### Environment Variables

| Variable | Description | Default |
|----------|-------------|---------|
| `WUSOEM_HTTP_MAX_CONNS` | Maximum concurrent connections | 2 |

---

## License Management

### Verification

BigCat uses hardware fingerprints (including TPM/EK) to compute SHA-256 license keys.

### License Methods

1. **License file**: Save the 64-character hex key to `license.txt`, placed in the same directory as `BigCat.exe`
2. **Environment variable**: Set `BIGCAT_LICENSE_KEY`
3. **Command line**: Pass the 64-character hex key directly

### Developer Bypass

- Only available in Debug builds by setting `BIGCAT_DISABLE_LICENSE_CHECK=1`
- Release builds ignore this setting

### License Tool

```powershell
# Generate local license
.\build\Release\bin\main\generate_license.exe -l -o license.txt

# Generate from fingerprint string
.\build\Release\bin\main\generate_license.exe -f "<fingerprint>" -o license.txt
```

GUI mode: Run `generate_license.exe` without arguments for visual license generation.

> ⚠️ `generate_license` (source and executable) is an owner-private licensing tool and is **not distributed with this public repository**; obtain the binary from the author when licensed. Builds without it simply skip the `generate_license` target and are unaffected otherwise.

### Verification Failure

On verification failure, a dialog displays:
- Device fingerprint
- Expected license key (automatically copied to clipboard)

---

## Advanced Features

### PDO Filter

`PDOFilter` provides real-time motion protection:

- **Velocity limiting**: Prevents overspeed
- **Acceleration/deceleration limiting**: Smooths motion curves
- **Hard-limit protection**: Prevents exceeding travel range
- **Violation callbacks**: Real-time violation notifications

### Stewart Kinematics

```cpp
#include "StewartIK.h"

WuSOEM::StewartParams params;
WuSOEM::Pose6D pose{0, 0, 450, 0, 0, 0}; // x, y, z, roll, pitch, yaw

auto result = WuSOEM::StewartIK::CalculateInverse(params, pose);
if (result.valid) {
    // Use result.servoAngles[0..5]
}
```

### Crash Handling

- Automatic minidump generation to `dumps/` directory
- Analysis with `scripts/analyze_dump_and_prepare_ai_prompt.ps1`
- Integrated AI-assisted diagnostics

### Status Logging

- Logs stored at `logs/status.log` (toggle via UI or INI: `[Logging] EnableStatusLog=1`, default: enabled)
- Automatic timestamped rotation
- Memory limit: Retains approximately the last 512KB
- Supports paginated history reading

---

## Common Operations & Tips

### SDO Read/Write Example

1. Select target slave
2. Enter Index (e.g., `0x1000`) and Subindex (e.g., `0x00`)
3. Click "Read SDO" or "Write SDO"

### Mailbox Example

1. Select target slave
2. Enter hex data (space-separated, e.g., `01 02 03 04`)
3. Click "Send Mailbox"

### Network Interface

SOEM on Windows uses the interface format:
```
\Device\NPF_{GUID}
```

Query available interfaces:
```powershell
Get-NetAdapter | Format-Table Name, InterfaceDescription, ifIndex
```

### Troubleshooting

| Problem | Solution |
|---------|----------|
| CMake can't find Windows SDK | Use VS Developer PowerShell |
| Linker errors for wpcap/Packet | Install Npcap SDK or set `-DNPCAP_SDK_DIR` |
| Cannot access network interface | Run as Administrator |

---

## CMake Options

| Option | Description | Default |
|--------|-------------|---------|
| `-DNPCAP_SDK_DIR=<path>` | Npcap SDK path | Auto-detect |
| `-DUSE_LOCAL_TINYXML2=ON` | Use local tinyxml2 | OFF |
| `-DGENERATE_PDB_FOR_RELEASE=ON` | Generate PDB for Release | ON |

---

## References

- [SOEM - Simple Open EtherCAT Master](https://github.com/OpenEtherCATsociety/SOEM)
- [Npcap](https://npcap.com/)
- [EtherCAT Technology Group](https://www.ethercat.org/)
- [tinyxml2](https://github.com/leethomason/tinyxml2)

---

## Version History

### v1.0.0 (Latest)

**2026-01-24 — CPU Performance Optimization**
- EtherCAT cyclic thread uses hybrid wait strategy (Sleep(0) + YieldProcessor)
- UI Timer interval adjusted from 100ms to 200ms
- HTTP URL refresh and link status check throttled to 1-second intervals
- MMF mapping failure adds 200ms backoff retry
- SSE broadcaster adds 200ms idle backoff when no clients connected

**2025-12-14 — Safety Stop Improvements**
- Fixed: Pressing "Immediate Stop" during soft-start could cause a position spike
- Improved: Immediate stop now sets hold target to current actual position
- Added `pdo_filter_stop_test` unit test to verify hold behavior

**2025-10-28 — Logging System Improvements**
- Status log uses timestamped rotation
- Status window paginated reading (default 64KB/page)
- Memory cap of approximately 2M wchars

**2025-10-20 — Foundation**
- Complete Win32 GUI implementation
- EtherCAT PDO/SDO/Mailbox support
- Stewart platform kinematics
- HTTP/SSE server
- License management

---

## Copyright & License

### Copyright Notice

Copyright © 2025-2026 **Yen-Hung Wu (wuyenhung)**. All rights reserved.

All source code, documentation, resource files, build scripts, and related designs of this project (BigCat) are the intellectual property of the author **Yen-Hung Wu (wuyenhung)**, protected under the Copyright Act of the Republic of China (Taiwan) and applicable international copyright treaties including the Berne Convention and the WIPO Copyright Treaty.

### Project License (Proprietary)

⚠️ **This project is NOT open-source software. It is NOT licensed under MIT, GPL, Apache, or any other open-source license.**

Without the prior written consent of the author **Yen-Hung Wu (wuyenhung)**, no individual or organization shall:

1. **NO USE** — Execute, deploy, or use any part of this project for any purpose, whether commercial or non-commercial
2. **NO MODIFICATION** — Modify, adapt, create derivative works from, decompile, or reverse-engineer the source code or binary files
3. **NO DISTRIBUTION** — Copy, distribute, sublicense, rent, lend, or otherwise make this project available to any third party
4. **NO CIRCUMVENTION** — Remove, circumvent, crack, or otherwise bypass the TPM (Trusted Platform Module) hardware-bound verification, license checks, or any other technical protection measures

> **Trial Mode Exception**: The Software includes a built-in trial function that allows execution without a license key for evaluation and testing. Trial mode use is exempt from restriction (1) above, but remains fully subject to restrictions (2), (3), (4) and the "Trial Mode Safety Disclaimer" below.

### ⚠️ Trial Mode Safety Disclaimer

The motion platform controlled by this Software involves mechanical movement and poses inherent risks of bodily injury, death, and equipment damage. With respect to trial (no-license-key) mode, the following is expressly declared:

| Item | Description |
|------|-------------|
| 🚫 **NO PERSONAL SAFETY GUARANTEE** | The author assumes NO liability for any bodily injury, death, or property loss under trial mode |
| 👤 **USER ASSUMES ALL RESPONSIBILITY** | Users must independently ensure the operational area is clear, equipment parameters are correct, emergency stop devices are functional, and operators are adequately trained |
| 📋 **NOT COVERED BY WARRANTY** | All use under trial mode is entirely outside the scope of any warranty, technical support, or after-sales service provided by the author or Li-Yang Technology Co. |
| ⚖️ **ASSUMPTION OF RISK** | By executing trial mode, the user acknowledges they have read, understood, and agreed to all above terms, and voluntarily assumes all consequences |

#### Legal Consequences of Violation

- Violation of any of the above terms results in immediate license termination; the author reserves the right to pursue all available civil and criminal remedies
- Circumventing or bypassing the TPM license verification mechanism may additionally constitute violations under:
  - **United States**: Digital Millennium Copyright Act (DMCA), 17 U.S.C. § 1201; Computer Fraud and Abuse Act (CFAA), 18 U.S.C. § 1030
  - **European Union**: Directive 2001/29/EC, Article 6 (Technological measures)
  - **International**: WIPO Copyright Treaty (WCT), Article 11
  - **Republic of China (Taiwan)**: Criminal Code Articles 358-362; Copyright Act Article 80-2

#### Obtaining a License

To use this project, please contact the author for written authorization:

- 📧 Email: wuyenhung@gmail.com
- 🏢 Company: Li-Yang Technology Co.

### Third-Party Library Licenses

This project incorporates the following third-party libraries, each subject to its own license:

| Library | License | Description |
|---------|---------|-------------|
| [SOEM](https://github.com/OpenEtherCATsociety/SOEM) | GPLv2 / Commercial | Simple Open EtherCAT Master |
| [tinyxml2](https://github.com/leethomason/tinyxml2) | zlib License | XML parser |
| [Npcap SDK](https://npcap.com/) | Npcap License | Network packet capture SDK |

Use and distribution of the above third-party libraries is governed by their respective original license terms. This project does not modify or impose additional restrictions on third-party library licenses.

---

**⚠️ NOTICE: This project is for development and testing purposes. Please test in a safe environment and follow equipment manuals before interacting with actual EtherCAT devices.**

**⚠️ WARNING: Unauthorized use of this project or circumvention of TPM protection mechanisms constitutes unlawful conduct. The author reserves all legal rights to pursue claims.**

**⚠️ SAFETY WARNING: Trial mode provides NO guarantee of personal safety. Users must ensure their own safety and assume all risks. Trial use is NOT covered by any warranty.**
