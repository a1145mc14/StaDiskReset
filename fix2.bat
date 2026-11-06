@echo off
chcp 65001 >nul
set LOG=D:\dg_recover\fix_test2.txt
del /q "%LOG%" 2>nul
D:\StaDiskReset\StaDiskReset.exe --carve E: --pattern "NTFS" --whole --limit 1 --context 1 --out D:\dg_recover\t_context >> "%LOG%" 2>&1
echo === DONE === >> "%LOG%"
