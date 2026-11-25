@echo off
powershell.exe -NoLogo -NoProfile -ExecutionPolicy RemoteSigned -File "%~dp0build.ps1" -Configuration Debug %*
exit /b %ERRORLEVEL%
