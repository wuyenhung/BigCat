# Download three.min.js (r128) into the current repo's js/ folder
# Usage: Run from PowerShell in this repo (resources/web) or from project root with correct path
# Example:
# Set-Location -Path 'D:\Yenhung\WORK\6Dof\PROG\WuSOEM_C++\resources\web\js'
# .\download_three.ps1

$uri = 'https://cdnjs.cloudflare.com/ajax/libs/three.js/r128/three.min.js'
$out = Join-Path -Path (Get-Location) -ChildPath 'three.min.js'
Write-Host "Downloading $uri to $out ..."
try {
    Invoke-WebRequest -Uri $uri -OutFile $out -UseBasicParsing -ErrorAction Stop
    Write-Host "Downloaded three.min.js successfully."
} catch {
    Write-Error "Failed to download three.min.js: $_"
}
