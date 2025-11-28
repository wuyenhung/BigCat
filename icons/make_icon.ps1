param(
    [string]$Source = "ah64.png",
    [string]$Out = "ah64.ico"
)

Write-Host "Running make_icon.py -> Source: $Source  Out: $Out" -ForegroundColor Cyan

# Find python in PATH and run it directly (avoids calling `powershell` from within PowerShell)
$pythonCmd = Get-Command python -ErrorAction SilentlyContinue
if (-not $pythonCmd) {
    Write-Host "Python not found in PATH. Please install Python or add it to PATH." -ForegroundColor Red
    exit 1
}

# Use the script folder so it works when executed from other working directories
$scriptPath = Join-Path $PSScriptRoot 'make_icon.py'
& $pythonCmd.Path -u $scriptPath $Source $Out

if ($LASTEXITCODE -ne 0) {
    Write-Host "make_icon.py failed with exit code $LASTEXITCODE" -ForegroundColor Red
    exit $LASTEXITCODE
}

Write-Host "Icon created: $Out" -ForegroundColor Green
