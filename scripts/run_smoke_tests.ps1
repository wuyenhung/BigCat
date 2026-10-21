param(
    [string]$BuildDir = (Join-Path $PSScriptRoot "..\build"),
    [string]$Config = "Release",
    [switch]$SkipBuild,
    [string]$LogFile,
    [switch]$DeleteLogOnSuccess,
    [switch]$VerboseTerminal,
    [ValidateRange(20, 1000)][int]$TerminalTailLines = 120,
    [ValidateRange(0, 3600)][int]$CTestTimeoutSeconds = 240
)

$ErrorActionPreference = "Stop"

$script:SmokeLogFile = $null
$script:SmokeLogWriter = $null
$script:CleanupLogOnSuccess = $false
$script:VerboseTerminalOutput = $VerboseTerminal.IsPresent
$script:TerminalTailLines = $TerminalTailLines

function Initialize-SmokeLog {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [switch]$AllowFallbackName
    )

    $fullPath = [System.IO.Path]::GetFullPath($Path)
    $parent = Split-Path -Parent $fullPath
    if ($parent -and -not (Test-Path $parent)) {
        New-Item -ItemType Directory -Path $parent -Force | Out-Null
    }

    $encoding = New-Object System.Text.UTF8Encoding($false)
    try {
        $script:SmokeLogWriter = New-Object System.IO.StreamWriter($fullPath, $false, $encoding)
    } catch {
        if (-not $AllowFallbackName) {
            throw
        }

        $fallbackName = "{0}_{1:yyyyMMdd_HHmmss}_{2}{3}" -f [System.IO.Path]::GetFileNameWithoutExtension($fullPath), (Get-Date), $PID, [System.IO.Path]::GetExtension($fullPath)
        $fallbackPath = Join-Path $parent $fallbackName
        Write-Warning "[smoke] Log file '$fullPath' is busy; using fallback log '$fallbackPath'."
        $script:SmokeLogWriter = New-Object System.IO.StreamWriter($fallbackPath, $false, $encoding)
        $fullPath = $fallbackPath
    }

    $script:SmokeLogWriter.AutoFlush = $true
    $script:SmokeLogFile = $fullPath
}

function Write-SmokeLine {
    param(
        [AllowNull()][string]$Message,
        [switch]$LogOnly
    )

    if ($null -eq $Message) {
        $Message = ""
    }

    if (-not $LogOnly) {
        Write-Host $Message
    }
    if ($script:SmokeLogWriter) {
        $script:SmokeLogWriter.WriteLine($Message)
    }
}

function Write-SmokeTerminalLine {
    param([AllowNull()][string]$Message)

    if ($null -eq $Message) {
        $Message = ""
    }

    Write-Host $Message
}

function Write-SmokeWarning {
    param([Parameter(Mandatory = $true)][string]$Message)

    Write-Warning $Message
    if ($script:SmokeLogWriter) {
        $script:SmokeLogWriter.WriteLine("WARNING: $Message")
    }
}

function Complete-Smoke {
    param([int]$ExitCode)

    Write-SmokeLine "[smoke] Exit code: $ExitCode"

    if ($script:SmokeLogWriter) {
        $script:SmokeLogWriter.Dispose()
        $script:SmokeLogWriter = $null
    }

    if ($script:SmokeLogFile) {
        $retainedLogFile = $script:SmokeLogFile
        $script:SmokeLogFile = $null

        if ($script:CleanupLogOnSuccess -and $ExitCode -eq 0) {
            try {
                Remove-Item -LiteralPath $retainedLogFile -Force -ErrorAction Stop
                Write-Host "[smoke] Removed successful run log: $retainedLogFile"
            } catch {
                Write-Warning "[smoke] Failed to remove successful run log '$retainedLogFile': $($_.Exception.Message)"
            }
        } else {
            Write-Host "[smoke] Full log: $retainedLogFile"
        }
    }

    exit $ExitCode
}

