<#
完整重建腳本。

用途:
- 無法排除的建置問題
- 懷疑 CMake cache 或舊產物污染
- 需要完整乾淨重編
#>
param(
    # ForceFull script: default to unconditional full rebuild
    [switch]$ForceFull = $true,
    [bool]$AutoKill = $true,
    [ValidateSet('auto','2022','2026')][string]$VSVersion = 'auto',
    [ValidateSet('VisualStudio','Ninja','Auto')][string]$PreferGenerator = 'Ninja',
    [bool]$PruneIntermediates = $true,
    [switch]$CopyWpcap,
    [string]$BuildDir = 'build',
    # Auto-initialize VC environment (vcvarsall) if needed
    [switch]$AutoInitVcVars = $true,
    # Optional: explicitly provide path to vcvarsall.bat to initialize VS env
    [string]$VcVarsPath = $null
)

$ErrorActionPreference = 'Stop'
Write-Host "=== Unified Build Script (ForceFull mode) ===" -ForegroundColor Cyan
Write-Host "Start: $(Get-Date -Format 'HH:mm:ss')" -ForegroundColor Gray
if ($ForceFull) { Write-Host "Mode: ForceFull = ON (unconditional clean full rebuild)" -ForegroundColor Yellow } else { Write-Host "Mode: Incremental allowed (ForceFull = OFF)" -ForegroundColor Gray }

function Resolve-RepoRoot {
    param([string]$ScriptRoot)

    if ($ScriptRoot -and (Test-Path (Join-Path $ScriptRoot 'CMakeLists.txt'))) {
        return $ScriptRoot
    }

    $parent = Split-Path -Parent $ScriptRoot
    if ($parent -and (Test-Path (Join-Path $parent 'CMakeLists.txt'))) {
        return $parent
    }

    return $ScriptRoot
}

$projectRoot = Resolve-RepoRoot -ScriptRoot $PSScriptRoot
$exeName = 'BigCat'
. (Join-Path $PSScriptRoot 'Invoke-BuildArtifactCleanup.ps1')

function Stop-RunningProcess {
    param([bool]$DoKill)
    if (-not $DoKill) { return }
    Write-Host "Checking for running process $exeName..." -ForegroundColor Yellow
    $proc = Get-Process -Name $exeName -ErrorAction SilentlyContinue
    if ($proc) {
        Write-Host "Found running process(es): $($proc | Select-Object -ExpandProperty Id)" -ForegroundColor Yellow
        try {
            $proc | Stop-Process -Force -ErrorAction Stop
            Write-Host "Stopped process(es)." -ForegroundColor Green
            Start-Sleep -Milliseconds 500
        } catch { Write-Warning "Failed to stop process(es): $_" }
    }
}

