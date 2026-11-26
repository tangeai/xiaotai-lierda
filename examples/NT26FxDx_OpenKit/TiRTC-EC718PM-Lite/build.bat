@echo off
setlocal
if not "%~1"=="" goto usage
cd /d "%~dp0..\..\.."
if errorlevel 1 exit /b 1
call "%~dp0build_app.bat"
if errorlevel 1 exit /b 1
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0publish_firmware.ps1"
if errorlevel 1 exit /b 1
exit /b 0

:usage
echo Usage: build.bat
echo Use build_app.bat for optional make arguments.
exit /b 2
