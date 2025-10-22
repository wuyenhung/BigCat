# test_build.ps1 - Test build script with local tinyxml2
$ErrorActionPreference = "Stop"

Write-Host "=== Testing SOEM EtherCAT Project Compilation ===" -ForegroundColor Cyan
Write-Host "Start: $(Get-Date -Format 'HH:mm:ss')" -ForegroundColor Gray

# Use vcvarsall.bat for better environment initialization (ensures INCLUDE/LIB/PATH are set correctly)
$vcvarsall = "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat"
$buildDir = "build"

# Step 1: Clean
Write-Host "`n[1/3] Cleaning build directory..." -ForegroundColor Yellow
$buildDirPath = Join-Path $PSScriptRoot $buildDir
if (Test-Path $buildDirPath) { 
    Remove-Item -Recurse -Force $buildDirPath 
    Write-Host "      Cleaned" -ForegroundColor Green
} else {
    Write-Host "      Skip (no build dir)" -ForegroundColor Green
}

# Step 2: Configure with local tinyxml2
Write-Host "`n[2/3] Configuring CMake (using local tinyxml2)..." -ForegroundColor Yellow
$projectRoot = $PSScriptRoot
& 'C:\Windows\System32\cmd.exe' /c "`"$vcvarsall`" x64 && cd /d $projectRoot && cmake -S . -B $buildDir -G `"Visual Studio 17 2022`" -A x64 -DCMAKE_BUILD_TYPE=Release -DUSE_LOCAL_TINYXML2=ON"

if ($LASTEXITCODE -ne 0) { 
    Write-Host "`n[ERROR] CMake configuration failed!" -ForegroundColor Red
    exit 1 
}
Write-Host "      Configuration complete" -ForegroundColor Green

# Step 3: Build
Write-Host "`n[3/3] Building project..." -ForegroundColor Yellow  
$projectRoot = $PSScriptRoot
& 'C:\Windows\System32\cmd.exe' /c "`"$vcvarsall`" x64 && cd /d $projectRoot && cmake --build $buildDir --config Release"

if ($LASTEXITCODE -ne 0) { 
    Write-Host "`n[ERROR] Build failed!" -ForegroundColor Red
    exit 1 
}
Write-Host "      Build complete" -ForegroundColor Green

# Check result
Write-Host "`n=== Build Result ===" -ForegroundColor Cyan
$exePath = Join-Path $PSScriptRoot (Join-Path $buildDir "Release\bin\main\BigCat.exe")
Write-Host "Looking for executable at: $exePath" -ForegroundColor Gray
if (Test-Path $exePath) {
    $info = Get-Item $exePath
    Write-Host "[SUCCESS] Compilation successful!" -ForegroundColor Green
    Write-Host "  Executable: $($info.FullName)" -ForegroundColor Gray
    Write-Host "  Size: $([math]::Round($info.Length / 1KB, 2)) KB" -ForegroundColor Gray
    Write-Host "  Created: $($info.LastWriteTime.ToString('yyyy-MM-dd HH:mm:ss'))" -ForegroundColor Gray
} else {
    Write-Host "[ERROR] Executable not found at: $exePath" -ForegroundColor Red
    Write-Host "  Script root: $PSScriptRoot" -ForegroundColor Red
    Write-Host "  Build directory contents:" -ForegroundColor Red
    $fullBuildDir = Join-Path $PSScriptRoot $buildDir
    if (Test-Path $fullBuildDir) {
        Get-ChildItem $fullBuildDir -Recurse -Filter *.exe | Select-Object FullName | ForEach-Object { Write-Host "    $($_.FullName)" -ForegroundColor Red }
    } else {
        Write-Host "    Build directory not found: $fullBuildDir" -ForegroundColor Red
    }
    exit 1
}

function Copy-RequiredDlls {
    param(
        [string]$buildDirPath
    )

    # 要複製的 DLL 清單（可根據需要擴充）
    $dlls = @('wpcap.dll', 'Packet.dll')

    # 可能的來源路徑（會依序搜尋）
    $candidateDirs = @(
    (Join-Path $PSScriptRoot 'lib\npcap-sdk'),
        "$env:ProgramFiles\Npcap",
        "$env:ProgramFiles(x86)\Npcap",
        'C:\Windows\System32',
        'C:\Windows\SysWOW64'
    )

    $destDir = Join-Path $PSScriptRoot (Join-Path $buildDirPath 'Release\bin\main')
    if (-not (Test-Path $destDir)) {
        New-Item -ItemType Directory -Path $destDir -Force | Out-Null
    }

    foreach ($dll in $dlls) {
        $found = $false
        foreach ($d in $candidateDirs) {
            if ([string]::IsNullOrEmpty($d)) { continue }
            $src = Join-Path $d $dll
            if (Test-Path $src) {
                try {
                    Copy-Item -Path $src -Destination $destDir -Force
                    Write-Host "Copied $dll from $src to $destDir" -ForegroundColor Green
                    $found = $true
                    break
                } catch {
                    Write-Warning ("Failed to copy {0} from {1}: {2}" -f $dll, $src, $_)
                }
            }
        }
        if (-not $found) {
            Write-Warning "Could not find $dll in any candidate paths. Please install/locate it and copy manually."
        }
    }
}

# 嘗試自動複製執行檔所需的本機 DLL（例如 wpcap.dll）
# 控制是否自動複製 wpcap/Packet DLL。預設為 false，避免在 build 時自動複製。
$COPY_WPCAP_DLLS = $false
if ($COPY_WPCAP_DLLS) {
    Copy-RequiredDlls -buildDirPath $buildDir
} else {
    Write-Host "Skipping automatic copy of wpcap.dll and Packet.dll (COPY_WPCAP_DLLS = $COPY_WPCAP_DLLS)" -ForegroundColor Yellow
}

Write-Host "`nEnd: $(Get-Date -Format 'HH:mm:ss')" -ForegroundColor Gray
Write-Host "=== Test Complete ===" -ForegroundColor Cyan
