@echo off
setlocal
set LT2_VK_UNCAPPED=1
"%~dp0LT2re.exe" "%~dp0." %*
exit /b %errorlevel%
