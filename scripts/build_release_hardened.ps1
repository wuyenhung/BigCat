<#
發行用最佳化建置腳本。

用途:
- Release 發行
- 速度優先的最佳化建置
- 需要 hardening、strip、移除 PDB
#>

[CmdletBinding()]
param(
    [string]$ProjectRoot,
    [string]$BuildDir = "build",
    [ValidateSet('Ninja','Visual Studio 17 2022','Visual Studio 17 2026','')]
    [string]$Generator = '',
    [string]$Arch = '',
    [ValidateSet('auto','2022','2026')][string]$VSVersion = 'auto',
    [ValidateSet('VisualStudio','Ninja','Auto')][string]$PreferGenerator = 'Ninja',
    [ValidateSet('Release','Debug')][string]$Configuration = 'Release',
    [switch]$NonInteractive,
    [switch]$StripBinary = $true,
    [bool]$RemovePdb = $true,
    [bool]$PruneIntermediates = $true,
    [int]$Parallel = 0,
    [switch]$RunDetached
)

function Write-Note($s) { Write-Host "[build_release_hardened] $s" -ForegroundColor Cyan }
function Write-Err($s) { Write-Host "[build_release_hardened] ERROR: $s" -ForegroundColor Red }
. (Join-Path $PSScriptRoot 'Invoke-BuildArtifactCleanup.ps1')

function Resolve-DefaultProjectRoot {
    param(
        [string]$ScriptRoot,
        [string]$InvocationPath
    )

    $candidates = @()
    if ($ScriptRoot -and -not [string]::IsNullOrWhiteSpace($ScriptRoot)) {
        $candidates += $ScriptRoot
        $candidates += (Split-Path -Parent $ScriptRoot)
    }
    if ($InvocationPath -and -not [string]::IsNullOrWhiteSpace($InvocationPath)) {
        $invDir = Split-Path -Parent $InvocationPath
        $candidates += $invDir
        $candidates += (Split-Path -Parent $invDir)
    }
    $candidates += (Get-Location).Path

    foreach ($c in $candidates) {
        if ($c -and (Test-Path (Join-Path $c 'CMakeLists.txt'))) { return $c }
    }
    foreach ($c in $candidates) {
        if ($c) { return $c }
    }
    return $null
}

# Normalize and resolve paths so the script can be executed from any working directory.
# If ProjectRoot not supplied we try multiple fallbacks (script folder, invocation path, current dir)
if ([string]::IsNullOrWhiteSpace($ProjectRoot)) {
    $invPath = $null
    if ($MyInvocation -and $MyInvocation.MyCommand -and $MyInvocation.MyCommand.Path) {
        $invPath = $MyInvocation.MyCommand.Path
    }
    $ProjectRoot = Resolve-DefaultProjectRoot -ScriptRoot $PSScriptRoot -InvocationPath $invPath
}
$ProjectRoot = Resolve-Path -Path $ProjectRoot -ErrorAction SilentlyContinue | ForEach-Object { $_.ProviderPath }
if (-not $ProjectRoot) { Write-Err "Cannot resolve ProjectRoot. Use -ProjectRoot to point to the repository root."; exit 2 }

# If BuildDir is a relative path, treat it relative to ProjectRoot
if (-not ([System.IO.Path]::IsPathRooted($BuildDir))) {
    $BuildDir = Join-Path $ProjectRoot $BuildDir
}

$BuildDir = Resolve-Path -Path $BuildDir -ErrorAction SilentlyContinue | ForEach-Object { $_.ProviderPath }
if (-not $BuildDir) { $BuildDir = Join-Path $ProjectRoot 'build' }

Write-Note "ProjectRoot: $ProjectRoot"
Write-Note "Target build directory: $BuildDir"
# Resolve parallelism if not explicitly passed
if ($Parallel -le 0) {
    if ($env:CMAKE_BUILD_PARALLEL_LEVEL) { $Parallel = [int]$env:CMAKE_BUILD_PARALLEL_LEVEL } else { $Parallel = [int][Environment]::ProcessorCount }
    Write-Note "Using parallelism level: $Parallel"
}
# Allow environment overrides for arch/generator already handled above

