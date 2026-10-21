# ============================================================================
# 立洋科技  簡易 EtherCAT 寸動測試程式
# Maintainer: wuyenhung@gmail.com  巫彥鴻
# Usage: .\test_Ethercat_jog.ps1 [adapter_name] [-Core <N>]
#   -Core <N> : 綁定 SOEM 執行序至邏輯 CPU N (0-based，與工作管理員編號一致)
#
# 操作說明
# 1. 選網卡（自動偵測，或命令列傳入）
# 2. 選軸 (1-6)，Q 結束程式
# 3. 選模式：C=CSP(位置) V=CSV(速度) B=回選軸
# 4. 方向鍵寸動：上 正轉  下 反轉（按住時顯示 PDO 即時狀態）
# 5. E=設定正向扭力  R=設定反向扭力
# 6. +/- 調整步進/速度  S=顯示當下狀態
# 7. Q/Esc 退出寸動，可換軸繼續
#
# SOEM 核心綁定說明
# - 將 SOEM 即時執行序固定在某個邏輯 CPU 上，可降低 PDO 抖動
# - 輸入數字與工作管理員 CPU 編號一致 (0-based)
# - 建議選擇閒置、非系統主要使用的核心（例如核心 4）
# - 輸入數字後按 Enter 確認，直接按 Enter 則跳過（不綁定）
# - 也可在命令列直接指定：.\test_Ethercat_jog.ps1 -Core 4
# - 直接固定到指定的邏輯 CPU，不會重新對應到同核的其他邏輯處理器
#
# 注意：勿縮放/點擊終端機視窗，否則 PDO 會中斷。
# ============================================================================

$ErrorActionPreference = "Stop"
Set-Location $PSScriptRoot\..\build\Release\bin\main

Write-Host "==============================================" -ForegroundColor Cyan
Write-Host "  立洋科技 - 簡易 EtherCAT 寸動測試程式" -ForegroundColor Yellow
Write-Host "  Maintainer: wuyenhung@gmail.com  巫彥鴻" -ForegroundColor Gray
Write-Host "==============================================" -ForegroundColor Cyan
Write-Host ""

# ── 解析命令列是否已傳入 -Core ──
$coreArg = $null
$passThruArgs = @()
for ($i = 0; $i -lt $args.Count; $i++) {
    if ($args[$i] -eq '-core' -and $i + 1 -lt $args.Count) {
        $coreArg = $args[$i + 1]
        $i++
    } else {
        $passThruArgs += $args[$i]
    }
}

# ── 若未從命令列指定，互動詢問核心編號 ──
if ($null -eq $coreArg) {
    Write-Host "── SOEM 核心綁定（降低 PDO 抖動）──" -ForegroundColor Magenta
    Write-Host ""

    # 顯示 CPU 拓撲（由 EXE 內建 -list-cores 取得，與工作管理員編號一致）
    Write-Host "  CPU 拓撲（工作管理員編號）：" -ForegroundColor Gray
    $topoResult = & .\test_Ethercat_jog.exe -list-cores 2>&1
    if ($LASTEXITCODE -eq 0 -and $topoResult) {
        foreach ($line in $topoResult) {
            if ($line -match '^Core\s+\d+|^Total:|^SMT|^Use -core') {
                Write-Host "  $line" -ForegroundColor White
            } elseif ($line -match '^---') {
                Write-Host "  $line" -ForegroundColor DarkGray
            } else {
                Write-Host "  $line" -ForegroundColor Gray
            }
        }
    } else {
        # 回退：顯示基本 CPU 資訊
        try {
            $cpuInfo = Get-CimInstance -ClassName Win32_Processor | Select-Object -First 1
            $cores   = $cpuInfo.NumberOfCores
            $logical = $cpuInfo.NumberOfLogicalProcessors
            $name    = $cpuInfo.Name
            Write-Host "  CPU: $name" -ForegroundColor Gray
            Write-Host "  實體核心: $cores  邏輯處理器: $logical" -ForegroundColor Gray
            if ($logical -gt $cores) {
                Write-Host "  (偵測到 HT/SMT，每個實體核心有多個邏輯處理器)" -ForegroundColor Gray
            }
        } catch {
            Write-Host "  (無法查詢 CPU 資訊)" -ForegroundColor Gray
        }
    }
    Write-Host ""

    Write-Host "  輸入邏輯 CPU 編號 (對照上表 LP 編號，與工作管理員一致)" -ForegroundColor White
    Write-Host "  直接按 Enter (不輸入數字) → 跳過，不綁定核心" -ForegroundColor White
    Write-Host ""
    $coreInput = Read-Host "  核心編號"

    if ($coreInput -match '^\d+$') {
        $coreArg = $coreInput
        Write-Host "  已選擇邏輯 CPU: $coreArg" -ForegroundColor Green
    } else {
        Write-Host "  略過核心綁定" -ForegroundColor Yellow
    }
    Write-Host ""
}

# ── 組裝最終引數 ──
$finalArgs = @()
if ($null -ne $coreArg) {
    $finalArgs += '-core'
    $finalArgs += $coreArg
}
$finalArgs += $passThruArgs

# Discard any stray keystrokes buffered from previous console interaction
try { while ([Console]::KeyAvailable) { $null = [Console]::ReadKey($true) } } catch { }

Write-Host "啟動 EtherCAT Jog 測試..." -ForegroundColor Cyan
& .\test_Ethercat_jog.exe @finalArgs

Write-Host ""
Write-Host "Exit code: $LASTEXITCODE" -ForegroundColor $(if ($LASTEXITCODE -eq 0) { "Green" } else { "Red" })
Write-Host "Log: $PWD\test_Ethercat_jog.log" -ForegroundColor Gray

if ($LASTEXITCODE -eq 0) {
    Write-Host "*** ETHERNET JOG TEST PASSED ***" -ForegroundColor Green
} else {
    Write-Host "*** ETHERNET JOG TEST FAILED ***" -ForegroundColor Red
}