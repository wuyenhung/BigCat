<#
增量建置腳本。

用途:
- 預設 Release 增量編譯
- 需要 CrashHandler、PDB
- 只有顯式要求 sanitizer 時才切到 Debug
#>

param(
    [string]$BuildDir = '',
    [ValidateSet('Debug','Release')]
    [string]$Config = 'Release',
    [int]$Parallel = [Environment]::ProcessorCount,
    [string]$Target = '',
    [bool]$AutoKill = $true,
    [bool]$PruneIntermediates = $true,

    [switch]$Crash = $true,
    [switch]$UseSanitizers = $false,
    [switch]$KeepPdb = $true,

    [switch]$ForceConfigure = $false,

    [ValidateSet('auto','2022','2026')][string]$VSVersion = 'auto',
    [ValidateSet('VisualStudio','Ninja','Auto')][string]$PreferGenerator = 'Auto',
    [ValidateSet('x64','x86')][string]$Arch = 'x64',
    [switch]$NonInteractive,
    [switch]$RunDetached,
    [switch]$AutoInitVcVars = $true
)

Write-Host "Incremental build script"
Write-Host "BuildDir: $BuildDir"
Write-Host "Config: $Config"
Write-Host "Parallel: $Parallel"
if ($Target -ne '') { Write-Host "Target: $Target" }
Write-Host "Crash: $Crash"
Write-Host "UseSanitizers: $UseSanitizers"
Write-Host "KeepPdb: $KeepPdb"
Write-Host "PruneIntermediates: $PruneIntermediates"
Write-Host "NonInteractive: $NonInteractive"
Write-Host "RunDetached: $RunDetached"

function Resolve-RepoRoot {
    $scriptRoot = $PSScriptRoot
    if ($scriptRoot -and (Test-Path (Join-Path $scriptRoot 'CMakeLists.txt'))) {
        return $scriptRoot
    }

    $parent = Split-Path -Parent $scriptRoot
    if ($parent -and (Test-Path (Join-Path $parent 'CMakeLists.txt'))) {
        return $parent
    }

    return $scriptRoot
}

$ProjectRoot = Resolve-RepoRoot
if ([string]::IsNullOrWhiteSpace($BuildDir)) {
    $BuildDir = Join-Path -Path $ProjectRoot -ChildPath 'build'
} elseif (-not [System.IO.Path]::IsPathRooted($BuildDir)) {
    $BuildDir = Join-Path -Path $ProjectRoot -ChildPath $BuildDir
}

. (Join-Path $PSScriptRoot 'Invoke-BuildArtifactCleanup.ps1')

if ($env:GITHUB_ACTIONS -or $env:CI) { $NonInteractive = $true }
try { if (-not $Host.UI.RawUI) { $NonInteractive = $true } } catch { $NonInteractive = $true }
if ($NonInteractive) { Write-Host "NonInteractive mode enabled (prompts will be skipped)" }

function Find-CMakeTool {
    param([Parameter(Mandatory = $true)][ValidateSet('cmake','ctest','ninja')][string]$ToolName)

    $cmd = Get-Command $ToolName -ErrorAction SilentlyContinue
    if ($cmd -and $cmd.Source) {
        return $cmd.Source
    }

    $suffix = switch ($ToolName) {
        'cmake' { 'CMake\bin\cmake.exe' }
        'ctest' { 'CMake\bin\ctest.exe' }
        'ninja' { 'Ninja\ninja.exe' }
    }

    $cands = @()
    if ($ToolName -eq 'cmake') {
        $cands += 'C:\Program Files\CMake\bin\cmake.exe'
    } elseif ($ToolName -eq 'ctest') {
        $cands += 'C:\Program Files\CMake\bin\ctest.exe'
    }

    $vsRoots = @(
        'C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake',
        'C:\Program Files\Microsoft Visual Studio\18\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake',
        'C:\Program Files\Microsoft Visual Studio\17\Community\Common7\IDE\CommonExtensions\Microsoft\CMake',
        'C:\Program Files\Microsoft Visual Studio\17\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake',
        'C:\Program Files\Microsoft Visual Studio\Common7\IDE\CommonExtensions\Microsoft\CMake'
    )
    foreach ($root in $vsRoots) {
        $cands += (Join-Path $root $suffix)
    }

    foreach ($c in $cands) {
        if (Test-Path $c) { return $c }
    }

    if ($ToolName -eq 'ninja') {
        return $null
    }

    return $ToolName
}
function Find-CMakeExe { return Find-CMakeTool -ToolName 'cmake' }
function Find-NinjaExe { return Find-CMakeTool -ToolName 'ninja' }
function Test-CMakeGenerator { param([string]$CMakeExe, [string]$GeneratorName) try { $help = & $CMakeExe --help | Out-String; return ($help -match [regex]::Escape($GeneratorName)) } catch { return $false } }
function Choose-Generator { param([string]$Prefer)
    $cmakeExe = Find-CMakeExe
    $candidates = @()
    # Prefer Ninja under MSVC toolchain to enable parallel builds with cl
    if ((Get-Command cl -ErrorAction SilentlyContinue) -and (Test-CMakeGenerator $cmakeExe 'Ninja')) { $candidates += 'Ninja' }
    if ($Prefer -eq 'VisualStudio') {
        if (Test-CMakeGenerator $cmakeExe 'Visual Studio 18 2026') { $candidates += 'Visual Studio 18 2026' }
        if (Test-CMakeGenerator $cmakeExe 'Visual Studio 17 2022') { $candidates += 'Visual Studio 17 2022' }
        if (Test-CMakeGenerator $cmakeExe 'NMake Makefiles') { $candidates += 'NMake Makefiles' }
    }
    # Generic fallbacks
    if (Test-CMakeGenerator $cmakeExe 'Ninja' -and -not ($candidates -contains 'Ninja')) { $candidates += 'Ninja' }
    if (Test-CMakeGenerator $cmakeExe 'NMake Makefiles' -and -not ($candidates -contains 'NMake Makefiles')) { $candidates += 'NMake Makefiles' }
    if ($candidates.Count -eq 0) { $candidates += 'Ninja' }
    return $candidates
}

