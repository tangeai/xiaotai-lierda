@echo off
setlocal
cd /d "%~dp0"
call build.bat build %* PROJECT=L_CT4IT00_YP00W_01_V04 APP_VARIANT=pro BUILD_MODE=tirtc TIRTC_ASSET_INSTALLER=y MODEM=NT26F6D0 MODEMPKG=F6D_A
exit /b %errorlevel%
