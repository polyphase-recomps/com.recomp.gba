# com.recomp.gba runtime

How the runtime works, for people changing it or adding a game. For using it in a project, see
the package [README](../README.md).

It runs 100%-decompiled GBA games (pret-style decomps: C game code built with agbcc, data units
in GNU-as). The game's own C sources are compiled for the host. The ARM-only parts (crt0, BIOS
calls, the m4a sound engine's assembly) are replaced by this runtime and a small per-game glue
file.

```
include/agb.h        what the game needs from "the hardware": memory regions, DMA, IRQs,
                     frame, PPU, BIOS calls, boot / soft reset
include/agb_host.h   what the runtime needs from a platform: present a 240x160 BGR555
                     frame, wait for the frame tick, keys, 44.1 kHz audio, save file, log
src/                 portable C99 core, no OS calls
  agb_ppu.c          scanline renderer: modes 0-5, affine BGs, sprites (affine, OBJ window),
                     windows, alpha blending, brightness, mosaic (BG vertical)
  agb_frame.c        VBlankIntrWait = 228 scanlines with HBlank/VCount/VBlank IRQs and DMA,
                     audio, present, pacing; simulated beam for REG_VCOUNT/DISPSTAT polling
  agb_dma.c          DMA channels driven by the IO registers
  agb_bios.c         CpuSet, CpuFastSet, LZ77/RL/Huffman, RegisterRamReset, Sqrt, Div,
                     ArcTan2, BgAffineSet, ObjAffineSet
  agb_m4a.c          m4a_1.s in C (mixer, sequencer, ply_note, ply_* commands), bit-exact;
                     compiled with the game's m4a.h
  agb_audio.c        mixes m4a output, other sound-FIFO DMA streams and the PSG to 44.1 kHz
  agb_psg.c          the four Game Boy sound channels
  agb_boot.c         boot, soft reset (zeroes the game's RAM variables), SRAM save polling
compat/agb_pret.h    prelude force-included in every game source (see below)
compat/libc/         string.h / stdlib.h for -nostdlibinc builds
host/win32/          Windows host: window, keyboard/XInput, waveOut, shared memory for the
                     GbaPlayer node, headless test options, crash log, NULL-read safety net
wasm/                the wasm2c guest: memory model (agbw.h, agbw_mem_ops.h), GBA division
                     rules (agbw_ops.h), instantiation and host imports (agbw_guest.c),
                     wasm2c runtime (agbw_rt.c), engine entry (agbw_bridge.c)
cmake/AgbGame.cmake  agb_add_game(): the whole build for one game
tools/               asm_port.py (data units -> host GNU-as), gen_memory.py (GBA memory
                     regions + absolute symbols), list_units.py, apply_patches.py,
                     elf2wasm.py, wasm_functypes.py, wasm_link.py, wasm_to_c.py,
                     publish_guest.py, build_game.ps1, buildlog.ps1, check_addon.ps1
```

## How the hardware is redirected

- The GBA memory regions are arrays (`agb_ewram`, `agb_iwram`, `agb_io`, `agb_pltt`,
  `agb_vram`, `agb_oam`, `agb_sram`), defined in a generated `agb_memory.s`. The decomp's
  absolute-address symbols are labels inside them. ROM-address symbols are aliased with
  `/alternatename` to the data label the decomp defines for that address, or the bytes are
  copied from the ROM.
- `compat/agb_pret.h` includes the decomp's `gba/*.h` and `macros.h`, then:
  - redefines `REG_BASE`, `VRAM`, `PLTT`, `OAM`, `EWRAM_START`, `IWRAM_START` and `INTR_VECTOR` to point at the arrays;
  - routes `DmaSet` to `agb_dma_set`;
  - routes `REG_VCOUNT`/`REG_DISPSTAT` reads through `agb_vcount_ptr()`/`agb_dispstat_ptr()`;
  - drops the EWRAM/IWRAM section attributes;
  - sets `#pragma pack(4)`, because agbcc aligns 64-bit members to 4;
  - puts game BSS in `.bss$agbg`, for soft reset.

  It must be `-include`d: quoted includes inside the decomp's `include/` would bypass
  include_next shims.
- The IO registers are plain memory, so no write is trapped. When polling code reads
  VCOUNT/DISPSTAT, the beam advances one step per read. If polling reaches VBlank, a whole
  frame runs, including interrupts, present and pacing. Movie players and other loops that
  spin on VCOUNT then work as on hardware.
