@echo off
chcp 65001 >nul
D:\StaDiskReset\StaDiskReset.exe --carve E: --whole --pattern "ViewOnlyGamePlay" --limit 1 > D:\dg_recover\carve_dbg.txt 2>&1
echo exit=%errorlevel% >> D:\dg_recover\carve_dbg.txt
