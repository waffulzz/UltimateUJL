# AGENTS.md

This file provides guidance to coding agents working with code in this repository.

## What this is

**Ultimate Um Jammer Lammy** (UltimateUJL): a PC port of Um Jammer Lammy built by **static recompilation**: the PS1
executable and its code overlays are translated MIPS → C, then compiled with clang against a hand-written PS1
hardware/BIOS layer. The base game is the JP disc (`SCPS_180.11`) and must boot and play with only that disc. The
USA disc (`SCUS_944.48`), when present, is used purely as an asset source: English logo, tooltips, memory card text,
the credits song and the optional censored Stages 1, 5 and 6. Every game asset is read from the user's own disc
images at runtime.

`docs/ARCHITECTURE.md` is the running engineering notebook: findings, addresses, root causes, the to-do queue. Read the
relevant section before touching a subsystem, and add a note when you learn something non-obvious.

## Hard rules

- Never access tcrf.net or hiddenpalace.org (also for subagents).
- Never ship or commit disc assets, and never paste caption, lyric or other disc text into chat or source. Read it from
  the disc at runtime (`aux_disc_read_file`, `disc_read_file`) and keep only offsets/structure in code.
- The memory card dumps under `X:\DATA\Memory Card Backups\` are read-only test data.
- Disc patches must never alter CD sector headers. The game reads positions back (GetlocL) as its song clock.
- Before launching `ujl.exe`, check `tasklist`: the user may be playing, and a running `ujl.exe` also blocks relinking.
  For your own runs, copy it to `build/cmake/ujl_dev.exe`.
- Run scripted frontend tests on screen by default. While the user is gaming, use `UJL_SCRIPT_HIDDEN=WxH` and
  `SDL_AUDIO_DRIVER=dummy` (no window, no audio).
- Git repo (user waffulzz). Disc images, build output and screenshots are gitignored; SDL and imgui are submodules
  under third_party/ (`git submodule update --init` on a fresh clone).

## Build (Git Bash)

```bash
source tools/env.sh            # llvm-mingw clang, cmake, ninja on PATH; sets MSYS_NO_PATHCONV=1 (needed for /S1/... disc paths)
bash tools/prepare.sh          # once: extract exe + COMOD* overlays from the JP disc, analyze, generate build/gen_jp
bash tools/prepare_us.sh       # optional: build/gen_us for the USA analysis probe
cmake -S . -B build/cmake -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
cmake --build build/cmake                       # all targets
cmake --build build/cmake --target ujl_probe    # just the headless probe
```

macOS and Linux build the same way with clang (Apple clang on macOS). Install `cmake ninja python3` (macOS:
`brew install cmake ninja`; Debian/Ubuntu: `clang cmake ninja-build python3 libgl-dev` plus SDL3's X11/Wayland/audio
dev packages, see `.github/workflows/ci.yml`), then run the same `prepare.sh` and `cmake` commands. Binaries have no
`.exe` suffix there. Host threads that run recompiled code are created with an explicit 8 MB stack
(`rt_create_thread` in `bios.c`), because macOS gives secondary threads only 512 KB.

iOS/iPadOS (on a Mac with Xcode and an iOS Simulator runtime; notes in docs/ARCHITECTURE.md, "iOS"): the same
`build/gen_jp`, cross-compiled into `UltimateUJL.app` (only the `ujl` target exists there). `bash tools/build_ios.sh
"Um Jammer Lammy (JP).bin"` configures `build/ios-sim`, builds, installs on the booted simulator and clones the disc
image into the app's Documents. `bash tools/build_ios.sh --ipa` writes an unsigned `build/ios-device/UltimateUJL.ipa`
for sideloading tools that sign it themselves (IPAs are gitignored: they hold code built from the user's disc, so never
share one). By hand:

```bash
cmake -S . -B build/ios-sim -G Ninja -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT=iphonesimulator \
  -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=15.0 -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