# Ensure cmake available
if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) {
    Write-Err "cmake is not available on PATH. Install CMake or run from developer shell."
    exit 2
}

# Try to find Visual Studio developer batch (vcvarsall) automatically
function Find-VcVarsAll {
    if ($env:VSINSTALLDIR) {
        $cand = Join-Path $env:VSINSTALLDIR 'VC\Auxiliary\Build\vcvarsall.bat'
        if (Test-Path $cand) { return $cand }
    }
    $vswhere = 'C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path $vswhere) {
        try {
            $inst = & $vswhere -latest -products * -requires Microsoft.Component.MSBuild -property installationPath 2>$null
            if ($inst) {
                $p = Join-Path $inst 'VC\Auxiliary\Build\vcvarsall.bat'
                if (Test-Path $p) { return $p }
            }
        } catch { }
    }
    # common fallback locations
    $cands = @( 
        'C:\Program Files\Microsoft Visual Studio\18\Insiders\VC\Auxiliary\Build\vcvarsall.bat',
        'C:\Program Files\Microsoft Visual Studio\17\Community\VC\Auxiliary\Build\vcvarsall.bat',
        'C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvarsall.bat'
    )
    foreach ($c in $cands) { if (Test-Path $c) { return $c } }
    return $null
}

$vcvarsall = Find-VcVarsAll

# --- Adaptive helper functions & environment overrides ---
function Find-CMakeExe {
    $cands = @(
        'C:\Program Files\CMake\bin\cmake.exe',
        'C:\Program Files\Microsoft Visual Studio\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
    )
    foreach ($c in $cands) { if (Test-Path $c) { return $c } }
    return 'cmake'
}
function Test-CMakeGenerator { param([string]$CMakeExe, [string]$GeneratorName) try { $help = & $CMakeExe --help | Out-String; return ($help -match [regex]::Escape($GeneratorName)) } catch { return $false } }
function Find-WindowsSdkBin { $kitsRoot = 'C:\Program Files (x86)\Windows Kits\10\bin'; if (-not (Test-Path $kitsRoot)) { return $null }; $verDir = Get-ChildItem -Path $kitsRoot -Directory | Where-Object { $_.Name -match '^\d+\.\d+\.\d+\.\d+$' } | Sort-Object Name -Descending | Select-Object -First 1; if (-not $verDir) { return $null }; $sdkBin = Join-Path $verDir.FullName 'x64'; if (Test-Path (Join-Path $sdkBin 'rc.exe')) { return $sdkBin }; return $null }

function Choose-Generator {
    param($VcVars, $Prefer)
    $cmakeExe = Find-CMakeExe

    # If user provided explicit BUILD_GENERATOR env list, honor it verbatim
    if ($env:BUILD_GENERATOR) {
        $envlist = ($env:BUILD_GENERATOR -split '[,;]') | ForEach-Object { $_.Trim() } | Where-Object { $_ -ne '' }
        Write-Note "Using BUILD_GENERATOR override: $($envlist -join ', ')"
        return $envlist
    }

    $candidates = @()

    # Allow explicit MSVC preference via parameter or env var
    $preferMsvc = $false
    if ($Prefer -eq 'VisualStudio') { $preferMsvc = $true }
    if ($env:BUILD_PREFER_MSVC) { if ($env:BUILD_PREFER_MSVC -match '^(1|true)$') { $preferMsvc = $true } }

    # If MSVC is preferred and vcvarsall exists, prioritize Ninja (using MSVC toolchain) then Visual Studio generators
    if ($preferMsvc -and $VcVars) {
        # If Ninja is available and cl exists, prefer it first so we get parallel builds with MSVC toolchain
        if ((Get-Command cl -ErrorAction SilentlyContinue) -and (Test-CMakeGenerator $cmakeExe 'Ninja')) { $candidates += 'Ninja'; Write-Note "MSVC environment: Ninja available -> preferring Ninja" }
        if (Test-CMakeGenerator $cmakeExe 'Visual Studio 18 2026') { $candidates += 'Visual Studio 18 2026' }
        if (Test-CMakeGenerator $cmakeExe 'Visual Studio 17 2022') { $candidates += 'Visual Studio 17 2022' }
        if (Test-CMakeGenerator $cmakeExe 'NMake Makefiles') { $candidates += 'NMake Makefiles' }
        if ($candidates.Count -gt 0) { Write-Note "MSVC-priority candidates: $($candidates -join ', ')"; return $candidates }
        # Fall back to generic candidates if none found
    }

    # Default candidate order for non-MSVC or when MSVC not preferred
    if (Test-CMakeGenerator $cmakeExe 'Ninja') { $candidates += 'Ninja' }
    if ((Get-Command cl -ErrorAction SilentlyContinue) -and (Test-CMakeGenerator $cmakeExe 'Ninja')) { Write-Note "cl detected; Ninja will use MSVC toolchain" }
    if (Test-CMakeGenerator $cmakeExe 'NMake Makefiles') { $candidates += 'NMake Makefiles' }
    if (Test-CMakeGenerator $cmakeExe 'MinGW Makefiles') { $candidates += 'MinGW Makefiles' }
    if (Test-CMakeGenerator $cmakeExe 'Visual Studio 17 2022') { $candidates += 'Visual Studio 17 2022' }

    if ($candidates.Count -gt 0) { Write-Note "Default candidates: $($candidates -join ', ')"; return $candidates }
    return @('Ninja')
}

