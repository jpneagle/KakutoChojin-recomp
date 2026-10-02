# KakutoChojin-recomp

English | [日本語](README.ja.md)

Turns the original Xbox game *Kakuto Chojin* into a native Windows program. The game's x86 machine code is
translated function by function into C (lifting) and compiled; the Xbox kernel, Direct3D 8, DirectSound and input
are replaced with PC implementations. Supports high resolutions (up to 4K), widescreen and game controllers.
It can also decompile the game into readable C with Ghidra.

**This repository contains no game data or code.** You use an image made from a disc you own (at your own
responsibility). For the tested versions, `hints/` holds lists of function start addresses (numbers only) that
help the conversion.

## Supported versions

| Version | Build | Status |
|---|---|---|
| North America (region: worldwide) | `3db43286` | Tested |
| Japan | `3dd89b56` | Tested |

Disc images can be redump or xiso images. Other versions are untested (the converter says "not tested"; see
[Untested versions](#untested-versions)).

## Usage

You need: Windows 10/11 (x64), an internet connection (first run only) and a disc image (`.iso`).
Nothing has to be installed — no Visual Studio, no Python.

1. Drag and drop the `.iso` onto `convert.bat` (an `.xbe` works too).
2. `<name>_win\` appears next to the ISO (a few minutes the first time because tools are downloaded, then 1–2 minutes).
3. Start `<name>_win\play.bat`. A settings window opens first.

### What the conversion does

1. Tools: on the first run the needed tools are downloaded into `deps\` (`tools\setup.ps1`, about 300 MB, no
   installation or administrator rights: the llvm-mingw C/C++ compiler, CMake, Ninja, Python with capstone, SDL3,
   XbSymbolDatabase). Every download is checked against its SHA-256.
2. Extracts the files from the disc and identifies the version (Japan, North America, ...)
3. Finds the XDK library functions and writes the manifest
4. (only with `--decompile`) Decompiles to readable C with Ghidra; Ghidra and a JDK are downloaded too (about 700 MB)
5. Translates the game code to C
6. Compiles it with Clang and packages the result

From a command line: `convert.bat <game.iso | default.xbe> [output dir] [--decompile] [--jobs N]`
(`--jobs`: parallel compile jobs, default 2).

### Output

| File | Contents |
|---|---|
| `play.bat` | Starts the game |
| `KakutoChojin.exe`, `SDL3.dll` | The converted game |
| `KakutoChojin.ini` | Video settings (changed in the start-up window and saved here) |
| `game\` | Files from the disc (ISO input) |
| `analysis\` | Manifest, symbols, lifted C (`lifted\`), decompiled C (`decomp\`, with `--decompile`) |
| `hdd\` | The Xbox hard disk (saves), created on the first start |
| `KakutoChojin.log` | Run log (look here when something goes wrong) |
| `_build\` | Compiler intermediates (can be deleted) |

### Video settings

Chosen in the start-up window (arrow keys, mouse or controller; Enter or A starts the game).

| Setting | Choices |
|---|---|
| Resolution | Original 480p / HD (720p) / Full HD (1080p) / WQHD (1440p) / 4K (2160p) |
| Aspect ratio | 4:3 (original) / 16:9 widescreen |
| Display | Window / fullscreen (Alt+Enter or F11 toggles it in game too) |
| Renderer | Direct3D 11 / OpenGL 4.5 |
| Show this window | At every start / not again (set `launcher=1` in `KakutoChojin.ini` to bring it back) |

The settings are stored in `KakutoChojin.ini`, which can also be edited by hand:

```ini
[video]
resolution=1080   ; output height: 0 = original 480, or 720 / 1080 / 1440 / 2160
widescreen=1      ; 1 = 16:9
fullscreen=0      ; 1 = start in borderless fullscreen
gpu=d3d11         ; renderer: d3d11 / gl (OpenGL 4.5)
launcher=1        ; 1 = show the settings window at start
```

- **High resolution**: the screen is rendered at the chosen height (the game's small internal render targets keep
  their original size).
- **Widescreen**: 3D scenes extend to the sides; menus, HUD and movies stay 4:3 in the middle. 2D elements that were
  placed off screen (large effect text) may become visible at the sides, and objects at the screen edges may vanish.
- **Window**: resizable; the picture keeps its aspect ratio.

### Controls

Game controllers supported by SDL3 (Xbox 360 / One / Series, DualShock / DualSense, Switch Pro, ...; up to four
players, hot-plugging and rumble):

| Xbox | Controller (Xbox layout names) |
|---|---|
| A / B / X / Y, D-pad, sticks, START, BACK | Same buttons |
| BLACK / WHITE | RB / LB |
| L / R triggers | LT / RT |

Keyboard (player 1, while the game window has focus): arrows = D-pad, Z/X/C/V = A/B/X/Y, A/S = WHITE/BLACK,
Q/W = LT/RT, Enter = START, Backspace = BACK

### Untested versions

Versions not in the table above may convert but stop while running: when the game reaches code the conversion
missed, `KakutoChojin.log` shows `FATAL: lift: ... to XXXXXXXX` with the address.

- Converting with `--decompile` lets Ghidra find the functions, which misses less.
- Adding the address on a line of `hints\<title id>_<build>.extra.txt` and converting again gets past that point.

## For developers

### Layout

| Path | Contents |
|---|---|
| `src/lift/kt_lift.h` | Run-time interface of the lifted C (CPU state, guest memory, x87/MMX/SSE helpers) |
| `src/host/` | Runtime: kernel (`kernel*.cpp`, `ob.*`, `hostfs.*`), D3D8 (`d3d/`), graphics API layer (`gpu/`: D3D11 / OpenGL), DirectSound (`dsound/`, `audio/`), input (`xapi/`), window and start-up settings (`platform/`), OS layer (`os.*`) |
| `tools/convert.py` | The whole conversion (run by `convert.bat`) |
| `tools/lift/` | x86 → C (`lift.py`, instruction translation `x86c.py`, MMX/SSE `simd.py`) |
| `tools/ghidra/` | Ghidra scripts (annotations, naming, type recovery, C export) |
| `hints/` | Per tested version: function list (`.functions.txt`) and functions reached only through pointers (`.extra.txt`) |

### How it works

| Layer | Approach |
|---|---|
| Game code | Each function becomes C as `void f_XXXXXXXX(KtCpu* c)`. Registers and (lazily evaluated) flags are locals; guest memory, stack and calling conventions stay exactly as on the Xbox. Instructions: integer, x87, MMX, SSE (Pentium III) |
| Memory | The Xbox's 4 GB address space is reserved on the host (`guest_mem.cpp`) and accessed as `KT_MEMBASE + address` |
| xboxkrnl | Own handle table, events, semaphores, threads and waits (`ob.*`); files reproduce NT create dispositions and information classes on `std::filesystem` (`hostfs.*`). `\Device\CdRom0` → game folder, `\Device\Harddisk0\PartitionN` → `hdd/` |
| D3D8 | Reimplemented on a graphics API layer (`gpu.h`). NV2A vertex programs, register combiners and the fixed-function pipeline are translated into a shared shader dialect, emitted as HLSL or GLSL |
| DirectSound | Own mixer with SDL3 output (Xbox ADPCM decoding). Without an audio device it still mixes in real time to keep the timing |
| Input, window | SDL3 |
| Console settings | The region follows the disc certificate; for titles that may only boot from DVD, the DVD authentication query (MODE SENSE security page) is answered as authenticated |

Calls from lifted C into the replacement functions go through adapters generated from the C++ function types
(`hle_invoke.h`): they read arguments from the guest stack, convert pointers and put the result in EAX/EDX/st(0).
Not supported: resuming from structured exceptions (`RtlRaiseException` / `RtlUnwind`).

### Building yourself

Use the tools downloaded by `tools\setup.ps1` through `tools\env.bat`:

```bat
powershell -ExecutionPolicy Bypass -File tools\setup.ps1
tools\env.bat cmake -S . -B build_clang -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DKT_LIFTED_DIR=%CD%\<output dir>\analysis\lifted
tools\env.bat cmake --build build_clang -j 2
```

### Environment variables

| Variable | Effect |
|---|---|
| `KT_RESOLUTION` / `KT_WIDESCREEN` / `KT_FULLSCREEN` / `KT_GPU` | Override `KakutoChojin.ini` |
| `KT_NO_LAUNCHER=1` | Skip the start-up settings window |
| `KT_NO_FRAME_LIMIT=1` | Remove the 60 Hz frame limit (testing) |
| `KT_SILENT=1` | No audio output (mixing continues in real time) |
| `KT_LANGUAGE=ja` / `en` | Console language. Default: Japanese when the console region is Japan |
| `KT_REGION=na` / `jp` / `eu` | Console region. Default: a region the disc certificate allows |
| `KT_TRACE=1` | Log every kernel call and file I/O |
| `KT_DUMP_FRAMES=N` | Save every Nth frame to `frames/*.bmp` |
| `KT_DUMP_SHADERS=1` | Save generated shaders (HLSL / GLSL) to `shaders/` |
| `KT_GL_DEBUG=1` | Log OpenGL debug output |
| `KT_TRACE_FRAME=N` | Log every draw call of frame N |
| `KT_AUTOPRESS=start@3100-3110,a@4300-4310` | Scripted button presses by frame number (unattended tests) |
| `KT_LAUNCHER_SHOT=<file.bmp>` | Save the settings window as an image and start (testing) |
| `SDL_AUDIO_DRIVER=disk` | Write the audio to `sdlaudio.raw` (float32 stereo, 48 kHz; an SDL feature) |

### Decompilation (Ghidra)

`convert.bat ... --decompile`, or `tools\decompile.bat` by hand, converts the XBE into an ELF with symbols, imports it
into headless Ghidra and writes every function as C to `analysis/decomp/` (`classes/<class>.c`,
`global/<address range>.c`, `index.tsv`). `KT_XBE` (the XBE) and `KT_ANALYSIS` (the output directory) select the input.

| Script | Does |
|---|---|
| `KtApply.java` | XDK function names, calling conventions and parameters, kernel functions, RTTI class namespaces and virtual functions |
| `KtNames.java` | MSVC demangling; names constructors / destructors from vtable stores |
| `KtMembers.java` | Places functions that take `this` (ECX) and are only called from one class as that class's methods |
| `KtPropagate.java` | Names remaining functions (STL, wrappers, helpers, identifier strings, module attribution, shared namespaces) |
| `KtTypes.java` | Recovers class structures from how `this` is used, typed vtables, inherited base-class fields |
| `KtExportC.java` | Exports every function as C |

The Ghidra project stays in `analysis/ghidra/kt.gpr`; open it in the GUI to continue the analysis.

### 32-bit build (mixed execution and verification)

A development build made with MSVC (Visual Studio 2019 or later, "Desktop development with C++"). It maps the XBE at
its real addresses and runs the original machine code, optionally replacing some or all functions with the lifted C
for verification. Get the SDL3 VC package with `tools\fetch_sdl3.ps1`.

```bat
powershell -ExecutionPolicy Bypass -File tools\fetch_sdl3.ps1
tools\vcenv.bat cmake -S . -B build -G Ninja -DKT_LIFTED_DIR=%CD%\<output dir>\analysis\lifted
tools\vcenv.bat cmake --build build -j 2
set KT_LIFT=all
build\bin\kt_loader.exe <game dir> <manifest.txt> hdd
```

| Variable | Use |
|---|---|
| `KT_LIFT=all` | Run all lifted functions as C (ranges like `10000-20000` or `all,-15000` work too) |
| `KT_VERIFY=all` | Runs call-free functions as C and as original code from the same state and compares registers, x87 and written memory |
| `KT_LIFT_STRICT=1` | With `KT_LIFT=all`: records any original instruction that still runs and writes functions that were not lifted to `lift_gaps.txt` (pass it to `lift.py --extra`) |

`tools/lift/bisect.py` bisects the replaced range to find a faulty lifted function.

### Other systems (Linux etc., untested)

The shared code does not use the Windows API (`tools\portable_check.bat` syntax-checks it without `<windows.h>`), and
the OS-specific files (`os.cpp`, `crash.cpp`, `guest_mem.cpp`) have POSIX versions. CMake has GCC / Clang settings,
but the build has not been tried on Linux yet.

```sh
# needs the SDL3 development package and an OpenGL 4.5 driver
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DKT_LIFTED_DIR=$PWD/<output dir>/analysis/lifted
cmake --build build -j 2
./build/bin/KakutoChojin game analysis/manifest.txt hdd
```

## Dependencies and licenses

- [XbSymbolDatabase](https://github.com/Cxbx-Reloaded/XbSymbolDatabase) — MIT
- `third_party/xboxkrnl.exe.def` ([nxdk](https://github.com/XboxDev/nxdk)) — CC0
- [Capstone](https://www.capstone-engine.org/) — BSD
- [SDL3](https://libsdl.org/) — zlib
- Downloaded by `tools/setup.ps1` (not part of the repository): [llvm-mingw](https://github.com/mstorsjo/llvm-mingw) — Apache 2.0 with LLVM exception,
  [CMake](https://cmake.org/) — BSD, [Ninja](https://ninja-build.org/) — Apache 2.0, [Python](https://www.python.org/) — PSF,
  optionally [Ghidra](https://github.com/NationalSecurityAgency/ghidra) — Apache 2.0 and [Temurin JDK](https://adoptium.net/) — GPLv2 with Classpath exception
