# Replay files: work in progress (handoff, 2026-11-16)

**Completed by Codex, 2026-11-16.** The notes below are the original handoff. The implementation, format,
hook addresses and verification are now documented in `docs/ARCHITECTURE.md`, "Host replay files".
Desktop builds have a "Replays as files" setting; iOS/builds with UJL_REPLAY_FILES=OFF keep card replays.
Returning from a file replay goes to all candy bars without reopening the file dialog.

Task: the to-do item in `docs/ARCHITECTURE.md` ("Replace the replay system with replay files"). Answering YES to the
game's replay-save prompt opens a native save-file dialog and writes the replay to a host file (not the memory
card). "Load replay" opens an open-file dialog and plays the chosen file. Put it behind a build/platform flag plus a
setting, because some targets (iOS without a usable file browser) keep the game's own memory-card replays.

## State of the tree

- Only change so far: `src/runtime/bios.c` got `UJL_CARD_LOG=1` (the `CARD_LOG` macro next to the memcard
  prototypes, ~line 349). It logs lseek/read/write/close/firstfile/nextfile/rename/erase with the guest `ra`.
  `open` was already logged by LOGI. Keep it as a debug switch and document it in AGENTS.md's env-var list.
  The probe builds with it (`ujl_probe.exe`, copied to `ujl_probe_run.exe`).
- Test cards (gitignored build output, from the user's read-only US card dump, never write the original):
  - `build/replay_test_card.mcd`: the BASCUS-94448 system save plus 5 replays renamed to `BISCPS-18011...`.
    Copy it to `build/rt_card.mcd` before each run, because the probe writes the card.
  - `build/rt_blank.mcd`: may exist half-written from an interrupted run. Delete it.
- An interrupted probe run (Stage 1 clear, 300 s) may still be running as `ujl_probe_run.exe`. Check `tasklist`.
- Helper scripts are in the Claude scratchpad, so they may be gone. Rewrite them if needed:
  - `cdir.py`: lists a .mcd directory (frame b*128: status byte, size at +4, next at +8, name at +10).
  - `montage.py`: tiles probe BMPs into one PNG with PIL.

## Findings (verified in the probe)

- Saves use PsyQ **libmcrd** ("bu00:" string at 0x80013134). The JP save base name "BISCPS-18011" is at 0x80012344.
  It's registered by `F_8001ef30` from `F_8002cfe0` (boot init).
- **Directory scan happens at boot** (`firstfile("bu00:*")` ra=80045834, then nextfile ra=80044dc8) and the game
  caches the whole list. The Replay menu (candy bar "Replay") does **no card I/O** to show its chocolate grid. The
  grid (stage number, name) comes from the cached file names. Expect a rescan after a save. Confirm this on the
  save run.
- Replay file name: `BISCPS-18011` + `0` + one char (stage/mode code, e.g. `d`, `f`, `F`, `R`) + player name, e.g.
  `BISCPS-180110dQUINN`. The real US card has `BASCUS-944480dQUINN` (the import already renames these). The size is
  1 block (8192 bytes).
- **Replay load** (selecting a chocolate):
  `open("bu00:BISCPS-180110dQUINN", 0x8001)` (FREAD|FASYNC), `lseek(fd, 512, 0)`,
  `read(fd, 0x80088cd0, 0x1E00)` (async, so the result arrives as an event), `close`. So the file is a 0x200 save header
  (SC, icon, SJIS title) followed by 0x1E00 bytes of replay data, loaded to **0x80088cd0**. The same buffer
  receives the 0x600-byte system data at boot (`read(..., 0x80088cd0, 0x600)` from `BISCPS-18011`). The replay then
  plays with a "REPLAY" HUD. Probe presses for the user's test card (it has a system save, so the title shows NEW
  GAME / MENU): `-press 15:start -press 20:start -press 36:down -press 38:circle` opens the candy-bar menu, then
  `44:down 46:down 48:circle` opens Replay, then `51:up 53:circle` picks one, and loading follows.
  Shots around t=52..80.