- Sound: the native `SoundMain` hands each frame's mix to the host directly. Other
  sound-FIFO DMA streams are followed by their source pointer at the timer's rate. The PSG
  is synthesised from the registers; NR52 channel bits are kept current because m4a reads them.

## A new game

1. Generate the decomp's assets and a units list: `{"units": [{"src": ...}]}` in link order,
   plus the linker script with its `name = 0x...;` symbols. See `com.recomp.khcom/Native/tools/kh_gen.py`.
2. Create a data-only package with `Assets/game.json` (`exe` relative to it, `saves`): the
   packager ships `Assets/` loose, so publish the built program to `Assets/Bin/`
   (`PUBLISH_DIR`). Then add `Native/`:
   - `CMakeLists.txt` calling `agb_add_game(...)`, with `project(<name> C ASM)`;
   - `game/<name>_glue.c`: `agb_game_entry()` (sets the IRQ dispatcher from the game's
     crt0 order and calls `agb_boot(AgbMain)`), plus the libagbsyscall names the game
     uses, forwarded to `agb_*`;
   - `game/<name>_prelude.h` for game headers that hard-code addresses: SRAM offsets,
     link-time numbers such as m4a's `gNumMusicPlayers`;
   - `patches/*.patch` for decomp fixes, wrapped in `#ifdef PORT`. Typical fixes: inline
     `asm`, code copied to RAM and executed, waits on hardware state the runtime does not
     model.
3. Exclude the ARM code units (`EXCLUDE_REGEX`) and port any game-specific ones to C.
4. `Native\build.ps1 -Log`, then run headless with `--frames/--dump/--script/--fuzz`.
5. `Native\build.ps1 -Guest wasm`, and compare its frames and audio with the native build on
   the same `--fuzz` seed. Differences usually mean undefined behaviour in the game that x86
   happens to tolerate: reads through NULL, out-of-range shifts, signed overflow, or code
   that copies past a buffer.

## The wasm2c guest

The native guest is x86 code in a 32-bit Windows process. Everything else runs the wasm2c
guest. The build (`_agb_wasm_game` in `cmake/AgbGame.cmake`):

1. Game C, the glue, `agb_m4a.c` and `src/` are compiled with wasi-sdk clang for
   `wasm32-wasip1`.
2. Data units don't go through the wasm assembler, which can't handle what they need. They
   go through `asm_port.py` like the native build, get assembled to an i686 ELF, and
   `elf2wasm.py` rewrites that as a wasm object: same bytes, same symbols, relocations turned
   into wasm data relocations. `wasm_functypes.py` supplies the signatures of functions whose
   addresses sit in data tables.
3. `wasm_link.py` links with wasm-ld (`--no-entry --no-stack-first --global-base=4096`). Calls
   whose prototypes don't match the callee (K&R declarations are common in decomps) are let
   through, and `wasm_to_c.py` later turns them into calls with the arguments the callee takes.
4. `wasm_to_c.py` moves every data segment out into `<name>.agbdata`, sizes the guest memory to
   what the module uses, writes `agbw_config.h`, runs `wasm2c`, and patches the generated
   loads and stores to use `wasm/agbw_mem_ops.h`.
5. `publish_guest.py` copies the generated C and `wasm/` into the game package's
   `Source/Guest/` (wrapped in `#if !EDITOR`, since the editor uses the native exe) and the data
   into `Assets/`.

The guest memory is one little-endian buffer of `AGBW_MEM_BYTES` (32 MB for KH). Every
address is masked or clamped into it, so no access can leave it and there are no bounds
checks. Big-endian hosts swap on every load and store. Division follows the GBA's `Div` rules
instead of trapping. Indirect calls skip the type check. The game is re-instantiated on every
start, and quitting longjmps out of the game thread.

Consoles that can't hold 32 MB page the read-only part. Everything below `AGBW_RAM_BASE` (the
first writable segment, rounded down to 16 KB) stays in the `.agbdata` file and is read in
16 KB pages into a round-robin cache of `AGBW_ROM_CACHE_BYTES`: Dreamcast 2 MB, GameCube 4 MB,
3DS 16 MB, 0 (everything resident) elsewhere. Pages that get written to are pinned. Accesses
that straddle two pages go through a scratch buffer, and `memory.fill`/`memory.copy` go page
by page. KH needs fewer than 4096 page loads in 40k frames of play with a 2 MB cache, and
stays bit-identical with as little as 256 KB.
