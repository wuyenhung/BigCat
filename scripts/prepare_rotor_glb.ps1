<#
.SYNOPSIS
    Split ka-52.glb main-rotor meshes so the coaxial upper/lower rotors can
    counter-rotate as separate animation nodes.

.DESCRIPTION
    The Ka-52 has a coaxial main rotor (3 upper blades + 3 lower blades).
    In this GLB, all 6 blades are merged into one mesh (node 11) and there is
    a second mesh (node 7) holding 6 small blade-tip pieces. This script splits
    both meshes into upper/lower groups by triangle centroid Y, rebuilds them
    as two nodes MainRotorUpper / MainRotorLower (each with 3 blades + 3 tips),
    and shifts the tips onto the unified rotor axis (node 11 origin) so the
    two rotors can spin in opposite directions.

    Only appends new BIN data; original geometry is preserved. Output overwrites
    the input GLB in place.

.PARAMETER GlbPath
    Path to the .glb file to modify (overwritten in place; back it up first).

.EXAMPLE
    .\scripts\prepare_rotor_glb.ps1 -GlbPath .\resources\web\ka-52.glb
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

# ============ Read accessor data ============
function Read-Accessor([int]$accIndex, $gltf, [byte[]]$bin) {
    $acc = $gltf.accessors[$accIndex]
    $bv = $gltf.bufferViews[$acc.bufferView]
    $off = [int]$bv.byteOffset + [int]$acc.byteOffset
    $count = [int]$acc.count
    $nc = @{ SCALAR = 1; VEC2 = 2; VEC3 = 3; VEC4 = 4 }[[string]$acc.type]
    if ([int]$acc.componentType -eq 5126) {
        $v = New-Object float[] ($count * $nc)
        for ($i = 0; $i -lt $v.Length; $i++) { $v[$i] = [BitConverter]::ToSingle($bin, $off + $i * 4) }
        Write-Output -NoEnumerate $v
    } elseif ([int]$acc.componentType -eq 5123) {
        $v = New-Object uint16[] ($count * $nc)
        for ($i = 0; $i -lt $v.Length; $i++) { $v[$i] = [BitConverter]::ToUInt16($bin, $off + $i * 2) }
        Write-Output -NoEnumerate $v
    } elseif ([int]$acc.componentType -eq 5125) {
        $v = New-Object uint32[] ($count * $nc)
        for ($i = 0; $i -lt $v.Length; $i++) { $v[$i] = [BitConverter]::ToUInt32($bin, $off + $i * 4) }
        Write-Output -NoEnumerate $v
    } else {
        throw "Unsupported componentType: $($acc.componentType)"
    }
}

$pos11 = Read-Accessor 38 $gltf $bin   # mesh 11 positions (6 blades)
$nrm11 = Read-Accessor 39 $gltf $bin
$idx11 = Read-Accessor 41 $gltf $bin
$pos7 = Read-Accessor 25 $gltf $bin    # mesh 7 positions (6 blade tips)
$nrm7 = Read-Accessor 26 $gltf $bin
$idx7 = Read-Accessor 27 $gltf $bin

# ============ Split by triangle centroid Y ============
function Split-ByCentroidY([float[]]$pos, [float[]]$nrm, [uint16[]]$idx) {
    $tri = [int]($idx.Length / 3)
    $up = New-Object System.Collections.Generic.List[uint16]
    $lo = New-Object System.Collections.Generic.List[uint16]
    for ($t = 0; $t -lt $tri; $t++) {
        $i0 = [int]$idx[$t * 3]; $i1 = [int]$idx[$t * 3 + 1]; $i2 = [int]$idx[$t * 3 + 2]
        $cy = ($pos[$i0 * 3 + 1] + $pos[$i1 * 3 + 1] + $pos[$i2 * 3 + 1]) / 3.0
        if ($cy -gt 0.0) { $up.Add($idx[$t * 3]); $up.Add($idx[$t * 3 + 1]); $up.Add($idx[$t * 3 + 2]) }
        else { $lo.Add($idx[$t * 3]); $lo.Add($idx[$t * 3 + 1]); $lo.Add($idx[$t * 3 + 2]) }
    }
    $res = @{}
    foreach ($nm in @('upper', 'lower')) {
        $src = if ($nm -eq 'upper') { $up } else { $lo }
        $map = @{}
        $p = New-Object System.Collections.Generic.List[float]
        $n = New-Object System.Collections.Generic.List[float]
        $x = New-Object System.Collections.Generic.List[uint16]
        foreach ($oi in $src) {
            $o = [int]$oi
            if (-not $map.ContainsKey($o)) {
                $map[$o] = $p.Count / 3
                $p.Add($pos[$o * 3]); $p.Add($pos[$o * 3 + 1]); $p.Add($pos[$o * 3 + 2])
                $n.Add($nrm[$o * 3]); $n.Add($nrm[$o * 3 + 1]); $n.Add($nrm[$o * 3 + 2])
            }
            $x.Add([uint16]$map[$o])
        }
        $res[$nm] = @{ pos = $p.ToArray(); nrm = $n.ToArray(); idx = $x.ToArray() }
    }
    return $res
}

