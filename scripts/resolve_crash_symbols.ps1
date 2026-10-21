# Resolve crash addresses from BigCat.pdb
$pdbPath = "D:\Wu6Dof\PROG\WuBigCat\build\Release\bin\main\BigCat.pdb"
$exePath = "D:\Wu6Dof\PROG\WuBigCat\build\Release\bin\main\BigCat.exe"

# Use cdb with dump, but write output to file directly
$cdbExe = "C:\Users\Administrator\AppData\Local\Temp\cdb_tools\cdb.exe"
$dumpPath = "D:\Wu6Dof\PROG\WuBigCat\build\Release\bin\main\BigCat_crash_20260612_225306.dmp"
$outFile = "D:\Wu6Dof\PROG\WuBigCat\build\resolve_out.txt"

$cmd = ".ecxr; .reload BigCat=00007ff690e40000,300000; k 50; q"

$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName = $cdbExe
$psi.Arguments = "-z `"$dumpPath`" -y `"$([System.IO.Path]::GetDirectoryName($pdbPath));srv*`" -c `"$cmd`""
$psi.UseShellExecute = $false
$psi.RedirectStandardOutput = $true
$psi.RedirectStandardError = $true
$psi.CreateNoWindow = $true

$proc = [System.Diagnostics.Process]::Start($psi)
$out = $proc.StandardOutput.ReadToEnd()
$err = $proc.StandardError.ReadToEnd()
$proc.WaitForExit(30000)

$out | Out-File -FilePath $outFile -Encoding utf8
Write-Host "Output written to $outFile"
Write-Host "=== Last 30 lines ==="
$lines = $out -split "`n"
$startLine = [Math]::Max(0, $lines.Count - 30)
$lines[$startLine..($lines.Count-1)] | ForEach-Object { Write-Host $_ }