# Honor simple environment overrides and detect CI / non-interactive environment
if ($env:BUILD_CONFIG) { $Configuration = $env:BUILD_CONFIG }
if ($env:BUILD_ARCH) { $Arch = $env:BUILD_ARCH }
# CI envs -> non-interactive
if ($env:GITHUB_ACTIONS -or $env:CI) { $NonInteractive = $true; Write-Note "CI environment detected -> NonInteractive mode" }

# Auto-detect non-interactive when Host UI unavailable
if (-not $NonInteractive) {
    try { if (-not $Host.UI.RawUI) { $NonInteractive = $true } } catch { $NonInteractive = $true }
}
if ($NonInteractive) { Write-Note "Running in NonInteractive mode -> prompts will be skipped/auto-accepted" }

function Find-WindowsPowerShellExe {
    $candidates = @()
    if ($env:SystemRoot) {
        $candidates += (Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe')
    }
    $candidates += 'powershell.exe'

    foreach ($candidate in $candidates) {
        if ($candidate -eq 'powershell.exe') {
            if (Get-Command $candidate -ErrorAction SilentlyContinue) { return $candidate }
            continue
        }

        if (Test-Path $candidate) { return $candidate }
    }

    throw 'Unable to locate Windows PowerShell executable.'
}

# If RunDetached is requested, re-launch in a new PowerShell window and exit (parent)
if ($RunDetached) {
    Write-Note "RunDetached requested - launching detached PowerShell and exiting parent process"
    $argsList = @('-NoProfile','-ExecutionPolicy','Bypass','-File', "`"$PSCommandPath`"", '-NonInteractive')
    # pass commonly used options
    if ($BuildDir -ne 'build') { $argsList += ('-BuildDir', $BuildDir) }
    if ($Generator -ne '') { $argsList += ('-Generator', $Generator) }
    if ($Arch -ne '') { $argsList += ('-Arch', $Arch) }
    if ($VSVersion -ne 'auto') { $argsList += ('-VSVersion', $VSVersion) }
    if ($PreferGenerator -ne 'VisualStudio') { $argsList += ('-PreferGenerator', $PreferGenerator) }
    if ($Configuration -ne 'Release') { $argsList += ('-Configuration', $Configuration) }
    if ($StripBinary) { $argsList += '-StripBinary' }
    if (-not $RemovePdb) { $argsList += ('-RemovePdb','$false') }
    if ($Parallel -gt 0) { $argsList += ('-Parallel',$Parallel) }

    $powerShellExe = Find-WindowsPowerShellExe
    Start-Process -FilePath $powerShellExe -ArgumentList $argsList -WorkingDirectory $ProjectRoot -WindowStyle Normal | Out-Null
    Write-Note "Detached build launched"
    exit 0
}

# Unconditionally remove and recreate the build directory (required by hardened release workflow)
Write-Note "Unconditionally removing and recreating build directory: $BuildDir"
if (Test-Path $BuildDir) {
    try {
        Remove-Item -Recurse -Force $BuildDir -ErrorAction Stop
        Write-Note "Removed existing build directory"
    } catch {
        Write-Err "Failed to remove existing build directory: $_"
        exit 2
    }
}
try {
    New-Item -ItemType Directory -Path $BuildDir -Force | Out-Null
    Write-Note "Created fresh build directory: $BuildDir"
} catch {
    Write-Err "Failed to create build directory: $_"
    exit 2
}

# Force a configure since we just recreated the build dir
$needConfigure = $true

# CMake configure args with hardening build flags
# Choose generator; if user provided a Generator use it, otherwise pick adaptively
if ($Generator -and $Generator -ne '') {
    $gen = $Generator
} else {
    # If $gen was already set (e.g., from cached CMakeCache.txt) reuse it; otherwise choose adaptively
    if (-not $gen -or $gen -eq '') { $gen = Choose-Generator $vcvarsall $PreferGenerator }
}

$cmakeConfigureArgs = @(
    # configure source & build root
    "-S", $ProjectRoot,
    "-B", $BuildDir
)

# Do not add a single generator here; we'll attempt multiple candidates during configure
if ($Arch -ne '') { $cmakeConfigureArgs += "-A"; $cmakeConfigureArgs += $Arch }

# Force configuration-related flags to make it harder to reverse engineer (uses $Configuration)
$extraDefs = @()
$extraDefs += "-DCMAKE_BUILD_TYPE=$Configuration"
# Try to avoid generating PDBs on MSVC-based generators when Release
if ($Configuration -eq 'Release') { $extraDefs += "-DCMAKE_MSVC_DEBUG_INFORMATION_FORMAT=None" }
$extraDefs += "-DCMAKE_INTERPROCEDURAL_OPTIMIZATION=ON"
# Strong optimization flags are only relevant for Release builds under MSVC; these are appended later when MSVC is detected.

# extraDefs will be appended inside the configure branch depending on whether
# MSVC is detected; do not append now to avoid duplicates

if ($needConfigure) {
    Write-Note "Running CMake configure with hardened release flags using generator: $gen"

    # Build MSVC-specific flags or generic flags depending on toolchain
    $useMSVC = $false
    if ($vcvarsall -or (Get-Command cl -ErrorAction SilentlyContinue)) { $useMSVC = $true }

    # Build MSVC-specific or generic extra defs using explicit conditionals (avoid inline if expressions)
    $msvcExtra = @()
    $gccExtra = @()
    if ($useMSVC) {
        # MSVC flags
        $msvcExtra += "-DCMAKE_BUILD_TYPE=$Configuration"
        if ($Configuration -eq 'Release') {
            # Disable PDB generation for Release to allow /GL+/LTCG to remain effective
            $msvcExtra += "-DGENERATE_PDB_FOR_RELEASE=OFF"
            $msvcExtra += "-DCMAKE_MSVC_DEBUG_INFORMATION_FORMAT=None"
            $msvcExtra += "-DCMAKE_C_FLAGS_RELEASE=/O2 /GL /Gw /Gy /Oi /Ot /Ob2 /GF /fp:fast /DNDEBUG"
            $msvcExtra += "-DCMAKE_CXX_FLAGS_RELEASE=/O2 /GL /Gw /Gy /Oi /Ot /Ob2 /GF /fp:fast /DNDEBUG"
            $msvcExtra += "-DCMAKE_EXE_LINKER_FLAGS_RELEASE=/LTCG /INCREMENTAL:NO /OPT:REF /OPT:ICF /DEBUG:NONE /GUARD:CF /DYNAMICBASE /NXCOMPAT"
            $msvcExtra += "-DCMAKE_SHARED_LINKER_FLAGS_RELEASE=/LTCG /INCREMENTAL:NO /OPT:REF /OPT:ICF /DEBUG:NONE /GUARD:CF /DYNAMICBASE /NXCOMPAT"
        }
        $extraDefs += $msvcExtra
    } else {
        # Generic flags for non-MSVC toolchains
        $gccExtra += "-DCMAKE_BUILD_TYPE=$Configuration"
        $gccExtra += "-DCMAKE_INTERPROCEDURAL_OPTIMIZATION=ON"
        if ($Configuration -eq 'Release') {
            $gccExtra += "-DCMAKE_C_FLAGS_RELEASE=-O3 -DNDEBUG -march=native -fomit-frame-pointer -ffunction-sections -fdata-sections -fvisibility=hidden"
            $gccExtra += "-DCMAKE_CXX_FLAGS_RELEASE=-O3 -DNDEBUG -march=native -fomit-frame-pointer -ffunction-sections -fdata-sections -fvisibility=hidden"
            $gccExtra += "-DCMAKE_EXE_LINKER_FLAGS_RELEASE=-s -Wl,--gc-sections"
            $gccExtra += "-DCMAKE_SHARED_LINKER_FLAGS_RELEASE=-s -Wl,--gc-sections"
        }
        $extraDefs += $gccExtra
    }
        # (duplicate block removed)

    # Prefer using local tinyxml2 when available; otherwise FetchContent requires git
    $localTiny = Test-Path (Join-Path $ProjectRoot 'lib\tinyxml2')
    if ($localTiny) {
        Write-Note "Found local tinyxml2 in repo -> using -DUSE_LOCAL_TINYXML2=ON"
        $cmakeConfigureArgs += "-DUSE_LOCAL_TINYXML2=ON"
    } else {
        # ensure git is available if we will fetch dependencies
        if (-not (Get-Command git -ErrorAction SilentlyContinue)) {
            Write-Err "git not found in PATH and local tinyxml2 not present. Install git or add lib/tinyxml2 to repository before configuring."
            exit 3
        }
    }

    # Enforce symbol visibility hiding to reduce exported symbols and make reverse engineering harder
    $cmakeConfigureArgs += "-DCMAKE_CXX_VISIBILITY_PRESET=hidden"
    $cmakeConfigureArgs += "-DCMAKE_VISIBILITY_INLINES_HIDDEN=1"

    $cmakeConfigureArgs += $extraDefs

    Write-Note "Will attempt generators in order and stop on first success."

    # Helper: run configure in the appropriate environment with a specific generator
    function Run-Configure {
        param($VSBatch, $TryGen)
        $cmdExe = $env:ComSpec; if (-not $cmdExe) { $cmdExe = 'cmd.exe' }
        # Build args array from base configure args, add generator if requested, then extra defs
        $argsArray = @($cmakeConfigureArgs)
        if ($TryGen -and $TryGen -ne '') { $argsArray += "-G"; $argsArray += $TryGen }
        if ($extraDefs) { $argsArray += $extraDefs }

        if ($VSBatch -and (Test-Path $VSBatch)) {
            $quotedArgs = ($argsArray | ForEach-Object {
                '"' + ($_ -replace '"', '\\"') + '"'
            }) -join ' '
            $cfg = "cmake $quotedArgs"
            & $cmdExe /c "`"$VSBatch`" x64 && cd /d `"$ProjectRoot`" && $cfg" | Out-Host
            return $LASTEXITCODE
        }

        & cmake @argsArray | Out-Host
        return $LASTEXITCODE
    }

    # Determine the ordered generator list to try. User-specified generator gets precedence.
    $genCandidates = Choose-Generator $vcvarsall $PreferGenerator
    $tryList = @()
    if ($Generator -ne '') { $tryList += $Generator }
    foreach ($g in $genCandidates) { if (-not ($tryList -contains $g)) { $tryList += $g } }

    $rc = -1
    $selectedGen = ''
    foreach ($g in $tryList) {
        Write-Note "Attempting generator: $g"
        $rc = Run-Configure $vcvarsall $g
        if ($rc -eq 0) { $selectedGen = $g; Write-Note "CMake configured successfully with generator: $g"; break }
        Write-Note "Generator $g failed (exit $rc), trying next..."
        # Remove any partial cache/CMakeFiles produced by the failed attempt before trying the next generator
        $tmpCache = Join-Path $BuildDir 'CMakeCache.txt'
        if (Test-Path $tmpCache) { try { Remove-Item -Path $tmpCache -Force -ErrorAction SilentlyContinue } catch { } }
        $tmpCMakeDir = Join-Path $BuildDir 'CMakeFiles'
        if (Test-Path $tmpCMakeDir) { try { Remove-Item -Path $tmpCMakeDir -Recurse -Force -ErrorAction SilentlyContinue } catch { } }
    }

    if ($rc -ne 0) { Write-Err "CMake configure failed (exit $rc)"; exit $rc }

    # Remember chosen generator for subsequent build steps
    if ($selectedGen -ne '') { $gen = $selectedGen }

}

