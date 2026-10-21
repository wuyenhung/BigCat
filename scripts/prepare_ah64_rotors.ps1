<#
.SYNOPSIS
    Re-home AH-64.glb main rotor and tail rotor meshes so the viewer can spin
    them around their own hubs, and rename the nodes MainRotor / TailRotor.

.DESCRIPTION
    In AH-64.glb, every mesh sits under an identical parent transform (scale
    35.2, 180deg rotation, translation) plus a root scale of 0.001. The main
    rotor (node_id27, mesh 5) is a disc in the local XY plane whose center is
    offset from the node origin; it must spin around local Z (which maps to
    world up). The tail rotor (node_id19, mesh 3) is a disc in the local YZ
    plane whose center is also offset; it must spin around local X.

    This script shifts each rotor mesh's positions in place so the blade center
    moves to the node origin, and sets the node translation to the former blade
    center so world placement is unchanged. It also renames the nodes.

    Only the position bytes are rewritten (same size); normals/indices untouched.

.PARAMETER GlbPath
    Path to the .glb file to modify (overwritten in place; back it up first).

.EXAMPLE
    .\scripts\prepare_ah64_rotors.ps1 -GlbPath .\resources\web\AH-64.glb
#>
param(
    [Parameter(Mandatory = $true)]
    [string]$GlbPath
)

$ErrorActionPreference = 'Stop'

# ============ Read GLB ============
$raw = [System.IO.File]::ReadAllBytes($GlbPath)
if ([System.Text.Encoding]::ASCII.GetString($raw, 0, 4) -ne 'glTF') { throw "Not a GLB file: $GlbPath" }
$version = [BitConverter]::ToUInt32($raw, 4)
$chunk0Len = [BitConverter]::ToUInt32($raw, 12)
if ([System.Text.Encoding]::ASCII.GetString($raw, 16, 4) -ne 'JSON') { throw 'chunk0 is not JSON' }
$jsonText = [System.Text.Encoding]::UTF8.GetString($raw, 20, $chunk0Len)
$binOff = 20 + $chunk0Len
$binLen = [BitConverter]::ToUInt32($raw, $binOff)
if ([System.Text.Encoding]::ASCII.GetString($raw, $binOff + 4, 4) -ne 'BIN') { throw 'chunk1 is not BIN' }
$bin = New-Object byte[] $binLen
[Array]::Copy($raw, $binOff + 8, $bin, 0, $binLen)
$gltf = $jsonText | ConvertFrom-Json

# ============ Re-home a rotor mesh ============
# Shifts the mesh positions so the geometry box center sits at the node origin,
# sets the node translation to that center (world placement unchanged),
# updates the position accessor min/max, and renames the node.
function Set-RotorMeshOrigin([int]$accPosIndex, [int]$nodeIndex, [string]$newName) {
    $acc = $gltf.accessors[$accPosIndex]
    $bv = $gltf.bufferViews[$acc.bufferView]
    $off = [int]$bv.byteOffset + [int]$acc.byteOffset
    $count = [int]$acc.count

    $minX = [double]$acc.min[0]; $minY = [double]$acc.min[1]; $minZ = [double]$acc.min[2]
    $maxX = [double]$acc.max[0]; $maxY = [double]$acc.max[1]; $maxZ = [double]$acc.max[2]
    $cX = ($minX + $maxX) / 2.0
    $cY = ($minY + $maxY) / 2.0
    $cZ = ($minZ + $maxZ) / 2.0

    # Shift positions in place (float32 triples)
    for ($i = 0; $i -lt $count; $i++) {
        $o = $off + $i * 12
        $x = [BitConverter]::ToSingle($bin, $o) - [single]$cX
        $y = [BitConverter]::ToSingle($bin, $o + 4) - [single]$cY
        $z = [BitConverter]::ToSingle($bin, $o + 8) - [single]$cZ
        [BitConverter]::GetBytes([single]$x).CopyTo($bin, $o)
        [BitConverter]::GetBytes([single]$y).CopyTo($bin, $o + 4)
        [BitConverter]::GetBytes([single]$z).CopyTo($bin, $o + 8)
    }

    # Update accessor min/max (shifted by -C)
    $acc | Add-Member -NotePropertyName min -NotePropertyValue @(($minX - $cX), ($minY - $cY), ($minZ - $cZ)) -Force
    $acc | Add-Member -NotePropertyName max -NotePropertyValue @(($maxX - $cX), ($maxY - $cY), ($maxZ - $cZ)) -Force

    # Node: rename + set translation to former blade center
    $node = $gltf.nodes[$nodeIndex]
    $node | Add-Member -NotePropertyName name -NotePropertyValue $newName -Force
    $node | Add-Member -NotePropertyName translation -NotePropertyValue @($cX, $cY, $cZ) -Force

    Write-Output ("re-homed '{0}' (node {1}, accessor {2}): center=({3:F4},{4:F4},{5:F4})" -f $newName, $nodeIndex, $accPosIndex, $cX, $cY, $cZ)
}

# Main rotor: mesh 5, accessor 20, node 15. Tail rotor: mesh 3, accessor 12, node 9.
Set-RotorMeshOrigin 20 15 'MainRotor'
Set-RotorMeshOrigin 12 9 'TailRotor'

# ============ Serialize JSON ============
$newJson = $gltf | ConvertTo-Json -Depth 100 -Compress
$jsonBytes = [System.Text.Encoding]::UTF8.GetBytes($newJson)
$pad = (4 - ($jsonBytes.Length % 4)) % 4
$jsonPadded = New-Object byte[] ($jsonBytes.Length + $pad)
[Array]::Copy($jsonBytes, $jsonPadded, $jsonBytes.Length)
for ($i = $jsonBytes.Length; $i -lt $jsonPadded.Length; $i++) { $jsonPadded[$i] = 0x20 }

# ============ Rebuild GLB (BIN length unchanged) ============
$total = 12 + 8 + $jsonPadded.Length + 8 + $binLen
$out = New-Object byte[] $total
[System.Text.Encoding]::ASCII.GetBytes('glTF').CopyTo($out, 0)
[BitConverter]::GetBytes([uint32]$version).CopyTo($out, 4)
[BitConverter]::GetBytes([uint32]$total).CopyTo($out, 8)
[BitConverter]::GetBytes([uint32]$jsonPadded.Length).CopyTo($out, 12)
[System.Text.Encoding]::ASCII.GetBytes('JSON').CopyTo($out, 16)
$jsonPadded.CopyTo($out, 20)
[BitConverter]::GetBytes([uint32]$binLen).CopyTo($out, 20 + $jsonPadded.Length)
[System.Text.Encoding]::ASCII.GetBytes('BIN').CopyTo($out, 24 + $jsonPadded.Length)
$bin.CopyTo($out, 28 + $jsonPadded.Length)

[System.IO.File]::WriteAllBytes($GlbPath, $out)
Write-Output ("written: {0} ({1} bytes)" -f $GlbPath, $total)
