@echo off
setlocal
chcp 65001 >nul
cd /d "%~dp0"
echo ============================================================
echo  Windows audio codec probe
echo ============================================================
echo.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0probe.ps1"
echo.
echo ------------------------------------------------------------
echo  Report saved to: %~dp0report.txt
echo  Please send report.txt back.
echo ------------------------------------------------------------
pause
