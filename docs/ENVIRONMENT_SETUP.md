# Environment Setup

This project is a Windows EtherCAT motion-control application. A complete local environment makes build, smoke-test, crash-analysis, Web UI validation, and AI-assisted diagnostics much faster and less error-prone.

## Recommended Policy

For day-to-day development, missing common tools such as Node.js, npm, Git, CMake, or package managers can usually be installed automatically when needed. Ask the project owner before changing major compiler versions, replacing the CMake generator/toolchain, changing EtherCAT/NIC driver behavior, or making other architecture-level environment changes.

## Required Runtime Environment

| Tool | Purpose | Notes |
| --- | --- | --- |
| Windows 10/11 x64 | Target OS | The control loop is near-real-time on Windows, not RTOS-based. Avoid background load when validating cyclic timing. |
| Administrator privileges | EtherCAT/Npcap access | Required for packet capture/injection and some NIC settings. |
| Npcap | EtherCAT raw Ethernet access | Install Npcap in WinPcap-compatible mode when physical EtherCAT adapters are used. Simulation mode does not require a physical NIC. |

## Required Build Environment

| Tool | Purpose | Recommended version/source |
| --- | --- | --- |
| Visual Studio Build Tools / Visual Studio | MSVC C++ compiler and Windows SDK | VS 2022 or newer. Current workspace presets target VS 2026 Build Tools paths when available. Install the Desktop development with C++ workload. |
| Windows SDK | Win32 headers, `rc.exe`, `mt.exe` | Installed with the C++ workload. The current preset references SDK `10.0.26100.0`. |
| CMake | Configure/build/test driver | 3.25+ required; 4.1.0 is the current validated local version. Prefer `C:\Program Files\CMake\bin` so VS Code, CTest, and agent terminals resolve the same tools. |
| Ninja | Default CMake generator in this workspace | Installed with VS CMake tools. |
| PowerShell 5.1 | Build/smoke/crash scripts | Use Windows PowerShell with `-ExecutionPolicy Bypass` as the VS Code tasks do. |
| Git | Source control and diff/status checks | Needed by agent workflows and normal development. |

## Recommended Developer Tools

| Tool | Purpose | Install command |
| --- | --- | --- |
| Node.js LTS + npm | Web UI JavaScript validation and lightweight front-end tests | `winget install --id OpenJS.NodeJS.LTS --exact --silent --accept-package-agreements --accept-source-agreements` |
| VS Code | Workspace tasks, CMake Tools integration, diagnostics | Install CMake Tools and C++ extensions. |
| Chocolatey or winget | Tool bootstrap/repair | `winget` is preferred when available; Chocolatey is an acceptable fallback. |
| Debugging Tools for Windows | Dump analysis | Useful for crash automation and minidump investigation. |

## Quick Bootstrap Checks

Run these from the repository root after opening a new PowerShell terminal:

```powershell
$tools = 'git','cmake','node','npm','winget'
foreach ($tool in $tools) {
    $cmd = Get-Command $tool -ErrorAction SilentlyContinue
    if ($cmd) { "$tool => $($cmd.Source)" } else { "$tool => MISSING" }
}

node --version
npm --version
cmake --version
```

If Node.js was just installed and the current terminal still cannot find `node`, refresh the session PATH or open a new terminal:

```powershell
$machinePath = [Environment]::GetEnvironmentVariable('Path','Machine')
$userPath = [Environment]::GetEnvironmentVariable('Path','User')
$env:Path = ($machinePath + ';' + $userPath)
```

## Recommended Validation Commands

Use the project scripts or workspace tasks instead of hand-written command lines when possible. For normal AI/developer validation, prefer the incremental crash build and smoke wrapper before raw CTest output:

```powershell
.\scripts\run_cmake.ps1 -Action configure -Preset default
.\scripts\run_cmake.ps1 -Action build -Preset default
.\scripts\build_incremental-Crash.ps1 -NonInteractive
.\scripts\run_smoke_tests.ps1 -BuildDir .\build -Config Release
.\scripts\run_ctest.ps1 -BuildDir .\build -Config Release
```

The root `CMakePresets.json` is the source of truth for generator, compiler, SDK, Ninja, and preset layout. Use preset commands for tool integration, configure repair, or explicit workflow checks; keep smoke/regression judgment on the wrapper scripts above:

```powershell
cmake --preset default
cmake --build --preset default
ctest --preset smoke
cmake --workflow --preset default
```

For Web UI-only checks after editing files under `resources/web/`, Node can compile inline JavaScript without starting the full application:

```powershell
node -e "const fs=require('fs'); const vm=require('vm'); const html=fs.readFileSync('resources/web/RT_scope.html','utf8'); const scripts=[...html.matchAll(/<script\b(?![^>]*\bsrc=)[^>]*>([\s\S]*?)<\/script>/gi)].map(m=>m[1]); scripts.forEach((script,i)=>new vm.Script(script,{filename:'RT_scope.html#inline-'+i})); console.log('OK inline scripts compiled:', scripts.length);"
```

## Notes For Physical EtherCAT Work

- Use the intended wired NIC and avoid routing EtherCAT traffic through Wi-Fi, VPN, or shared adapters.
- Run the application as Administrator when using real EtherCAT hardware.
- Keep CPU affinity, thread priority, high-resolution timer, and PDO-cycle scheduling settings stable unless the task explicitly targets timing work.
- Validate motion with small amplitude first, and confirm servo state, PDO mapping, and CiA402 mode before enabling motion.
