@echo off
setlocal
cd /d "%~dp0"
if not "%~2"=="" goto usage
if /i "%~1"=="lite" goto lite
if /i "%~1"=="pro" goto pro
:usage
echo Usage: build_tirtc.bat lite ^| pro
exit /b 2

:lite
call build_tirtc_lite.bat
if errorlevel 1 exit /b 1
set "TIRTC_VERSION="
for /f "tokens=3" %%V in ('findstr /b "APP_VERSION " examples\L_CT4IT00_YP00W_01_V04\tirtc_lite\config\tirtc_config.mk') do set "TIRTC_VERSION=%%V"
if not defined TIRTC_VERSION exit /b 1
if not exist firmware\lite mkdir firmware\lite
copy /y "gccout\tirtc_lite\TiRTC_lite_NT26F6D0_%TIRTC_VERSION%.binpkg" firmware\lite\latest.binpkg >nul
if errorlevel 1 exit /b 1
powershell -NoProfile -Command "$ErrorActionPreference='Stop'; $sha=[Security.Cryptography.SHA256]::Create(); try { $h=[BitConverter]::ToString($sha.ComputeHash([IO.File]::ReadAllBytes((Join-Path (Get-Location) 'firmware/lite/latest.binpkg')))).Replace('-','').ToLowerInvariant(); [IO.File]::WriteAllText((Join-Path (Get-Location) 'firmware/lite/SHA256SUMS.txt'), $h+'  latest.binpkg'+[Environment]::NewLine, [Text.Encoding]::ASCII) } finally { $sha.Dispose() }"
if errorlevel 1 exit /b 1
echo Ready: firmware\lite\latest.binpkg
exit /b 0

:pro
if not defined TIRTC_PYTHON set "TIRTC_PYTHON=python"
"%TIRTC_PYTHON%" -c "import sys; sys.exit(0 if sys.version_info >= (3, 10) else 1)" >nul 2>nul
if errorlevel 1 (
    echo Python 3.10 or newer is required for pro factory packaging.
    echo Install Python or set TIRTC_PYTHON to the full path of python.exe.
    exit /b 1
)
call build_tirtc_pro.bat
if errorlevel 1 exit /b 1
"%TIRTC_PYTHON%" examples\L_CT4IT00_YP00W_01_V04\tirtc_pro\resource_store\package_factory.py
exit /b %errorlevel%
