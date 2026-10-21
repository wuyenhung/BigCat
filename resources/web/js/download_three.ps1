# Download three.min.js (r128) + GLTFLoader.js into this script's js/ folder (offline support)
# Usage: Run from PowerShell:  .\download_three.ps1  (path is resolved via $PSScriptRoot)

$uri = 'https://cdnjs.cloudflare.com/ajax/libs/three.js/r128/three.min.js'
$out = Join-Path -Path $PSScriptRoot -ChildPath 'three.min.js'
Write-Host "Downloading $uri to $out ..."
try {
    Invoke-WebRequest -Uri $uri -OutFile $out -UseBasicParsing -ErrorAction Stop
    Write-Host "Downloaded three.min.js successfully."
} catch {
    Write-Error "Failed to download three.min.js: $_"
}

# GLTFLoader 用於載入 .glb 模型（viewer_6dof.html 需要）
# 注意：cdnjs 的 three.js r128 不含 examples/js/loaders/GLTFLoader.js，改用 jsdelivr（同版本）
$gltfUri = 'https://cdn.jsdelivr.net/npm/three@0.128.0/examples/js/loaders/GLTFLoader.js'
$gltfOut = Join-Path -Path $PSScriptRoot -ChildPath 'GLTFLoader.js'
Write-Host "Downloading $gltfUri to $gltfOut ..."
try {
    Invoke-WebRequest -Uri $gltfUri -OutFile $gltfOut -UseBasicParsing -ErrorAction Stop
    Write-Host "Downloaded GLTFLoader.js successfully."
} catch {
    Write-Error "Failed to download GLTFLoader.js: $_"
}
