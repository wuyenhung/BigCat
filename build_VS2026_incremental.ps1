param(
    [string]$BuildDir = (Join-Path -Path $PSScriptRoot -ChildPath 'build'),
    [ValidateSet('Debug','Release')]
    [string]$Config = 'Release',
    [int]$Parallel = [Environment]::ProcessorCount,
    [string]$Target = '',
    [bool]$AutoKill = $true
)

Write-Host "Incremental build script"
Write-Host "BuildDir: $BuildDir"
Write-Host "Config: $Config"
Write-Host "Parallel: $Parallel"
if ($Target -ne '') { Write-Host "Target: $Target" }

if (-not (Test-Path $BuildDir)) {
    Write-Error "Build directory '$BuildDir' does not exist. Create it with CMake first or specify correct path."
    exit 2
}

    Push-Location $BuildDir
    try {
        # If AutoKill is requested, attempt to find and stop the running executable that would lock the output
        if ($AutoKill) {
            $exeName = 'BigCat'
            Write-Host "AutoKill enabled: checking for running process $exeName..."
            $proc = Get-Process -Name $exeName -ErrorAction SilentlyContinue
            if ($proc) {
                Write-Host "Found running process(es):" ($proc | Select-Object -ExpandProperty Id) -ForegroundColor Yellow
                try {
                    $proc | Stop-Process -Force -ErrorAction Stop
                    Write-Host "Stopped process(es)." -ForegroundColor Green
                    Start-Sleep -Milliseconds 500
                } catch {
                    Write-Warning "Failed to stop process(es): $_"
                    Write-Warning "You may need to stop them manually or re-run with -AutoKill:$false"
                    exit 3
                }
            } else {
                Write-Host "No running $exeName process found." -ForegroundColor Gray
            }
        }

        # ...existing build execution code...
    # Use cmake --build for cross-generator compatibility. 
    # Detect generator type to pass correct parallel build arguments.
    $buildCmd = "cmake --build . --config $Config"
    if ($Target -ne '') { $buildCmd += " --target $Target" }

    # Check if using Ninja or MSBuild generator by looking at build files
    $isNinja = Test-Path (Join-Path $BuildDir "build.ninja")
    $isMSBuild = Test-Path (Join-Path $BuildDir "*.sln")
    
    if ($isNinja) {
        # Ninja uses -j for parallel jobs
        $parallelArgs = "-- -j $Parallel"
        Write-Host "Detected Ninja generator" -ForegroundColor Cyan
    } elseif ($isMSBuild) {
        # MSBuild uses /m for parallel jobs
        $parallelArgs = "-- /m:$Parallel /p:BuildInParallel=true"
        Write-Host "Detected MSBuild generator" -ForegroundColor Cyan
    } else {
        # Unknown generator - use cmake's generic --parallel flag
        $parallelArgs = "--parallel $Parallel"
        Write-Host "Using generic parallel flag" -ForegroundColor Yellow
    }
    
    $fullCmd = "$buildCmd $parallelArgs"
    Write-Host "Running: $fullCmd"

    # Execute the build command in the current PowerShell process and examine $LASTEXITCODE
    Write-Host "Executing build command in current shell..."
    Invoke-Expression $fullCmd
    $exitCode = $LASTEXITCODE
    if ($exitCode -ne 0) {
        Write-Error "Build failed with exit code $exitCode"
        exit $exitCode
    } else {
        Write-Host "Build completed successfully." -ForegroundColor Green
    }
    } finally {
        Pop-Location
    }
