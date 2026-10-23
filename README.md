# N.O.V.A. 3 reimplementation

A native reimplementation of Gameloft's *N.O.V.A. 3: Near Orbit Vanguard Alliance* for the
desktop (C++20, SDL3). It runs the original game's own files: the levels, models, textures,
sounds, the GLSL shaders and the Lua level scripts. Behaviour comes from reading the
original Android library (`libNOVA3_neon.so`, which ships with its symbols) in Ghidra.

**The game's files are not included and never will be.** You need your own copy of the
Android build (1.0.8e: the APK's `.gla` archives) or the iOS build (Premium Edition 1.0.5:
the `.app` folder from the IPA).

## Layout

| Path | What |
| --- | --- |
| `engine/` | The game: `src/core` (archives), `src/assets` (decoders), `src/shader` (the game's GLSL to SPIR-V), `src/script` (Lua 5.1), `src/render` (SDL_GPU), `src/game` (levels, scripts, combat, AI, HUD, menus), `src/audio`, `src/app` (options and the game loop) |
| `engine/tools/check.cpp` | `nova3-check`: decoder and shader checks over a data folder |
| `tools/` | Python reference decoders and the Ghidra scripts used to read the original |
| `work/` | Local only (gitignored): extracted data, the Ghidra project, references |

## Building

All platforms need CMake 3.24+, Ninja (or another generator), a C++20 compiler, and `glslc`
(from shaderc or the Vulkan SDK) on the build machine for the renderer's own shaders.

### Linux

With the libraries from the system (Arch: `sdl3 sdl3_ttf zlib lua51 shaderc glm`):

```bash
cmake -S engine -B engine/build -G Ninja
cmake --build engine/build
```

The build uses Vulkan. Add `-DNOVA3_VENDORED=ON` to build every library from pinned
sources instead, and `-DNOVA3_SHADERCROSS=ON` for the Direct3D 12 / Metal shader path.

### Windows

`NOVA3_VENDORED` and `NOVA3_SHADERCROSS` are on by default: SDL3, SDL3_ttf (FreeType,
HarfBuzz), zlib, Lua 5.1.5, shaderc (glslang, SPIRV-Tools), GLM, SPIRV-Cross and
SDL_shadercross are downloaded and built. The game runs on Vulkan, or on Direct3D 12 with
the shaders converted through the system's `d3dcompiler_47.dll`.

Cross-compiled from Linux with [llvm-mingw](https://github.com/mstorsjo/llvm-mingw):

```bash
cmake -S engine -B engine/build-win -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE=engine/cmake/toolchains/llvm-mingw-x86_64.cmake
cmake --build engine/build-win
```

### macOS

Same as Windows (vendored libraries, shaders converted to Metal by SDL_shadercross). This
path has not been built or run yet.

### Packages

`cpack --config engine/build/CPackConfig.cmake` makes a `.zip` (Windows) or `.tar.gz`
holding `nova3`, `nova3-check` and the `shaders` folder.

## Running

```bash
nova3 --data /path/to/the/game/data
```

Without `--data` the game looks for `work/data`, then a `data` folder beside the
executable. The iOS build's folder works on its own or, mounted after the Android data
(`--extra-data`), fills in what Android lacks.

Desktop controls: mouse to look and fire (right button aims), WASD to move, Shift to sprint,
Space to jump, E to use, R to reload, G to throw a grenade (H switches to mines, X detonates),
Q for the special power (Tab switches), 1-9 or the wheel for weapons, Esc for the menu.
`--touch` shows the original's touch controls instead.

The game keeps its settings and save file in SDL's preferences folder, and its compiled
shaders in the user's cache folder.

### Headless checks

`--screenshot OUT.bmp` renders one frame without a window, after `--run SECONDS` of level
logic. Tests build on it (`--combat-test`, `--walk-test`, `--grenade-test`, `--menu`,
`--main-menu`, `--sound-render`, ...): see `engine/src/app/options.h`. `nova3-check` checks the
decoders (`vfs`, `textures`, `models`, `levels`, `shaders`, `cross`).