# Build step
Write-Note "Starting build ($Configuration) with parallel=$Parallel"
Push-Location $BuildDir
try {
    # Build using cmake --build for generator compatibility
    $buildCmd = "cmake --build . --config Release --parallel $Parallel"
    Write-Note "Running: $buildCmd"
    # Run build inside Visual Studio environment if available
    function Run-Build {
        param($VSBatch, $BuildRoot, $Parallel)
        $cmdExe = $env:ComSpec; if (-not $cmdExe) { $cmdExe = 'cmd.exe' }
        if ($VSBatch -and (Test-Path $VSBatch)) {
            if ($gen -like '*Ninja*') {
                $cmd = "`"$VSBatch`" x64 && cd /d `"$BuildRoot`" && cmake --build . --config $Configuration --parallel $Parallel"
            } elseif ($gen -like '*Visual Studio*') {
                $cmd = "`"$VSBatch`" x64 && cd /d `"$BuildRoot`" && cmake --build . --config $Configuration -- /m:$Parallel"
            } elseif ($gen -like '*NMake*') {
                # Use nmake directly to avoid cmake mapping parallel flags incorrectly
                $cmd = "`"$VSBatch`" x64 && cd /d `"$BuildRoot`" && (set CMAKE_BUILD_PARALLEL_LEVEL=) && nmake"
            } else {
                $cmd = "`"$VSBatch`" x64 && cd /d `"$BuildRoot`" && cmake --build . --config $Configuration"
            }
            Write-Note "Executing (cmd): $cmd"
            & $cmdExe /c $cmd | Out-Host
            $code = $LASTEXITCODE
            return $code
        }

        if ($gen -like '*Ninja*') {
            & cmake --build . --config $Configuration --parallel $Parallel | Out-Host
        } elseif ($gen -like '*Visual Studio*') {
            & cmake --build . --config $Configuration -- /m:$Parallel | Out-Host
        } elseif ($gen -like '*NMake*') {
            # Use nmake directly to avoid cmake mapping parallel flags incorrectly
            # Unset CMAKE_BUILD_PARALLEL_LEVEL temporarily to avoid cmake/nmake receiving a parallel flag
            $oldCb = $env:CMAKE_BUILD_PARALLEL_LEVEL
            Remove-Item Env:CMAKE_BUILD_PARALLEL_LEVEL -ErrorAction SilentlyContinue
            Write-Note "Executing: nmake"
            & nmake | Out-Host
            if ($oldCb) { $env:CMAKE_BUILD_PARALLEL_LEVEL = $oldCb }
        } else {
            Write-Note "Executing: cmake --build . --config $Configuration"
            & cmake --build . --config $Configuration | Out-Host
        }
        $code = $LASTEXITCODE
        return $code
    }

    $rc = Run-Build $vcvarsall (Get-Location) $Parallel
    if ($rc -ne 0) { Write-Err "Build failed (exit $rc)"; Pop-Location; exit $rc }
} finally {
    Pop-Location
}

