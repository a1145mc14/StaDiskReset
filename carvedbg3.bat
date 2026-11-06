@echo off
chcp 65001 >nul
D:\StaDiskReset\StaDiskReset.exe --carve E: --whole --pattern "ViewOnlyGamePlay" --limit 1 --context 8 --out D:\dg_recover\t_context > D:\dg_recover\carve_dbg3.txt 2>&1
