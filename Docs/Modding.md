# Modding GBA recomp games

This covers every game built on `com.recomp.gba` (Kingdom Hearts: Chain of Memories is the
reference game). What a specific game offers and where things are in its source is in that
game's package, e.g. [`com.recomp.khcom/Docs/Modding.md`](../../com.recomp.khcom/Docs/Modding.md).

A recompiled game is the game's own C code (from its decompilation) running on this runtime.
A mod is therefore ordinary C compiled into the game, and it runs on every target the game
builds for (the native Windows exe and the wasm2c guest used by packaged builds). There's no
hex editing, no pointer tables and no free space to hunt for: everything is recompiled and
relinked, so a function or a string can grow without breaking whatever sits after it.

| You want to... | Use |
|---|---|
| Change how existing game code behaves | a **patch** in the game package's `Native/patches/` |
| Add new functions or data | a **C file** in the game package's `Native/game/`, called from a small patch |
| Change a define or a struct from a header | an override in the game's **prelude** (`Native/game/<name>_prelude.h`) |
| Change text, graphics or sound | the decomp's **extracted assets**, then regenerate (see the game's page) |

## Patches

The decomp checkout is never modified. Code changes live as unified diffs in the game
package's `Native/patches/`. At configure time `Runtime/tools/apply_patches.py` copies every
file a patch touches to `Native/build/<config>/gen/patched/`, applies the hunks there, and the
build compiles the copy instead of the original.

- **Diff against the clean decomp**, not against another patch's output. Hunks for the same
  file from several patches are merged by line number, so two patches can't change the same
  lines.
- **Only `.c` files listed as units can be patched.** A patched header is written out but never
  used, because includes still resolve to the decomp's `include/`. Override header contents in
  the prelude instead (it's force-included into every game source after the runtime's
  `compat/agb_pret.h`), or inside the `.c` patch.
- **Wrap changes in `#ifdef PORT`** (or a define of your own). The file then still builds the
  original ROM, the decomp's matching build keeps working, and it's obvious what changed.
- **Number mods from `0100`.** Lower numbers are the port's own patches.

Hunks are applied loosely: line endings don't matter (decomps are often CRLF, patches can be
LF), and a hunk is found up to 200 lines away from the line it names. If it can't be found,
configure stops with `hunk at line N does not apply to <file>`.

### Making one

In the decomp checkout (a git repo):

```bash
git diff src/evt/mode_movie.c > "<project>/Packages/<game package>/Native/patches/0100-my-change.patch"
git checkout src/evt/mode_movie.c
```

The build notices new or edited patches and reconfigures by itself. Rebuild the native exe
(`Native\build.ps1`) to try it in the editor, and the guest (`Native\build.ps1 -Guest wasm`)
before packaging.

To experiment, you can edit the patched copy in `Native/build/<config>/gen/patched/` directly,
but the next configure overwrites it. Move the change into a patch before you lose it.

## New C files

Every `.c` in the game package's `Native/game/` is compiled and linked with the game, with
the decomp's headers, defines and the prelude. Mod code there can call any game function and
use any game variable. Hook it in with a patch that calls your function from the right place
in the game.

## What doesn't work

The runtime replaces the GBA's hardware with plain memory and C. Code that depends on the real
CPU or hardware needs rewriting:

- inline `asm`
- copying code into RAM and running it
- loops that wait on hardware state other than VCOUNT and DISPSTAT (polling those two is
  simulated; most other registers only change when the game writes them)
- writing through absolute GBA addresses that aren't one of the decomp's symbols (`REG_*`,
  `VRAM`, `PLTT` and friends are fine)

## Testing

Each game's test exe can play without a window and drive the buttons from the command line.
The options are in the [runtime README](../README.md#building-and-testing-a-game). Always pass a
scratch `--saves` folder so tests don't touch your real save.

Before sharing a change, run the native exe and the wasm test exe
(`Native/build/wasm-RelWithDebInfo/`) with the same `--fuzz` seed for a few tens of thousands
of frames, and compare the dumped frames and the `--wav` output. They should be
byte-identical. If they aren't, the change has undefined behaviour that x86 happens to
tolerate (an uninitialised variable, a read past an array, a NULL pointer). It'll misbehave on
a console sooner or later.

Don't combine `--script` with `--fuzz`: the two are ORed together and can hold
A+B+Select+Start, which soft-resets the game every frame.

## Sharing mods

Share the patch files and any `Native/game/*.c` you added. They're source code against the
decomp and hold none of the game. Don't share the built exe, `Source/Guest/`, `*.agbdata` or
anything from the decomp's `assets/`. Those are made from the ROM.
