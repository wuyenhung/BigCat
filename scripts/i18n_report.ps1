<#
.SYNOPSIS
    Web UI 多語系目錄覆蓋率報告（i18n coverage report）。

.DESCRIPTION
    掃描 resources/web 的 HTML/JS，抽出「含中日韓字元的 UI 字串」（翻譯引擎的 key），
    再比對 locales/*.json 的 messages，回報每個語系覆蓋率與缺譯清單。

    翻譯機制見 docs/WEB_I18N.md：以「中文原文」作為 key，所以這裡抽出的原文就是
    locales/<code>.json 應該收錄的 key。本腳本唯讀，不修改任何檔案。

    比對前會做與引擎一致的正規化（空白正規化）並外加寬容處理：
      - 空白差異不影響比對（引擎把樣板裡的空白當成彈性空白）
      - HTML entity 解碼（&amp; &copy;）、JS 跳脫還原（\\ → \）
      - 樣板字串 ${...} 一律換成 * （模板字串在執行期就是 wildcard pattern）
      - 含 HTML tag 的字串先去掉 tag（innerHTML 插入後 DOM 只剩文字）
      - 以分隔符結尾的原文同時檢查 "原文 *" pattern（串接訊息）
      - 跳過 <pre>/<textarea>/註解/console.* 輸出／vendor JS／i18n 引擎自身

    注意：CJK 範圍一律寫 \\uXXXX 轉義。直接把「豈-﫿」這類字面字元放進字元類別，
    正規則對重疊範圍會把 surrogate 也判為符合，emoji 會被誤認為中文。

.PARAMETER WebDir
    Web UI 目錄，預設 <repo>/resources/web。

.PARAMETER LocaleDir
    語系目錄，預設 <WebDir>/locales。

.PARAMETER Detailed
    缺譯清單附上來源檔案（最多 3 個）。

.PARAMETER OutFile
    把報告另存成 UTF-8（無 BOM）檔案。

.PARAMETER FailOnMissing
    只要有非來源語系缺譯就回傳 exit code 1（可接進 CI／驗收流程）。

.EXAMPLE
    powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\i18n_report.ps1 -Detailed
#>
[CmdletBinding()]
param(
    [string]$WebDir,
    [string]$LocaleDir,
    [switch]$Detailed,
    [string]$OutFile,
    [switch]$FailOnMissing
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot          # 可從任何工作目錄執行
if (-not $WebDir) { $WebDir = Join-Path $root 'resources\web' }
if (-not $LocaleDir) { $LocaleDir = Join-Path $WebDir 'locales' }
if (-not (Test-Path $WebDir)) { throw "Web UI 目錄不存在: $WebDir" }
if (-not (Test-Path $LocaleDir)) { throw "語系目錄不存在: $LocaleDir" }

$CJK = '[\u3040-\u30ff\u3400-\u4dbf\u4e00-\u9fff\uf900-\ufaff]'
$TAG = '<[^>]{0,400}>'
$TEMPLATE = '\$\{[^{}]*(?:\{[^{}]*\}[^{}]*)*\}'

# 比對用正規劃：去 tag、樣板→*、還原跳脫、去空白（空白差異不影響覆蓋判定）
function Canonical([string]$s, [string]$kind) {
    $v = $s
    if ($kind -like 'js*') { $v = $v.Replace('\\', '\') }
    if ($kind -like 'markup*') { try { $v = [System.Net.WebUtility]::HtmlDecode($v) } catch { } }
    $v = [regex]::Replace($v, $TAG, ' ')
    $v = [regex]::Replace($v, $TEMPLATE, '*')
    $v = [regex]::Replace($v, '\s+', '')
    return $v.Trim()
}
function Pretty([string]$s) { return ($s -replace '\s+', ' ').Trim() }

# ---------- 1. 抽出原文 key ----------
$excludeName = '^i18n\.js$|\.min\.js$|GLTFLoader|three\.js|hammer'
$files = Get-ChildItem $WebDir -Include *.html, *.js -Recurse -File |
    Where-Object { $_.FullName -notmatch [regex]::Escape($LocaleDir) -and $_.Name -notmatch $excludeName }

$keys = [ordered]@{}      # canonical -> @{ display; sources }
function Add-Key([string]$raw, [string]$src, [string]$kind) {
    $disp = Pretty $raw
    if (-not $disp -or $disp -notmatch $CJK -or $disp.Length -gt 2000) { return }
    $canon = Canonical $raw $kind
    if (-not $canon -or $canon -notmatch $CJK) { return }
    $variants = New-Object System.Collections.Generic.List[string]
    $variants.Add($canon)
    # 樣板字串：另外產生「代入 konkrete 值」的形状（${...} → 1），
    # 讓「力臂${j+1}」這類其實已由 forceArm1..6 exact key 覆蓋的情況能被判定為已覆蓋
    if ($raw -match '\$\{') {
        $concrete = Canonical ([regex]::Replace($raw, $TEMPLATE, '1')) $kind
        if ($concrete) { $variants.Add($concrete) }
    }
    # 串接型訊息（"连接失败 - " + err）：引擎靠 "原文 *" pattern 比對
    if ($canon -match '[:\uFF1A(\uFF08(]$' -or $canon -match '[-(]$') { $variants.Add($canon + '*') }
    foreach ($c in ($variants | Select-Object -Unique)) {
        if (-not $keys.Contains($c)) { $keys[$c] = @{ display = $disp; sources = [System.Collections.Generic.HashSet[string]]::new() } }
        [void]$keys[$c].sources.Add($src)
    }
}

foreach ($f in $files) {
    $rel = $f.Name
    $raw = Get-Content $f.FullName -Raw -Encoding UTF8
    $t = [regex]::Replace($raw, '(?s)<!--.*?-->', ' ')
    $scripts = [regex]::Matches($t, '(?si)<script\b[^>]*>(.*?)</script>')
    if ($scripts.Count -eq 0 -and $f.Extension -eq '.js') {
        $safe = $t -replace '</script>', ' '
        $scripts = [regex]::Matches("<script>$safe</script>", '(?si)<script\b[^>]*>(.*?)</script>')
    }
    $markup = [regex]::Replace($t, '(?si)<script\b.*?</script>', ' ')
    $markup = [regex]::Replace($markup, '(?si)<style\b.*?</style>', ' ')
    $markup = [regex]::Replace($markup, '(?si)<pre\b.*?</pre>', ' ')

    foreach ($m in [regex]::Matches($markup, '(?i)\b(title|placeholder|aria-label|alt)="([^"]*)"')) {
        Add-Key $m.Groups[2].Value $rel ('markup-attr-' + $m.Groups[1].Value.ToLower())
    }
    foreach ($m in [regex]::Matches($markup, '>(([^<>]*[\u4e00-\u9fff])[^<>]*)<')) { Add-Key $m.Groups[1].Value $rel 'markup-text' }

    foreach ($sb in $scripts) {
        $js = [regex]::Replace($sb.Groups[1].Value, '(?m)//[^\r\n]*', ' ')
        $js = [regex]::Replace($js, '(?s)/\*.*?\*/', ' ')
        foreach ($q in [regex]::Matches($js, "(?m)['`"]([^'`"`r`n]*[\u4e00-\u9fff][^'`"`r`n]*)['`"]|``([^``]*[\u4e00-\u9fff][^``]*)``")) {
            $val = if ($q.Groups[1].Success) { $q.Groups[1].Value } else { $q.Groups[2].Value }
            $prefix = $js.Substring(0, $q.Index)
            if ($prefix -match 'console\.\w+\([^)]*$') { continue }        # 開發 log 不進目錄
            $flat = ($val -replace '\s+', ' ')
            if ($flat.Length -gt 900 -and $flat -match '(function |document\.|=>|\{\{)') { continue }  # 跨行樣板噪音
            Add-Key $val $rel 'js-string'
        }
    }
}

