@echo off
chcp 65001 >nul
powershell -NoProfile -Command "$fs=[System.IO.File]::Open('\\.\E:','Open','Read','ReadWrite'); $b=New-Object byte[] 65536; $fs.Position=113489920; $n=$fs.Read($b,0,65536); $fs.Dispose(); $t=[System.Text.Encoding]::GetEncoding(28591).GetString($b,0,$n); Write-Output ('bytes=' + $n + ' found=' + $t.Contains('ViewOnlyGamePlay'))" > D:\dg_recover\rawcheck.txt 2>&1
