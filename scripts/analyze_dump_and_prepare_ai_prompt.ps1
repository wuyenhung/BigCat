<#
  analyze_dump_and_prepare_ai_prompt.ps1

  - Finds the latest BigCat minidump under build/
  - Uses cdb.exe (from Windows Debugging Tools) to produce an analysis
  - Attempts to extract the topmost source file:line from the stack
  - Creates .vscode/last_crash_ai_request.txt containing a concise description,
    the relevant stack/source snippets and an AI-friendly prompt you can paste
    into the assistant to request suggested fixes.

  Note: Requires cdb.exe on PATH or installed under Windows Kits Debuggers.
        If cdb isn't found this script will generate a fallback report containing
        the raw dump path and instructions.
#>

param(
    [string]$WorkspaceRoot = (Get-Location).Path,
    [string]$BuildDir = "${PWD}\build",
    [string]$DumpPath = '' ,
    [string]$SymbolCache = (Join-Path -Path $env:TEMP -ChildPath 'symcache'),
    [int]$CdbTimeoutSec = 90,
    [switch]$OpenInEditor
)

function Find-Cdb {
    $cdb = (Get-Command cdb.exe -ErrorAction SilentlyContinue).Path
    if ($cdb) { return $cdb }

    # common Windows Kits location for Debugging Tools
    $candidates = @( 
        "C:\\Program Files (x86)\\Windows Kits\\10\\Debuggers\\x64\\cdb.exe",
        "C:\\Program Files\\Windows Kits\\10\\Debuggers\\x64\\cdb.exe",
        "C:\\Program Files (x86)\\Windows Kits\\8.1\\Debuggers\\x64\\cdb.exe"
    )
    foreach ($p in $candidates) { if (Test-Path $p) { return $p } }
    return $null
}

if (-not (Test-Path $BuildDir)) {
    Write-Host "Build directory not found at $BuildDir" -ForegroundColor Yellow
    exit 2
}

if ([string]::IsNullOrWhiteSpace($DumpPath)) {
    $dumps = Get-ChildItem -Path $BuildDir -Filter BigCat_crash_*.dmp -Recurse -ErrorAction SilentlyContinue | Sort-Object LastWriteTime -Descending
    if (-not $dumps) { Write-Host "No BigCat_crash_*.dmp found under $BuildDir" -ForegroundColor Yellow; exit 1 }
    $DumpPath = $dumps[0].FullName
}

Write-Host "Using dump: $DumpPath" -ForegroundColor Cyan

$cdb = Find-Cdb
$reportDir = Join-Path -Path $WorkspaceRoot -ChildPath '.vscode'
if (-not (Test-Path $reportDir)) { New-Item -ItemType Directory -Path $reportDir | Out-Null }

$analysisFile = Join-Path -Path $reportDir -ChildPath ('dump_analysis_{0}.txt' -f ([DateTime]::UtcNow.ToString('yyyyMMdd_HHmmss')))

