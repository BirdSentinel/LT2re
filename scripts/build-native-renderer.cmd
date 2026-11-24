@echo off
setlocal
set LT2_NATIVE_SCENE=1
cd /d "%~dp0.."
call scripts\msvc-env.cmd
if errorlevel 1 exit /b 1
if not exist build\native mkdir build\native
.venv\Scripts\python.exe scripts\build-vulkan-shaders.py
if errorlevel 1 exit /b 1
pushd build\native
cl /nologo /c /O2 /MT /I..\..\tools\vulkan\headers\include ..\..\tools\vulkan\volk.c
if errorlevel 1 exit /b 1
cl /nologo /LD /O2 /MT /EHsc /std:c++17 /DNOMINMAX /I. /I..\..\tools\vulkan /I..\..\tools\vulkan\headers\include ..\..\src\renderer\game_scene_port.cpp ..\..\src\renderer\scene.cpp ..\..\src\renderer\vulkan.cpp ..\..\src\renderer\platform_win32.cpp volk.obj /Fe:lt2-renderer.dll /link user32.lib
set result=%errorlevel%
popd
exit /b %result%
