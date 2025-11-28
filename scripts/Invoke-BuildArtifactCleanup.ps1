Set-StrictMode -Version 3.0

function Get-BuildCleanupBytes {
    param([string]$Path)

    if (-not (Test-Path -LiteralPath $Path)) {
        return 0L
    }

    $item = Get-Item -LiteralPath $Path -Force -ErrorAction SilentlyContinue
    if (-not $item) {
        return 0L
    }

    if (-not $item.PSIsContainer) {
        return [int64]$item.Length
    }

    $measure = Get-ChildItem -LiteralPath $Path -Recurse -File -Force -ErrorAction SilentlyContinue |
        Measure-Object -Property Length -Sum
    if ($null -eq $measure) {
        return 0L
    }

    $sum = $measure.Sum
    if ($null -eq $sum) {
        return 0L
    }

    return [int64]$sum
}

function Remove-BuildCleanupPath {
    param(
        [string]$Path,
        [hashtable]$Stats
    )

    if (-not (Test-Path -LiteralPath $Path)) {
        return
    }

    $item = Get-Item -LiteralPath $Path -Force -ErrorAction SilentlyContinue
    if (-not $item) {
        return
    }

    $bytes = Get-BuildCleanupBytes -Path $Path

    try {
        Remove-Item -LiteralPath $Path -Recurse -Force -ErrorAction Stop
        if ($item.PSIsContainer) {
            $Stats.Directories++
        } else {
            $Stats.Files++
        }
        $Stats.Bytes += $bytes
    } catch {
        Write-Warning "[cleanup] Failed to remove '$Path': $_"
    }
}

function Remove-BuildCleanupMatches {
    param(
        [string]$RootPath,
        [string[]]$DirectoryPatterns,
        [string[]]$FilePatterns,
        [hashtable]$Stats
    )

    if (-not (Test-Path -LiteralPath $RootPath)) {
        return
    }

    if ($DirectoryPatterns) {
        $dirs = @()
        foreach ($pattern in $DirectoryPatterns) {
            $dirs += Get-ChildItem -Path $RootPath -Recurse -Directory -Filter $pattern -Force -ErrorAction SilentlyContinue
        }

        $dirs |
            Sort-Object @{ Expression = { $_.FullName.Length }; Descending = $true }, @{ Expression = { $_.FullName }; Descending = $false } |
            Select-Object -Unique -ExpandProperty FullName |
            ForEach-Object { Remove-BuildCleanupPath -Path $_ -Stats $Stats }
    }

    if ($FilePatterns) {
        $files = @()
        foreach ($pattern in $FilePatterns) {
            $files += Get-ChildItem -Path $RootPath -Recurse -File -Filter $pattern -Force -ErrorAction SilentlyContinue
        }

        $files |
            Sort-Object FullName |
            Select-Object -Unique -ExpandProperty FullName |
            ForEach-Object { Remove-BuildCleanupPath -Path $_ -Stats $Stats }
    }
}

function Invoke-BuildArtifactCleanup {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)]
        [string]$BuildDir,
        [string]$Config = 'Release',
        [bool]$PreservePdb = $true,
        [bool]$PreserveCrashDumps = $true
    )

    $resolvedBuildDir = Resolve-Path -Path $BuildDir -ErrorAction SilentlyContinue | Select-Object -ExpandProperty ProviderPath -First 1
    if (-not $resolvedBuildDir) {
        Write-Host "[cleanup] Skipped: build directory not found: $BuildDir" -ForegroundColor Yellow
        return [pscustomobject]@{
            BuildDir = $BuildDir
            Config = $Config
            RemovedDirectories = 0
            RemovedFiles = 0
            ReclaimedBytes = 0L
        }
    }

    $stats = @{
        Directories = 0
        Files = 0
        Bytes = 0L
    }

    Write-Host "[cleanup] Pruning intermediate build artifacts under $resolvedBuildDir" -ForegroundColor Yellow

    $explicitDirectories = @(
        (Join-Path $resolvedBuildDir 'Testing\Temporary'),
        (Join-Path $resolvedBuildDir 'CMakeFiles\CMakeScratch'),
        (Join-Path $resolvedBuildDir 'CMakeFiles\ShowIncludes'),
        (Join-Path $resolvedBuildDir 'CMakeFiles\pkgRedirects')
    )
    foreach ($path in $explicitDirectories) {
        Remove-BuildCleanupPath -Path $path -Stats $stats
    }

    Remove-BuildCleanupMatches -RootPath $resolvedBuildDir -DirectoryPatterns @('*.tlog', 'ipch') -FilePatterns @() -Stats $stats

    $filePatterns = @(
        '*.obj',
        '*.o',
        '*.res',
        '*.ilk',
        '*.idb',
        '*.iobj',
        '*.ipdb',
        '*.tmp'
    )
    if (-not $PreservePdb) {
        $filePatterns += '*.pdb'
    }
    if (-not $PreserveCrashDumps) {
        $filePatterns += '*.dmp'
    }

    Remove-BuildCleanupMatches -RootPath $resolvedBuildDir -DirectoryPatterns @() -FilePatterns $filePatterns -Stats $stats

    $reclaimedMb = [math]::Round(($stats.Bytes / 1MB), 2)
    Write-Host "[cleanup] Removed $($stats.Directories) directories, $($stats.Files) files, reclaimed ${reclaimedMb} MB" -ForegroundColor Green

    return [pscustomobject]@{
        BuildDir = $resolvedBuildDir
        Config = $Config
        RemovedDirectories = $stats.Directories
        RemovedFiles = $stats.Files
        ReclaimedBytes = $stats.Bytes
    }
}