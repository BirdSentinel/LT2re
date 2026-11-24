@echo off
if defined VSCMD_ARG_TGT_ARCH if /i "%VSCMD_ARG_TGT_ARCH%"=="x86" exit /b 0
set "LT2_VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%LT2_VSWHERE%" (
 echo Install Visual Studio 2022 Build Tools with Desktop development with C++ and Windows SDK.
 exit /b 1
)
set "LT2_VS="
for /f "usebackq tokens=*" %%i in (`"%LT2_VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "LT2_VS=%%i"
if not defined LT2_VS exit /b 1
call "%LT2_VS%\VC\Auxiliary\Build\vcvarsall.bat" x86
exit /b %errorlevel%