function Test-IncrementalBuildNeedsReconfigure {
    param([string]$BuildDirPath)

    $buildNinja = Join-Path $BuildDirPath 'build.ninja'
    if (-not (Test-Path $buildNinja)) {
        return $false
    }

    $pchDir = Join-Path $BuildDirPath 'CMakeFiles\BigCat.dir'
    $pchSource = Join-Path $pchDir 'cmake_pch.cxx'
    $pchHeader = Join-Path $pchDir 'cmake_pch.hxx'

    if ((-not (Test-Path $pchDir)) -or (-not (Test-Path $pchSource)) -or (-not (Test-Path $pchHeader))) {
        return $true
    }

    return $false
}

function Get-CMakeCacheEntryValue {
    param(
        [Parameter(Mandatory = $true)][string]$CacheText,
        [Parameter(Mandatory = $true)][string]$Key
    )

    $match = [regex]::Match($CacheText, "(?m)^$([regex]::Escape($Key)):[^=]*=(.*)$")
    if ($match.Success) {
        return $match.Groups[1].Value.Trim()
    }

    return $null
}

function Test-BuildTreeCacheHealth {
    param(
        [Parameter(Mandatory = $true)][string]$BuildDirPath,
        [Parameter(Mandatory = $true)][string]$DesiredConfig
    )

    $cacheFilePath = Join-Path $BuildDirPath 'CMakeCache.txt'
    if (-not (Test-Path $cacheFilePath)) {
        return @{ Valid = $true; Reason = '' }
    }

    try {
        $cacheText = Get-Content $cacheFilePath -Raw -ErrorAction Stop
    } catch {
        return @{ Valid = $false; Reason = "Unable to read CMakeCache.txt: $_" }
    }

    $toolKeys = @(
        'CMAKE_C_COMPILER',
        'CMAKE_CXX_COMPILER',
        'CMAKE_LINKER',
        'CMAKE_AR',
        'CMAKE_MAKE_PROGRAM'
    )

    foreach ($key in $toolKeys) {
        $value = Get-CMakeCacheEntryValue -CacheText $cacheText -Key $key
        if ([string]::IsNullOrWhiteSpace($value) -or $value -match 'NOTFOUND$') {
            continue
        }
        if (($value -match '^[A-Za-z]:[\\/]' -or $value -match '^/') -and -not (Test-Path $value)) {
            return @{ Valid = $false; Reason = "Cached tool path missing for ${key}: $value" }
        }
    }

    $generator = Get-CMakeCacheEntryValue -CacheText $cacheText -Key 'CMAKE_GENERATOR'
    $buildType = Get-CMakeCacheEntryValue -CacheText $cacheText -Key 'CMAKE_BUILD_TYPE'
    if ($generator -and $DesiredConfig -and
        $generator -notmatch 'Visual Studio|Xcode' -and
        $buildType -and $buildType -ne $DesiredConfig) {
        return @{ Valid = $false; Reason = "Single-config generator '$generator' cached as '$buildType' but requested '$DesiredConfig'" }
    }

    $buildNinjaPath = Join-Path $BuildDirPath 'build.ninja'
    if (Test-Path $buildNinjaPath) {
        try {
            $buildNinjaText = Get-Content $buildNinjaPath -Raw -ErrorAction Stop
            if ($buildNinjaText -match 'Windows Kits\\10\\Include\\wdf\\(ucrt|um|shared)') {
                return @{ Valid = $false; Reason = 'Generated Ninja graph is pinned to WDF Windows SDK include paths instead of the standard WinSDK include version.' }
            }
        } catch {
            return @{ Valid = $false; Reason = "Unable to inspect build.ninja for SDK contamination: $_" }
        }
    }

    return @{ Valid = $true; Reason = '' }
}

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
    $cands = @( 
        'C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat',
        'C:\Program Files\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvarsall.bat',
        'C:\Program Files\Microsoft Visual Studio\18\Insiders\VC\Auxiliary\Build\vcvarsall.bat',
        'C:\Program Files\Microsoft Visual Studio\17\Community\VC\Auxiliary\Build\vcvarsall.bat'
    )
    foreach ($c in $cands) { if (Test-Path $c) { return $c } }
    return $null
}