# ---------- 2. 載入語系目錄 ----------
$index = Get-Content (Join-Path $LocaleDir 'index.json') -Raw -Encoding UTF8 | ConvertFrom-Json
$source = $index.source
$locales = @()
foreach ($meta in $index.supported) {
    $path = Join-Path $LocaleDir $meta.file
    if (-not (Test-Path $path)) { Write-Warning "缺少語系檔: $path"; continue }
    $data = Get-Content $path -Raw -Encoding UTF8 | ConvertFrom-Json
    $set = @{}
    $pats = [System.Collections.Generic.List[object]]::new()
    # 幽靈 key：目錄 key 內含 HTML tag（多半是把 innerHTML 樣板整段抓下來）。執行期 DOM 裡
    # tag 不會出現在 text node，所以這類 key 永遠不會被命中 → 列出來請維護者清理或改成純文字。
    $dead = [System.Collections.Generic.List[string]]::new()
    if ($data.messages) {
        foreach ($p in $data.messages.PSObject.Properties) {
            $name = $p.Name
            if ($name -match '\*') {
                # pattern key：重現引擎規則（* → 非貪萬用；空白 → 零個或多個空白）
                $parts = @((Pretty $name) -split '\*')
                $re = (($parts | ForEach-Object {
                    # 先切空白再跳脫：[regex]::Escape 會把空白輸出成「\ 」，
                    # 事後取代空格會留下懸掛反斜線而誤變成「字面左方括號」
                    (($_ -split ' ' | ForEach-Object { if ($_) { [regex]::Escape($_) } else { '' } }) -join '[ \t]*')
                }) -join '((?:.|\s)*?)')
                try { $pats.Add([regex]::new('^' + $re + '$')) } catch { }
                $set[(Canonical ($name -replace '\s*\*\s*$', '*') 'report')] = $p.Value
            } else {
                $k = Canonical $name 'report'
                $set[$k] = $p.Value
                # 含 tag 的 key 另外註冊「去 tag」形状，讓同一句話的純文字版仍算已覆蓋
                #（例：目錄只有 `<strong>從站列表：</strong>...`，執行期文字是「從站列表：」）。
                $strip = Canonical ([regex]::Replace($name, '<[^>]*>', '')) 'report'
                if ($strip -and $strip -ne $k) { $set[$strip] = $p.Value }
                if ($name -match '</?[A-Za-z][A-Za-z0-9]*[\s/>]') { [void]$dead.Add($name) }
            }
        }
    }
    $locales += [pscustomobject]@{ code = $meta.code; native = $meta.nativeName; file = $meta.file; entries = $data.messages.PSObject.Properties.Name.Count; map = $set; patterns = $pats; dead = $dead }
}