# Find vcvarsall for requested versions
function Find-VcVarsAll {
    param([string]$VersionHint = 'auto')

    # If VSINSTALLDIR present, prefer it
    if ($env:VSINSTALLDIR) {
        $c = Join-Path $env:VSINSTALLDIR 'VC\Auxiliary\Build\vcvarsall.bat'
        if (Test-Path $c) { return $c }
    }

    # Prefer vswhere when available
    $vswhere = 'C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path $vswhere) {
        try {
            $queryVersionRange = if ($VersionHint -eq '2022') { '[17.0,18.0)' } elseif ($VersionHint -eq '2026') { '[18.0,19.0)' } else { '[17.0,20.0)' }
            $inst = & $vswhere -latest -products * -version $queryVersionRange -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2>$null
            if ($inst) {
                $p = Join-Path $inst 'VC\Auxiliary\Build\vcvarsall.bat'
                if (Test-Path $p) { return $p }
            }
        } catch { }
    }

    # Common installed edition paths (including Insiders)
    $cands = @()
    if ($VersionHint -eq '2022' -or $VersionHint -eq 'auto') {
        $cands += @(
            'C:\Program Files\Microsoft Visual Studio\17\Enterprise\VC\Auxiliary\Build\vcvarsall.bat',
            'C:\Program Files\Microsoft Visual Studio\17\Professional\VC\Auxiliary\Build\vcvarsall.bat',
            'C:\Program Files\Microsoft Visual Studio\17\Community\VC\Auxiliary\Build\vcvarsall.bat'
        )
    }
    if ($VersionHint -eq '2026' -or $VersionHint -eq 'auto') {
        $cands += @(
            'C:\Program Files\Microsoft Visual Studio\18\Enterprise\VC\Auxiliary\Build\vcvarsall.bat',
            'C:\Program Files\Microsoft Visual Studio\18\Professional\VC\Auxiliary\Build\vcvarsall.bat',
            'C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat',
            'C:\Program Files\Microsoft Visual Studio\18\Insiders\VC\Auxiliary\Build\vcvarsall.bat'
        )
    }
    foreach ($c in $cands) { if (Test-Path $c) { return $c } }

    # Fallback: scan VS install roots for any vcvars script (covers Insiders/Preview)
    $rootDirs = @('C:\Program Files\Microsoft Visual Studio', 'C:\Program Files (x86)\Microsoft Visual Studio')
    foreach ($root in $rootDirs) {
        if (Test-Path $root) {
            try {
                $found = Get-ChildItem -Path $root -Filter 'vcvarsall.bat' -Recurse -ErrorAction SilentlyContinue | Select-Object -First 1
                if ($found) { return $found.FullName }
            } catch { }
        }
    }

    return $null
}

function Find-CMakeExe {
    $cands = @(
        'C:\Program Files\CMake\bin\cmake.exe',
        'C:\Program Files\Microsoft Visual Studio\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
    )
    foreach ($c in $cands) { if (Test-Path $c) { return $c } }
    return 'cmake'
}

function Test-CMakeGenerator {
    param([string]$CMakeExe, [string]$GeneratorName)
    try {
        $help = & $CMakeExe --help | Out-String
        return ($help -match [regex]::Escape($GeneratorName))
    } catch { return $false }
}

function Choose-Generator {
    param([string]$Prefer, [string]$VcVars)
    $cmakeExe = Find-CMakeExe
    $candidates = @()
    # If MSVC toolchain present, prefer Ninja (uses cl) for faster parallel builds
    if ($VcVars -or (Get-Command cl -ErrorAction SilentlyContinue)) {
        if (Test-CMakeGenerator $cmakeExe 'Ninja') { $candidates += 'Ninja' }
        if (Test-CMakeGenerator $cmakeExe 'Visual Studio 18 2026') { $candidates += 'Visual Studio 18 2026' }
        if (Test-CMakeGenerator $cmakeExe 'Visual Studio 17 2022') { $candidates += 'Visual Studio 17 2022' }
        if (Test-CMakeGenerator $cmakeExe 'NMake Makefiles') { $candidates += 'NMake Makefiles' }
        if ($candidates.Count -gt 0) { return $candidates }
    }
    # Non-MSVC or fallback ordering: consider gcc/MinGW too
    $haveCl = $(Get-Command cl -ErrorAction SilentlyContinue) -ne $null
    $haveClang = $(Get-Command clang -ErrorAction SilentlyContinue) -ne $null
    $haveGcc = $(Get-Command gcc -ErrorAction SilentlyContinue) -ne $null

    # Only add Ninja if CMake supports it and a compiler is available (cl/clang/gcc) or user explicitly prefers Ninja
    if ((Test-CMakeGenerator $cmakeExe 'Ninja') -and ($Prefer -eq 'Ninja' -or $haveCl -or $haveClang -or $haveGcc)) { $candidates += 'Ninja' }
    if (($haveGcc) -and (Test-CMakeGenerator $cmakeExe 'MinGW Makefiles')) { $candidates += 'MinGW Makefiles' }
    if (Test-CMakeGenerator $cmakeExe 'NMake Makefiles') { $candidates += 'NMake Makefiles' }
    if (Test-CMakeGenerator $cmakeExe 'Visual Studio 17 2022') { $candidates += 'Visual Studio 17 2022' }
    if ($candidates.Count -eq 0) { $candidates += 'Ninja' }
    return $candidates
}