function Test-SmokeTerminalLine {
    param(
        [AllowNull()][string]$Line,
        [ValidateSet('Build','CTestDiscover','CTestRun','Other')][string]$Phase = 'Other'
    )

    if ($script:VerboseTerminalOutput) {
        return $true
    }

    if ([string]::IsNullOrWhiteSpace($Line)) {
        return $false
    }

    if ($Line -match '(?i)(error|fatal|failed|failure|timeout|unknown target|not found|cannot|could not|exception|warning)') {
        return $true
    }

    switch ($Phase) {
        'Build' {
            return ($Line -match '^\s*\[\d+/\d+\]' -or
                    $Line -match '^ninja:' -or
                    $Line -match '^\s*--\s+' -or
                    $Line -match '^\s*(Build succeeded|Build FAILED|Generating|Building)')
        }
        'CTestDiscover' {
            return ($Line -match '^(Test project|Total Tests:)' -or
                    $Line -match '^\s*Test\s+#\d+:')
        }
        'CTestRun' {
            return ($Line -match '^\s*Start\s+\d+:' -or
                    $Line -match '^\s*\d+/\d+\s+Test\s+#\d+:' -or
                    $Line -match '^\d+% tests passed' -or
                    $Line -match '^The following tests FAILED' -or
                    $Line -match '^Errors while running CTest' -or
                    $Line -match '^Total Test time')
        }
        default {
            return $true
        }
    }
}

function Write-SmokeTail {
    param(
        [AllowNull()][string[]]$Lines,
        [Parameter(Mandatory = $true)][string]$Header
    )

    if (-not $Lines -or $Lines.Count -eq 0) {
        return
    }

    $tailCount = [Math]::Min($script:TerminalTailLines, $Lines.Count)
    $startIndex = $Lines.Count - $tailCount
    Write-SmokeTerminalLine $Header
    for ($i = $startIndex; $i -lt $Lines.Count; $i++) {
        Write-SmokeTerminalLine $Lines[$i]
    }
}

function Find-CMakeTool {
    param([Parameter(Mandatory = $true)][ValidateSet('cmake','ctest')][string]$ToolName)

    $cmd = Get-Command $ToolName -ErrorAction SilentlyContinue
    if ($cmd -and $cmd.Source) {
        return $cmd.Source
    }

    $cands = @()
    if ($ToolName -eq 'cmake') {
        $cands += 'C:\Program Files\CMake\bin\cmake.exe'
    } else {
        $cands += 'C:\Program Files\CMake\bin\ctest.exe'
    }

    $vsRoots = @(
        'C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin',
        'C:\Program Files\Microsoft Visual Studio\18\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin',
        'C:\Program Files\Microsoft Visual Studio\17\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin',
        'C:\Program Files\Microsoft Visual Studio\17\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin'
    )
    foreach ($root in $vsRoots) {
        $cands += (Join-Path $root ($ToolName + '.exe'))
    }

    foreach ($candidate in $cands) {
        if (Test-Path $candidate) {
            return $candidate
        }
    }

    return $ToolName
}

function Invoke-ProcessCapture {
    param(
        [Parameter(Mandatory = $true)][string]$FilePath,
        [Parameter(Mandatory = $true)][string[]]$ArgumentList,
        [ValidateSet('Build','CTestDiscover','CTestRun','Other')][string]$Phase = 'Other'
    )

    $output = New-Object 'System.Collections.Generic.List[string]'
    $previousErrorActionPreference = $ErrorActionPreference
    try {
        $ErrorActionPreference = 'Continue'
        & $FilePath @ArgumentList 2>&1 | ForEach-Object {
            $line = if ($_ -is [System.Management.Automation.ErrorRecord]) { $_.ToString() } else { [string]$_ }
            $output.Add($line)
            $logOnly = -not (Test-SmokeTerminalLine -Line $line -Phase $Phase)
            Write-SmokeLine $line -LogOnly:$logOnly
        }
        $exitCode = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $previousErrorActionPreference
    }

    return [pscustomobject]@{
        ExitCode = $exitCode
        Output   = $output.ToArray()
    }
}

$resolvedBuildDir = [System.IO.Path]::GetFullPath($BuildDir)
if (-not (Test-Path $resolvedBuildDir)) {
    throw "Build directory not found: $resolvedBuildDir"
}

$resolvedLogFile = if ([string]::IsNullOrWhiteSpace($LogFile)) {
    Join-Path $resolvedBuildDir "smoke_test_latest.log"
} else {
    $LogFile
}

$usingDefaultLogFile = [string]::IsNullOrWhiteSpace($LogFile)
$script:CleanupLogOnSuccess = $DeleteLogOnSuccess.IsPresent

