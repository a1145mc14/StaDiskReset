@echo off
chcp 65001 >nul
set LOG=D:\dg_recover\fix_test.txt
set T=D:\StaDiskReset\StaDiskReset.exe
del /q "%LOG%" 2>nul
powershell -NoProfile -Command "Write-VolumeCache -DriveLetter D" >> "%LOG%" 2>&1
echo === RESIDENT RECOVER === >> "%LOG%"
"%T%" --recover D: --all --name "tiny_cfg*" --out "C:\Users\Administrator\AppData\Local\Temp\sdtest" >> "%LOG%" 2>&1
echo === CARVE WHOLE FIRST HIT === >> "%LOG%"
"%T%" --carve E: --pattern "ViewOnlyGamePlay" --whole --limit 1 --context 8 --out "D:\dg_recover\t_context" >> "%LOG%" 2>&1
echo === DONE === >> "%LOG%"