# Initialize VC environment (vcvarsall) and import important env vars
function Initialize-VcVars {
    param([string]$VcVarsPath, [string]$Arch = 'x64')
    if (-not (Test-Path $VcVarsPath)) { return $false }
    Write-Host "Initializing Visual Studio environment using: $VcVarsPath $Arch" -ForegroundColor Cyan
    $cmdExe = $env:ComSpec; if (-not $cmdExe) { $cmdExe = 'cmd.exe' }
    $out = & $cmdExe /c "`"$VcVarsPath`" $Arch && set" 2>&1
    if (-not $out) { Write-Warning "vcvarsall did not return environment variables or failed."; return $false }
    foreach ($line in $out) {
        if ($line -match '^(.*?)=(.*)$') {
            $k = $matches[1]; $v = $matches[2]
            if ($k -in @('PATH','INCLUDE','LIB','LIBPATH','VSINSTALLDIR','VCINSTALLDIR')) {
                try { Set-Item -Path ("Env:$k") -Value $v -ErrorAction SilentlyContinue } catch { }
            }
        }
    }
    if (Get-Command cl -ErrorAction SilentlyContinue) { Write-Host "cl visible after vcvarsall initialization." -ForegroundColor Green; return $true }
    Write-Warning "cl not found even after vcvarsall initialization."; return $false
}

function Find-WindowsSdkBin {
    $kitsRoot = 'C:\Program Files (x86)\Windows Kits\10\bin'
    if (-not (Test-Path $kitsRoot)) { return $null }
    $verDir = Get-ChildItem -Path $kitsRoot -Directory | Where-Object { $_.Name -match '^\d+\.\d+\.\d+\.\d+$' } | Sort-Object Name -Descending | Select-Object -First 1
    if (-not $verDir) { return $null }
    $sdkBin = Join-Path $verDir.FullName 'x64'
    if (Test-Path (Join-Path $sdkBin 'rc.exe')) { return $sdkBin }
    return $null
}

function Run-Configure {
    param($VSBatch, $ProjectRoot, $BuildDir, $Generator, $CMakeExe, $ExtraDefs)
    $cmdExe = $env:ComSpec
    if (-not $cmdExe) { $cmdExe = Join-Path $env:SystemRoot 'System32\cmd.exe' }
    $sdkBin = Find-WindowsSdkBin
    if ($sdkBin) { $env:Path = "$sdkBin;$env:Path" }

    # Build a base args list
    $baseArgs = @('-S', $ProjectRoot, '-B', $BuildDir, '-DCMAKE_BUILD_TYPE=Release', '-DUSE_LOCAL_TINYXML2=ON')
    if ($ExtraDefs) { $baseArgs += $ExtraDefs }

    if ($VSBatch -and (Test-Path $VSBatch)) {
        Write-Host "Using Visual Studio environment: $VSBatch" -ForegroundColor Gray
        if ($Generator -like 'Ninja*') {
            $args = $baseArgs + @('-G', $Generator)
            $quoted = ($args | ForEach-Object { '"' + ($_ -replace '"','\"') + '"' }) -join ' '
            & $cmdExe /c "`"$VSBatch`" x64 && cd /d `"$ProjectRoot`" && `"$CMakeExe`" $quoted" | Out-Host
        } else {
            $args = $baseArgs + @('-G', $Generator, '-A', 'x64')
            $quoted = ($args | ForEach-Object { '"' + ($_ -replace '"','\"') + '"' }) -join ' '
            & $cmdExe /c "`"$VSBatch`" x64 && cd /d `"$ProjectRoot`" && `"$CMakeExe`" $quoted" | Out-Host
        }
        return $LASTEXITCODE
    }

    # If cl is available, prefer Ninja with MSVC toolchain
    if (Get-Command cl -ErrorAction SilentlyContinue) {
        Write-Host "cl.exe found in PATH — configuring with Ninja (MSVC)" -ForegroundColor Gray
        $args = $baseArgs + @('-G', 'Ninja')
        & $CMakeExe @args | Out-Host
        return $LASTEXITCODE
    }

    if ((Get-Command -Name 'clang' -ErrorAction SilentlyContinue) -or (Get-Command -Name 'clang-cl' -ErrorAction SilentlyContinue)) {
        Write-Host "Clang detected — configuring with clang+Ninja" -ForegroundColor Gray
        $args = $baseArgs + @('-G', 'Ninja', '-DCMAKE_C_COMPILER=clang', '-DCMAKE_CXX_COMPILER=clang++')
        & $CMakeExe @args | Out-Host
        return $LASTEXITCODE
    }

    # Support GCC/MinGW if present
    if ((Get-Command gcc -ErrorAction SilentlyContinue) -or (Get-Command g++ -ErrorAction SilentlyContinue)) {
        if (Test-CMakeGenerator $CMakeExe 'MinGW Makefiles') {
            Write-Host "GCC detected — configuring with MinGW Makefiles" -ForegroundColor Gray
            $args = $baseArgs + @('-G', 'MinGW Makefiles', '-DCMAKE_C_COMPILER=gcc', '-DCMAKE_CXX_COMPILER=g++')
            & $CMakeExe @args | Out-Host
            return $LASTEXITCODE
        } elseif (Test-CMakeGenerator $CMakeExe 'Unix Makefiles') {
            Write-Host "GCC detected — configuring with Unix Makefiles" -ForegroundColor Gray
            $args = $baseArgs + @('-G', 'Unix Makefiles', '-DCMAKE_C_COMPILER=gcc', '-DCMAKE_CXX_COMPILER=g++')
            & $CMakeExe @args | Out-Host
            return $LASTEXITCODE
        } else {
            Write-Warning "GCC found but no suitable MinGW/Unix generator available in CMake."
        }
    }

    Write-Host "No suitable compiler (cl/clang/gcc) or Visual Studio environment found. Open Developer PowerShell or install a compiler." -ForegroundColor Red
    return 2
}

function Run-Build {
    param($VSBatch, $ProjectRoot, $BuildDir, $CMakeExe, $Parallel)
    $cmdExe = $env:ComSpec
    if (-not $cmdExe) { $cmdExe = Join-Path $env:SystemRoot 'System32\cmd.exe' }
    $sdkBin = Find-WindowsSdkBin
    if ($sdkBin) { $env:Path = "$sdkBin;$env:Path" }

    # Helper to choose appropriate build invocation based on generator files
    function Get-DetectedGenerator($buildDirPath) {
        if (Test-Path (Join-Path $buildDirPath 'build.ninja')) { return 'Ninja' }
        if (Get-ChildItem -Path $buildDirPath -Filter '*.sln' -ErrorAction SilentlyContinue) { return 'Visual Studio' }
        if (Test-Path (Join-Path $buildDirPath 'Makefile')) { return 'NMake' }
        return 'Unknown'
    }

    $detected = Get-DetectedGenerator $BuildDir

    if ($VSBatch -and (Test-Path $VSBatch)) {
        if ($detected -eq 'Ninja') {
            & $cmdExe /c "`"$VSBatch`" x64 && cd /d `"$ProjectRoot`" && `"$CMakeExe`" --build `"$BuildDir`" --config Release --parallel $Parallel" | Out-Host
        } elseif ($detected -eq 'Visual Studio') {
            & $cmdExe /c "`"$VSBatch`" x64 && cd /d `"$ProjectRoot`" && `"$CMakeExe`" --build `"$BuildDir`" --config Release -- /m:$Parallel" | Out-Host
        } elseif ($detected -eq 'NMake') {
            & $cmdExe /c "`"$VSBatch`" x64 && cd /d `"$BuildDir`" && nmake" | Out-Host
        } else {
            & $cmdExe /c "`"$VSBatch`" x64 && cd /d `"$ProjectRoot`" && `"$CMakeExe`" --build `"$BuildDir`" --config Release" | Out-Host
        }
        return $LASTEXITCODE
    }

    if ($detected -eq 'Ninja') {
        & $CMakeExe --build $BuildDir --config Release --parallel $Parallel | Out-Host
    } elseif ($detected -eq 'Visual Studio') {
        & $CMakeExe --build $BuildDir --config Release -- /m:$Parallel | Out-Host
    } elseif ($detected -eq 'NMake') {
        & nmake | Out-Host
    } else {
        & $CMakeExe --build $BuildDir --config Release --parallel $Parallel | Out-Host
    }
    return $LASTEXITCODE
}

function Copy-RequiredDlls {
    param([string]$BuildDirPath)
    $dlls = @('wpcap.dll', 'Packet.dll')
    $candidateDirs = @(
        (Join-Path $projectRoot 'lib\npcap-sdk'),
        "$env:ProgramFiles\Npcap",
        "$env:ProgramFiles(x86)\Npcap",
        'C:\Windows\System32',
        'C:\Windows\SysWOW64'
    )
    $destDir = Join-Path $projectRoot (Join-Path $BuildDirPath 'Release\bin\main')
    if (-not (Test-Path $destDir)) { New-Item -ItemType Directory -Path $destDir -Force | Out-Null }
    foreach ($dll in $dlls) {
        $found = $false
        foreach ($d in $candidateDirs) {
            if ([string]::IsNullOrEmpty($d)) { continue }
            $src = Join-Path $d $dll
            if (Test-Path $src) {
                try { Copy-Item -Path $src -Destination $destDir -Force; Write-Host "Copied $dll from $src to $destDir" -ForegroundColor Green; $found = $true; break } catch { Write-Warning ("Failed to copy {0} from {1}: {2}" -f $dll, $src, $_) }
            }
        }
        if (-not $found) { Write-Warning "Could not find $dll in any candidate paths." }
    }
}

# Start build flow
Stop-RunningProcess -DoKill $AutoKill

$vcvars = $null
# If user provided explicit VcVarsPath, try to initialize it first
if ($VcVarsPath) {
    if (Test-Path $VcVarsPath) {
        Write-Host "User-specified VcVarsPath provided: $VcVarsPath" -ForegroundColor Gray
        $ok = Initialize-VcVars -VcVarsPath $VcVarsPath -Arch 'x64'
        if ($ok) { $vcvars = $VcVarsPath; Write-Host "Initialized VC environment from VcVarsPath" -ForegroundColor Green }
        else { Write-Warning "Failed to initialize VC environment from provided VcVarsPath. Will attempt auto-detection." }
    } else {
        Write-Warning "Provided -VcVarsPath '$VcVarsPath' not found. Will attempt auto-detection." 
    }
}

if (-not $vcvars) {
    if ($VSVersion -ne 'auto') { $vcvars = Find-VcVarsAll -VersionHint $VSVersion } else { $vcvars = Find-VcVarsAll -VersionHint 'auto' }
    if ($vcvars) { Write-Host "Found VC environment: $vcvars" -ForegroundColor Gray } else { Write-Host "No VC environment found; will fallback to Ninja/clang if available." -ForegroundColor Yellow }
}

$cmakeExe = Find-CMakeExe
$parallel = [Environment]::ProcessorCount
$buildDirPath = if ([System.IO.Path]::IsPathRooted($BuildDir)) { $BuildDir } else { Join-Path $projectRoot $BuildDir }
$didIncremental = $false

# Incremental build attempt
if ((Test-Path $buildDirPath) -and (-not $ForceFull)) {
    Write-Host "`n[Phase 1] Attempting Incremental Build..." -ForegroundColor Yellow
    # Choose build method: prefer Visual Studio if requested and vcvars available
    $incRc = 0
    if ($vcvars -and $PreferGenerator -eq 'VisualStudio') {
        $incRc = Run-Build $vcvars $projectRoot $BuildDir $cmakeExe $parallel
    } else {
        $incRc = Run-Build $null $projectRoot $BuildDir $cmakeExe $parallel
    }
    if ($incRc -eq 0) { Write-Host "Incremental Build Successful!" -ForegroundColor Green; $didIncremental = $true } else { Write-Host "Incremental Build Failed (rc=$incRc). Will perform full build." -ForegroundColor Red }
} else {
    if ($ForceFull) { Write-Host "ForceFull specified. Skipping incremental build." -ForegroundColor Yellow } else { Write-Host "No build directory found. Skipping incremental build." -ForegroundColor Yellow }
}

