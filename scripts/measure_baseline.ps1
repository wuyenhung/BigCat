#Requires -Version 5.1
<#
.SYNOPSIS
    P0 measurement baseline script - collects EtherCAT cycle stability metrics
    and produces a comparable baseline report.

.DESCRIPTION
    This script collects EtherCAT cycle stability metrics and produces a
    comparable baseline report.
    It polls the running WuBigCat application via HTTP API to collect real-time
    metrics, records key indicators for both idle and motion scenarios, and
    produces a fixed-format baseline report for later regression comparison.
    See docs/windows-realtime-optimization.md for the optimization context.

    Metrics collected:
      - commsTime          (SOEM send/receive time, us)
      - actualCycleTime    (observed cycle-to-cycle period, us)
      - maxObservedCycleTime (high-water-mark cycle period, us)

.PARAMETER HttpHost
    Application HTTP server address (default 127.0.0.1).

.PARAMETER HttpPort
    Application HTTP server port (default 8080).

.PARAMETER IdleDurationSec
    Idle scenario measurement duration in seconds (default 60).

.PARAMETER MotionDurationSec
    Motion scenario measurement duration in seconds (default 60).
    Set to 0 to skip motion scenario.

.PARAMETER OutputDir
    Baseline report output directory. Default: <project>/baseline_results/

.PARAMETER BaselineName
    Baseline name used for the output filename. Default: auto-generated timestamp.

.PARAMETER CompareWith
    Path to a previous baseline JSON for automatic comparison after measurement.

.PARAMETER PollIntervalMs
    HTTP polling interval in ms (default 200). Should not be less than SSE broadcast period.

.PARAMETER WarmupSec
    Warm-up time before starting measurement, allowing the system to reach steady state.

.PARAMETER RegressionMaxCycleRatio
    Max cycle regression threshold ratio (default 1.30 = 30% degradation allowed).

.PARAMETER RegressionMaxCommsRatio
    Max comms regression threshold ratio (default 1.50 = 50% degradation allowed).

.PARAMETER SkipHttp
    Skip HTTP polling and use interactive manual recording mode instead.

.EXAMPLE
    # Basic usage: 60s idle + 60s motion
    .\scripts\measure_baseline.ps1

.EXAMPLE
    # Idle-only measurement for 120 seconds
    .\scripts\measure_baseline.ps1 -IdleDurationSec 120 -MotionDurationSec 0

.EXAMPLE
    # Measure and compare against a previous baseline
    .\scripts\measure_baseline.ps1 -CompareWith .\baseline_results\baseline_20260101.json
#>

param(
    [string]$HttpHost = "127.0.0.1",
    [int]$HttpPort = 8080,
    [int]$IdleDurationSec = 60,
    [int]$MotionDurationSec = 60,
    [string]$OutputDir,
    [string]$BaselineName,
    [string]$CompareWith,
    [int]$PollIntervalMs = 200,
    [int]$WarmupSec = 10,
    [double]$RegressionMaxCycleRatio = 1.30,
    [double]$RegressionMaxCommsRatio = 1.50,
    [switch]$SkipHttp
)

$ErrorActionPreference = "Stop"
$script:ScriptRoot = $PSScriptRoot
$script:ProjectRoot = Resolve-Path (Join-Path $ScriptRoot "..")

if (-not $OutputDir) {
    $OutputDir = Join-Path $ProjectRoot "baseline_results"
}
if (-not (Test-Path $OutputDir)) {
    New-Item -ItemType Directory -Path $OutputDir -Force | Out-Null
}

# ============================================================================
# Helper functions
# ============================================================================

function Write-Banner {
    param([string]$Text)
    $line = "=" * 70
    Write-Host ""
    Write-Host $line -ForegroundColor Cyan
    Write-Host "  $Text" -ForegroundColor Cyan
    Write-Host $line -ForegroundColor Cyan
    Write-Host ""
}

function Write-Step {
    param([string]$Text)
    Write-Host ">>> $Text" -ForegroundColor Yellow
}