Initialize-SmokeLog -Path $resolvedLogFile -AllowFallbackName:$usingDefaultLogFile
Write-SmokeLine "[smoke] Full log: $script:SmokeLogFile"
if (-not $script:VerboseTerminalOutput) {
    Write-SmokeLine "[smoke] Terminal output is summarized; pass -VerboseTerminal for full subprocess output."
}

$cmakeExe = Find-CMakeTool -ToolName 'cmake'
$ctestExe = Find-CMakeTool -ToolName 'ctest'

$targets = @(
    "BigCat",
    "test_appcontroller_contract",
    "test_system_state_policy",
    "test_coordinate_transform",
    "test_machine_config_mmf_pose_coordinate",
    "test_machine_config_xml_io",
    "test_motion_config_defaults",
    "test_stewart_ik",
    "test_pdofilter_velocity_continuity"
)

if (-not $SkipBuild) {
    Write-SmokeLine "[smoke] Building targets in $resolvedBuildDir ($Config)..."
    $buildArgs = @("--build", $resolvedBuildDir, "--config", $Config)
    foreach ($t in $targets) { $buildArgs += @("--target", $t) }
    $buildResult = Invoke-ProcessCapture -FilePath $cmakeExe -ArgumentList $buildArgs -Phase Build
    if ($buildResult.ExitCode -ne 0) {
        $buildFailedWithMissingTarget = ($buildResult.Output | Out-String) -match "unknown target"
        if (-not $buildFailedWithMissingTarget) {
            Write-SmokeTail -Lines $buildResult.Output -Header "[smoke] Build output tail:"
            Complete-Smoke -ExitCode $buildResult.ExitCode
        }

        Write-SmokeWarning "[smoke] One or more named smoke targets are unavailable in this build tree; falling back to a full build."
        $fullBuildResult = Invoke-ProcessCapture -FilePath $cmakeExe -ArgumentList @("--build", $resolvedBuildDir, "--config", $Config) -Phase Build
        if ($fullBuildResult.ExitCode -ne 0) {
            Write-SmokeTail -Lines $fullBuildResult.Output -Header "[smoke] Full build output tail:"
            Complete-Smoke -ExitCode $fullBuildResult.ExitCode
        }
    }
}

$ctestArgs = @(
    "--test-dir", $resolvedBuildDir,
    "--output-on-failure",
    "-L", "smoke"
)

if ($Config) {
    $ctestArgs += @("-C", $Config)
}

if ($CTestTimeoutSeconds -gt 0) {
    $ctestArgs += @("--timeout", [string]$CTestTimeoutSeconds)
    Write-SmokeLine "[smoke] CTest per-test timeout: ${CTestTimeoutSeconds}s"
}

Write-SmokeLine "[smoke] Discovering CTest smoke cases..."
$listResult = Invoke-ProcessCapture -FilePath $ctestExe -ArgumentList ($ctestArgs + @("-N")) -Phase CTestDiscover
if ($listResult.ExitCode -ne 0) {
    Write-SmokeTail -Lines $listResult.Output -Header "[smoke] CTest discovery output tail:"
    Complete-Smoke -ExitCode $listResult.ExitCode
}

$smokeTestCount = $null
foreach ($line in $listResult.Output) {
    if ($line -match "Total Tests:\s+(\d+)") {
        $smokeTestCount = [int]$matches[1]
    }
}

if ($null -ne $smokeTestCount -and $smokeTestCount -eq 0) {
    Write-SmokeWarning "[smoke] No smoke tests are currently configured in this build tree; skipping CTest execution."
    Complete-Smoke -ExitCode 0
}

Write-SmokeLine "[smoke] Running CTest smoke suite..."
# Note: avoid `--progress` here. It emits TTY control codes (carriage returns)
# that get mangled when the script's output is redirected to a file/log,
# which previously made the captured log appear truncated at the start of the
# CTest run (no per-test lines and no final summary). `-V` gives line-oriented
# verbose output that captures cleanly.
$ctestRunArgs = $ctestArgs + @("-V")
$runResult = Invoke-ProcessCapture -FilePath $ctestExe -ArgumentList $ctestRunArgs -Phase CTestRun
if ($runResult.ExitCode -ne 0) {
    Write-SmokeTail -Lines $runResult.Output -Header "[smoke] CTest run output tail:"
}
Complete-Smoke -ExitCode $runResult.ExitCode