Write-Note "Build succeeded — running hardening post-steps (StripBinary=$StripBinary, RemovePdb=$RemovePdb)"

# Helper to find outputs (common CMake layout)
function Get-BinaryPaths($root) {
    $exes = Get-ChildItem -Path $root -Recurse -Include *.exe,*.dll -File -ErrorAction SilentlyContinue
    return $exes
}

# Determine probable output directory(s)
$possibleOuts = @()
$possibleOuts += Join-Path $BuildDir "$Configuration"
$possibleOuts += Join-Path $BuildDir "bin\$Configuration"
$possibleOuts += Join-Path $BuildDir "bin\main\$Configuration"
$possibleOuts += Join-Path $BuildDir "$Configuration\bin\main"
$possibleOuts += Join-Path $BuildDir "bin\main"
$possibleOuts += Join-Path $BuildDir "bin"

$existingOuts = $possibleOuts | Where-Object { Test-Path $_ }
if (-not $existingOuts -or $existingOuts.Count -eq 0) {
    Write-Note "No canonical Release output folder detected. Searching under build dir for exe/dlls..."
    $bins = Get-BinaryPaths $BuildDir
} else {
    $bins = @()
    foreach ($p in $existingOuts) {
        $bins += Get-BinaryPaths $p
    }
}