if ($cdb) {
    Write-Host "Found cdb at: $cdb" -ForegroundColor Green

    # Build symbol path for cdb via -y (more reliable than scripting .sympath).
    # Include the local build output directories so BigCat.pdb resolves quickly.
    $releaseSym = (Join-Path -Path $BuildDir -ChildPath 'Release')
    $releaseBin = (Join-Path -Path $BuildDir -ChildPath 'Release\bin\main')
    $debugSym = (Join-Path -Path $BuildDir -ChildPath 'Debug')
    # Use local symbols only to avoid long-running downloads from symbol servers.
    # Also clear inherited symbol server env vars for this run.
    $symParts = @()
    if (Test-Path $releaseBin) { $symParts += $releaseBin }
    if (Test-Path $releaseSym) { $symParts += $releaseSym }
    if (Test-Path $debugSym) { $symParts += $debugSym }
    $symString = [string]::Join(';', $symParts)

    # Keep commands minimal first to avoid long symbol load stalls.
    $cdbCmds = @(
        ".reload /f BigCat.exe",
        ".ecxr",
        "kv",
        "lm",
        "!analyze -v",
        "q"
    )

    $stderrFile = ($analysisFile -replace '\.txt$', '.err.txt')
    $argLine = "-y `"$symString`" -z `"$DumpPath`" -lines -c `"$($cdbCmds -join '; ')`""
    Write-Host "Running: $cdb $argLine" -ForegroundColor Gray

    $oldNtSymbolPath = $env:_NT_SYMBOL_PATH
    $oldNtAltSymbolPath = $env:_NT_ALT_SYMBOL_PATH
    $oldNtExecImagePath = $env:_NT_EXECUTABLE_IMAGE_PATH
    try {
        $env:_NT_SYMBOL_PATH = ''
        $env:_NT_ALT_SYMBOL_PATH = ''
        $env:_NT_EXECUTABLE_IMAGE_PATH = ''

        $proc = Start-Process -FilePath $cdb -ArgumentList $argLine -NoNewWindow -PassThru -RedirectStandardOutput $analysisFile -RedirectStandardError $stderrFile
        $timeoutMs = [Math]::Max(1, $CdbTimeoutSec) * 1000
        if (-not $proc.WaitForExit($timeoutMs)) {
            try { $proc.Kill() } catch { }
            Add-Content -Path $analysisFile -Value "`n[analyze_dump] Timeout after ${CdbTimeoutSec}s; cdb was killed. Output may be partial." -Encoding utf8
        }
    } finally {
        $env:_NT_SYMBOL_PATH = $oldNtSymbolPath
        $env:_NT_ALT_SYMBOL_PATH = $oldNtAltSymbolPath
        $env:_NT_EXECUTABLE_IMAGE_PATH = $oldNtExecImagePath
    }

    Write-Host "Analysis written to $analysisFile" -ForegroundColor Green
    if (Test-Path $stderrFile) {
        $errLen = (Get-Item $stderrFile).Length
        if ($errLen -gt 0) { Write-Host "cdb stderr written to $stderrFile" -ForegroundColor Yellow }
    }

    # Attempt to extract source file:line from analysis
    $analysisText = Get-Content -Raw -Path $analysisFile -ErrorAction SilentlyContinue
    $match = [regex]::Match($analysisText, "(?m)([A-Za-z0-9_:\\/.\\-]+\.(?:cpp|cxx|c|h|hpp)):(\d+)")
    $sourcePath = $null; $sourceLine = $null
    if ($match.Success) { $sourcePath = $match.Groups[1].Value; $sourceLine = [int]$match.Groups[2].Value }
} else {
    Write-Warning "cdb.exe not found. Analysis will be limited to listing the dump path."
    $analysisFile = Join-Path -Path $reportDir -ChildPath ('dump_analysis_raw_{0}.txt' -f ([DateTime]::UtcNow.ToString('yyyyMMdd_HHmmss')))
    "Dump: $DumpPath`nNo cdb.exe found on PATH or in common Windows Kits locations. Install Debugging Tools for Windows (cdb.exe) to get full analysis." | Out-File -FilePath $analysisFile -Encoding utf8
    $analysisText = Get-Content -Raw -Path $analysisFile
}

# Prepare AI prompt file
$aiFile = Join-Path -Path $reportDir -ChildPath 'last_crash_ai_request.txt'

$promptHeader = @()
$promptHeader += "--- BigCat Crash Report (auto-generated) ---"
$promptHeader += "Dump file: $DumpPath"
$promptHeader += "Analysis file: $analysisFile"

if ($sourcePath) {
    $promptHeader += ("Probable crash location: {0}:{1}" -f $sourcePath, $sourceLine)
    if (Test-Path $sourcePath) {
        # extract context lines
        $startLine = [Math]::Max(1, $sourceLine - 8)
        $endLine = $sourceLine + 8
        $srcLines = Get-Content -Path $sourcePath -ErrorAction SilentlyContinue | Select-Object -Index ($startLine - 1)..($endLine - 1)
        $promptHeader += ("\n---- RELEVANT SOURCE (around {0}:{1}) ----" -f $sourcePath, $sourceLine)
        $i = $startLine
        foreach ($l in $srcLines) { $promptHeader += ('{0,6}: {1}' -f $i, $l); $i++ }
    } else {
        $promptHeader += "(Source file $sourcePath described by the dump is not present in workspace)"
    }
} else {
    $promptHeader += "Probable crash location: Not found in analysis output. See analysis file for raw stack/analysis." 
}

$promptHeader += "\n---- FULL STACK / ANALYSIS excerpt (first 200 lines) ----"
$analysisSnippet = ($analysisText -split "`n") | Select-Object -First 200 -ErrorAction SilentlyContinue

$promptFooter = @()
$promptFooter += "\nPlease analyze the crash and suggest the most likely code fix."
$promptFooter += "If you identify a bug, provide a minimal patch (unified diff or 'git apply' style), explain the cause, and add a small unit or integration test if reasonable." 
$promptFooter += "Include exact filenames and line numbers and be conservative about behavior changes."

# Write AI prompt with analysis snippet
$all = $promptHeader + $analysisSnippet + $promptFooter
$all | Out-File -FilePath $aiFile -Encoding utf8

Write-Host "AI prompt prepared: $aiFile" -ForegroundColor Green
Write-Host "Open that file (or copy content) and paste it into an assistant to request code changes." -ForegroundColor Cyan

if ($OpenInEditor) {
    # Try to use 'code' CLI if available
    $codeCmd = (Get-Command code -ErrorAction SilentlyContinue).Path
    if ($codeCmd) {
        & $codeCmd $aiFile
    } else {
        Start-Process -FilePath $aiFile
    }
}

exit 0
