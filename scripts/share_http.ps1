<#
share_http.ps1
用途：快速在本機開放 HTTP 埠並顯示可分享的連接 URL（支援防火牆與 urlacl 設定）
使用方式（必要以系統管理員執行）：
    .\share_http.ps1 -Port 8080 -Path "/viewer_6dof.html" -UseUrlAcl
說明：
  -Port: 要分享的 TCP 埠（預設 8080）
    -Path: URL path（預設 "/"，建議設定為 web 檔案路徑，例如 "/resources/web/viewer_6dof.html"）
  -UseUrlAcl: 若設定，會呼叫 netsh http add urlacl 對 http.sys 登錄（需要 admin）
  -User: URL ACL 使用者（預設 Everyone）

注意：此腳本只處理系統層面的設定（防火牆、urlacl），伺服器本身必須能監聽該埠並綁定對應介面（INADDR_ANY 或該 IP），否則連線仍無法到達。
#>
param(
    [int]$Port = 8080,
    [string]$Path = "/",
    [switch]$UseUrlAcl,
    [string]$User = "Everyone"
)

function Ensure-RunningAsAdmin {
    $current = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = New-Object Security.Principal.WindowsPrincipal($current)
    return $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

if (-not (Ensure-RunningAsAdmin)) {
    Write-Host "需要系統管理員權限才能修改防火牆或 urlacl。嘗試以提升權限重新執行..." -ForegroundColor Yellow
    $ps = "-NoProfile -ExecutionPolicy Bypass -File `"$($MyInvocation.MyCommand.Path)`" -Port $Port -Path `"$Path`""
    if ($UseUrlAcl) { $ps += ' -UseUrlAcl' }
    $ps += " -User `"$User`""
    Start-Process -FilePath "powershell.exe" -ArgumentList $ps -Verb RunAs
    exit
}

Write-Host "以管理員身份執行。開始建立分享設定...`n"

# 取得本機 IPv4 位址（排除 127.x 與 APIPA 169.x）
[System.Net.Dns]::GetHostEntry([System.Net.Dns]::GetHostName()).AddressList |
    Where-Object { $_.AddressFamily -eq 'InterNetwork' } | ForEach-Object {
        $_.ToString()
    } | Where-Object { $_ -notlike '127.*' -and $_ -notlike '169.*' } | Sort-Object -Unique

$ips = ([System.Net.Dns]::GetHostEntry([System.Net.Dns]::GetHostName()).AddressList | Where-Object { $_.AddressFamily -eq 'InterNetwork' }).IPAddressToString | Where-Object { $_ -ne '127.0.0.1' -and $_ -notlike '169.*' } | Sort-Object -Unique
if (-not $ips -or $ips.Count -eq 0) {
    Write-Warning "找不到可用的非回送 (non-loopback) IPv4 位址。請確認網路介面已啟用。"
}

# 顯示即將使用的 URL（含 localhost）
$urls = @()
$urls += "http://localhost:$Port$Path"
foreach ($ip in $ips) { $urls += "http://$($ip):$($Port)$Path" }

Write-Host "將開放以下連接（伺服器需監聽對應埠與介面）:`n" -ForegroundColor Cyan
foreach ($u in $urls) { Write-Host "  $u" }

# 開防火牆規則
$ruleName = "WuSOEM HTTP $Port"
$netFirewallAvailable = (Get-Command -Name Get-NetFirewallRule -ErrorAction SilentlyContinue) -ne $null
if ($netFirewallAvailable) {
    $existing = Get-NetFirewallRule -DisplayName $ruleName -ErrorAction SilentlyContinue
    if ($existing) {
        Write-Host "防火牆規則已存在： $ruleName (跳過建立)" -ForegroundColor Yellow
    } else {
        Write-Host "建立防火牆允許規則: $ruleName ..." -NoNewline
        try {
            New-NetFirewallRule -DisplayName $ruleName -Direction Inbound -LocalPort $Port -Protocol TCP -Action Allow -Profile Any | Out-Null
            Write-Host " 完成" -ForegroundColor Green
        } catch {
            Write-Host " 失敗: $_" -ForegroundColor Red
        }
    }
    } else {
    Write-Host "系統上找不到 Get-NetFirewallRule (使用 netsh 建立防火牆規則)" -ForegroundColor Yellow
    # 使用字串連接以避免內部引號與變數插值解析問題
    $cmd = 'netsh advfirewall firewall add rule name="' + $ruleName + '" dir=in action=allow protocol=TCP localport=' + $Port
    Write-Host "執行: $cmd"
    cmd /c $cmd
}

# 如果需要，建立 urlacl（對使用 http.sys 的內建 HTTP 監聽者必要）
if ($UseUrlAcl) {
    Write-Host "嘗試註冊 URL ACL（netsh http add urlacl）..." -ForegroundColor Cyan
    # 先嘗試註冊通配符 +，若失敗則改為逐一 IP
    $aclUrl = "http://+:$Port/"
    $addCmd = "netsh http add urlacl url=$aclUrl user=$User"
    $res = & netsh http add urlacl url=$aclUrl user=$User 2>&1
    if ($LASTEXITCODE -eq 0) {
        Write-Host "註冊成功: $aclUrl" -ForegroundColor Green
    } else {
        Write-Host "註冊通配符失敗，嘗試對各 IP 註冊..." -ForegroundColor Yellow
        foreach ($ip in $ips) {
            $u = "http://$($ip):$($Port)/"
            Write-Host "  註冊: $u ... " -NoNewline
            $r = & netsh http add urlacl url=$u user=$User 2>&1
            if ($LASTEXITCODE -eq 0) { Write-Host "完成" -ForegroundColor Green } else { Write-Host "失敗" -ForegroundColor Red }
        }
    }
}

Write-Host "`n設定完成。請確認您的 HTTP 伺服器已啟動且監聽該埠及介面。" -ForegroundColor Green
Write-Host "在其他裝置上測試列出的 URL（例如 http://<this-host-ip>:$Port$Path）。" -ForegroundColor Cyan

# 額外提示
Write-Host "`n注意安全性：若只要在 LAN 使用，建議未必要使用 '+' wildcard，改為註冊或綁定具體的局域網介面 IP 並在防火牆上限制來源子網。" -ForegroundColor Yellow

exit 0
