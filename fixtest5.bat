@echo off
chcp 65001 >nul
set LOG=D:\dg_recover\fix_test5.txt
set T=D:\StaDiskReset\StaDiskReset.exe
del /q "%LOG%" 2>nul
powershell -NoProfile -Command "Write-VolumeCache -DriveLetter D" >> "%LOG%" 2>&1
echo === A. HEX ?? (NTFS -> 4E544653) === >> "%LOG%"
"%T%" --carve E: --pattern-hex "4E544653" --whole --limit 1 --out D:\dg_recover\t_hex >> "%LOG%" 2>&1
echo === B. ?????? === >> "%LOG%"
"%T%" --recover D: --all --name "dup*" --out "C:\Users\Administrator\AppData\Local\Temp\sdtest" >> "%LOG%" 2>&1
echo === DONE === >> "%LOG%"