function Write-OK {
    param([string]$Text)
    Write-Host "  [OK] $Text" -ForegroundColor Green
}

function Write-Warn {
    param([string]$Text)
    Write-Host "  [WARN] $Text" -ForegroundColor Magenta
}

function Write-Info {
    param([string]$Text)
    Write-Host "  [INFO] $Text" -ForegroundColor Gray
}

# ============================================================================
# HTTP metric collector
# ============================================================================

function Get-MetricsFromHttp {
    param(
        [string]$HostAddr = $HttpHost,
        [int]$Port = $HttpPort
    )

    try {
        $uri = "http://${HostAddr}:${Port}/ui_state"
        $request = [System.Net.WebRequest]::Create($uri)
        $request.Timeout = 3000
        $request.Method = "GET"

        $response = $request.GetResponse()
        $stream = $response.GetResponseStream()
        $reader = New-Object System.IO.StreamReader($stream)
        $body = $reader.ReadToEnd()
        $reader.Close()
        $response.Close()

        if (-not $body) { return $null }

        # Manual light JSON parse for the fields we need
        # /ui_state exposes: desiredCycleTime, commsTime, actualCycleTime, maxObservedCycleTime
        $result = @{}
        $metricKeys = @('commsTime', 'actualCycleTime', 'maxObservedCycleTime', 'desiredCycleTime')
        foreach ($key in $metricKeys) {
            if ($body -match """$key"":\s*(\d+)") {
                $result[$key] = [int]$Matches[1]
            }
        }

        # Add client-side timestamp and HTTP-ok marker
        $result['_ts'] = Get-Date
        $result['_http_ok'] = $true
        $result['_source'] = 'ui_state'

        # If commsTime is 0, try SSE for the real ecatCycleUs from the master
        $commsVal = if ($result.ContainsKey('commsTime')) { $result['commsTime'] } else { 0 }
        if ($commsVal -eq 0) {
            $sseMetrics = Get-MetricsFromSse -HostAddr $HostAddr -Port $Port
            if ($sseMetrics -and $sseMetrics.ecatCycleUs -gt 0) {
                # SSE provides the TRUE observed cycle from the master
                $result['commsTime'] = $sseMetrics.commsTime
                $result['actualCycleTime'] = $sseMetrics.ecatCycleUs
                $result['maxObservedCycleTime'] = $sseMetrics.maxObservedCycleTime
                $result['_source'] = 'sse'
            }
        }

        return [PSCustomObject]$result
    } catch {
        # Silently return null on HTTP failure
    }
    return $null
}

function Get-MetricsFromSse {
    param(
        [string]$HostAddr = $HttpHost,
        [int]$Port = $HttpPort
    )

    try {
        # Open a short-lived SSE connection to /pdo_sse, read the first event, then close.
        # The SSE broadcasts ecatCycleUs from master->GetObservedCycleTimeUs().
        $tcp = New-Object System.Net.Sockets.TcpClient($HostAddr, $Port)
        $tcp.ReceiveTimeout = 5000
        $tcp.SendTimeout = 3000
        $stream = $tcp.GetStream()
        $writer = New-Object System.IO.StreamWriter($stream)
        $writer.AutoFlush = $true
        $reader = New-Object System.IO.StreamReader($stream)

        # Send HTTP GET for SSE (bypasses auth since SSE handshake is handled server-side)
        $writer.WriteLine("GET /pdo_sse HTTP/1.1")
        $writer.WriteLine("Host: ${HostAddr}:${Port}")
        $writer.WriteLine("Accept: text/event-stream")
        $writer.WriteLine("Connection: close")
        $writer.WriteLine("")
        $writer.Flush()

        # Read response headers
        $headerComplete = $false
        $deadline = (Get-Date).AddSeconds(5)
        while (-not $headerComplete -and (Get-Date) -lt $deadline) {
            $line = $reader.ReadLine()
            if ($line -eq $null -or $line -eq '') {
                $headerComplete = $true
            } elseif ($line -match 'HTTP/1\.[01] (\d+)') {
                if ([int]$Matches[1] -ge 400) {
                    $reader.Close(); $writer.Close(); $stream.Close(); $tcp.Close()
                    return $null
                }
            }
        }

        if (-not $headerComplete) {
            $reader.Close(); $writer.Close(); $stream.Close(); $tcp.Close()
            return $null
        }

        # Read first SSE event (data: {...}\n\n)
        $sseData = ''
        $deadline = (Get-Date).AddSeconds(3)
        while ((Get-Date) -lt $deadline) {
            $line = $reader.ReadLine()
            if ($line -eq $null) { break }
            if ($line -eq '') {
                if ($sseData -ne '') { break }
            } elseif ($line -match '^data:\s*(.+)$') {
                $sseData = $Matches[1]
            }
        }

        $reader.Close(); $writer.Close(); $stream.Close(); $tcp.Close()

        if (-not $sseData -or $sseData -eq '{}') { return $null }

        # Parse ecatCycleUs from SSE JSON
        $result = @{}
        if ($sseData -match '"ecatCycleUs":\s*(\d+)') {
            $result['ecatCycleUs'] = [int]$Matches[1]
        }
        if ($sseData -match '"commsTime":\s*(\d+)') {
            $result['commsTime'] = [int]$Matches[1]
        }
        if ($sseData -match '"maxObservedCycleTime":\s*(\d+)') {
            $result['maxObservedCycleTime'] = [int]$Matches[1]
        }

        if ($result.Count -gt 0) {
            $result['_source'] = 'sse'
            return [PSCustomObject]$result
        }
    } catch {
        # SSE not available or failed
    }
    return $null
}

function Wait-ForAppReady {
    param(
        [int]$TimeoutSec = 120,
        [string]$HostAddr = $HttpHost,
        [int]$Port = $HttpPort
    )

    $deadline = (Get-Date).AddSeconds($TimeoutSec)
    while ((Get-Date) -lt $deadline) {
        $metrics = Get-MetricsFromHttp -HostAddr $HostAddr -Port $Port
        if ($metrics) {
            Write-OK "Application HTTP API is ready"
            return $true
        }
        Write-Info "Waiting for application HTTP API ..."
        Start-Sleep -Seconds 2
    }
    Write-Warn "Cannot connect to application HTTP API (${HostAddr}:${Port})"
    return $false
}

function Assert-EtherCatOnline {
    param(
        [string]$HostAddr = $HttpHost,
        [int]$Port = $HttpPort
    )
    $metrics = Get-MetricsFromHttp -HostAddr $HostAddr -Port $Port
    if (-not $metrics) {
        Write-Warn "Cannot reach HTTP API to check EtherCAT state"
        return $false
    }
    # /ui_state does NOT expose oOnline/oEtherCatOk (SSE-only fields).
    # Proxy: if commsTime > 0 then processdata is flowing = Operational.
    $commsOk = ($metrics.commsTime -gt 0) -or ($metrics.actualCycleTime -gt 0)
    if ($commsOk) {
        Write-OK "EtherCAT appears ONLINE (commsTime=$($metrics.commsTime) us, cycleTime=$($metrics.actualCycleTime) us)"
        return $true
    }
    Write-Warn "EtherCAT does NOT appear to be in Operational state (commsTime=0, cycleTime=0)."
    Write-Warn "Connect and enable operation before measuring idle."
    return $false
}

function Assert-MetricsNonZero {
    param([hashtable]$Result, [string]$ScenarioName)

    if (-not $Result -or $Result.SampleCount -eq 0) { return }

    $allZero = $true
    foreach ($metric in @('commsTime', 'actualCycleTime', 'maxObservedCycleTime')) {
        $stats = Measure-SampleStatistics -Samples $Result.Samples -FieldName $metric
        if ($stats.Max -gt 0) {
            $allZero = $false
            break
        }
    }

    if ($allZero) {
        Write-Warn "[$ScenarioName] All metrics are ZERO. This usually means:"
        Write-Warn "  - EtherCAT was not in Operational state during measurement"
        Write-Warn "  - The HTTP /ui_state endpoint is not receiving cycle updates"
        Write-Warn "  - Re-run after confirming EtherCAT is connected and Operation Enabled"
    }
}

function Invoke-MetricsCollection {
    param(
        [int]$DurationSec,
        [string]$ScenarioName,
        [string]$HostAddr = $HttpHost,
        [int]$Port = $HttpPort,
        [int]$IntervalMs = $PollIntervalMs
    )

    Write-Step "Collecting [$ScenarioName] metrics for ${DurationSec}s ..."

    $samples = [System.Collections.ArrayList]::new()
    $deadline = (Get-Date).AddSeconds($DurationSec)
    $wkcAnomalies = 0
    $httpErrors = 0
    $lastCommsOk = $false

    while ((Get-Date) -lt $deadline) {
        $httpOk = $false
        try {
            $metrics = Get-MetricsFromHttp -HostAddr $HostAddr -Port $Port
            if ($metrics) {
                $httpOk = $true
                [void]$samples.Add($metrics)

                # Track WKC anomalies via commsTime transitions (proxy for processdata flow)
                # /ui_state does not expose oOnline; commsTime>0 means PDO exchange is active
                $nowCommsOk = ($metrics.commsTime -gt 0)
                if ($lastCommsOk -and -not $nowCommsOk) {
                    $wkcAnomalies++
                }
                $lastCommsOk = $nowCommsOk
            }
        } catch {
            # Exception in HTTP call or sample processing
        }
        if (-not $httpOk) {
            $httpErrors++
        }

        # Progress indicator
        $remaining = ($deadline - (Get-Date)).TotalSeconds
        $pct = [math]::Min(100, [math]::Round((1 - $remaining / $DurationSec) * 100))
        Write-Progress -Activity "Collecting $ScenarioName data" `
            -Status "$pct% ($($samples.Count) samples)" `
            -PercentComplete $pct

        $sleepMs = [math]::Max(10, [math]::Min($IntervalMs, [int]($remaining * 1000)))
        if ($sleepMs -gt 0) {
            Start-Sleep -Milliseconds $sleepMs
        }
    }
    Write-Progress -Activity "Collecting $ScenarioName data" -Completed

    Write-OK "$ScenarioName collection complete: $($samples.Count) samples, $httpErrors HTTP errors, $wkcAnomalies WKC anomalies"

    return @{
        Samples       = $samples
        SampleCount   = $samples.Count
        HttpErrors    = $httpErrors
        WkcAnomalies  = $wkcAnomalies
    }
}

function Measure-SampleStatistics {
    param([array]$Samples, [string]$FieldName)

    if ($Samples.Count -eq 0) {
        return @{ Min = 0; Max = 0; Avg = 0; P50 = 0; P95 = 0; P99 = 0; StdDev = 0; Count = 0 }
    }

    $values = @($Samples | ForEach-Object {
        $val = $_.$FieldName
        if ($null -ne $val) { [double]$val } else { $null }
    } | Where-Object { $null -ne $_ })

    if ($values.Count -eq 0) {
        return @{ Min = 0; Max = 0; Avg = 0; P50 = 0; P95 = 0; P99 = 0; StdDev = 0; Count = 0 }
    }

    $sorted = $values | Sort-Object
    $count = $sorted.Count
    $min = $sorted[0]
    $max = $sorted[-1]
    $avg = [math]::Round(($values | Measure-Object -Average).Average, 1)
    $p50 = $sorted[[math]::Floor($count * 0.50)]
    $p95 = $sorted[[math]::Floor($count * 0.95)]
    $p99 = $sorted[[math]::Floor($count * 0.99)]

    # Standard deviation
    if ($count -gt 1) {
        $sumSq = 0
        foreach ($v in $values) { $sumSq += [math]::Pow($v - $avg, 2) }
        $stdDev = [math]::Round([math]::Sqrt($sumSq / ($count - 1)), 1)
    } else {
        $stdDev = 0
    }

    return @{
        Min     = [double]$min
        Max     = [double]$max
        Avg     = $avg
        P50     = [double]$p50
        P95     = [double]$p95
        P99     = [double]$p99
        StdDev  = $stdDev
        Count   = $count
    }
}

function Export-BaselineReport {
    param(
        [hashtable]$IdleResult,
        [hashtable]$MotionResult,
        [hashtable]$TestConditions,
        [string]$OutputPath
    )

    # Compute statistics for each key metric
    $metrics = @('commsTime', 'actualCycleTime', 'maxObservedCycleTime')

    $report = [ordered]@{
        report_version     = "1.0"
        report_type        = "ethercat_cycle_baseline"
        generated_at       = (Get-Date -Format "yyyy-MM-ddTHH:mm:sszzz")
        test_conditions    = $TestConditions
        scenarios          = [ordered]@{}
    }

    foreach ($scenarioName in @('idle', 'motion')) {
        $result = if ($scenarioName -eq 'idle') { $IdleResult } else { $MotionResult }
        if (-not $result -or $result.SampleCount -eq 0) { continue }

        $scenarioStats = [ordered]@{
            sample_count   = $result.SampleCount
            http_errors    = $result.HttpErrors
            wkc_anomalies  = $result.WkcAnomalies
            duration_sec   = if ($scenarioName -eq 'idle') { $IdleDurationSec } else { $MotionDurationSec }
            metrics        = [ordered]@{}
        }

        foreach ($metric in $metrics) {
            $stats = Measure-SampleStatistics -Samples $result.Samples -FieldName $metric
            $scenarioStats.metrics[$metric] = $stats
        }
        $report.scenarios[$scenarioName] = $scenarioStats
    }

    # Regression thresholds
    $report.regression_thresholds = [ordered]@{
        max_cycle_ratio     = $RegressionMaxCycleRatio
        max_comms_ratio     = $RegressionMaxCommsRatio
        description         = "maxObservedCycleTime must not degrade more than $($RegressionMaxCycleRatio * 100 - 100)% vs baseline"
    }

    $json = $report | ConvertTo-Json -Depth 6
    $utf8 = New-Object System.Text.UTF8Encoding($false)
    [System.IO.File]::WriteAllText($OutputPath, $json, $utf8)
    Write-OK "Baseline report saved: $OutputPath"
    return $OutputPath
}

function Compare-Baselines {
    param(
        [string]$NewBaselinePath,
        [string]$OldBaselinePath
    )

    if (-not (Test-Path $OldBaselinePath)) {
        Write-Warn "Comparison baseline file not found: $OldBaselinePath"
        return $false
    }

    Write-Step "Comparing baselines: $NewBaselinePath vs $OldBaselinePath"

    try {
        $new = Get-Content $NewBaselinePath -Raw -Encoding UTF8 | ConvertFrom-Json
        $old = Get-Content $OldBaselinePath -Raw -Encoding UTF8 | ConvertFrom-Json
    } catch {
        Write-Warn "Failed to parse baseline JSON: $_"
        return $false
    }

    $pass = $true
    $results = @()

    foreach ($scenario in @('idle', 'motion')) {
        $newSc = $new.scenarios.$scenario
        $oldSc = $old.scenarios.$scenario
        if (-not $newSc -or -not $oldSc) { continue }

        foreach ($metric in @('commsTime', 'actualCycleTime', 'maxObservedCycleTime')) {
            $newStats = $newSc.metrics.$metric
            $oldStats = $oldSc.metrics.$metric
            if (-not $newStats -or -not $oldStats) { continue }

            $newMax = [double]$newStats.Max
            $oldMax = [double]$oldStats.Max
            if ($oldMax -le 0) { continue }

            $ratio = $newMax / $oldMax
            $threshold = if ($metric -eq 'commsTime') { $RegressionMaxCommsRatio } else { $RegressionMaxCycleRatio }
            $degraded = $ratio -gt $threshold

            $result = [PSCustomObject]@{
                Scenario    = $scenario
                Metric      = $metric
                OldMax      = $oldMax
                NewMax      = $newMax
                Ratio       = [math]::Round($ratio, 3)
                Threshold   = $threshold
                Degraded    = $degraded
            }
            $results += $result

            if ($degraded) {
                Write-Warn "REGRESSION: [$scenario] $metric Max: $oldMax -> $newMax (ratio=$($ratio.ToString('F2')), threshold=$threshold)"
                $pass = $false
            } else {
                Write-OK "[$scenario] $metric Max: $oldMax -> $newMax (ratio=$($ratio.ToString('F2')), OK)"
            }
        }
    }

    Write-Host ""
    if ($pass) {
        Write-Host "  ====== Regression check PASSED ======" -ForegroundColor Green
    } else {
        Write-Host "  ====== Regression check FAILED: performance degradation detected ======" -ForegroundColor Red
    }

    return $pass
}

function Get-SystemTestConditions {
    $conditions = [ordered]@{
        hostname          = $env:COMPUTERNAME
        os_version        = [System.Environment]::OSVersion.VersionString
        script_run_at     = (Get-Date -Format "yyyy-MM-ddTHH:mm:sszzz")
        idle_duration_s   = $IdleDurationSec
        motion_duration_s = $MotionDurationSec
        warmup_s          = $WarmupSec
        poll_interval_ms  = $PollIntervalMs
        http_endpoint     = "${HttpHost}:${HttpPort}"
    }

    # Try to get NIC info from the app
    try {
        $metrics = Get-MetricsFromHttp
        if ($metrics -and $metrics.desiredCycleTime) {
            $conditions.desired_cycle_us = $metrics.desiredCycleTime
        }
    } catch {}

    return $conditions
}

# ============================================================================
# Interactive (non-HTTP) mode
# ============================================================================

function Start-InteractiveBaseline {
    Write-Banner "Interactive Baseline Measurement Mode"
    Write-Host ""
    Write-Host "  This mode requires manual recording of metrics from the application UI."
    Write-Host "  Observe the following values in the application's Cycle panel:"
    Write-Host ""
    Write-Host "    - Comms Time (us)"
    Write-Host "    - Actual Cycle (us)"
    Write-Host "    - Max Cycle (us)"
    Write-Host "    - WKC (Working Counter)"
    Write-Host ""

    $timestamp = Get-Date -Format "yyyyMMdd_HHmmss"
    $outputFile = Join-Path $OutputDir "baseline_interactive_${timestamp}.txt"

    $lines = @()
    $lines += "=== WuBigCat P0 Baseline - Interactive Mode ==="
    $lines += "Timestamp: $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')"
    $lines += "Hostname: $env:COMPUTERNAME"
    $lines += "OS: $([System.Environment]::OSVersion.VersionString)"
    $lines += ""

    # Idle phase
    Write-Step "IDLE phase: Confirm the application is connected with NO motion commands."
    Write-Step "Press Enter when ready to record idle metrics"
    Read-Host
    Write-Info "Record the following values (observe max over 10-20 seconds):"
    $idleComms = Read-Host "  Comms Time (us)"
    $idleCycle = Read-Host "  Actual Cycle (us)"
    $idleMaxCycle = Read-Host "  Max Cycle (us)"
    $idleWkc = Read-Host "  WKC"

    $lines += "[IDLE]"
    $lines += "  commsTime=$idleComms"
    $lines += "  actualCycle=$idleCycle"
    $lines += "  maxCycle=$idleMaxCycle"
    $lines += "  WKC=$idleWkc"
    $lines += ""

    # Motion phase
    if ($MotionDurationSec -gt 0) {
        Write-Step "MOTION phase: Start representative motion (e.g. small sine oscillation +/-2 deg)."
        Write-Step "Press Enter when motion is stable to record motion metrics"
        Read-Host
        Write-Info "Record the following values (observe max during motion):"
        $motionComms = Read-Host "  Comms Time (us)"
        $motionCycle = Read-Host "  Actual Cycle (us)"
        $motionMaxCycle = Read-Host "  Max Cycle (us)"
        $motionWkc = Read-Host "  WKC"

        $lines += "[MOTION]"
        $lines += "  commsTime=$motionComms"
        $lines += "  actualCycle=$motionCycle"
        $lines += "  maxCycle=$motionMaxCycle"
        $lines += "  WKC=$motionWkc"
    }

    $utf8 = New-Object System.Text.UTF8Encoding($false)
    [System.IO.File]::WriteAllLines($outputFile, $lines, $utf8)
    Write-OK "Interactive recording saved: $outputFile"
}

# ============================================================================
# Main
# ============================================================================

function Main {
    Write-Banner "WuBigCat P0 EtherCAT Cycle Stability Baseline Measurement"

    if ($SkipHttp) {
        Start-InteractiveBaseline
        return
    }

    # Check if app is running
    Write-Step "Checking application HTTP API (${HttpHost}:${HttpPort}) ..."
    if (-not (Wait-ForAppReady -TimeoutSec 30)) {
        Write-Warn "Cannot connect to the application HTTP API."
        $choice = Read-Host "Switch to interactive manual recording mode? (Y/n)"
        if ($choice -ne 'n' -and $choice -ne 'N') {
            Start-InteractiveBaseline
        } else {
            Write-Host "Cancelled." -ForegroundColor Red
        }
        return
    }

    # Collect system info
    $testConditions = Get-SystemTestConditions

    # Display test conditions
    Write-Host ""
    Write-Host "  --- Test Conditions ---" -ForegroundColor Cyan
    foreach ($kv in $testConditions.GetEnumerator()) {
        Write-Host "  $($kv.Key): $($kv.Value)" -ForegroundColor Gray
    }
    Write-Host ""

    # Confirm before starting
    $confirm = Read-Host "Press Enter to start measurement, or type 'q' to cancel"
    if ($confirm -eq 'q') { return }

    # Warmup
    if ($WarmupSec -gt 0) {
        Write-Step "Warming up (${WarmupSec}s) ..."
        $warmupDeadline = (Get-Date).AddSeconds($WarmupSec)
        while ((Get-Date) -lt $warmupDeadline) {
            $remaining = [int](($warmupDeadline - (Get-Date)).TotalSeconds)
            Write-Progress -Activity "Warming up" -Status "${remaining}s remaining" `
                -PercentComplete (100 - $remaining * 100 / $WarmupSec)
            Start-Sleep -Milliseconds 500
        }
        Write-Progress -Activity "Warming up" -Completed
        Write-OK "Warmup complete"
    }

    # ---- Idle scenario ----
    Write-Host ""
    Write-Banner "Scenario 1: IDLE (no motion)"
    Write-Host "  Confirm EtherCAT is connected and Operation Enabled (Online)." -ForegroundColor Yellow
    Write-Host "  NO motion commands should be active." -ForegroundColor Yellow

    if (-not (Assert-EtherCatOnline)) {
        $choice = Read-Host "  Continue with idle measurement anyway? (y/N)"
        if ($choice -ne 'y' -and $choice -ne 'Y') {
            Write-Host "  Skipping idle measurement." -ForegroundColor Yellow
            $idleResult = $null
        } else {
            Read-Host "  Press Enter to start idle measurement"
            $idleResult = Invoke-MetricsCollection -DurationSec $IdleDurationSec -ScenarioName "idle"
        }
    } else {
        Read-Host "  Press Enter to start idle measurement"
        $idleResult = Invoke-MetricsCollection -DurationSec $IdleDurationSec -ScenarioName "idle"
    }

    if ($idleResult) {
        Assert-MetricsNonZero -Result $idleResult -ScenarioName "idle"
    }

    # ---- Motion scenario ----
    $motionResult = $null
    if ($MotionDurationSec -gt 0) {
        Write-Host ""
        Write-Banner "Scenario 2: MOTION (representative movement)"
        Write-Host "  Start representative motion (e.g. platform small sine oscillation +/-2 deg)." -ForegroundColor Yellow
        Write-Host "  Wait for motion to stabilize, then press Enter." -ForegroundColor Yellow
        Read-Host "  Press Enter to start motion measurement"

        $motionResult = Invoke-MetricsCollection -DurationSec $MotionDurationSec -ScenarioName "motion"
        if ($motionResult) {
            Assert-MetricsNonZero -Result $motionResult -ScenarioName "motion"
        }
    }

    # ---- Generate report ----
    Write-Host ""
    Write-Banner "Generating Baseline Report"

    $timestamp = if ($BaselineName) { $BaselineName } else { "baseline_$(Get-Date -Format 'yyyyMMdd_HHmmss')" }
    $outputPath = Join-Path $OutputDir "${timestamp}.json"
    Export-BaselineReport -IdleResult $idleResult -MotionResult $motionResult `
        -TestConditions $testConditions -OutputPath $outputPath

    # ---- Print summary ----
    Write-Host ""
    Write-Host "  --- Measurement Summary ---" -ForegroundColor Cyan
        $allZeroWarning = $false
        foreach ($scenarioName in @('idle', 'motion')) {
            $result = if ($scenarioName -eq 'idle') { $idleResult } else { $motionResult }
            if (-not $result) { continue }
            Write-Host "  [$scenarioName] samples: $($result.SampleCount)" -ForegroundColor White

            $allZero = $true
            foreach ($metric in @('commsTime', 'actualCycleTime', 'maxObservedCycleTime')) {
                $stats = Measure-SampleStatistics -Samples $result.Samples -FieldName $metric
                Write-Host "    $metric`: avg=$($stats.Avg), max=$($stats.Max), p95=$($stats.P95), stddev=$($stats.StdDev)" -ForegroundColor Gray
                if ($stats.Max -gt 0) { $allZero = $false }
            }
            if ($allZero) {
                Write-Warn "  [$scenarioName] WARNING: All metrics are zero -- measurement may be invalid"
                Write-Warn "  This usually means the MainWindow UI timer is not publishing cycle metrics."
                Write-Warn "  Ensure the PC application window is open and EtherCAT is in Operational state."
                $allZeroWarning = $true
            }
        }
        if ($allZeroWarning) {
            Write-Host ""
            Write-Warn "=== DIAGNOSTIC: Why are metrics zero? ==="
            Write-Warn "  /ui_state endpoint returned cycle data but commsTime=0."
            Write-Warn "  This happens when MainWindow's UI timer hasn't updated g_commsTimeUs."
            Write-Warn "  Possible causes:"
            Write-Warn "    1. MainWindow is minimized or hidden (suppressMainWindowLiveUi=true)"
            Write-Warn "    2. cyclicPdoCycleDetectReady is false (PDO cycle not detected)"
            Write-Warn "    3. MasterGetCommsTime() returns 0 (cyclic thread not posting metrics)"
            Write-Warn "  Check the PC application window - ensure it's visible and shows cycle values."
        }

    # ---- Compare with previous baseline ----
    if ($CompareWith) {
        Write-Host ""
        Write-Banner "Baseline Comparison"
        $passed = Compare-Baselines -NewBaselinePath $outputPath -OldBaselinePath $CompareWith
        if (-not $passed) {
            Write-Host ""
            Write-Warn "Regression check failed. Review system state and re-measure."
            exit 1
        }
    }

    Write-Host ""
    Write-OK "P0 baseline measurement complete!"
    Write-Info "Report saved to: $outputPath"
    Write-Info "Next time, use: -CompareWith '$outputPath' for regression comparison."
}

Main