if (-not $didIncremental) {
    Write-Host "`n[Phase 2] Starting Full Build..." -ForegroundColor Yellow
    if ($ForceFull) {
        Write-Host "  ForceFull specified -> performing unconditional clean rebuild" -ForegroundColor Yellow
        if (Test-Path $buildDirPath) {
            try { Remove-Item -Recurse -Force $buildDirPath -ErrorAction Stop; Write-Host "  Removed existing build directory" -ForegroundColor Green } catch { Write-Host "  ERROR: Failed to remove build directory: $_" -ForegroundColor Red; exit 2 }
        } else {
            Write-Host "  No existing build directory found; will create a fresh one" -ForegroundColor Gray
        }
        try { New-Item -ItemType Directory -Path $buildDirPath -Force | Out-Null; Write-Host "  Recreated build directory: $buildDirPath" -ForegroundColor Green } catch { Write-Host "  ERROR: Failed to create build directory: $_" -ForegroundColor Red; exit 2 }
    } else {
        if (Test-Path $buildDirPath) { Remove-Item -Recurse -Force $buildDirPath; Write-Host "  Cleaned" -ForegroundColor Green } else { Write-Host "  No previous build dir to clean (fresh full build)" -ForegroundColor Gray }
    }

    # Pick generator candidates (prefer Ninja with MSVC when available)
    $candidates = Choose-Generator $PreferGenerator $vcvars
    Write-Host "  Generator candidates: $($candidates -join ', ')" -ForegroundColor Yellow

    # Auto-init VC environment if requested
    if ($AutoInitVcVars -and $vcvars -and -not (Get-Command cl -ErrorAction SilentlyContinue)) {
        Write-Host "Found vcvars at: $vcvars; attempting to initialize..." -ForegroundColor Gray
        $ok = Initialize-VcVars -VcVarsPath $vcvars -Arch 'x64'
        if (-not $ok) { Write-Warning "vcvars initialization did not expose 'cl'. Proceeding; certain generators may fail configure." }
    }

    # Try candidates sequentially and stop on first success
    $configured = $false
    $selectedGen = ''
    foreach ($g in $candidates) {
        Write-Host "  Attempting configure with generator: $g" -ForegroundColor Yellow

        # Pre-flight checks: ensure required toolchain present for the generator we're about to try
        $haveCl = $(Get-Command cl -ErrorAction SilentlyContinue) -ne $null
        $haveClang = $(Get-Command clang -ErrorAction SilentlyContinue) -ne $null
        $haveGcc = $(Get-Command gcc -ErrorAction SilentlyContinue) -ne $null
        $haveNmake = $(Get-Command nmake -ErrorAction SilentlyContinue) -ne $null

        if ($g -eq 'Ninja') {
            if (-not ($haveCl -or $haveClang -or $haveGcc -or $vcvars)) {
                Write-Warning "  Skipping Ninja: no compiler detected (cl/clang/gcc) and no VC environment available."; continue
            }
        }
        if ($g -like '*NMake*') {
            if (-not ($haveNmake -or $vcvars)) { Write-Warning "  Skipping NMake: 'nmake' not found and no VC environment available."; continue }
        }
        if ($g -like '*Visual Studio*') {
            if (-not $vcvars) { Write-Warning "  Skipping Visual Studio generator: no VC environment available."; continue }
        }
        if ($g -eq 'MinGW Makefiles') {
            if (-not $haveGcc) { Write-Warning "  Skipping MinGW Makefiles: gcc not found."; continue }
        }

        $extra = @()
        # For hardened full Release builds, prefer disabling PDBs under MSVC to allow LTCG
        if ($g -like '*Visual Studio*' -or $vcvars) {
            $extra += '-DGENERATE_PDB_FOR_RELEASE=OFF'
            $extra += '-DCMAKE_MSVC_DEBUG_INFORMATION_FORMAT=None'
        }
        $rc = Run-Configure $vcvars $projectRoot $BuildDir $g $cmakeExe $extra
        if ($rc -eq 0) { Write-Host "  Configured successfully with: $g" -ForegroundColor Green; $configured = $true; $selectedGen = $g; break }
        Write-Warning "  Generator $g failed (rc=$rc); trying next candidate..."
        # ensure clean state before next attempt
        if (Test-Path $buildDirPath) {
            try { Remove-Item -Path (Join-Path $buildDirPath 'CMakeCache.txt') -Force -ErrorAction SilentlyContinue } catch { }
            $cmakeFilesDir = Join-Path $buildDirPath 'CMakeFiles'
            if (Test-Path $cmakeFilesDir) { try { Remove-Item -Path $cmakeFilesDir -Recurse -Force -ErrorAction SilentlyContinue } catch { } }
        }
    }
    if (-not $configured) {
        Write-Error "All generator candidates failed to configure."
        $haveCl = $(Get-Command cl -ErrorAction SilentlyContinue) -ne $null
        $haveClang = $(Get-Command clang -ErrorAction SilentlyContinue) -ne $null
        $haveGcc = $(Get-Command gcc -ErrorAction SilentlyContinue) -ne $null
        $haveNinja = $(Get-Command ninja -ErrorAction SilentlyContinue) -ne $null
        Write-Error "Detected tools: cl=$haveCl  clang=$haveClang  gcc=$haveGcc  ninja=$haveNinja"
        Write-Error "Suggested actions:"
        Write-Error "  - If using MSVC, open a Visual Studio Developer Command Prompt (so 'cl'/'nmake' are available) and re-run with -ForceFull."
        Write-Error "  - Install Ninja and ensure it's on PATH, then re-run with -ForceFull to use Ninja."
        Write-Error "  - If you have GCC/MinGW, install MinGW and ensure gcc/g++ are on PATH, or install a CMake generator that supports them (MinGW Makefiles)."
        exit 4
    }

    Write-Host "  Building project with selected generator: $selectedGen" -ForegroundColor Yellow
    $rc = Run-Build $vcvars $projectRoot $BuildDir $cmakeExe $parallel
    if ($rc -ne 0) { Write-Host "`n[ERROR] Build failed (rc=$rc)" -ForegroundColor Red; exit $rc }
}