function Initialize-VcVars {
    param([string]$VcVarsPath, [string]$Arch = 'x64')
    if (-not (Test-Path $VcVarsPath)) { return $false }
    Write-Host "Initializing Visual Studio environment using: $VcVarsPath $Arch" -ForegroundColor Cyan
    $cmdExe = $env:ComSpec; if (-not $cmdExe) { $cmdExe = 'cmd.exe' }
    # Run vcvarsall and capture environment via 'set'
    $out = & $cmdExe /c "`"$VcVarsPath`" $Arch && set" 2>&1
    if (-not $out) { Write-Warning "vcvarsall did not return environment variables or failed."; return $false }
    $importVars = @(
        'PATH',
        'INCLUDE',
        'LIB',
        'LIBPATH',
        'VSINSTALLDIR',
        'VCINSTALLDIR',
        'VCToolsInstallDir',
        'VCToolsVersion',
        'WindowsSdkDir',
        'WindowsSDKVersion',
        'WindowsSDKLibVersion',
        'WindowsLibPath',
        'UniversalCRTSdkDir',
        'UCRTVersion',
        'ExtensionSdkDir'
    )
    foreach ($line in $out) {
        if ($line -match '^(.*?)=(.*)$') {
                $k = $matches[1]; $v = $matches[2]
                # Only import relevant variables (PATH, LIB, INCLUDE, etc.) to avoid overriding shell behavior
                if ($k -in $importVars) {
                    try { Set-Item -Path ("Env:$k") -Value $v -ErrorAction SilentlyContinue } catch { }
                }
            }
    }
    # Validate cl is now available
    if (Get-Command cl -ErrorAction SilentlyContinue) { Write-Host "cl visible after vcvarsall initialization." -ForegroundColor Green; return $true }
    Write-Warning "cl not found even after vcvarsall initialization."; return $false
}

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

function Test-CommandLineReferencesPath {
    param(
        [string]$CommandLine,
        [string]$Path
    )

    if ([string]::IsNullOrWhiteSpace($CommandLine) -or [string]::IsNullOrWhiteSpace($Path)) {
        return $false
    }

    $normalizedPath = $Path.TrimEnd('\', '/')
    if ([string]::IsNullOrWhiteSpace($normalizedPath)) {
        return $false
    }

    $slashPath = $normalizedPath.Replace('\', '/')
    return ($CommandLine.IndexOf($normalizedPath, [System.StringComparison]::OrdinalIgnoreCase) -ge 0) -or
           ($CommandLine.IndexOf($slashPath, [System.StringComparison]::OrdinalIgnoreCase) -ge 0)
}

function Get-BuildOutputProcesses {
    param(
        [Parameter(Mandatory = $true)][string]$BuildDirPath,
        [string]$PrimaryProcessName = 'BigCat'
    )

    $resolvedBuildDir = $BuildDirPath
    try {
        $resolvedBuildDir = (Resolve-Path -Path $BuildDirPath -ErrorAction Stop).ProviderPath
    } catch {
    }
    $buildPrefix = $resolvedBuildDir.TrimEnd('\\') + '\\'

    $processMap = @{}
    $candidateNames = New-Object 'System.Collections.Generic.HashSet[string]' ([System.StringComparer]::OrdinalIgnoreCase)
    $null = $candidateNames.Add($PrimaryProcessName)

    foreach ($exe in @(Get-ChildItem -Path $resolvedBuildDir -Filter '*.exe' -Recurse -File -ErrorAction SilentlyContinue)) {
        if (-not [string]::IsNullOrWhiteSpace($exe.BaseName)) {
            $null = $candidateNames.Add($exe.BaseName)
        }
    }

    foreach ($proc in @(Get-Process -Name @($candidateNames) -ErrorAction SilentlyContinue)) {
        $procPath = $null
        try {
            $procPath = $proc.Path
        } catch {
        }

        $processMap[$proc.Id] = [pscustomobject]@{
            Id = $proc.Id
            ProcessName = $proc.ProcessName
            Path = $procPath
        }
    }

    $cimPattern = ($buildPrefix.Replace('\\', '\\\\')).Replace("'", "''") + '%'
    try {
        foreach ($proc in @(Get-CimInstance Win32_Process -Filter "ExecutablePath LIKE '$cimPattern'" -ErrorAction Stop)) {
            if ($proc.ProcessId -eq $PID) {
                continue
            }

            $processMap[$proc.ProcessId] = [pscustomobject]@{
                Id = [int]$proc.ProcessId
                ProcessName = $proc.Name
                Path = $proc.ExecutablePath
            }
        }
    } catch {
        foreach ($proc in @(Get-Process -ErrorAction SilentlyContinue)) {
            $procPath = $null
            try {
                $procPath = $proc.Path
            } catch {
                continue
            }

            if ([string]::IsNullOrWhiteSpace($procPath)) {
                continue
            }

            if ($procPath.StartsWith($buildPrefix, [System.StringComparison]::OrdinalIgnoreCase)) {
                $processMap[$proc.Id] = [pscustomobject]@{
                    Id = $proc.Id
                    ProcessName = $proc.ProcessName
                    Path = $procPath
                }
            }
        }
    }

    try {
        foreach ($proc in @(Get-CimInstance Win32_Process -Filter "Name = 'ctest.exe' OR Name = 'powershell.exe' OR Name = 'pwsh.exe'" -ErrorAction Stop)) {
            if ($proc.ProcessId -eq $PID) {
                continue
            }

            $cmdLine = $proc.CommandLine
            $isBuildCtest = ($proc.Name -ieq 'ctest.exe') -and
                (Test-CommandLineReferencesPath -CommandLine $cmdLine -Path $resolvedBuildDir)
            $isSmokeScript = (($proc.Name -ieq 'powershell.exe') -or ($proc.Name -ieq 'pwsh.exe')) -and
                ($cmdLine -match 'run_smoke_tests\.ps1') -and
                ((Test-CommandLineReferencesPath -CommandLine $cmdLine -Path $resolvedBuildDir) -or
                 (Test-CommandLineReferencesPath -CommandLine $cmdLine -Path $ProjectRoot))

            if (-not ($isBuildCtest -or $isSmokeScript)) {
                continue
            }

            $processMap[$proc.ProcessId] = [pscustomobject]@{
                Id = [int]$proc.ProcessId
                ProcessName = $proc.Name
                Path = $null
                CommandLine = $cmdLine
            }
        }
    } catch {
    }

    return @($processMap.Values | Sort-Object ProcessName, Id)
}

function Stop-BuildOutputProcessesForBuild {
    param(
        [Parameter(Mandatory = $true)][string]$BuildDirPath,
        [string]$PrimaryProcessName = 'BigCat',
        [bool]$NonInteractiveMode = $false
    )

    Write-Host "AutoKill enabled: checking for running build output processes..."
    $proc = @(Get-BuildOutputProcesses -BuildDirPath $BuildDirPath -PrimaryProcessName $PrimaryProcessName)
    if ($proc.Count -eq 0) {
        Write-Host "No running build output process found." -ForegroundColor Gray
        return $false
    }

    $procSummary = $proc | ForEach-Object {
        $procPath = $null
        $cmdLine = $null
        try { $procPath = $_.Path } catch { }
        try { $cmdLine = $_.CommandLine } catch { }
        if (-not [string]::IsNullOrWhiteSpace($procPath)) {
            return ("{0} (PID {1}) [{2}]" -f $_.ProcessName, $_.Id, $procPath)
        }
        if (-not [string]::IsNullOrWhiteSpace($cmdLine)) {
            return ("{0} (PID {1}) [{2}]" -f $_.ProcessName, $_.Id, $cmdLine)
        }
        return ("{0} (PID {1})" -f $_.ProcessName, $_.Id)
    }
    Write-Host "Found running build output process(es):" -ForegroundColor Yellow
    $procSummary | ForEach-Object { Write-Host "  $_" -ForegroundColor Yellow }

    try {
        $procIds = @($proc | Select-Object -ExpandProperty Id)
        $resolvedBuildDir = $BuildDirPath
        try {
            $resolvedBuildDir = (Resolve-Path -Path $BuildDirPath -ErrorAction Stop).ProviderPath
        } catch {
        }
        $buildPrefix = $resolvedBuildDir.TrimEnd('\') + '\'
        $procPaths = @($proc | ForEach-Object {
            $procPath = $null
            try { $procPath = $_.Path } catch { }
            if (-not [string]::IsNullOrWhiteSpace($procPath) -and
                $procPath.StartsWith($buildPrefix, [System.StringComparison]::OrdinalIgnoreCase)) {
                $procPath
            }
        })

        if ($NonInteractiveMode) {
            Write-Host "NonInteractive -> stopping running build output process(es)"
            Stop-Process -Id $procIds -Force -ErrorAction Stop
        } else {
            $ans = Read-Host "Stop running build output process(es) to allow build to proceed? (Y/n) [Y]"
            if ([string]::IsNullOrEmpty($ans) -or $ans.ToLower().StartsWith('y')) { Stop-Process -Id $procIds -Force -ErrorAction Stop }
            else { Write-Warning "User declined to stop processes; aborting."; exit 3 }
        }
        Wait-Process -Id $procIds -Timeout 5 -ErrorAction SilentlyContinue
        Wait-BuildOutputPathsReleased -Paths $procPaths -TimeoutMs 5000 | Out-Null
        Write-Host "Stopped process(es)." -ForegroundColor Green
        return $true
    } catch {
        Write-Warning "Failed to stop process(es): $_"
        Write-Warning "You may need to stop them manually or re-run with -AutoKill:`$false"
        exit 3
    }
}

