@echo off
setlocal
if not "%~1"=="" goto usage
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0publish_firmware.ps1"
exit /b %errorlevel%

:usage
echo Usage: build.bat
echo Use build_app.bat for optional make arguments.
exit /b 2