function Get-MinMax([float[]]$pos) {
    $min = @([float]::MaxValue, [float]::MaxValue, [float]::MaxValue)
    $max = @([float]::MinValue, [float]::MinValue, [float]::MinValue)
    for ($i = 0; $i -lt $pos.Length; $i += 3) {
        for ($k = 0; $k -lt 3; $k++) {
            if ($pos[$i + $k] -lt $min[$k]) { $min[$k] = $pos[$i + $k] }
            if ($pos[$i + $k] -gt $max[$k]) { $max[$k] = $pos[$i + $k] }
        }
    }
    return @{ min = $min; max = $max }
}

function ConvertTo-FloatBytes([float[]]$a) {
    $b = New-Object byte[] ($a.Length * 4)
    for ($i = 0; $i -lt $a.Length; $i++) { [BitConverter]::GetBytes($a[$i]).CopyTo($b, $i * 4) }
    Write-Output -NoEnumerate $b
}

function ConvertTo-UInt16Bytes([uint16[]]$a) {
    $b = New-Object byte[] ($a.Length * 2)
    for ($i = 0; $i -lt $a.Length; $i++) { [BitConverter]::GetBytes($a[$i]).CopyTo($b, $i * 2) }
    Write-Output -NoEnumerate $b
}

# ============ Unify rotor axis, shift tips ============
$node7 = $gltf.nodes[7]
$node11 = $gltf.nodes[11]
$S = [double]$node11.scale[0]
$shift = @(
    (([double]$node7.translation[0] - [double]$node11.translation[0]) / $S),
    (([double]$node7.translation[1] - [double]$node11.translation[1]) / $S),
    (([double]$node7.translation[2] - [double]$node11.translation[2]) / $S)
)
for ($i = 0; $i -lt $pos7.Length; $i += 3) {
    $pos7[$i] += $shift[0]; $pos7[$i + 1] += $shift[1]; $pos7[$i + 2] += $shift[2]
}

$split11 = Split-ByCentroidY $pos11 $nrm11 $idx11
$split7 = Split-ByCentroidY $pos7 $nrm7 $idx7
Write-Output ("mesh11 split: upper={0} tris / lower={1} tris" -f ($split11.upper.idx.Length / 3), ($split11.lower.idx.Length / 3))
Write-Output ("mesh7  split: upper={0} tris / lower={1} tris" -f ($split7.upper.idx.Length / 3), ($split7.lower.idx.Length / 3))

# ============ Append BIN data ============
$append = New-Object System.Collections.Generic.List[byte]
function Add-Data([byte[]]$d) {
    while (($append.Count % 4) -ne 0) { $append.Add(0) }
    $off = $append.Count
    $append.AddRange($d)
    return $off
}

# Ensure arrays so += works
$gltf.bufferViews = @($gltf.bufferViews)
$gltf.accessors = @($gltf.accessors)
$gltf.meshes = @($gltf.meshes)
$gltf.nodes = @($gltf.nodes)

function New-BufferViewObj([int]$off, [int]$len, [int]$target) {
    $o = [PSCustomObject]@{ buffer = 0; byteOffset = $off; byteLength = $len }
    if ($target -ne 0) { $o | Add-Member -NotePropertyName target -NotePropertyValue $target }
    return $o
}

function New-AccessorObj([int]$bv, [string]$type, [int]$ct, [int]$count, [int]$byteOffset, $min, $max) {
    $o = [PSCustomObject]@{}
    if ($null -ne $min) { $o | Add-Member -NotePropertyName min -NotePropertyValue $min }
    if ($null -ne $max) { $o | Add-Member -NotePropertyName max -NotePropertyValue $max }
    $o | Add-Member -NotePropertyName bufferView -NotePropertyValue $bv
    if ($byteOffset -ne 0) { $o | Add-Member -NotePropertyName byteOffset -NotePropertyValue $byteOffset }
    $o | Add-Member -NotePropertyName componentType -NotePropertyValue $ct
    $o | Add-Member -NotePropertyName count -NotePropertyValue $count
    $o | Add-Member -NotePropertyName type -NotePropertyValue $type
    return $o
}