if (-not $bins -or $bins.Count -eq 0) {
    Write-Note "No binaries found in build outputs — nothing to strip or sanitize."
} else {
    Write-Note "Found $($bins.Count) binaries in build outputs to consider hardening."

    # Remove PDBs
    if ($RemovePdb) {
        Write-Note "Removing .pdb files in build output to avoid leaking debug symbols (RemovePdb = $RemovePdb)"
        $pdbs = Get-ChildItem -Path $BuildDir -Recurse -Include *.pdb -File -ErrorAction SilentlyContinue
        foreach ($f in $pdbs) {
            try {
                Write-Note "Removing PDB: $($f.FullName)"
                Remove-Item -Path $f.FullName -Force -ErrorAction SilentlyContinue
            } catch {
                Write-Err "Failed to remove $($f.FullName): $_"
            }
        }
    }

    # Attempt to strip binaries using available strip tools
    if ($StripBinary) {
        # prefer llvm-strip if present, then GNU strip, then try objcopy
        $stripTool = $null
        if (Get-Command -Name llvm-strip -ErrorAction SilentlyContinue) { $stripTool = "llvm-strip" }
        elseif (Get-Command -Name strip -ErrorAction SilentlyContinue) { $stripTool = "strip" }
        elseif (Get-Command -Name objcopy -ErrorAction SilentlyContinue) { $stripTool = "objcopy" }

        if (-not $stripTool) {
            Write-Note "No strip tool found in PATH (llvm-strip/strip/objcopy). Skipping binary strip."
        } else {
            Write-Note "Using strip tool: $stripTool"
            foreach ($bin in $bins) {
                $exePath = $bin.FullName
                $ext = [IO.Path]::GetExtension($exePath)
                $baseName = [IO.Path]::GetFileNameWithoutExtension($exePath)
                # If the executable is running, attempt to stop it (NonInteractive -> force stop; otherwise ask)
                if ($ext -ieq '.exe') {
                    $running = Get-Process -Name $baseName -ErrorAction SilentlyContinue
                    if ($running) {
                        if ($NonInteractive) {
                            Write-Note "Stopping running process $baseName to allow strip..."
                            $running | Stop-Process -Force -ErrorAction SilentlyContinue
                            Start-Sleep -Milliseconds 200
                        } else {
                            $ans = Read-Host "Process $baseName is running. Stop it to strip symbols? (Y/N) [Y]"
                            if ($NonInteractive) {
                                Write-Note "NonInteractive -> stopping process $baseName to allow strip"
                                $running | Stop-Process -Force -ErrorAction SilentlyContinue
                                Start-Sleep -Milliseconds 200
                            } else {
                                if ([string]::IsNullOrEmpty($ans) -or $ans.ToUpper().StartsWith('Y')) {
                                    $running | Stop-Process -Force -ErrorAction SilentlyContinue
                                    Start-Sleep -Milliseconds 200
                                } else {
                                    Write-Note "Skipping strip for $exePath (process running)"
                                    continue
                                }
                            }
                        }
                    }
                }
                try {
                    Write-Note "Stripping symbols from: $exePath"
                    if ($stripTool -eq 'objcopy') { & $stripTool --strip-all $exePath 2>&1 | ForEach-Object { Write-Host $_ } }
                    else { & $stripTool -s $exePath 2>&1 | ForEach-Object { Write-Host $_ } }
                } catch {
                    Write-Err ("Strip failed for {0}: {1}" -f $exePath, $_)
                }
            }
        }
    }

    # As a final step ensure PDBs are removed even if generator bypassed our config
    if ($RemovePdb) {
        Write-Note "(final sweep) Removing any remaining PDBs in build dir"
        $pdbs = Get-ChildItem -Path $BuildDir -Recurse -Include *.pdb -File -ErrorAction SilentlyContinue
        foreach ($f in $pdbs) {
            try { Remove-Item -Path $f.FullName -Force -ErrorAction SilentlyContinue; Write-Note "Removed: $($f.FullName)" } catch {}
        }
    }
}

