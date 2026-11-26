@echo off
setlocal
cd /d "%~dp0..\..\.."
if errorlevel 1 exit /b 1
call build.bat build %* PROJECT=NT26FxDx_OpenKit APP_VARIANT=pro BOARD_PROFILE=nt26f9d0_prodisplay BUILD_MODE=tirtc TIRTC_ASSET_INSTALLER=n MODEM=NT26F9D0 MODEMPKG=F9D_D
exit /b %errorlevel%
