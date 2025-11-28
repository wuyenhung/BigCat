param(
    [string]$BuildDir = (Join-Path $PSScriptRoot "..\build"),
    [string]$Config = "Release",
    [string]$Regex,
    [string]$Label,
    [switch]$SkipBuild,
    [switch]$ForceConfigure,
    [int]$TimeoutSeconds = 300,
    [int]$Jobs = 4
)

$ErrorActionPreference = "Stop"

function Find-CMakeTool {
    param([Parameter(Mandatory = $true)][ValidateSet('cmake','ctest')][string]$ToolName)

    $cmd = Get-Command $ToolName -ErrorAction SilentlyContinue
    if ($cmd -and $cmd.Source) {
        return $cmd.Source
    }

    $candidates = @()
    if ($ToolName -eq 'cmake') {
        $candidates += 'C:\Program Files\CMake\bin\cmake.exe'
    } else {
        $candidates += 'C:\Program Files\CMake\bin\ctest.exe'
    }

    $vsRoots = @(
        'C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin',
        'C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin',
        'C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin',
        'C:\Program Files\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin'
    )
    foreach ($root in $vsRoots) {
        $candidates += (Join-Path $root ($ToolName + '.exe'))
    }

    foreach ($candidate in $candidates) {
        if (Test-Path $candidate) {
            return $candidate
        }
    }

    return $ToolName
}

function Invoke-LoggedProcess {
    param(
        [Parameter(Mandatory = $true)][string]$FilePath,
        [Parameter(Mandatory = $true)][string[]]$ArgumentList
    )

    Write-Host ("[ctest] > {0} {1}" -f $FilePath, ($ArgumentList -join ' '))
    & $FilePath @ArgumentList 2>&1 | ForEach-Object {
        if ($_ -is [System.Management.Automation.ErrorRecord]) {
            Write-Host $_.ToString()
        } else {
            Write-Host ([string]$_)
        }
    }
    $exitCode = $LASTEXITCODE
    return $exitCode
}

$projectRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$resolvedBuildDir = [System.IO.Path]::GetFullPath($BuildDir)
$cmakeExe = Find-CMakeTool -ToolName 'cmake'
$ctestExe = Find-CMakeTool -ToolName 'ctest'

$needsConfigure = $ForceConfigure -or -not (Test-Path (Join-Path $resolvedBuildDir 'CTestTestfile.cmake'))
if ($needsConfigure) {
    $configureArgs = @('--preset', 'default')
    Write-Host "[ctest] Configuring from $projectRoot"
    Push-Location $projectRoot
    try {
        $exitCode = Invoke-LoggedProcess -FilePath $cmakeExe -ArgumentList $configureArgs
    } finally {
        Pop-Location
    }
    if ($exitCode -ne 0) {
        exit $exitCode
    }
}

if (-not (Test-Path $resolvedBuildDir)) {
    Write-Error "Build directory not found: $resolvedBuildDir"
    exit 2
}

if (-not $SkipBuild) {
    $buildArgs = @('--build', $resolvedBuildDir, '--config', $Config, '--parallel')
    Write-Host "[ctest] Building before running tests"
    $exitCode = Invoke-LoggedProcess -FilePath $cmakeExe -ArgumentList $buildArgs
    if ($exitCode -ne 0) {
        exit $exitCode
    }
}

$ctestArgs = @(
    '--test-dir', $resolvedBuildDir,
    '--output-on-failure',
    '--timeout', $TimeoutSeconds.ToString(),
    '--parallel', $Jobs.ToString()
)

if ($Config) {
    $ctestArgs += @('-C', $Config)
}
if ($Label) {
    $ctestArgs += @('-L', $Label)
}
if ($Regex) {
    $ctestArgs += @('-R', $Regex)
}

Write-Host "[ctest] Running tests in $resolvedBuildDir"
$exitCode = Invoke-LoggedProcess -FilePath $ctestExe -ArgumentList $ctestArgs
exit $exitCode