# Summarize build method and environment for user
# Ensure we have a canonical generator value
if (-not $gen -or $gen -eq '') {
    try {
        $cacheLines = Get-Content $cache -ErrorAction SilentlyContinue
        $gline = $cacheLines | Where-Object { $_ -match '^CMAKE_GENERATOR:INTERNAL=' } | Select-Object -First 1
        if ($gline) { $gen = $gline -replace '^CMAKE_GENERATOR:INTERNAL=', '' }
    } catch { }
}

# Detect toolchain
$msvcDetected = $false
if ($vcvarsall -or (Get-Command cl -ErrorAction SilentlyContinue)) { $msvcDetected = $true }
if ($gen -like '*Ninja*' -and $msvcDetected) { $toolchain = 'Ninja (MSVC toolchain)' }
elseif ($gen -like '*Visual Studio*') { $toolchain = 'MSBuild (Visual Studio)' }
elseif ($gen -like '*NMake*') { $toolchain = 'NMake' }
elseif ($gen -like '*Ninja*') { $toolchain = 'Ninja' }
else { $toolchain = 'Unknown' }

# GENERATE_PDB_FOR_RELEASE value (if present in cache)
$generatePdb = 'unknown'
try { if (-not $cacheLines) { $cacheLines = Get-Content $cache -ErrorAction SilentlyContinue } ; $pdbLine = $cacheLines | Where-Object { $_ -match '^GENERATE_PDB_FOR_RELEASE:BOOL=' } | Select-Object -First 1; if ($pdbLine) { $generatePdb = $pdbLine -replace '^GENERATE_PDB_FOR_RELEASE:BOOL=', '' } } catch { }