# ---------- 3. 覆蓋率 ----------
$sb = [System.Text.StringBuilder]::new()
[void]$sb.AppendLine("Web UI i18n 覆蓋率報告  $(Get-Date -Format 'yyyy-MM-dd HH:mm')")
[void]$sb.AppendLine("Web UI: $WebDir")
[void]$sb.AppendLine("來源語系: $source    抽出原文: $($keys.Count) 條    掃描檔案: $($files.Count)")
[void]$sb.AppendLine('')
$missingAny = $false
foreach ($loc in $locales) {
    if ($loc.code -eq $source) {
        [void]$sb.AppendLine(("{0,-8} 來源語系：原文直接顯示，不需要目錄（目前另收錄 {1} 條改寫）" -f $loc.code, $loc.entries))
        continue
    }
    $missing = [System.Collections.Generic.List[string]]::new()
    $checked = 0
    # 同一原文可能有多種正規化形状（樣板→*、${}→1、pattern 後綴 *）；
    # 只要其中一個形状已被目錄覆蓋，就算覆蓋。
    $byDisplay = [ordered]@{}
    foreach ($c in $keys.Keys) {
        $d = $keys[$c].display
        if (-not $byDisplay.Contains($d)) { $byDisplay[$d] = @{ canon = [System.Collections.Generic.List[string]]::new(); sources = [System.Collections.Generic.HashSet[string]]::new() } }
        [void]$byDisplay[$d].canon.Add($c)
        foreach ($s in $keys[$c].sources) { [void]$byDisplay[$d].sources.Add($s) }
    }
    foreach ($d in $byDisplay.Keys) {
        # 抽取噪音：跨行樣板／markup 區塊、程式碼片段（其執行期文字節點會由個別 key 覆蓋）
        if ($d -match '(function |const |document\.|this\.|=>|<span|<div|<button|class=")') { continue }
        if ($d -match '[{}]' -and $d -match ';' -and $d -notmatch '[。！？]') { continue }
        $checked++
        $hit = $false
        foreach ($c in $byDisplay[$d].canon) { if ($loc.map.ContainsKey($c)) { $hit = $true; break } }
        if (-not $hit) {
            foreach ($re in $loc.patterns) { if ($re.IsMatch($d)) { $hit = $true; break } }
        }
        if (-not $hit -and $d -match '</?[A-Za-z][A-Za-z0-9]*[\s/>]') {
            # JS 裡的 innerHTML 片段（例：'<option value="">(未選擇)</option>'）：執行期 text node
            # 只有去 tag 後的純文字（可能殘留 >、< 邊角），所以要拿剝掉 tag 的形状再查一次，
            # 否則會誤報缺譯（真正該有的 key 是「(未選擇)」）。
            $plain = [regex]::Replace($d, '<[^>]*>', ' ') -replace '[<>]', ' '
            $plain = Canonical $plain 'report'
            if ($plain -and $loc.map.ContainsKey($plain)) { $hit = $true }
            if (-not $hit -and $plain) {
                foreach ($re in $loc.patterns) { if ($re.IsMatch($plain)) { $hit = $true; break } }
            }
        }
        if (-not $hit -and $d -match '\$\{') {
            # 樣板字串：把 ${...} 當成數字代入試比（例：`力臂${j+1}` 執行期就是目錄裡的 力臂1..6）
            foreach ($n in 1..12) {
                $cand = Canonical ([regex]::Replace($d, $TEMPLATE, "$n")) 'js-string'
                if ($cand -and $loc.map.ContainsKey($cand)) { $hit = $true; break }
            }
        }
        if (-not $hit) {
            $missing.Add($d)
            $byDisplay[$d].missingSources = $byDisplay[$d].sources
        }
    }
    $covered = $checked - $missing.Count
    $pct = if ($checked) { [Math]::Round(100.0 * $covered / $checked, 1) } else { 100 }
    [void]$sb.AppendLine(("{0,-8} {1,-12} 目錄 {2} 條 / 覆蓋 {3}/{4} = {5}%  缺譯 {6}" -f $loc.code, $loc.native, $loc.entries, $covered, $checked, $pct, $missing.Count))
    if ($loc.dead.Count) {
        # 幽靈 key：執行期 DOM 的文字節點不會含 tag，所以這類 key 永不命中，
        # 只是把同句話的另一種正規化形状頂過去（讓覆蓋率看起來達標）。建議刪掉或改存純文字。
        [void]$sb.AppendLine("   幽靈 key（含 HTML tag，執行期永不命中）: $($loc.dead.Count) 條")
        foreach ($dk in ($loc.dead | Sort-Object)) { [void]$sb.AppendLine("     · $dk") }
    }
    if ($missing.Count) {
        $missingAny = $true
        foreach ($m in ($missing | Sort-Object)) {
            if ($Detailed) {
                $srcs = ($byDisplay[$m].sources | Select-Object -First 3) -join ', '
                [void]$sb.AppendLine("   - [$srcs] $m")
            } else {
                [void]$sb.AppendLine("   - $m")
            }
        }
    }
    [void]$sb.AppendLine('')
}

$report = $sb.ToString()
Write-Output $report
if ($OutFile) {
    $full = if ([System.IO.Path]::IsPathRooted($OutFile)) { $OutFile } else { Join-Path (Get-Location) $OutFile }
    [System.IO.File]::WriteAllText($full, $report, (New-Object System.Text.UTF8Encoding($false)))
    Write-Output "報告已儲存: $full"
}
if ($FailOnMissing -and $missingAny) { exit 1 }
exit 0
