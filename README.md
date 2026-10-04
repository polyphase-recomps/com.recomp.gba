# com.recomp.gba — shared runtime for native GBA ports in Polyphase

The game-independent half of a GBA native port: a replacement for the console's hardware
(memory, PPU, DMA, interrupts, BIOS calls, the m4a sound engine, the PSG) that a decompiled
game is compiled against, the `GbaPlayer` node that plays the game in Polyphase, and the
build pieces and tools game packages share.

There's no emulator. The game's own C code is compiled for the host CPU. It still writes to
VRAM, sets up DMA and waits for VBlank as on a GBA; those land in plain arrays, and the
runtime draws the frame and mixes the audio from them.

> **No game is included.** This package contains no ROM, no game assets and no game code.
> A game package (for example `com.recomp.khcom`) builds the game from **your own ROM** and a
> decompilation, on your machine. See that package's README for the steps; nothing here needs
> building on its own. With this package alone, the `GbaPlayer` node exists but has nothing
> to play.

## Layout

```
Source/                 the Polyphase addon: GbaPlayer node, in-process game runner,
                        AgbGuestApi.h (how a game package registers its game)
Runtime/include/        agb.h (what the game sees), agb_host.h (what a platform provides)
Runtime/src/            portable C core: PPU, DMA, IRQs, BIOS, m4a, PSG, audio, boot
Runtime/compat/         prelude force-included into every game source, small libc
Runtime/host/win32/     Windows test runner: window, keyboard, XInput, headless testing
Runtime/wasm/           memory model and glue for the portable (wasm2c) build
Runtime/cmake/          AgbGame.cmake: agb_add_game(), the whole build for one game
Runtime/tools/          build scripts and the Python tools the build runs
Runtime/README.md       how the runtime works, adding a game
Docs/Modding.md         patches, extra C files, testing mods
```

## Using it for a game

A game package supplies the decomp's sources and generated assets, a glue file for the ARM
startup code, a prelude for headers that hard-code addresses, patches under `#ifdef PORT`, and
a `CMakeLists.txt` that calls `agb_add_game()` from `Runtime/cmake/AgbGame.cmake`.
`com.recomp.khcom` is the reference; [Runtime/README.md](Runtime/README.md) walks through it.

Game data is never shipped: the decomp extracts the assets from the user's ROM, and
everything built from them (`Assets/Bin/*.exe`, `Source/Guest/`, `*.agbdata`) is git-ignored
and built on the user's machine.

## How a game runs

Each game is built two ways from the same code.

**Native.** A 32-bit Windows program (`Assets/Bin/<name>.exe` in the game package).
`GbaPlayer` starts it as a child process and talks to it over shared memory
(`Runtime/include/port_shm.h`): frames come back as RGBA, buttons go out, and the child plays
its own audio. The editor always uses this, so a game can be rebuilt without restarting
Polyphase, and it's the easiest version to debug.

**Guest (wasm2c).** The game, its data and the runtime core are compiled to one wasm32 module,
which WABT's `wasm2c` turns back into C. The C goes into the game package (`Source/Guest/`),
is compiled into packaged builds like any other addon code, and runs on a thread inside the
engine. Going through wasm keeps every pointer 32-bit, which the decomps need because they
store pointers in 32-bit fields; that's what lets the same game build for 64-bit PCs and
PowerPC consoles. The game's data (about 30 MB for KH) isn't compiled in: it's written to
`Assets/<name>.agbdata` and loaded at start.

Packaged builds use the guest unless the game's `game.json` lists the platform under
`"native"`. The two builds are checked against each other frame for frame, pixels and audio,
with long random-input runs. If they ever disagree, that's a bug.

## In Polyphase

Add a `GbaPlayer` node to a scene and set **Game** to the game's package id. Make the scene the
project's default scene so packaged builds open it.

| Property | |
|---|---|
| **Game** | Package id of the game, e.g. `com.recomp.khcom`. Defaults come from its `game.json` |
| **Game Executable** | Override for the native exe; empty uses `game.json` |
| **Save Folder** | Override for the save folder; empty uses `game.json` |

The node creates a full-screen child Quad, `GBA Display`, hidden until the first frame.

`game.json` lives in the game package's `Assets/` folder (the packager only copies `Assets/`,
`Scripts/` and `Shaders/` from packages):

