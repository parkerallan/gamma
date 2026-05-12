# Gamma

Raytracing Game Engine

## Pinned Dependencies
- SDL3: `release-3.4.4`
- Dear ImGui: `v1.92.6-docking`
- ImNodeFlow: `5c93f4822869223cc1abe5ce5fe51e6bee1f5925`
- Jolt Physics: `v5.3.0`
- miniaudio: `v0.11.21`
- ffmpeg: latest prebuilt

## Build The Engine (Windows)

Build Debug (includes debug terminal):
```powershell
cmake -S . -B build
cmake --build build --config Debug
```

Build Release:
```powershell
cmake -S . -B build
cmake --build build --config Release
```

Clean:
```powershell
cmake --build build --config Debug --target clean
```

Run:
```powershell
.\build\Debug\engine.exe
```

Run Release:
```powershell
.\build\Release\engine.exe
```

Note: on Windows, the `engine` Release build is configured without a console window.

## Building Your Game

The editor builds the standalone `game` target when you open Build in the UI.

In Build Settings you can choose:
- Build platform: `Windows (MSVC)` or `Linux (GCC)`
- Build type: `Debug` or `Final`

The staged output is created in the folder you pick in Build Settings.

## Windows Game Builds (MSVC)

Windows game builds use the MSVC toolchain from Visual Studio.

Install prerequisites:

1. Visual Studio 2022 (or Build Tools 2022) with:
	- MSVC v143 - VS 2022 C++ x64/x86 build tools
	- Windows 10/11 SDK
	- C++ CMake tools for Windows
2. Vulkan SDK for Windows (includes glslc)
3. CMake 3.24+
4. Git

Then in the engine build menu:
- Set Build platform to `Windows (MSVC)`
- Choose `Debug` or `Final`
- Queue build

The staged Windows executable will be `<game-name>.exe` in your chosen output folder.

## Linux Game Builds (Via WSL)

Linux builds are executed from Windows through WSL. Install the following dependencies on your linux distro:

```bash
sudo apt update
sudo apt install -y build-essential cmake git libvulkan-dev glslang-tools libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxfixes-dev libxi-dev libxss-dev libxtst-dev libxinerama-dev libxkbcommon-dev libwayland-dev wayland-protocols libdecor-0-dev
```

## Run A Staged Linux Game

If your staged folder is `E:/Projects/test2` and executable name is `linuxapp`, run:

```powershell
wsl bash -lc "cd /mnt/e/Projects/test2 && chmod +x linuxapp && ./linuxapp"
```

Path conversion rule:
- `C:/...` -> `/mnt/c/...` 

## Developer API

See `docs/developer-api.md` for a quick reference of all Lua developer functions, summaries, examples, and package notes. **Disclaimer these function are WIP and all subject to change!**

## Troubleshooting

- `glslc was not found`
	Install `glslang-tools` in WSL, or ensure `glslc`/`glslangValidator` is in WSL `PATH`.

- `No such file or directory` from `gmake` / missing `Makefile`
	The build cache may be stale. Re-run build from the editor; it auto-recreates incomplete Linux build caches.

- `SDL could not find X11 or Wayland development libraries`
	Make sure to install the full dependency list in the Linux section above.