# Check result
Write-Host "`n=== Build Result ===" -ForegroundColor Cyan
$exePath = Join-Path $projectRoot (Join-Path $BuildDir "Release\bin\main\$exeName.exe")
if (Test-Path $exePath) {
    $info = Get-Item $exePath
    Write-Host "[SUCCESS] Compilation successful!" -ForegroundColor Green
    Write-Host "  Executable: $($info.FullName)" -ForegroundColor Gray
    Write-Host "  Size: $([math]::Round($info.Length / 1KB, 2)) KB" -ForegroundColor Gray
    Write-Host "  Created: $($info.LastWriteTime.ToString('yyyy-MM-dd HH:mm:ss'))" -ForegroundColor Gray
} else {
    Write-Host "[ERROR] Executable not found at: $exePath" -ForegroundColor Red
    exit 1
}

if ($CopyWpcap) { Copy-RequiredDlls -BuildDirPath $BuildDir }

if ($PruneIntermediates) {
    Invoke-BuildArtifactCleanup -BuildDir $buildDirPath -Config 'Release' -PreservePdb:$false -PreserveCrashDumps:$true | Out-Null
}

Write-Host "`nEnd: $(Get-Date -Format 'HH:mm:ss')" -ForegroundColor Gray
Write-Host "=== Build Complete ===" -ForegroundColor Cyan