```json
{
    "title": "Kingdom Hearts: Chain of Memories",
    "exe": "Bin/khcom.exe",
    "saves": "Saves/KHCoM",
    "native": "Windows"
}
```

| Field | |
|---|---|
| `exe` | the native program, relative to `game.json` |
| `saves` | save folder, relative to the project. The save is `<name>.sav`, the raw 64 KB cartridge SRAM |
| `native` | optional, comma-separated platforms that run the exe instead of the guest: `Windows`, `Linux`, `Mac`, `Android`, `Wii`, `GameCube`, `3DS` |

### Controls

| Input | GBA |
|---|---|
| Keyboard | arrows, **X** = A, **Z** = B, **A** / **Q** = L, **S** / **W** = R, **Enter** = Start, **Backspace** = Select |
| Gamepad, Classic Controller, GameCube pad | d-pad or left stick, A / B, shoulders or triggers = L / R, Start / Select |
| Wiimote alone, held sideways | d-pad, **2** = A, **1** = B, **B trigger** = L, **A** = R, **+** = Start, **−** = Select |
| Wiimote + Nunchuk | stick or d-pad, A / B, **Z** / **C** = L / R, + / −. Switches over once the stick or Z / C is used |

## Building and testing a game

Requirements: Visual Studio 2022 with the *C++ Clang tools* and *CMake tools* components,
Python 3. The guest build also needs `wasi-sdk-*` (34) and `wabt-*` (1.0.42) unpacked into a
`Tools/` folder somewhere above the project; CMake searches upwards from the game's `Native/`
folder.

From the game package's `Native/` folder:

```powershell
.\build.ps1                 # native exe -> ..\Assets\Bin\<name>.exe
.\build.ps1 -Guest wasm     # guest -> ..\Source\Guest, ..\Assets\<name>.agbdata, plus a test exe
.\build.ps1 -Log            # keep going past errors, write build\last_build_<guest>.log
```

The test exe (both builds) takes:

```
--headless --frames N      run without a window for N frames
--dump DIR --every N       write every Nth frame as a .ppm (--dump-from F to start later)
--wav FILE                 record the audio
--script F:KEYS[:DUR],...  hold buttons: frame, hex KEYINPUT bits, duration in frames
                           (A=1 B=2 Select=4 Start=8 Right=10 Left=20 Up=40 Down=80 R=100 L=200)
--fuzz SEED                random button mashing (never the soft-reset combo)
--saves DIR                save folder (use a scratch folder for tests)
--dump-state F             dump RAM, VRAM, palette and IO at frame F, for diffing two builds
--profile                  sampling profiler, report at exit
--log FILE                 copy the log to a file
--scale N                  window size (windowed mode)
```

Don't combine `--script` with `--fuzz`: they're ORed together and can hold A+B+Select+Start,
which soft-resets the game every frame.

A crash in the native build prints a backtrace with game function names. In the wasm test
build, configure with `-Define AGB_WASM_WATCH=ON` and set `AGBW_WATCH=lo-hi` (hex guest
addresses) to log every store to a memory range and who made it.

To check a change, run both builds with the same `--fuzz` seed and compare the dumped frames
and the `.wav`. They should be byte-identical.

## Platforms

| Platform | State |
|---|---|
| Windows | Native and guest both work |
| Wii | Guest runs in Dolphin with audio and boots on a real Wii from the Homebrew Channel. The Wiimote layout isn't confirmed on hardware yet. Copy the whole `Packaged/Wii` folder into `sd:/apps/<name>/` and rename the `.dol` to `boot.dol` |
| GameCube | Builds. The game doesn't fit in 24 MB unpaged, so the read-only data is paged from the `.agbdata` file through a 4 MB cache. Tested on PC and on big-endian PowerPC under qemu, not on hardware yet |
| Dreamcast, 3DS | Paging sizes set (2 MB / 16 MB cache). Not built yet |
| Linux, Mac, Android | Should work through the guest. Not tried |

Paging can be tried on PC: `.\build.ps1 -Guest wasm -Define AGB_WASM_ROM_CACHE_KB=256` (and
`-Define AGB_WASM_ROM_CACHE_KB=` to turn it off). The output has to match the native build.