function Wait-BuildOutputPathsReleased {
    param(
        [string[]]$Paths,
        [int]$TimeoutMs = 5000
    )

    $pendingPaths = @($Paths | Where-Object { -not [string]::IsNullOrWhiteSpace($_) } | Sort-Object -Unique)
    if ($pendingPaths.Count -eq 0) {
        return $true
    }

    $deadline = [DateTime]::UtcNow.AddMilliseconds($TimeoutMs)
    do {
        $lockedPaths = @()
        foreach ($path in $pendingPaths) {
            if (-not (Test-Path $path)) {
                continue
            }

            $handle = $null
            try {
                $handle = [System.IO.File]::Open($path, [System.IO.FileMode]::Open, [System.IO.FileAccess]::ReadWrite, [System.IO.FileShare]::None)
            } catch {
                $lockedPaths += $path
            } finally {
                if ($handle) {
                    $handle.Dispose()
                }
            }
        }

        if ($lockedPaths.Count -eq 0) {
            return $true
        }

        Start-Sleep -Milliseconds 250
    } while ([DateTime]::UtcNow -lt $deadline)

    Write-Warning "Timed out waiting for build output files to unlock:"
    $pendingPaths | ForEach-Object { Write-Warning "  $_" }
    return $false
}

if ($RunDetached -and -not $NonInteractive) {
    Write-Host "Launching detached build..."
    $args = @('-NoProfile','-ExecutionPolicy','Bypass','-File',"`"$PSCommandPath`"", '-NonInteractive')
    if ($BuildDir -ne (Join-Path -Path $ProjectRoot -ChildPath 'build')) { $args += ('-BuildDir', $BuildDir) }
    if ($Target -ne '') { $args += ('-Target', $Target) }
    if ($Parallel -gt 0) { $args += ('-Parallel', $Parallel) }
    if ($Crash) { $args += '-Crash' }
    if ($UseSanitizers) { $args += '-UseSanitizers' }
    $powerShellExe = Find-WindowsPowerShellExe
    Start-Process -FilePath $powerShellExe -ArgumentList $args -WorkingDirectory $ProjectRoot -WindowStyle Normal | Out-Null
    Write-Host "Detached build launched"; exit 0
}

if (-not (Test-Path $BuildDir)) {
    Write-Error "Build directory '$BuildDir' does not exist. Create it with CMake first or specify correct path."
    exit 2
}

if ($Crash) {
    if ($UseSanitizers -and $Config -ne 'Debug') {
        Write-Host "-UseSanitizers specified: switching build configuration to Debug"
        $Config = 'Debug'
    }

    $cacheFile = Join-Path $BuildDir 'CMakeCache.txt'
    $repairBrokenBuildGraph = $false
    $needConfigure = $true
    if (Test-Path $cacheFile) {
        $cacheText = Get-Content $cacheFile -Raw -ErrorAction SilentlyContinue
        if ($cacheText -match 'ENABLE_CRASH_HANDLER:BOOL=ON') {
            Write-Host "CMakeCache already has ENABLE_CRASH_HANDLER=ON"
            $needConfigure = $false
        }
    }

    $cacheHealth = Test-BuildTreeCacheHealth -BuildDirPath $BuildDir -DesiredConfig $Config
    if (-not $cacheHealth.Valid) {
        Write-Warning "Build cache/toolchain drift detected: $($cacheHealth.Reason)"
        Write-Warning "Forcing a CMake reconfigure and build-graph repair."
        $repairBrokenBuildGraph = $true
        $needConfigure = $true
    }

    if ((-not $needConfigure) -and (Test-IncrementalBuildNeedsReconfigure -BuildDirPath $BuildDir)) {
        Write-Warning "Incremental build tree is missing generated precompiled-header artifacts. Forcing CMake reconfigure to heal the Ninja build graph."
        $repairBrokenBuildGraph = $true
        $needConfigure = $true
    }

    if ($ForceConfigure -or $needConfigure) {
        Write-Host "Configuring project to enable crash handler (ENABLE_CRASH_HANDLER=ON) ..."

        # Auto-initialize Visual Studio dev environment if requested and needed
        if ($AutoInitVcVars -and -not (Get-Command cl -ErrorAction SilentlyContinue)) {
            $vc = Find-VcVarsAll
            if ($vc) {
                Write-Host "Found vcvarsall at: $vc; attempting to initialize environment..."
                $ok = Initialize-VcVars -VcVarsPath $vc -Arch $Arch
                if (-not $ok) { Write-Warning "vcvarsall initialization did not expose 'cl'. You may need to run this script inside a Developer Command Prompt or install Visual Studio Build Tools." }
            } else {
                Write-Host "vcvarsall not found on system; cannot auto-initialize MSVC environment." -ForegroundColor Yellow
            }
        }
        $cmakeBase = @('-S', "`"$ProjectRoot`"", '-B', "`"$BuildDir`"", '-DENABLE_CRASH_HANDLER=ON')
        if ($UseSanitizers) {
            Write-Host "Enabling sanitizers (ASAN/UBSAN) for debug build"
            $cmakeBase += '-DENABLE_ADDRESS_SANITIZER=ON'
            $cmakeBase += '-DENABLE_UNDEFINED_BEHAVIOR_SANITIZER=ON'
        }
        if ($KeepPdb) { Write-Host "Will keep PDBs for post-mortem (KeepPdb = $KeepPdb)"; $cmakeBase += '-DGENERATE_PDB_FOR_RELEASE=ON' }
        # Prefer local tinyxml2 to avoid requiring git during configure
        if (Test-Path (Join-Path $ProjectRoot 'lib\tinyxml2')) { Write-Host "Found local tinyxml2 -> using -DUSE_LOCAL_TINYXML2=ON"; $cmakeBase += '-DUSE_LOCAL_TINYXML2=ON' }
        # Do not add -A here; -A (platform) is only valid for Visual Studio generators and will be added when appropriate for the generator chosen.

        # If cache exists and ForceConfigure not requested, reuse it and warn if mismatch
        $cacheFile = Join-Path $BuildDir 'CMakeCache.txt'
        $configured = $false
        if ((Test-Path $cacheFile) -and -not $ForceConfigure -and -not $repairBrokenBuildGraph) {
            Write-Host "CMake cache exists; reusing existing configuration (incremental mode)"
            # If cache lacks GENERATE_PDB_FOR_RELEASE and KeepPdb requested, prompt user to force reconfigure
            try { $cacheText = Get-Content $cacheFile -Raw -ErrorAction SilentlyContinue } catch { $cacheText = '' }
            if ($KeepPdb -and ($cacheText -notmatch 'GENERATE_PDB_FOR_RELEASE:BOOL=ON')) {
                Write-Warning "Cache does not request PDB generation. Re-run with -ForceConfigure to enable PDB generation for crash debugging."
            }
            $configured = $true
        }

        if (-not $configured) {
            if ($repairBrokenBuildGraph) {
                Write-Host "Healing broken Ninja/CMake build graph before reconfigure..." -ForegroundColor Yellow
                $repairPaths = @(
                    (Join-Path $BuildDir 'CMakeCache.txt'),
                    (Join-Path $BuildDir 'CMakeFiles'),
                    (Join-Path $BuildDir 'build.ninja'),
                    (Join-Path $BuildDir '.ninja_deps'),
                    (Join-Path $BuildDir '.ninja_log')
                )
                foreach ($repairPath in $repairPaths) {
                    if (Test-Path $repairPath) {
                        try {
                            Remove-Item -Path $repairPath -Recurse -Force -ErrorAction Stop
                        } catch {
                            Write-Warning "Failed to remove stale build graph path '$repairPath': $_"
                        }
                    }
                }
            }

            # Attempt adaptive generator configure using candidate list (do not forcefully delete cache unless we override)
            $cmakeExe = Find-CMakeExe
            $ninjaExe = Find-NinjaExe
            $candidates = Choose-Generator $PreferGenerator
            $selectedGen = ''
            foreach ($g in $candidates) {
                if ($g -like '*Ninja*' -and -not $ninjaExe) {
                    Write-Warning "Generator $g requested, but ninja.exe was not found. Trying next candidate..."
                    continue
                }

                $cmakeArgs = @($cmakeBase) + @('-G', $g)
                if ($g -like '*Ninja*') {
                    $cmakeArgs += "-DCMAKE_MAKE_PROGRAM=$ninjaExe"
                }
                if ($g -notlike '*Visual Studio*') {
                    $cmakeArgs += "-DCMAKE_BUILD_TYPE=$Config"
                }

                $cmd = ('"{0}" ' -f $cmakeExe) + (($cmakeArgs | ForEach-Object {
                    if ($_ -match '\s') { '"' + $_ + '"' } else { $_ }
                }) -join ' ')
                Write-Host "Attempting configure with generator: $g"
                Write-Host "Running: $cmd"
                & $cmakeExe @cmakeArgs
                if ($LASTEXITCODE -eq 0) { $selectedGen = $g; Write-Host "Configured successfully with generator: $g"; break }
                Write-Warning "Generator $g failed (exit $LASTEXITCODE), trying next candidate..."
                # Clean partial cache/CMakeFiles before next attempt
                if (Test-Path $cacheFile) { Remove-Item -Path $cacheFile -Force -ErrorAction SilentlyContinue }
                $cmakeFilesDir = Join-Path $BuildDir 'CMakeFiles'
                if (Test-Path $cmakeFilesDir) { Remove-Item -Path $cmakeFilesDir -Recurse -Force -ErrorAction SilentlyContinue }
            }
            if ($selectedGen -eq '') { 
                Write-Error "All generator candidates failed to configure. Common causes: missing compiler toolchain (cl/nmake) or missing build tools."
                Write-Error "Suggested actions:"
                Write-Error "  - Open a Visual Studio Developer Command Prompt (so 'cl'/'nmake' are available) and re-run with -ForceConfigure."
                Write-Error "  - Install Ninja and ensure it's on PATH, then re-run with -ForceConfigure to switch to Ninja."
                Write-Error "  - Or run with -ForceConfigure in an environment where compilers are available."
                exit 4
            }
        }
    }
}

    $cachedGen = ''
    Push-Location $BuildDir
    try {
        # If AutoKill is requested, stop build outputs that may still be holding PCH or target binaries open.
        if ($AutoKill) {
            Stop-BuildOutputProcessesForBuild -BuildDirPath $BuildDir -PrimaryProcessName 'BigCat' -NonInteractiveMode:$NonInteractive | Out-Null
        }

        # Use adaptive build invocation based on cached or detected generator
    $cacheFile = Join-Path $BuildDir 'CMakeCache.txt'
    if (Test-Path $cacheFile) {
        try { $cacheLines = Get-Content $cacheFile -ErrorAction SilentlyContinue; $gline = $cacheLines | Where-Object { $_ -match '^CMAKE_GENERATOR:INTERNAL=' } | Select-Object -First 1; if ($gline) { $cachedGen = $gline -replace '^CMAKE_GENERATOR:INTERNAL=', '' } } catch { }
    }

    if ($cachedGen -ne '') { Write-Host "Detected cached generator: $cachedGen" -ForegroundColor Cyan }

    # Detect generator by files if cache not present
    if (-not $cachedGen) {
        if (Test-Path (Join-Path $BuildDir 'build.ninja')) { $cachedGen = 'Ninja' }
        elseif (Get-ChildItem -Path $BuildDir -Filter '*.sln' -ErrorAction SilentlyContinue) { $cachedGen = 'Visual Studio' }
        elseif (Test-Path (Join-Path $BuildDir 'Makefile')) { $cachedGen = 'Unix Makefiles' }
        else { Write-Host "Generator unknown; using generic cmake --build invocation" -ForegroundColor Yellow }
    }

    # Prepare and run build command depending on generator
    if ($cachedGen -like '*Ninja*' -or $cachedGen -eq 'Ninja') {
        $cmakeExe = Find-CMakeExe
        $buildArgs = @('--build', '.', '--config', $Config, '--parallel', $Parallel)
        if ($Target -ne '') { $buildArgs += @('--target', $Target) }
        $cmd = ('"{0}" ' -f $cmakeExe) + (($buildArgs | ForEach-Object {
            if ($_ -match '\s') { '"' + $_ + '"' } else { $_ }
        }) -join ' ')
    } elseif ($cachedGen -like '*Visual Studio*' -or $cachedGen -eq 'Visual Studio') {
        $cmakeExe = Find-CMakeExe
        $buildArgs = @('--build', '.', '--config', $Config, '--parallel', $Parallel)
        if ($Target -ne '') { $buildArgs += @('--target', $Target) }
        $cmd = ('"{0}" ' -f $cmakeExe) + (($buildArgs | ForEach-Object {
            if ($_ -match '\s') { '"' + $_ + '"' } else { $_ }
        }) -join ' ')
    } elseif ($cachedGen -like '*NMake*') {
        # Prefer nmake if available; otherwise attempt to switch to Ninja if present (adaptive behaviour)
        if (Get-Command -Name nmake -ErrorAction SilentlyContinue) {
            if ($Target -ne '') { $cmd = "nmake $Target" } else { $cmd = "nmake" }
        } else {
            $cmakeExe = Find-CMakeExe
            if (Test-CMakeGenerator $cmakeExe 'Ninja') {
                $doSwitch = $false
                if ($NonInteractive) { $doSwitch = $true } else {
                    $ans = Read-Host "nmake not found; Ninja generator available. Reconfigure build dir to use Ninja? (Y/n) [Y]"
                    if ([string]::IsNullOrEmpty($ans) -or $ans.ToLower().StartsWith('y')) { $doSwitch = $true }
                }
                if ($doSwitch) {
                    Write-Host "Switching generator to Ninja and reconfiguring build dir..."
                    # Remove cache and CMakeFiles, then reconfigure with Ninja and current crash settings
                    if (Test-Path $cacheFile) { Remove-Item -Path $cacheFile -Force -ErrorAction SilentlyContinue }
                    $cmakeFilesDir = Join-Path $BuildDir 'CMakeFiles'
                    if (Test-Path $cmakeFilesDir) { Remove-Item -Path $cmakeFilesDir -Recurse -Force -ErrorAction SilentlyContinue }

                    $ninjaExe = Find-NinjaExe
                    if (-not $ninjaExe) {
                        Write-Error "Ninja generator selected but ninja.exe was not found on this machine."
                        exit 1
                    }

                    $reconfArgs = @('-S', $ProjectRoot, '-B', $BuildDir, '-G', 'Ninja', '-DENABLE_CRASH_HANDLER=ON', "-DCMAKE_MAKE_PROGRAM=$ninjaExe", "-DCMAKE_BUILD_TYPE=$Config")
                    if ($UseSanitizers) { $reconfArgs += '-DENABLE_ADDRESS_SANITIZER=ON'; $reconfArgs += '-DENABLE_UNDEFINED_BEHAVIOR_SANITIZER=ON' }
                    if ($KeepPdb) { $reconfArgs += '-DGENERATE_PDB_FOR_RELEASE=ON' }
                    if ($Arch -ne '' -and $reconfArgs -match ' -G "?Visual Studio') { $reconfArgs += ('-A', $Arch) }

                    $cmakeExe = Find-CMakeExe
                    $cmdReconf = ('"{0}" ' -f $cmakeExe) + (($reconfArgs | ForEach-Object {
                        if ($_ -match '\s') { '"' + $_ + '"' } else { $_ }
                    }) -join ' ')
                    Write-Host "Reconfigure: $cmdReconf"
                    & $cmakeExe @reconfArgs
                    if ($LASTEXITCODE -ne 0) { Write-Error "Reconfigure to Ninja failed (exit $LASTEXITCODE). Install nmake or run with -ForceConfigure to retry."; exit $LASTEXITCODE }
                    $cachedGen = 'Ninja'
                    $buildArgs = @('--build', '.', '--config', $Config, '--parallel', $Parallel)
                    if ($Target -ne '') { $buildArgs += @('--target', $Target) }
                    $cmd = ('"{0}" ' -f $cmakeExe) + (($buildArgs | ForEach-Object {
                        if ($_ -match '\s') { '"' + $_ + '"' } else { $_ }
                    }) -join ' ')
                } else {
                    Write-Error "nmake not found and user declined to switch generator. Install nmake or re-run with -ForceConfigure to reconfigure."; exit 1
                }
            } else {
                Write-Error "nmake not found and Ninja generator not available. Install Visual Studio Build Tools or regenerate with a different generator."; exit 1
            }
        }
    } else {
        # Fallback: ask cmake to handle parallelism
        $cmakeExe = Find-CMakeExe
        $buildArgs = @('--build', '.', '--config', $Config, '--parallel', $Parallel)
        if ($Target -ne '') { $buildArgs += @('--target', $Target) }
        $cmd = ('"{0}" ' -f $cmakeExe) + (($buildArgs | ForEach-Object {
            if ($_ -match '\s') { '"' + $_ + '"' } else { $_ }
        }) -join ' ')
    }

    Write-Host "Build command: $cmd" -ForegroundColor Cyan
    Write-Host "Keeping PDBs: $KeepPdb (no stripping during incremental build)" -ForegroundColor Green

    # Execute build
    Write-Host "Executing: $cmd"
    if ($cachedGen -like '*NMake*' -and (Get-Command -Name nmake -ErrorAction SilentlyContinue)) {
        if ($Target -ne '') { & nmake $Target } else { & nmake }
    } else {
        & $cmakeExe @buildArgs
    }
    $exitCode = $LASTEXITCODE
    if ($exitCode -ne 0 -and $AutoKill) {
        Write-Warning "Build failed with exit code $exitCode. Checking for late build-output/CTest locks before one retry..."
        Stop-BuildOutputProcessesForBuild -BuildDirPath $BuildDir -PrimaryProcessName 'BigCat' -NonInteractiveMode:$NonInteractive | Out-Null
        Write-Host "Retrying build once after AutoKill cleanup..." -ForegroundColor Cyan
        if ($cachedGen -like '*NMake*' -and (Get-Command -Name nmake -ErrorAction SilentlyContinue)) {
            if ($Target -ne '') { & nmake $Target } else { & nmake }
        } else {
            & $cmakeExe @buildArgs
        }
        $exitCode = $LASTEXITCODE
    }
    if ($exitCode -ne 0) { Write-Error "Build failed with exit code $exitCode"; exit $exitCode } else { Write-Host "Build completed successfully." -ForegroundColor Green }
    } finally {
        Pop-Location
    }

# Build summary
$toolchain = 'Unknown'
if ($cachedGen -like '*Ninja*' -or $cachedGen -eq 'Ninja') { $toolchain = 'Ninja' }
elseif ($cachedGen -like '*Visual Studio*' -or $cachedGen -eq 'Visual Studio') { $toolchain = 'MSBuild (Visual Studio)' }
elseif ($cachedGen -like '*NMake*') { $toolchain = 'NMake' }
Write-Host "Build summary: Generator: $cachedGen    Toolchain: $toolchain    Configuration: $Config    Parallel: $Parallel"
Write-Host "Crash-mode: $Crash    KeepPdb: $KeepPdb    UseSanitizers: $UseSanitizers"

if ($PruneIntermediates) {
    Invoke-BuildArtifactCleanup -BuildDir $BuildDir -Config $Config -PreservePdb:$KeepPdb -PreserveCrashDumps:$true | Out-Null
}

# Return success
exit 0
