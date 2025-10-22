<#
cleanup_project.ps1

用途: 將專案中常見的暫存或重複檔案移至 archive/cleanup_<timestamp>/ 以便整理專案

使用方法:
  在 PowerShell 中於專案根目錄執行:
    .\scripts\cleanup_project.ps1

會執行下列動作:
- 建立 archive/cleanup_<timestamp>/ 目錄
- 將 .snapshots/ (若存在)、COMPILATION_SUCCESS_REPORT.md、BUILD_STATUS.md、TEST_REPORT.md
  及指定的範例/臨時檔案移到該備份目錄
#>

param(
    [switch]$WhatIf
)

Set-StrictMode -Version Latest

$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).ProviderPath
$timestamp = Get-Date -Format "yyyyMMdd_HHmmss"
$dest = Join-Path $root "archive\cleanup_$timestamp"

if (-not (Test-Path $dest)) {
    if ($WhatIf) { Write-Host "[WhatIf] New-Item -ItemType Directory -Path $dest"; } else { New-Item -ItemType Directory -Path $dest | Out-Null }
}

$itemsToMove = @(
    '.snapshots',
    'COMPILATION_SUCCESS_REPORT.md',
    'BUILD_STATUS.md',
    'TEST_REPORT.md',
    'src\EtherCATExample_Advanced.cpp',
    'lib\npcap-sdk\Examples-remote'
)

foreach ($rel in $itemsToMove) {
    $src = Join-Path $root $rel
    if (Test-Path $src) {
        $target = Join-Path $dest (Split-Path $rel -Leaf)
        if ($WhatIf) {
            Write-Host "[WhatIf] Move-Item -Path '$src' -Destination '$target'"
        } else {
            try {
                Move-Item -Path $src -Destination $target -Force -ErrorAction Stop
                Write-Host "Moved: $rel -> archive/cleanup_$timestamp/"
            } catch {
                Write-Warning ("Failed to move {0}: {1}" -f $rel, $_)
            }
        }
    }
}

Write-Host "Cleanup complete. Backup location: $dest"