- The card's lseek/read callers are libmcrd internals (ra 800444c0/800444f4/80044990), so they are shared by every
  load. Identify a replay by the file name (`BISCPS-180110*`, i.e. the base name plus a suffix starting with `0`),
  not by the caller.
- Menu screens (autoplay.c): 27 SYSTEM SAVE, 28 REPLAY SAVE, 31/29 replay steps. `hook_menu_screen` is a pre-hook
  on `menu_screen_dispatch` (COMOD0.MEN 0x801c99c4), and autoplay already uses it to skip those screens.

## Still unknown (next step)

Capture the **save** flow. Clear Stage 1 on a blank card with `UJL_FORCE_RATING=1` (GOOD with no input) and
`UJL_CARD_LOG=1`, with the standard probe Stage 1 presses (AGENTS.md), and run long enough to pass the results,
name entry (10), SYSTEM SAVE (27) and REPLAY SAVE (28). Press through the prompts and watch the shots for which
buttons are needed. Expect `open(name, 0x200|blocks<<16|...)` FCREAT, then a write of 0x2000 (or 0x200 header + 0x1E00),
then close, then a directory rescan. Note the source buffer of the 0x1E00 replay data (probably 0x80088cd0 again or
the live recording buffer) and how the game picks the name suffix. Also check what it does when a replay with the same
name exists or the card is full: it may first erase, or ask to overwrite.

## Planned design

Intercept at the bu00: file layer in `memcard.c`, so no game code changes are needed:
- New `src/runtime/replay_files.c` (or a section of memcard.c) plus a frontend part in main.cpp. The mode is on when
  the build defines `UJL_REPLAY_FILES` (default on for Windows, macOS and Linux; off for iOS unless the document
  picker is wired) **and** the setting "Replays as files" in ujl.ini is on.
- **Save**: when the game creates/opens for write a `BISCPS-180110*` name, back the fd with a RAM buffer (8192 bytes,
  the same File struct with a `virt` flag) instead of card blocks, so the card never fills up with replays. On close,
  hand the 8 KB (full save image: the header keeps the icon/title) to the UI thread, which calls
  `SDL_ShowSaveFileDialog` (async callback) with a default name like `<name> Stage N.ujlreplay`. Don't block the
  game thread; the game carries on to the next screen. If the game checks free space or existence first (firstfile
  or a cached directory), make the virtual file look created/free. Check this against the save log.
- **Load**: when the Replay menu opens (screen id from the `hook_menu_screen` log: entering 29/31, or the candy-bar
  selection), start `SDL_ShowOpenFileDialog`. Either (a) inject the picked file as a virtual directory entry so the
  grid shows it and opening/reading it is served from the buffer, or (b) simpler: the dialog's pick directly answers
  the `open()` of whatever chocolate is chosen. (a) is cleaner but the directory is cached at boot, so it needs the
  game's rescan path (find the libmcrd directory refresh after a save and trigger it, or patch the cached list in RAM).
  Check the cached directory's RAM layout with `PROBE_WATCH` around the boot scan.
- File format: write the raw 8 KB block (0x200 header + 0x1E00 data), the same as a `.mcb`/bare single save, so
  existing tools and the card import still understand it. Accept `.mcs` (0x80 frame + block) on load too.
- iOS/no-file-system builds keep the plain card path untouched.
- Add `replay_files_*` names to `tools/recomp/symbols_jp.txt` for anything hooked. Update docs/ARCHITECTURE.md
  (move the to-do to done with the findings above) and AGENTS.md (`UJL_CARD_LOG`, any new env var).

## Rules to remember (AGENTS.md)

Check `tasklist` before running `ujl.exe`, and use `build/cmake/ujl_dev.exe` for your own runs. Run frontend scripts on
screen unless the user is playing. Never commit disc data or card dumps. Commits are by waffulzz only, with no
co-author line.
