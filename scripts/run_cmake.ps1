param(
    [ValidateSet('configure','build','workflow')]
    [string]$Action = 'build',
    [string]$Preset = 'default',
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$ExtraArgs
)

$ErrorActionPreference = "Stop"

function Find-CMakeExe {
    $cmd = Get-Command cmake -ErrorAction SilentlyContinue
    if ($cmd -and $cmd.Source) {
        return $cmd.Source
    }

    $candidates = @(
        'C:\Program Files\CMake\bin\cmake.exe',
        'C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe',
        'C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe',
        'C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe',
        'C:\Program Files\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
    )

    foreach ($candidate in $candidates) {
        if (Test-Path $candidate) {
            return $candidate
        }
    }

    return 'cmake'
}

function Invoke-LoggedCMake {
    param(
        [Parameter(Mandatory = $true)][string]$FilePath,
        [Parameter(Mandatory = $true)][string[]]$ArgumentList
    )

    Write-Host ("[cmake] > {0} {1}" -f $FilePath, ($ArgumentList -join ' '))
    & $FilePath @ArgumentList 2>&1 | ForEach-Object {
        if ($_ -is [System.Management.Automation.ErrorRecord]) {
            Write-Host $_.ToString()
        } else {
            Write-Host ([string]$_)
        }
    }
    return $LASTEXITCODE
}

$cmakeExe = Find-CMakeExe

switch ($Action) {
    'configure' { $cmakeArgs = @('--preset', $Preset) }
    'build' { $cmakeArgs = @('--build', '--preset', $Preset) }
    'workflow' { $cmakeArgs = @('--workflow', '--preset', $Preset) }
}

if ($ExtraArgs) {
    $cmakeArgs += $ExtraArgs
}

$exitCode = Invoke-LoggedCMake -FilePath $cmakeExe -ArgumentList $cmakeArgs
exit $exitCode
