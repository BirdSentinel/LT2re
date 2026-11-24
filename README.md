# LT2re - Taxi Racer London 2 for modern Windows

LT2re is a static recompilation of *Taxi Racer London 2*. The original game's
x86 machine code is translated to C with [pcrecomp](https://github.com/sp00nznet/pcrecomp)
and compiled into a new Windows program. It needs your own copy of the
original game. No game files are included.

## Features

- **Native game code**: runs the recompiled game logic, not an emulator.
- **Vulkan renderer**: replaces the original Direct3D 8 rendering, including
  the game's own vertex shaders converted to SPIR-V.
- **SDL3 audio, window and mouse**: modern sound output and a Windows cursor.
- **Any resolution**: fullscreen by default, or a freely resizable window
  (`-window <width> <height>`).
- **It's just as shitty as you remember**: It has all the flaws of the original, because if it were good, it wouldn't be good at all!

## Requirements

- Windows 10/11 x64
- Visual Studio 2022 or Build Tools with **Desktop development with C++**
  (x86/x64 MSVC tools and Windows SDK)
- Python 3.11 or newer, Git, PowerShell 5.1 or newer
- An internet connection for the first build, and several GB of free disk space
- The original game. Only this `LT2.exe` is supported (SHA256):
  `3242DA6EE1C453D124DEA26769BD802B0346AC383D9F5CA96784A9F1FC27380F`

## Building

From the root of this repository:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\Build.ps1 -GameDirectory "D:\Games\London Taxi"
```

The script checks `LT2.exe`, copies it and the two shader files
(`misc/dcWorld1.vso`, `misc/dcWorld2.vso`) to a temporary folder,
downloads pinned SDL3, Vulkan and LLVM dependencies, generates the C code,
and compiles everything into `dist/`. The temporary folder is deleted when the build ends.

Options:

- `-SkipSetup`: rebuild without reinstalling dependencies.
- `-Python "C:\Python311\python.exe"`: use a specific Python.

## Running

Copy everything in `dist/` into the original game folder, next to `LT2.exe`:
`LT2re.exe`, `lt2-renderer.dll`, `SDL3.dll`, `Play.cmd` and `licenses/`.
Keep `LT2.exe` there, because LT2re reads game data from it.

```cmd
Play.cmd                     :: fullscreen
Play.cmd -window 1280 720    :: windowed
```

Or start the EXE directly: `LT2re.exe "D:\Games\London Taxi" -window 1280 720`.

## Repository layout

- `src/`: host program, SDL3 audio/window/mouse, Vulkan renderer
- `scripts/`: code generation, game patches, build steps
- `analysis/`: function addresses and render-call maps used during generation
- `tools/pcrecomp/`: patched subset of pcrecomp (base version in `tools/pcrecomp.version`)
- `licenses/`: third-party licenses