# Append one vertex group (pos/nrm/idx) and return the new accessor indices
function Add-VertexGroup([float[]]$pos, [float[]]$nrm, [uint16[]]$idx) {
    $pB = ConvertTo-FloatBytes $pos
    $nB = ConvertTo-FloatBytes $nrm
    $iB = ConvertTo-UInt16Bytes $idx
    $pOff = Add-Data $pB
    $nOff = Add-Data $nB
    $iOff = Add-Data $iB
    $mm = Get-MinMax $pos

    # byteOffset 必須是 buffer 內絕對偏移：原始 BIN 長度 + 追加區塊內偏移
    $baseOff = $bin.Length
    $gltf.bufferViews += (New-BufferViewObj ($baseOff + $pOff) $pB.Length 34962)
    $gltf.bufferViews += (New-BufferViewObj ($baseOff + $nOff) $nB.Length 34962)
    $gltf.bufferViews += (New-BufferViewObj ($baseOff + $iOff) $iB.Length 34963)
    $bvP = $gltf.bufferViews.Count - 3
    $bvN = $gltf.bufferViews.Count - 2
    $bvI = $gltf.bufferViews.Count - 1

    $gltf.accessors += (New-AccessorObj $bvP 'VEC3' 5126 ([int]($pos.Length / 3)) 0 $mm.min $mm.max)
    $accP = $gltf.accessors.Count - 1
    $gltf.accessors += (New-AccessorObj $bvN 'VEC3' 5126 ([int]($nrm.Length / 3)) 0 $null $null)
    $accN = $gltf.accessors.Count - 1
    $gltf.accessors += (New-AccessorObj $bvI 'SCALAR' 5123 $idx.Length 0 $null $null)
    $accI = $gltf.accessors.Count - 1

    return @{ pos = $accP; nrm = $accN; idx = $accI }
}

$rotors = @(
    @{ name = 'MainRotorUpper'; blades = $split11.upper; tips = $split7.upper },
    @{ name = 'MainRotorLower'; blades = $split11.lower; tips = $split7.lower }
)

$newNodeIndices = New-Object System.Collections.Generic.List[int]
foreach ($r in $rotors) {
    $b = Add-VertexGroup $r.blades.pos $r.blades.nrm $r.blades.idx   # material 3 (blades)
    $t = Add-VertexGroup $r.tips.pos $r.tips.nrm $r.tips.idx         # material 7 (tips)
    $gltf.meshes += ([PSCustomObject]@{
            name       = "$($r.name)Mesh"
            primitives = @(
                [PSCustomObject]@{ attributes = ([PSCustomObject]@{ POSITION = $b.pos; NORMAL = $b.nrm }); indices = $b.idx; material = 3 },
                [PSCustomObject]@{ attributes = ([PSCustomObject]@{ POSITION = $t.pos; NORMAL = $t.nrm }); indices = $t.idx; material = 7 }
            )
        })
    $mi = $gltf.meshes.Count - 1
    $gltf.nodes += ([PSCustomObject]@{
            name        = $r.name
            mesh        = $mi
            translation = @($node11.translation)
            scale       = @($node11.scale)
        })
    $newNodeIndices.Add($gltf.nodes.Count - 1)
    Write-Output ("added node '{0}' (mesh={1})" -f $r.name, $mi)
}

# ============ Update scene graph: drop old nodes 7/11, attach new nodes ============
$oldChildren = @($gltf.nodes[15].children)
$newChildren = @()
foreach ($c in $oldChildren) {
    if ([int]$c -ne 7 -and [int]$c -ne 11) { $newChildren += [int]$c }
}
foreach ($ni in $newNodeIndices) { $newChildren += $ni }
$gltf.nodes[15].children = $newChildren

# ============ Update buffer length ============
while (($append.Count % 4) -ne 0) { $append.Add(0) }
$newBinLen = $bin.Length + $append.Count
$gltf.buffers[0].byteLength = $newBinLen

# ============ Serialize JSON ============
$newJson = $gltf | ConvertTo-Json -Depth 100 -Compress
$jsonBytes = [System.Text.Encoding]::UTF8.GetBytes($newJson)
$pad = (4 - ($jsonBytes.Length % 4)) % 4
$jsonPadded = New-Object byte[] ($jsonBytes.Length + $pad)
[Array]::Copy($jsonBytes, $jsonPadded, $jsonBytes.Length)
for ($i = $jsonBytes.Length; $i -lt $jsonPadded.Length; $i++) { $jsonPadded[$i] = 0x20 }

# ============ Rebuild GLB ============
$newBin = New-Object byte[] $newBinLen
[Array]::Copy($bin, $newBin, $bin.Length)
$append.ToArray().CopyTo($newBin, $bin.Length)

$total = 12 + 8 + $jsonPadded.Length + 8 + $newBinLen
$out = New-Object byte[] $total
[System.Text.Encoding]::ASCII.GetBytes('glTF').CopyTo($out, 0)
[BitConverter]::GetBytes([uint32]$version).CopyTo($out, 4)
[BitConverter]::GetBytes([uint32]$total).CopyTo($out, 8)
[BitConverter]::GetBytes([uint32]$jsonPadded.Length).CopyTo($out, 12)
[System.Text.Encoding]::ASCII.GetBytes('JSON').CopyTo($out, 16)
$jsonPadded.CopyTo($out, 20)
[BitConverter]::GetBytes([uint32]$newBinLen).CopyTo($out, 20 + $jsonPadded.Length)
[System.Text.Encoding]::ASCII.GetBytes('BIN').CopyTo($out, 24 + $jsonPadded.Length)
$newBin.CopyTo($out, 28 + $jsonPadded.Length)

[System.IO.File]::WriteAllBytes($GlbPath, $out)
Write-Output ("written: {0} ({1} bytes)" -f $GlbPath, $total)
Write-Output ("BIN chunk: {0} -> {1} bytes" -f $binLen, $newBinLen)
