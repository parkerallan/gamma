# Engine

C++ game engine

Pinned dependency versions:
- SDL3: `release-3.4.4`
- Dear ImGui: `v1.92.6-docking`
- ImNodeFlow: `5c93f4822869223cc1abe5ce5fe51e6bee1f5925`

Build:

```powershell
cmake -S . -B build
cmake --build build --config Debug
```

Clean:

```
cmake --build build --config Debug --target clean
```

Run:

```powershell
.\build\Debug\engine.exe
```