cmake --build build/ios-sim
```

After editing `tools/recomp/gen.py`, `overrides_*.txt` or `symbols_jp.txt`, regenerate. This is the generator step of
`prepare.sh`:

```bash
python -I tools/recomp/gen.py build/exe/SCPS_180.11 build/gen_jp --seeds tools/recomp/seeds_jp.txt \
  --overlays build/exe/ov 801c5110 --overrides tools/recomp/overrides_jp.txt --symbols tools/recomp/symbols_jp.txt
```

Targets: `ujl` (the game: SDL3, OpenGL, ImGui), `ujl_probe` (headless test harness), `ujl_probe_us` (USA build, analysis
only), `ujl_padtest`. Run the game with `build/cmake/ujl.exe "Um Jammer Lammy (JP).bin"` (or `Play UJL.bat`, `play-ujl.sh` on macOS/Linux).
Settings persist in `build/cmake/ujl.ini`. The USA disc is found automatically next to the JP image.

## Testing and debugging

Verification primarily means running the game headless or scripted and inspecting output. The focused
`ujl_score_test` target checks score traces/native HUD primitives using synthetic RAM (no disc needed):
`cmake --build build/cmake --target ujl_score_test`, then `build/cmake/ujl_score_test.exe build`.

- **CI smoke test** (`tests/smoke/smoke.py`, run by `.github/workflows/ci.yml` on Windows, Linux and macOS). CI has no
  disc, so it assembles a small synthetic PS-X EXE onto a generated ISO image, recompiles it with `gen.py`, builds
  every target against that (`-DRECOMP_DIR=build/smoke/gen`), boots it in the probe and checks guest RAM and the
  displayed frame. On Linux it also runs `ujl` under Xvfb and checks the hardware renderer's screenshot. Run it locally
  with a separate build dir:
  `python -I tests/smoke/smoke.py gen build/smoke`, configure `build/smoke/cmake` with
  `-DRECOMP_DIR=<abs path>/build/smoke/gen` (Git Bash: `$(pwd -W)`), build, then
  `python -I tests/smoke/smoke.py run build/smoke/cmake/ujl_probe.exe build/smoke` (and `frontend ... ujl.exe ...`).
  Because of this, the runtime must not reference generated `F_`/`O..._` functions directly: call game functions
  by address with `call_indirect` (see `fbstream.c`), or the smoke build fails to link.

- **Probe** (software renderer, deterministic-ish, fastest):
  `build/cmake/ujl_probe.exe "Um Jammer Lammy (JP).bin" -t SECS [-shot] [-burst SEC N] [-wav] [-unlock] -press T:BTN ...`
  It writes `build/shots/tNNN.bmp` (per second) and `bNNNNN.bmp` (burst, per vblank). The time `t=` counts emulated
  frames, not real seconds. Stage 1 from boot: `-unlock -press 15:start -press 20:start -press 36:down -press 38:circle
  -press 44:circle -press 50:circle -press 53:circle -press 57:circle -press 61:circle -press 65:circle`, with gameplay
  from about 60–75 s. Useful env vars:
  - `PROBE_RECLOG_AT=sec PROBE_RECLOG_N=frames`: dumps the recorded GPU stream to `build/reclog.txt`.
  - `PROBE_CAPTURE_AT=sec`: dumps one frame's primitives.
  - `PROBE_WATCH=lo,hi` (+`_AT`, `_JUMPS`, `_SKIPPC`, `_N`): write-watch with guest stack.
  - `PROBE_WATCH_CLUT=hex`: finds the code that draws a texture.
  - `PROBE_MEMLOG=addr,len,sec,n`: per-frame RAM log.
  - `PROBE_STACK_AT`, `PROBE_COPY`, `PROBE_FREEZE`, `PROBE_POKE`, `PROBE_VRAMDUMP_AT`, `PROBE_CARD=file.mcd`.

  Copy the probe to `ujl_probe_run.exe` before long runs so rebuilds aren't blocked.
  Videos: `-wav -burst START N` writes every vblank to `build/shots/bNNNNN.bmp` (N = 10000 + vblank) and the audio of
  the whole run to `build/shots/audio.wav`, on the same emulated clock (59.826 vblanks/s). Encode with ffmpeg
  (ShareX ships one at `C:\Program Files\ShareX\ffmpeg.exe`): `-framerate 59.826 -start_number <10000+START*60>`, the
  wav with `-ss <START*60/59.826>`, `-vf scale=640:480:flags=neighbor`. Keep files sent to the user under 30 MB
  (crf ~29 for a 3-minute stage).
- **Frontend scripts** (the real hardware renderer): `UJL_SCRIPT=file`, with lines
  `<sec> <button>|shot|burst N|overlay|quit` (`overlay` toggles the F1 settings window).
  Shots go to `build/cmake/shots/hw_vNNNNN.bmp`. Related variables: `UJL_SCRIPT_ASPECT` (0 4:3 … 3 21:9, 5 auto),
  `UJL_SCRIPT_UNLOCK=1`, `UJL_SCRIPT_CARD`, `UJL_SCRIPT_HIDDEN=WxH`. Confirm-with-Cross is on by default, so menus take
  `cross`.
- Runtime stats/A-B switches (all env vars): `UJL_CD_STATS`, `UJL_FLIP_STATS`, `UJL_FPS_STATS`, `UJL_AUDIO_STATS`, `UJL_GTE_STATS`, `UJL_SUBPIX_STATS`,
  `UJL_WAIT_STATS`, `UJL_FAST_LOAD`, `UJL_SOFT_RASTER`, `UJL_FORCE_RATING`, `UJL_FORCE_SCENE` (8,3 = staff roll from Stage 1), `UJL_NCLIP_INT` (integer-only backface test), and
  `UJL_JP_*` to disable each English patch. `UJL_GL_CHECK=1` names the step behind any GL error, `UJL_TOUCH_LOG=1`
  logs finger events (iOS simulator: prefix env vars with `SIMCTL_CHILD_` on `xcrun simctl launch`). `UJL_FORCE_CENSORED=1` serves the USA (censored) Stages 1, 5 and 6
  (`censor.c`; the setting "Uncensored content" is on by default because the JP release is uncensored). `UJL_AUTOPLAY=1/2/3`
  turns on Autoplay / Auto-COOL / Auto-FEVER (`autoplay.c`), `UJL_AUTOPLAY_LOG=1` logs its windows, presses and meter. Setup wizard: `UJL_SETUP=1` forces it, `UJL_SETUP_SHOTS=1` saves each
  page and quits, `UJL_SETUP_DROP="a;b"` drops files on it; scripted runs skip it and, once `setup_done=1` is in
  ujl.ini, use only the discs imported there. `UJL_FORCE_RATING=1` keeps a stage at GOOD (BAD distorts the picture).
  The game log is `build/cmake/ujl.log`.
  Live score comparisons store completed unassisted PB traces beside the executable in `score_traces/`.
  `UJL_SCORE_TRACE_DIR=path` isolates reference files for testing; `UJL_SCORE_LOG=1` logs their sampling/loading.
  `UJL_CARD_LOG=1` logs card file operations and guest callers. Replay files: desktop builds default to
  `-DUJL_REPLAY_FILES=ON` (iOS defaults OFF); the setting/ini key is `replay_files`. The probe opts in with
  `UJL_REPLAY_FILES=1`. `UJL_REPLAY_OPEN=path` / `UJL_REPLAY_SAVE=path` answer a file request without a dialog
  for scripted tests (empty value cancels; errors are logged and cancel). Use only disposable output paths/cards.

## Architecture (big picture)

**Recompiler (`tools/recomp/`).**
- `analyze.py` finds functions.
- `gen.py` emits one C function per MIPS function (`F_xxxxxxxx` for the main exe, `Onn_xxxxxxxx` per overlay). Guest
  registers live in `CPU`, branches become `goto`, indirect calls go through `call_indirect`, and backward branches hit
  `SAFEPOINT`, where time advances and interrupts are delivered.
- `symbols_jp.txt` names functions (`addr name`, `addr@COMOD0.MEN name`): gen.py emits `void name(CPU *c)` instead,
  keeps the F_/Onn_ name as a #define, and stack dumps print the name. Name every function you hook or write up there,
  and use the names in notes and comments.
- The 42 `COMOD*` overlays all load at `0x801c5110`. The runtime picks the active one by a code signature
  (`rt_current_overlay_name()`).
- `overrides_jp.txt` replaces a function with a native one (`name func`) or runs a pre-hook first (`name hook_x`).
  The name comes from `symbols_jp.txt` (with its overlay); a bare `addr` or `addr@COMOD0.MEN` works too.
  A `filter_` pre-hook returns nonzero when it handled the call, or zero to run the original guest body.
- SWC2 of SXY registers becomes `gte_store_sxy` (see GTE below).

**Runtime (`src/runtime/`)**, a register-level PS1:
- `cpu.c`: clock and scheduler. Emulated time follows the host clock. `rt_idle` sleeps until the next event. A loading
  turbo jumps time ahead while instant loading reads data.
- `bios.c`: HLE BIOS, including guest threads, each on its own host thread passing a baton, and IntRP.
- `mem.c`: MMIO, DMA, timers.
- `cdrom.c`: CD controller and XA-ADPCM.
- `spu.c`: voices, capture buffers, reverb.
- `gte.c`: GTE, plus a recent-SXY cache and a RAM-address map that give the renderer 3D tagging and sub-pixel positions.
- `gpu.c`: decodes GP0 into `GpuPrim`.
- `memcard.c`, `sio.c`: memory card.
- `fbstream.c`, `unlock.c`, `autoplay.c`, `censor.c`, `score_delta.c`: game-specific HLE and hooks.

**Rendering path.** `gpu.c` emits primitives to a backend. `gpu_soft.c` rasterizes into `g_vram`. `gpu_rec.c` records
the stream for the frontend; while recording, the software raster is skipped, so `g_vram` lacks drawn pixels.

`src/frontend/hw_renderer.cpp` replays that stream on the UI thread in OpenGL at N× scale, with MSAA, texture
smoothing and sub-pixel vertices.

Widescreen:
- `g_gte_xscale` squeezes the GTE projection.
- Each displayed frame buffer gets a "wide twin".
- GTE-tagged 3D primitives are un-squeezed and full-width; 2D is pillarboxed. HUD sprites rotated around their own
  OFX stay 2D.

`main.cpp` holds the window, ImGui settings, input, audio and the script runner.

**Disc patch layer (`patch.c`, `title_logo.c`).** Files are patched in memory and served in place of the original
sectors (`patch_sector`), so the JP code sees modified data with no code changes. Examples:
- English menu TIMs and the tooltip word sheet.
- Memory card dialogs: the sprite-table w/h are resized, and the GPU-level `mc_text_filter` draws US strings with the
  game's English font.
- US logo injection (`title_logo_filter`).
- The credits song: channel 7 of `ST1.XA` from the USA disc, audio payload only.
- Censored content (`censor.c`, setting off only): USA song vocals, lyric lines, archive entries and the Stage 6
  movie for Stages 1, 5 and 6. Notes in docs/ARCHITECTURE.md (To do, "Uncensored content toggle").

## Shell gotchas

- Git Bash heredocs mangle backslashes, so C string escapes like `\n` get broken. Write C code with the Write/Edit tools,
  or with Python scripts saved to a file.
- `cdrom.c` can't include SDL headers: its global `stat` clashes. Declare the one function you need instead.
- `hw_renderer.cpp` internals sit in an anonymous namespace. Expose things through functions declared in
  `hw_renderer.h`.