$stripToolName = $null
if (Get-Command -Name 'llvm-strip' -ErrorAction SilentlyContinue) { $stripToolName = 'llvm-strip' }
elseif (Get-Command -Name 'strip' -ErrorAction SilentlyContinue) { $stripToolName = 'strip' }
elseif (Get-Command -Name 'objcopy' -ErrorAction SilentlyContinue) { $stripToolName = 'objcopy' }
if (-not $stripToolName) { $gv = Get-Variable -Name 'stripTool' -ErrorAction SilentlyContinue; if ($gv) { $stripToolName = $gv.Value } }

# Count binaries in build output
$bins = Get-ChildItem -Path $BuildDir -Recurse -Include *.exe,*.dll -File -ErrorAction SilentlyContinue
$binCount = 0 ; if ($bins) { $binCount = ($bins | Measure-Object).Count }

Write-Note "Build summary:"
Write-Note "  Generator: $gen"
Write-Note "  Toolchain: $toolchain"
Write-Note "  Configuration: $Configuration    Parallel: $Parallel"
Write-Note "  StripBinary: $StripBinary    RemovePdb: $RemovePdb    GENERATE_PDB_FOR_RELEASE: $generatePdb"
$stripDisplay = if ($stripToolName) { $stripToolName } else { 'None' }
Write-Note "  Strip tool: $stripDisplay"
Write-Note "  Binaries found: $binCount (under $BuildDir)"

if ($PruneIntermediates) {
    Invoke-BuildArtifactCleanup -BuildDir $BuildDir -Config $Configuration -PreservePdb:(-not $RemovePdb) -PreserveCrashDumps:$true | Out-Null
}

Write-Note "Hardened Release build finished. Review output artifacts in $BuildDir"

# Return success
exit 0
