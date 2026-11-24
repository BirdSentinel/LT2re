@echo off
setlocal
set LT2_NATIVE_SCENE=1
set LT2_AUDIT_GRAPHICS=
cd /d "%~dp0.."
call scripts\build-native-renderer.cmd
if errorlevel 1 exit /b 1
call scripts\msvc-env.cmd
if errorlevel 1 exit /b 1
if not exist build\native mkdir build\native
.venv\Scripts\python.exe scripts\patch-mouse.py
if errorlevel 1 exit /b 1
.venv\Scripts\python.exe scripts\prepare-optimized.py
if errorlevel 1 exit /b 1
pushd build\native
rc /nologo /fo lt2-icon.res lt2-icon.rc
if errorlevel 1 exit /b 1
set FLAGS=/DLT2_NATIVE_SCENE /nologo /Od /MT /W1 /DRECOMP_TRACE /DRECOMP_X87_RING /FIrecomp_funcs.h /I..\..\src /I..\..\tools\pcrecomp\runtime\recomp32 /I..\..\tools\pcrecomp\runtime\native32 /I..\..\src\generated
set GENFLAGS=/DRECOMP_LOCAL_REGS /DRECOMP_X87_LOCAL /nologo /O2 /Ob2 /fp:strict /MT /W1 /DRECOMP_X87_RING /FIrecomp_funcs.h /I..\..\tools\pcrecomp\runtime\recomp32 /I..\..\tools\pcrecomp\runtime\native32 /I..\..\src\generated
..\..\.venv\Scripts\python.exe ..\..\scripts\compile-clang.py
if errorlevel 1 exit /b 1
cl /nologo /O2 /MT /W3 /I..\..\tools\SDL3-3.4.20\include /I..\..\tools\pcrecomp\runtime\recomp32 /I..\..\tools\pcrecomp\runtime\native32 /c ..\..\src\sdl_sound.c ..\..\src\sdl_platform.c
if errorlevel 1 exit /b 1
copy /y ..\..\tools\SDL3-3.4.20\lib\x86\SDL3.dll . >nul
cl %FLAGS% ..\..\src\host.c recomp_extra.obj recomp_00*.obj recomp_dispatch.obj sdl_sound.obj sdl_platform.obj ..\..\tools\pcrecomp\runtime\native32\native32.c ..\..\tools\pcrecomp\runtime\native32\modules32.c ..\..\tools\pcrecomp\runtime\recomp32\image_loader.c ..\..\tools\pcrecomp\runtime\recomp32\recomp_trace.c lt2-icon.res /Fe:LT2re.exe lt2-renderer.lib /link /MAP:lt2-recomp.map /BASE:0x10000000 /DYNAMICBASE:NO /FIXED /NXCOMPAT /STACK:262144 user32.lib gdi32.lib advapi32.lib shell32.lib ..\..\tools\SDL3-3.4.20\lib\x86\SDL3.lib
set result=%errorlevel%

popd
exit /b %result%





