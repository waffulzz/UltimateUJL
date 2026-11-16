# Ultimate Um Jammer Lammy (UltimateUJL) architecture (working notes)

## Pipeline
1. `tools/psxdisc.py` reads the user's own disc images (Mode2/2352 ISO9660). Nothing from the discs is shipped.
2. `tools/recomp/analyze.py` finds functions in the PS-X EXE (entry, `jal` targets, code pointers in data,
   lui/addiu pointer pairs, jump tables, BIOS A0/B0/C0 stubs).
3. `tools/recomp/gen.py` emits C: one C function per MIPS function, guest regs live in `CPU`,
   intra-function branches become `goto`, delay slots are emitted before the branch (the condition is captured first),
   `jal` becomes a direct C call, `jr ra` becomes `return`, and jump tables become a `switch`. Indirect
   calls go through `call_indirect` (binary search over `recomp_table`). Backward branches hit `SAFEPOINT`,
   which is where IRQs (VBlank, CD, SPU, timers) are delivered.
4. `src/runtime/` is the hardware/BIOS layer the generated code runs on.

## Base executable
JP `SCPS_180.11` (base, uncensored); USA `SCUS_944.48` provides the English layer.
Both load at 0x80010000. Code ends around 0x80054e88 (JP); data follows. PsyQ 1997 libraries.

## Findings
- JP exe: 1176 functions, 21 jump tables, 8 unresolved `jr`, 45 BIOS stubs. All of it compiles with clang.
- The game uses BIOS threads (B0:0E OpenThread, B0:0F CloseThread, B0:10 ChangeThread), so the runtime
  needs cooperative guest threads, each with its own host stack.
- Uses SysEnqIntRP (C0:02/03), so the PsyQ interrupt dispatcher is installed through the BIOS IntRP queues.
- Disc layout: S0..S8 stage folders. `*.IKI` = movies, `CMP*.INT` = TIM texture archives with a name table,
  `COMOD*.SNG/CMP/COP` = per-mode stage data (SNG = solo, CMP = versus vs computer?, COP = co-op),
  `ST*.XA` = streamed music, `WP*.INT` = HUD textures, `CM/*.IK2` = commercials.
  `S*/CMP1x*` / `COMOD1x*` = PaRappa-mode variants.
- The exe references `\S8\ST8A.XA`, which isn't on the disc (cut content?).
- Arcade (UJL NOW!, Namco System 12) CD: `MOVIE/AM01..AM06.IKI` (new cutscenes), `XA/JOE1..6.XA` (Joe Chin
  voice), `MOVIE/NAMCO.IKI`, `ASOBI.IKI`. Arcade program/charts are in the `ul1rom*` flash ROMs.

## Status (2026-11-15)
Project name: **Ultimate Um Jammer Lammy** (UltimateUJL). The whole game plays from the JP disc alone: attract
loop, menus, story mode, every stage in solo, versus and co-op, PaRappa mode, the staff roll, memory card saves.
Sound: SPU voices, reverb and XA through SDL (no SPU noise yet). Two renderers: the software rasterizer (original
320x240) and the OpenGL hardware renderer (up to 8x internal resolution, MSAA, smooth textures, sub-pixel
vertices, widescreen up to 32:9 with the menus pillarboxed; movies stay native 4:3). Builds on Windows, macOS
and Linux; CI runs a disc-free smoke test on all three.
With the USA disc imported (first-run setup wizard, Settings > Discs): English title logo, menus, tooltips,
memory card messages and credits song, plus the option to switch Stages 1, 5 and 6 to the censored USA versions
("Uncensored content", on by default). Also: instant loading, unlock-all, confirm with Cross, Autoplay and
Auto-COOL (watermarked, kept out of the high scores).
Open: SPU noise, the credits-screen line (user report, not reproduced), the arcade (UJL NOW!) features, pause
screen; see To do.
Probe: `ujl_probe.exe -t SECS -shot -wav -press T:BTN ...` → build/shots/tNNN.bmp, audio.wav.

Build: `source tools/env.sh && bash tools/prepare.sh && cmake -S . -B build/cmake -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ && cmake --build build/cmake`

## Things learned the hard way
- **Overlays**: `COMOD*.{MEN,SNG,CMP,COP}` are code overlays loaded at 0x801c5110 (right after BSS). There are 42 on
  the JP disc, recompiled with prefix `Onn_`. The runtime picks the active one by a 32-byte code signature
  (`recomp_overlays.c`). Main-exe functions that only overlays call are seeded from overlay references.
- **Threads**: 5–6 guest threads with a cooperative round-robin scheduler (0x8002e050). Each guest thread runs on
  its own host thread, and they pass a baton (only one runs at a time).
- **Interrupts**: libetc does `setjmp` into the BIOS custom exit (0x80069a48, ra=0x800358e0), so 0x800358e0
  is a seeded continuation. `ReturnFromException` = host `longjmp` back into `bios_exception`.
- IntRP struct: +8 is called first (check), then +4 if the check returned non-zero.
- The pad library drives SIO0 directly from the vblank IntRP handler, so `sio.c` emulates the pad and memory card
  at register level.
- libcd/libpress update DICR with **byte** writes to 0x1F8010F6. 32-bit MMIO blocks must support sub-word
  access (`is_reg32` in mem.c).
- CD data FIFO in 0x924 mode starts at the sector header (offset 12). Setting BFRD again must not rewind.
- `ExitCriticalSection` sets IEc directly (there's no `rfe` in HLE).
- **BIOS kernel tables**: the game builds the table of tables at 0x100 itself (through alloc_kernel_memory). Its
  task creator (0x8002dc80) writes a new thread's SR into `TCB[i]+0x94` (TCBs are 0xC0 bytes, array at
  `*(0x110)`, 12 of them). bios.c mirrors thread state into those TCBs and allows as many threads as TCBs.
  Slots are reused: a parked host thread `longjmp`s back to its entry. (With only 8 slots, task creation failed
  and the menu, subtitles and so on silently never ran.)
- **SPU IRQ**: the sound library init waits for an SPU IRQ (IRQ address 0x1A4, SPUCNT bit 6). It's raised from
  voice block fetches, transfers and the capture-buffer writes the SPU makes every sample.
- Jump-table detection must track constants without crossing writes to the register (`find_const` stops at
  clobbers). A stale `lui` once produced a wrong table.
- **Controller /ACK**: the pad driver clears I_STAT bit 7 right *after* writing each byte, then polls it, and
  before each transfer it waits for SIO_STAT bit 7 (/ACK level) to read as released. So the ack IRQ is
  raised lazily on the next I_STAT read, and the /ACK line is never reported as held (on hardware it's a
  ~2 µs pulse). Getting either wrong makes the game think no controller is connected.
- **Periodic events must be grid-scheduled** (`next += period`). Scheduling relative to when the event fired
  accumulates safepoint lateness: CD sectors ran at 143/s instead of 150/s and XA music underran
  every sector (audible clicking).
- **VSync** (libetc 0x8003562c) has an iteration-count timeout that a native host exhausts in microseconds,
  so the game ran uncapped (captions flashed by, animations broke). It's replaced by `hle_vsync_wait`
  through `tools/recomp/overrides_jp.txt` (the generator's `--overrides` emits a call to a runtime function).
- **English layer (src/runtime/patch.c)**: files are patched in memory and served by `disc_read_sector`, so
  the JP code sees modified assets.
  - Title logo: the Japanese lettering is LGT1/LGT2 (VRAM 512,43 / 512,86). There are 3 copies of each in
    S0/CMP0.INT, two of them in an *unindexed tail* after the TIM table. All are blanked, which leaves the
    shared emblem that reads "Um Jammer Lammy".
  - Startup publisher card (2026-11-16): the same-size menu swap replaces the JP 256x32 TIM with only the
    first US piece (SCEAP41.TIM). The US card is 320x32: SCEAP42.TIM supplies the last 64 pixels. `patch.c`
    reads that continuation from the USA disc and uploads it at (864,288), palette (848,288), when the first
    piece is uploaded. Those rectangles do not overlap the JP menu archive's images/palettes. The GPU filter
    shifts the original sprite left 32 pixels and emits the continuation, keeping both lines centered and
    preserving the original fade. With no USA disc (or menu swapping off), the JP card is unchanged.
  - Captions: **the JP release has its own LANGUAGE option** (main menu: ENGLISH / Japanese) and ships the
    final English subtitles and lyrics. That's why every JP overlay carries English timelines byte-identical to
    the USA ones. The setting is the word at 0x801a5338 (settings block 0x801a5310 + 0x28): 5 = Japanese,
    0 = English. The boot settings init (settings_init) stores 5, then calls settings_init_records. `hook_default_language`
    (src/runtime/language.c, overrides_jp.txt) changes it to 0 at that call, so the game boots in English and
    the in-game LANGUAGE menu still works. `UJL_JAPANESE=1` keeps Japanese. (An earlier hand-rolled caption
    hook re-drew the English lines itself. Retired: attic/captions.c.old.)
- **USA caption system** (found with the USA tracing build, `tools/prepare_us.sh` + `ujl_probe_us`):
  - The text is plain NUL-terminated ASCII with `\n` for 2-line captions, stored in the USA COMOD overlays.
  - Timelines are arrays of 12-byte records `{u32 duration, u32 end_time (cumulative), u32 text_ptr}`. Each
    timeline appears 5x per overlay. Short ones (13–39 entries, end ~1–4k) are cutscene captions, long ones
    (40–100 entries, end ~18–36k) are song lyrics, and tiny 4–5 entry ones are messages.
    Intro/menu: COMOD0.MEN @0x12d94.
  - Renderer (USA exe): F_800260b0(text) -> F_80025d2c(text, 475) draws proportional glyphs from LETTER_E.TIM
    (VRAM 960,385, CLUT 1008,475, 15 px tall, widths 7–11). F_800260f4 renders text into VRAM (768,256), the
    same place the JP release loads its pre-rendered caption sheet.
  - JP caption sheet row counts don't match the USA timelines (e.g. the intro is 10 rows vs ~30 records), so the
    port must drive captions from the USA timeline + a clock and draw the English text itself. It can't just
    fill the JP rows.
  - **The JP disc already contains the final English captions.** JP COMOD0.MEN has the caption timeline at
    file 0x16960 (USA: 0x12d94). It has the same 30 records with identical timing, and its strings are byte-
    identical to the USA ones (checked by hash). The JP game never reads it: a read-watch saw no access during
    the intro. The JP code calls the same renderer (JP caption_render_vram / caption_draw) with a NULL text field and
    shows the pre-rendered Japanese sheet instead.
  - JP equivalents of the USA renderer: caption_draw (=US 800260b0), caption_draw_glyphs (=US 80025d2c), caption_render_vram
    (=US 800260f4). Overlay caller JP 0x801cb4d4 (=US 0x801cbf04) draws obj[1016] (sheet) or obj[1012].
    USA loads its caption timeline in overlay F_801cb868 (JP caption_timeline_load) from a pointer variable (US 0x801d7c88,
    JP 0x801db348). On JP that variable ends up pointing at a different (sequence) table. The branch that makes
    that choice isn't identified yet. Next: read-watch/write-watch that variable and the scene setup
    (JP caption_scene_setup picks directory entry 0 at file 0x17b64), or, failing that, hook JP caption_draw and feed it the
    English line for the current time from the JP-resident timeline.
  - Tooling: the recompiler supports `hook_*` overrides (call a runtime hook, then run the original). The probe
    supports a debug write-watch (`PROBE_WATCH_CLUT`/`PROBE_WATCH_AT`) and prim dumps (`PROBE_DUMP_AT`).
- **Title logo (src/runtime/title_logo.c)**: the USA logo is an 80-triangle mesh (`title_logo_mesh.h`) over three
  USA-only 4bpp images (tail of USA S0/CMP0.INT, CLUTs 752,0/2/3). At the GPU-primitive level the JP lettering
  (tpage 512,0 4bpp, CLUT 560,7/8) is replaced by that mesh and the JP flames (8bpp tpage 960,0, CLUT 576,511)
  are dropped. The images are read at runtime from the user's USA disc, which the frontend locates next to the
  JP disc (`search_usa_disc`; since the setup wizard, the USA disc imported in ujl.ini). Without it, the title keeps
  the shared emblem.
- **Function discovery**: overlay code reached only through pointers (e.g. COMOD11.SNG 0x801c6de0, the Stage 1
  crash) is accepted when it looks like a function start: it follows `jr ra`/`j`/an unconditional branch (+ nop
  padding), or it opens with `addiu sp,sp,-N`. A pointer-table target that no analyzed function covers counts too:
  COMOD12.COP 0x801c6d94 (a type-3 "wait until" handler in a cutscene event table at 0x801ddc48) follows embedded
  sequence data, and the Stage 2 sub 5 intro called it only with player 2 on a controller (FATAL call_indirect).
- **Confirm button**: JP confirms with Circle. `g_confirm_cross` (settings: "Confirm with Cross in menus",
  saved as `confirm_cross` in ujl.ini) swaps Circle/Cross in `rt_pad_state()` while a `*.MEN` overlay is
  loaded, or while the in-stage "AGAIN?" prompt is up. button_swap.c spots that prompt's ○/✕ icon sprites
  (texpage 896,256 4bpp, CLUTs 832,284/285, 32x29) and trades their art, so it reads ✕ YES ○ NO. Songs are
  untouched. When the swap ends, held Circle/Cross are masked until released.
- IKI movies: libpress-style ring (globals at 0x801c0390.., ring headers at *(0x801c03c8)). XA audio sits on
  channel 7, every 8th sector.

## Renderer (high resolution)
- `src/runtime/gpu_rec.c`: the game thread records each frame's primitives, VRAM uploads, fills and copies
  (uploads carry their pixels) and hands the stream to the UI thread at vblank. The software rasterizer
  keeps running, so `g_vram` stays exact for CPU readbacks and for the native frame used by FMV.
- `src/frontend/hw_renderer.cpp`: replays the stream in OpenGL 3.3 into an RGBA8 VRAM at N x (1-8x, "Auto"
  matches the window). A raw VRAM word is stored losslessly (r8 = r5<<3|r5>>2 ..., a = bit 15) so the shader can
  rebuild CLUT indices. Textures are read with texelFetch from a second copy, refreshed from the draw target
  when a primitive samples a 64x64 cell written since the last refresh (render-to-texture). Semi-transparency:
  the 4 PS1 modes map to GL blend state, and textured semi prims draw in two passes (opaque texels, then
  bit-15 texels). Output is true color with no dithering.
- The display texture swizzles alpha to 1: alpha is the PS1 mask bit, and ImGui's alpha blending made every
  bit-15-clear pixel vanish (caption box/text went black).
- FMV (24bpp display) is shown from the native software frame, unscaled, as required.
- Textures setting: "Smooth (vector-like HQ)" (default) vs "Original". Per pixel, over the 4x4 texel
  neighbourhood, clamped to the primitive's UV box so atlas neighbours don't bleed: flat areas (5 or fewer
  distinct colors, near-duplicates merged) pick the color with the strongest Catmull-Rom-smoothed coverage
  field, anti-aliased against the runner-up. Busy areas (gradients) blend the central 2x2 bilinearly.
- "Keep UI pixel-sharp" (default on): textured prims drawn at their original size (all sprites, plus polygons
  whose UV->screen mapping has unit scale on both axes) get F_NEAREST, meaning plain nearest-neighbour upscaling,
  no smoothing. That covers HUD, menus, prompts and captions. Side effect: 1:1 world art (title-screen Lammy,
  logo, Stage 1 foreground crowd) also stays pixel-sharp. Refine with per-texture rules if that's unwanted.
- Render-to-texture: the BAD/AWFUL wave and the AGAIN? background redraw a copy of the screen as 16bpp
  strips. `rendered[][]` tracks 64x64 cells holding GPU-drawn content (draws/fills mark them; an upload that
  fully covers a cell clears it). 16bpp prims sampling such cells get F_HIRES: they read the upscaled pixels
  at sub-texel precision with no smoothing. (Before this they were resampled at 320x240 and then smoothed,
  which made the wave look shredded.) Next: a texture cache that upscales whole textures once (keyed
  by page+CLUT content hash) with a stronger region-based vectorizer, plus a replacement-pack loader.
- Anti-aliasing (setting: Off / 2x / 4x default / 8x MSAA): the draw target is a multisampled renderbuffer.
  It is resolved into the read copy when textures are synced (render-to-texture) and, for the display rect,
  into tex_disp each frame. Samples are capped by GL_MAX_SAMPLES and ~768 MB. Uploads/copies write all samples
  (fullscreen-triangle copies), so texture data stays exact after resolve. Sprite silhouettes (transparent
  texels, which MSAA alone can't smooth) get coverage from the smoothing field through gl_SampleMask. That
  needs GLSL 4.00, so the frontend asks for a GL 4.1 core context and falls back to 3.3 (polygon edges only).
  gl_SampleMask must be written on every path once it's used anywhere, or interiors vanish.
- Stage 1 always redraws the screen through 240 one-line 16bpp strips (the wave pass, flat when rated well),
  so frame-buffer sampling is on the hot path, not a corner case.
- Very magnified low-res art (Stage 1 foreground crowd) comes out with rounded bumps instead of straight
  outlines: the limit of local 4x4 smoothing. The texture-cache upscaler (global contour fitting) would fix it.
- Widescreen (setting "Aspect ratio": 4:3, 16:9, 16:10, 21:9, 32:9, Auto = window; default Auto, hardware
  renderer only). The GTE squeezes projected X by k = (4/3)/aspect (`g_gte_xscale`), so the game draws a wider
  view. gte.c records each projected SXY. gpu.c tags a tri/line `gte3d` when 2+ of its vertices match. Each
  displayed 320x240 frame buffer gets a wide twin (2 LRU slots, MSAA like VRAM). Every prim drawing into a frame
  buffer is drawn into VRAM (exact, for render-to-texture) and into the twin:
    3D -> X un-squeezed about the center (full-width scissor); 2D -> unscaled and centered (pillarboxed) unless
    "Stretch UI to fill (for sadists)"; 16bpp screen copies whose source is a twin -> mapped wide-to-wide
    (F_WIDESRC samples the twin's read copy). Uploads (FMV frames) and partial fills land centered; a full fill
    clears the whole twin. The display shows the twin at the chosen aspect. 24bpp FMV stays 4:3 native.
- Performance (found through a 9 GB "leak"): VRAM and twin batches must sit in separate lists and vertex arrays
  (else ~2000 MSAA target switches per frame and no batch merging). Dirty tracking is per row with 16-px column
  groups, and the texture source is the prim's texel box, not the whole page. The BAD wave shifts rows of the
  frame buffer in place, and the old 64x64-cell tracking forced a flush+blit per strip. The recorder drops the
  backlog and resyncs once 6+ frames are waiting, so a slow moment becomes a stutter instead of a spiral.
  `hw: stats` lines in ujl.log every 600 frames (and whenever a drop happens).
- 3D/2D classification is per whole polygon (both halves of a quad agree): 3D if 2+ vertices exactly match a
  projected SXY from the last 2 frames, or 2+ lie within ~1 cell (8 px) of one. UJL's cartoon outlines and some
  shapes are built from projected points shifted on the CPU, so their vertices aren't exact matches. Tagging them
  2D drew them un-widened, which was the black wedge over Chop Chop Master Onion at 21:9. Menus/title have no projected
  points, so nothing there is tagged 3D, and sprites (HUD) never are.
- Margins: a full fill clears the margins to its color only if the twin showed widened content last time (a 3D
  scene's backdrop), else black pillarbox bars. A shown twin with nothing widened drawn into the margins since its
  last check gets them cleared. That's checked only after new drawing: 30 fps frames are shown for 2 vblanks,
  and checking every vblank wiped the margins of the second showing.
- AGAIN? doesn't redraw its melted background: each frame it fills the back buffer and VRAM-copies the previous
  buffer (320x240) into it, then draws ~10 UI sprites. A whole-buffer copy between two buffers that both have wide
  twins is done twin-to-twin (`copy_wide_to_wide`), so the frozen frame stays full width.
- Fill-rate (Stage 2 burning-building flyover dropped to 10-15 fps): big overlapping semi-transparent smoke/fire
  sprites x 2 passes (PS1 semi) x 2 targets (VRAM + twin) x 32 texel fetches (smoothing) at 5-6x scale.
  Cuts so far: the smoothing shader returns early when the 4 nearest texels match (most pixels), and the
  hidden VRAM pass uses nearest when the prim also draws into a wide twin. If still slow: single-pass semi
  for modes 0/1/3 via dual-source blending (mode 2 needs 2 passes), and a lower auto scale.
  The FPS counter now shows real frames/s next to the emulated vblank rate.
- Full-frame 2D primitives are backdrops/overlays, not UI: Stage 2 clears to grey (0x503232) and paints its
  sky as two flat 2D triangles covering exactly 320x240, which pillarboxed into a "grey box" at 16:9+.
  Untextured full-frame 2D prims always stretch to the wide frame; textured ones only when the twin showed
  widened content last time (3D scene), so 2D menu backgrounds stay pillarboxed.
- World-positioned 2D sprites (if any) would be placed at squeezed X.
- Black window: the frontend shows and raises the window at startup, stops swapping (vsync wait) while the window
  is occluded or minimized (paced with an 8 ms sleep instead), and logs "present: swap stalled N ms" if a present
  takes over 250 ms. Seen only when another app's window overlapped the game at launch; not reproducible since.
- Slow on a Retina Mac (M2, 2560x1600 window, 2026-11-16). Three causes, all fixed:
  - No CMAKE_BUILD_TYPE meant no optimization for anything without its own -O2: the frontend (replay), ImGui
    and SDL were -O0. CMakeLists now defaults to Release. Menu UI rate at 7x went from ~45 to ~58 fps.
  - "Auto" resolution matched the window's pixel height, so a Retina window got 7x (7168x3584 VRAM target, 4x
    MSAA: ~400 MB per target). Apple's tile GPU loads and stores the whole attachment on every render-target
    switch, and the frame switches VRAM <-> twin and resolves (sync_read) a few times per frame: Stage 1 ran
    at 12-20 UI fps while the game thread kept 60. Measured
    (Stage 1 UI fps avg/min): 7x no MSAA 18/5, 5x 2x 31/9, 4x 4x 49/16, 4x 2x 53/22, 3x 4x 56/48, 2x 4x 58/53, so
    it's the scale, not MSAA. On macOS Auto now stops at 3x (the combo says so); higher scales are explicit choices.
  - Window size changes (fullscreen transitions, the menu bar over a fullscreen window on a notched display,
    live resize) changed the auto aspect / auto scale and freed and rebuilt the twins (or all targets) each
    time; the picture fell back to 4:3 until rebuilt. Auto aspect and auto scale now follow the window size
    once it has held for 0.5 s.
  `UJL_FPS_STATS=1` logs game vblank/s, UI frames/s and the scale once a second, plus each 4:3/wide switch of
  the shown picture ("view:"); "aspect:" lines log every change of the widescreen setting in effect.
- Audio pitch (same machine): when the UI thread falls behind, the game thread waits in gpu_rec_vblank (up to
  250 ms), then catches up with the host clock and the SPU's samples arrive in a burst. The output pacing
  absorbed that by speeding playback up to +1.5%, so after each stall the queue sat above target with the speed
  pinned at +1.5% (a quarter semitone sharp), worst in the menus. Now the speed nudge is limited to +-0.5% (clock
  drift only) and bursts are trimmed by leaving out one 64-sample chunk in eight while over 120 ms is queued,
  crossfaded over 32 samples (host_audio_push). `UJL_AUDIO_STATS=1` logs samples made per second, queue range,
  average ratio and trimmed samples.
- Not yet: sub-pixel
  vertex precision (PGXP-style: the GTE outputs integer coords, so 3D wobbles at high resolution).
- Debug: `PROBE_RECLOG_AT=sec` logs the recorded stream of a few frames to build/reclog.txt.

## Memory card, unlocks, sound engine threads (2026-11-15)
- **Memory card (src/runtime/memcard.c)**: HLE of the BIOS card API and the bu00: file system on a standard raw
  128 KB image (ujl_card1.mcd next to the exe; created blank/formatted if missing; flushed ~1 s after a write
  and on exit). Things the game relies on:
  - Card results (`_card_info`, `_card_load`, sector read/write, async file I/O) are *queued* and delivered
    from the next interrupt (memcard_deliver at the start of bios_exception). libcard clears its result flags
    after starting an operation, so delivering inside the call hangs it.
  - SwCARD vs HwCARD: software ops signal both, but the libcard directory scan waits on HwCARD only;
    firstfile/nextfile that find nothing post a HwCARD IOE (otherwise its retry loop never ends).
  - libcard walks the BIOS device table (ToT 0x150/0x154) for the "bu" DCB and hooks DCB+0x34; the HLE BIOS
    provides a dummy "bu" DCB (device_table_init) so the walk succeeds.
  - Async (FASYNC) read/write return 0 = accepted (the game retries while non-zero); the result is an event.
  - Save = 1 block: 0x200 header (SC, icon, SJIS title) + 0x600 system data. The system data is the block at
    0x801a5310 (settings + progress + hi-scores; language at +0x28).
  - Import (settings > Memory card): whole cards (raw .mcr/.mcd/.srm, .gme, .mem/.vgs, .vmp) or single saves
    (.mcs/.psx/.mcb, bare SC data). Mode "copy this game's saves" also takes US BASCUS-94448* saves and renames
    them BISCPS-18011*: the first 0x600 bytes share the layout (verified with a real US card: progress and
    unlocks carried over). Imports run on the game thread at vblank.
- **Host replay files (2026-11-16, src/runtime/replay_files.c)**: desktop builds default to
  `UJL_REPLAY_FILES=ON`; setting/ini `replay_files` selects native file dialogs instead of card replay slots.
  iOS defaults OFF and retains the guest path (enabling it requires a save picker). System saves stay on the card.
  - `replay_directory_entries` (8001fe94) builds the menu's replay list from the boot-cached card directory.
    Its three callers are replay-only: the save prompt's capacity check and `replay_slot_picker`
    (801c7adc@COMOD0.MEN). Conditional `filter_` hooks return nonzero to handle these calls, or zero to execute
    the unchanged guest body. Host mode supplies one virtual slot, so a full card cannot block replays.
    No virtual directory entries or card blocks are persisted.
  - `replay_card_load` (8001ff78) normally reads 0x1e00 bytes at file offset 0x200 into temporary RAM
    (usually 80088cd0), validates `save_checksum` (8001f8e8), then copies 0x1dd4 bytes to 801a3538.
    The raw file is one 8192-byte SC block. Payload +0/+4 are stage (1..7) and mode (0..5); +0x1dd0 is the
    little-endian byte sum of 0x1dd4 bytes with that word zeroed. Host loads validate size, SC header, stage/mode
    and checksum before changing guest RAM. A .mcs/.psx/.mcb directory frame (128 bytes, JP or US replay name)
    followed by the block is also accepted. Bare blocks have no filename metadata; playback uses the payload.
  - Host loading replaces the slot picker, fills its first 40-byte entry (stage/mode at +32/+36), and returns
    slot 0. The original caller then configures the replay and starts it (screens 5 -> 36). Cancellation returns
    -1 through the original exit/fade path. Returning from a file replay normally enters screen 5 again; the
    menu hook redirects that one return to screen 3 (all candy bars), then clears the flag so an explicit
    subsequent Replay selection opens the picker again.
  - After YES at the original save prompt (screen 28), host mode replaces the chocolate slots (31/29) with
    the save dialog, using the current stage/mode/score/rating and the name already entered after the song.
    It skips the second card-name entry screen and uses the original cancel/continuation route after saving.
    A `replay_card_save` (8002011c) filter also handles any in-flight card-name screen when the setting changes.
    `replay_build_save`
    (80020060), using `temp_buffer_alloc`/`temp_buffer_release` (8001473c/800147d0), produces the original SC
    header/icon/title and checksummed recording. Every byte of that content comes from the user's disc/RAM.
    The host writes an adjacent temporary file and renames it over the destination after a complete write.
  - The game thread publishes a request using SDL atomics and services interrupts with `rt_idle` while waiting;
    the UI thread launches SDL's asynchronous dialog. Callbacks publish only a path. The UI handles file I/O
    and errors; invalid choices or failed writes show an error and allow another choice. Cancelling save skips
    it without entering the memory-card error screen. The setting is disabled while a request is active.
    Probe opt-in: `UJL_REPLAY_FILES=1`; `UJL_REPLAY_OPEN` / `UJL_REPLAY_SAVE` supply test paths (empty cancels).
  - Verified: an imported US replay plays from a raw block and .mcs; checksum failure, truncation, missing file
    and cancellation return safely; full-card loading leaves the card byte-for-byte unchanged; Start -> NO
    returns to all candy bars with exactly one file load. Desktop builds and the synthetic smoke test pass
    with the build flag ON and OFF. Native open dialog -> playback verified on Windows.
    Save prompt -> export -> load also verified, along with full-card save, cancelled save and failed-write
    handling (all leave the card unchanged). The save-path probe routes the Replay candy bar to screen 28
    (`PROBE_POKE=801dae9c,8e069c05,40`, stage 1 via `PROBE_FREEZE=80078ae0,1,40,75`), then confirms at 54 s;
    it tests the real prompt/builder/continuation with an empty recording, without having to clear a song.
    A no-input Stage 1 attempt with FORCE_RATING still reached AGAIN?, so that is not a stage-clear test.
- **Unlock all (src/runtime/unlock.c)**: ORs progress bits into 0x801a5310 every vblank: +0x07 stage mask,
  +0x08/09, +0x0A..0F and +0x1A..1F per-stage flags (0x3f), +0x10..18 mode flags. Saving with it on keeps them.
- **Sound engine thread / guitar audio**: the game's sound engine is an object whose tick (vtable entry,
  sound_engine_tick -> sound_flush_key_masks flushes pending key-off/key-on masks) runs in its own guest thread (body
  sound_thread_main). It's scheduled by writing PCB->current TCB (*(0x108)) directly (sound_thread_schedule) and relying on the
  BIOS to resume that thread on exception return. bios_exception now honours that (exc_thread_switch).
  Without it, voices queued in the key-on mask (Lammy's guitar notes, voice lines) were set up and never
  keyed on.
- **Guitar (fixed, src/runtime/fbstream.c + spu.c reverb)**: the guitar goes through the game's
  CSpuFeedbackStream (fbstream.cpp). The sequencer plays the guitar track on voices 1/3 at volume 0; the SPU
  writes their output (post-ADSR, pre-volume) into the capture buffers at 0x800/0xC00. An SPU IRQ on the
  capture position (0x400/0x600, every 256 samples) makes the game DMA the finished half of both capture
  buffers (1536 bytes from 0x800/0xA00), process it (muting when the player misses), and DMA 256 bytes into a
  512-byte ring at 0x7FE00 = the reverb work area (mBASE 0xFFC0). The reverb unit plays it: master enable
  off (no buffer writes), every reverb register 0, vLOUT/vROUT 0x4800, so the output is ring[pos] * vLOUT at
  22050 Hz. Two things were missing: (1) libspu's SpuSetTransferStartAddr rejects addresses
  below 0x1010; the game nops that check (bnez at 0x8004e56c) at init, which recompiled code can't see, so
  hle_spu_set_transfer_addr skips the check once the word there is no longer 0x1440000b. (2) spu.c had no
  voice 1/3 capture and no reverb. Reverb is now the public formula (full IIR/comb/APF, writes gated by
  SPUCNT bit 7), with the buffer position locked to the capture counter (the game times its writes by it).
  Measured: output = voice 1 delayed ~17 ms, correlation 0.84. UJL_RVB_DUMP=file writes reverb out / v1 / v3.
- **Rating (debug)**: player struct 0x80071dbc (+2 rating, +4 meter); rating = table[meter] (rating_from_meter, table
  0x80010b0c: 0-2 COOL, 3-6 GOOD, 7-10 BAD, 11+ AWFUL). UJL_FORCE_RATING=0..3 pins it (hook_force_rating).
  Pinning COOL doesn't start the COOL freestyle; that needs a high phrase score.
- **Widescreen HUD (GTE sprites)**: the head flip and the spinning button symbols are 2D sprites the HUD
  transforms with RotTransPers around their own position (OFX = sprite position), inside small draw-area
  windows (19..53 / 55..298 x 14..47). They matched the GTE tag and were stretched around the screen center,
  landing in the wrong place. gte.c now records OFX per projected vertex, prims carry gte_ofx, and the
  renderer undoes the squeeze around that OFX and places them like 2D UI when OFX isn't the screen center.
- **Fever burst (widescreen)**: large prims that reach the 4:3 edges in 3D scenes are stretched (fever mode's
  burst rays are 2D polygons from the center outwards; one stopped at the 4:3 edge and the stage showed
  through in the margin). From the user's screenshot; not reproduced headless (COOL freestyle needs real play).
- **Sub-pixel vertices (no jitter)**: gte.c keeps the 1/256-px fraction of each projected SXY in a 4-way
  set-associative cache (direct-mapped collisions gave neighbouring polygons different fractions: visible
  cracks); gpu.c attaches it to every exactly matched polygon vertex (GpuVertex fx/fy) and the hardware
  renderer draws 3D at that position. Setting "Sub-pixel precision (no polygon jitter)", ini key subpixel.
- **Front-end menus are pillarboxed**: COMOD0.MEN (title, candy bars, options, stage select) rotates its 2D art
  through the GTE around the screen center, so widescreen stretched it and the menu's beige full-screen clear
  leaked into the margins. main.cpp turns widescreen off while that overlay is active (unless Stretch UI).
  UJL_WIDE_DEBUG=1 logs the first primitive that reaches the margins each frame.
- **Fever burst rule** applies to untextured prims only, and only when the previous frame drew GTE 3D
  (W.last3d): with textured prims and last_wide it kept itself alive in 2D menus.
- **Frontend automation (dev)**: UJL_SCRIPT=file of "<sec> <button>|shot|quit" lines drives ujl.exe and saves
  window shots to shots/hw_vNNNNN.bmp (vblank number); UJL_SCRIPT_CARD (default script_card.mcd, never the
  player's card), UJL_SCRIPT_ASPECT, UJL_SCRIPT_UNLOCK. Confirm-with-Cross is on by default there, so menus
  take "cross". Stage 1 from boot: 15/20 start (skip intro), 36 down, 38/44/50 cross, 53/57/61/65 cross;
  gameplay from ~78 s. The probe's limit is now 4096 presses; PROBE_FREEZE=addr,val,from[,to].
- Rasterizer: the software triangle top-left test was mirrored (quads covered x0+1..x1 instead of x0..x1-1).
  That left 1-px gaps where polygons met sprites (vertical lines on the Hi-Score table and beat lines). Fixed.
- **XA**: stage songs are one XA file with 8 channels; the game switches channels (Setfilter) with the rating.
- **English menu textures**: patch.c swaps same-rect/same-CLUT TIMs in S0/CMP0.INT for the US disc's (14 of
  them: HI-SCORE banners, OPTION candy bar, English font, credits).
- **English tooltip bar (patch.c, hook_tooltip_sprite)**: every candy-bar menu draws its tooltip line from a word
  sheet TIM at (832,350): JP 64x98 with 14-px rows (CLUT 832,448), US 64x112 with 16-px rows (CLUT 832,462). Both
  list the same words in the same order per row. The US sheet is written to free VRAM at (960,256) (CLUT
  960,368) whenever the game uploads the JP sheet (tooltip_note_upload); the JP sheet stays untouched.
  The menu overlay submits each word through menu_sprite_submit (sprite struct: +8 x, +10 y, +12 u, +13 v, +14 clut,
  +16 w, +18 h, +24 draw mode); a pre-hook (overlay-scoped override "801cbc4c@COMOD0.MEN", new in gen.py)
  retargets sheet sprites to the US sheet: the Stage Select builder (stage_select_tooltip_build: 38 messages x 5 slots of x
  and u/v/w tables at 0x801c5848..) gets its tables rewritten from the US overlay's (US builder O00_801cdcbc,
  tables at 0x801c6198.., overlay base 0x801c5750), so only v/clut/page/height change; the other seven builders
  keep JP tables and each JP word rect maps to its US word via tt_words (coordinates only), with x shifted by
  the accumulated width difference. Unknown words stay Japanese (logged once as "tooltip: unmapped JP word").
  UJL_JP_TOOLTIPS=1 disables it.
- **English memory card messages (patch.c english_mc_messages / mc_text_filter)**: the JP menus draw each of 12
  messages as one pre-rendered image (AL_*.TIM) via menu_card_message_draw(msg): msg -> sprite through a 13-byte table
  (COMOD0.MEN +0x5c8), sprite table at +0x18194 (12-byte entries: u16 tpage, u8 u, u8 v, u16 w, u16 h, u16 clut
  x, y), sprite centered, box sized from w/h. The US draws the same messages as strings (US COMOD0.MEN pointer
  table +0x814; JP msg k = US string k-1). At boot (USA disc present) the entries' w/h are set to the English text
  size (font metrics from the exe's glyph table 0x80063c98; 15-px lines; US lines' trailing spaces trimmed), so
  the box fits; at the GPU each message sprite (matched by tpage/clut/uv) is replaced by glyph sprites from
  LETTER_E (VRAM 960,385, CLUT 1008,475; resident in the menus), lines centered. Yes/No (AL_YESNO halves) -> YES /
  NO with the game's highlight tint. UJL_JP_MEMCARD_TEXT=1 disables it. Not covered yet: any memory card message
  shown from a stage overlay (they use their own sprite tables).
- Title logo injection now saves/restores the VRAM it borrows (704,0 etc.): it had clobbered the menu's
  memory card message sheet (the grey "bubble" was the US logo with the menu's palette).
- Probe additions: PROBE_CARD, PROBE_IMPORT(+_MODE), PROBE_VRAMDUMP_AT, PROBE_SPUW_AT, PROBE_RECLOG_N,
  -unlock, -spulog (key-on log + per-voice peaks), -xscale; gp in guest stack dumps.

- **Sub-pixel regressions fixed**: fractions now go only to polygons with 2+ exactly matched vertices and not to
  16bpp (screen-copy) polygons. Applying them to any vertex that happened to match a projected point put seams in
  the US title logo (its injected triangles also inherited the JP polygon's fx/fy: title_logo.c zeroes them) and
  made Stage 1's wave pass (240 one-line strips) choppy.
- **New wide twins start from the frame buffer**: note_display seeds a freshly allocated twin with the native frame,
  centered (was black). Screens drawn once and partly redrawn (loading screen) kept black holes after switching
  to widescreen.
- **Vertical lines at non-standard window aspects** (Auto with a taskbar: 3440x1400-ish, not at true 21:9): the
  2D margin was a fractional number of target pixels. It is now snapped (round(margin*S)/S), and the nearest path clamps to the texel box.
  Centroid interpolation was tried too and reverted: with MSAA the two triangles of a quad then sample different
  UVs along their shared diagonal, a crawling seam on big 1:1 sprites (Stage 1 crowd jitter, drumset).
- **English credits song (patch.c)**: scene 8 (staff roll, /S8/COMOD8.SNG) streams /S1/ST1.XA channel 7 (Stage 1
  uses 0-6). The US file has the same size and layout; only channel 7 differs (vocal version). patch_sector serves
  channel-7 sectors of that file from the USA disc. UJL_JP_CREDITS_SONG=1 disables it.
- Stage table in SCPS_180.11: 0x128-byte entries from 0x8005841c (scene 1 SNG), 3 per stage for 2-7 (SNG/CMP/COP),
  scene 8 (staff roll) at 0x80059a14. stage_entry_set(entry) stores the current entry at 0x801a23ec.
  PROBE_COPY=dst,src,len,sec copies guest memory once (e.g. scene 8's entry over Stage 1's).
- **Scene selection**: the loader scene_loader maps the stage/sub pair at 0x80078ae0/e4 to a scene with scene_for_stage
  (stage+10 for sub 0/1/5, else stage) and a column with scene_column_for_stage (0 = SNG for sub 0/3/4, 1 = CMP/COP). The
  overlay entry comes from the table at 0x8005b3b0 (48 bytes per scene, 16 per column: entry fn, stage-table data,
  next scene). Stage 1 story is stage 1 sub 0 = scene 11 (COMOD11.SNG); the staff roll is scene 8 column 0
  (stage 8 sub 3), overlay entry credits_entry. Debug: UJL_FORCE_SCENE=8,3 turns a Stage 1 story start into the
  staff roll (hook_force_scene, unlock.c). Probe: `UJL_FORCE_SCENE=8,3 ... -press 15:start -press 20:start
  -press 36:down -press 38:circle -press 44:circle -press 50:circle`; the song starts at ~54 s.
- **Credits song cut off (fixed, patch.c hook_credits_hold)**: the JP staff roll ends ~82 s into the song: its
  timeline raises event bit 0x100 (buffer 0x80078330), the timeline thread credits_timeline_thread sets the overlay flag
  0x801c6368, and the main loop credits_main_loop (which also exits on a skip button or song-state bit 0x20000000 of
  0x801a2364) fades out over 90 frames. The JP and US channel-7 songs both fill the whole 145.8 s of ST1.XA; the JP
  roll just fades its instrumental early, which cut the US vocal song mid-verse. While the US song is swapped in,
  a hook on credits_timeline_step (called by the timeline thread right after the flag check, no yield between) holds the
  flag clear, so the roll rests on its final logo card, and raises it itself 120 sectors (~1.6 s) before the
  song's last channel-7 sector: the cue is one-shot, and left alone the CD streams on into the next file. Probe:
  hold at ~136 s, fade at ~198 s, Stage Select at ~201 s.

## iOS / iPadOS (2026-11-16)
- **Build**: CMake's own iOS support (`-DCMAKE_SYSTEM_NAME=iOS`, sysroot `iphonesimulator` or `iphoneos`, arm64,
  deployment target 15.0) with Ninja, the same clang flags and the same `build/gen_jp`; see AGENTS.md. Only `ujl`
  is built there (`UJL_IOS` in CMakeLists.txt): a `MACOSX_BUNDLE` named `UltimateUJL.app`, Info.plist from
  `assets/ios/Info.plist.in`, the home screen icon copied from `assets/icon/ujl_256.png`, bundle id in the cache
  variable `UJL_IOS_BUNDLE_ID`. With Ninja a post-build step signs the bundle (`UJL_IOS_SIGN_IDENTITY`, default
  ad hoc `-`, which is all the simulator needs). For a phone, either pass a real identity plus a provisioning
  profile, or generate an Xcode project (`-G Xcode -DCMAKE_XCODE_ATTRIBUTE_DEVELOPMENT_TEAM=...`) and run it from
  Xcode. `tools/build_ios.sh` builds, installs on the booted simulator and clones disc images into it;
  `--ipa` builds for devices with `UJL_IOS_SIGN_IDENTITY` empty (no signing step), strips the linker's own ad hoc
  signature and zips `Payload/UltimateUJL.app` into an unsigned IPA for sideloading tools (AltStore, Sideloadly)
  that sign it with the user's Apple ID. The Info.plist template carries MinimumOSVersion, UIDeviceFamily and
  CFBundleSupportedPlatforms, which Xcode adds itself and a device checks on install. CI builds
  both SDKs against the smoke test code (job `ios`, compile and link only).
- **Renderer: OpenGL ES 3.0, not Metal.** UIKit still has OpenGL ES 3.0 (deprecated since iOS 12, still in the
  iOS 27 SDK and the simulator), and SDL creates the EAGL context. `hw_renderer.cpp` needed only its shaders
  changed: `glsl()` rewrites `#version 330 core` to `#version 300 es` with highp default precisions, and drops
  `noperspective` (not in GLSL ES 3.00; every vertex has w = 1, so it changes nothing). Everything else it uses
  is in ES 3.0: texelFetch, integer attributes, MSAA renderbuffers + glBlitFramebuffer resolves, alpha swizzle,
  constant-color blending. No GL 4 sample masks there, so sprite-edge AA is off (polygon MSAA works).
  `gl_funcs.h` takes the OpenGLES headers and SDL_GL_GetProcAddress still finds every entry point. ImGui uses its
  ES3 backend (`IMGUI_IMPL_OPENGL_ES3`, `#version 300 es`). A Metal backend would be a rewrite of the renderer for
  no gain while Apple keeps GLES; revisit if it is removed.
- **The window framebuffer isn't 0 on iOS**: UIKit's view owns a framebuffer object, which SDL reports as
  `SDL_PROP_WINDOW_UIKIT_OPENGL_FRAMEBUFFER_NUMBER`. `hw_init` used to finish with `glBindFramebuffer(0)`, which
  draws nowhere there: it now restores whatever was bound, and `main.cpp` binds the window's framebuffer
  (`bind_window_fb`) before drawing the frame. It also rebinds the view's *renderbuffer*
  (`..._OPENGL_RENDERBUFFER_NUMBER`): SDL's UIKit swap presents whatever renderbuffer is bound, and the renderer
  leaves its MSAA renderbuffer bound, so every present failed with GL_INVALID_OPERATION (0x502, logged as "hw: GL
  error" at the next replay) until it did.
- **`UJL_GL_CHECK=1`** (hw_renderer.cpp `gl_check`, main.cpp `gl_check_main`): glGetError after every replayed
  command and every step of the frame loop, naming the step. That is how the swap above was found: the error was
  already pending when a replay started. In the simulator pass env vars as `SIMCTL_CHILD_UJL_GL_CHECK=1 xcrun
  simctl launch booted io.github.waffulzz.UltimateUJL`. `UJL_TOUCH_LOG=1` logs every finger down/up.
- **The iOS Simulator's OpenGL ES is a CPU rasterizer** (`GLRendererFloat` in a `sample` of the process): fine for
  checking that things work, useless for judging speed. Cutscenes ran at 25-35 UI fps at 1x, the front-end menus
  at ~1 fps with smoothed textures and 4x MSAA. In the simulator only (`TARGET_OS_SIMULATOR`), Auto resolution
  stops at 1x and textures/AA default to Original/off; the settings still change them. A real device renders on its
  GPU at the normal defaults (Auto up to 2x, Smooth, 4x MSAA). Not yet measured on a phone.
- **A UI slower than 250 ms per frame flickered** between the picture and bare backgrounds (seen on the title
  screen in the simulator, would happen on any very slow GPU). `gpu_rec_vblank` drops the backlog when the UI
  falls 6 frames behind and restarts from a full VRAM upload, but with the hardware renderer recording, the
  software raster is off and `g_vram` has no drawn pixels: every restart showed the backgrounds alone. Now a drop
  pauses recording for 5 vblanks (`resync_defer`; `rec_prim` rasterizes in software whenever it isn't recording, so
  g_vram gets two whole drawn frames even at 30 fps) and restarts after that. The first version kept the software
  raster on for good after the first drop, which brought back its 20-35 ms game-thread spikes: on an iPhone 16 Pro
  that was lag "sometimes", after any slow moment such as a load. Desktop GPUs that keep up never take this path.
- **Files**: the bundle is read-only, so `platform_data_dir()` (`platform.h`, `platform_ios.mm`) is the app's
  Documents folder and replaces the exe folder (`g_base_dir`): ujl.ini, ujl.log, ujl_imgui.ini, ujl_card1.mcd,
  shots. Info.plist sets `UIFileSharingEnabled` and `LSSupportsOpeningDocumentsInPlace`, so that folder is "On My
  iPhone > Ultimate UJL" in the Files app and shows in Finder's file sharing: disc images are copied in there, and
  the disc search and setup wizard look only there (and in Documents/Inbox), never in parent folders. SDL has no
  file dialog on iOS, so `platform_open_file_dialog` presents `UIDocumentPickerViewController` (any file type,
  `asCopy`, several files at once) and moves the copies into Documents before calling back; the wizard queues
  picks and handles a `.cue` after its data file. **The container moves**: reinstalling (or updating) the app gives
  its data container a new UUID, so the absolute disc path in ujl.ini pointed nowhere and the wizard came back.
  Paths inside Documents are stored relative to it (`disc_path_to_ini` / `disc_path_from_ini`, discs.cpp), and an
  absolute path into an older container's Documents is remapped to the same file in the current one. The wizard's Japanese-disc page has a "Look again" button for
  images copied in with the Files app while it is open. Folder picking (UJL NOW! ROM folder) is hidden there: a
  folder outside the sandbox is only readable through a security scope. Not handled: "Open in Ultimate UJL" from
  another app sends a path SDL can't keep access to (SDL drops the security-scoped URL); use Files or the picker.
- **Lifecycle**: in the background GL calls kill the process, so on `SDL_EVENT_WILL_ENTER_BACKGROUND` the frontend
  writes the memory card out and stops replaying and presenting until `DID_ENTER_FOREGROUND`. The game thread then
  blocks in `gpu_rec_vblank` waiting for the replay, which pauses the game. Landscape only (Info.plist and
  `SDL_HINT_ORIENTATIONS`), full screen, the home indicator dimmed (first swipe doesn't leave the game).
- **Threads**: recompiled code only runs on threads from `rt_create_thread` (the game thread and guest threads),
  which ask SDL for an 8 MB stack. SDL's pthread backend honours that on iOS too (secondary threads default to
  512 KB there as on macOS). `rt_boot` on the main thread only loads the executable, no guest code.
- **Touch controller** (`touch_pad.cpp`): a PS1 pad drawn with ImGui's foreground draw list: d-pad (8-way, a finger
  past cos 67.5 degrees on an axis presses it, so diagonals press two), Triangle/Circle/Cross/Square in the
  PlayStation colours (nearest button within reach), L1/L2/R1/R2, Select, Start, and a menu button that toggles
  the settings window (iOS has no F1/Esc). It reads SDL finger events itself (multi-touch, sliding moves the
  press) and its bits are OR-ed into `g_input_buttons` with the controllers and keyboard. Setting "Touch controls"
  (ujl.ini `touch_controls`): 0 Auto, shown while no controller is connected (SDL gamepad added/removed events
  keep `g_pads` current, checked every frame), 1 Always, 2 Never; the default is Auto on iOS and Never on desktop.
  With a controller connected a touch brings up the menu button alone for 5 s. The pad and the menu button are
  hidden while the settings or the disc wizard are open (the window's close button closes them). A press is
  latched until the frame loop has read it once (`g_latched`), so a tap shorter than one UI frame still reaches
  the game. The Simulator passes the Mac's paired controllers through to iOS (one showed up as "Gamepad"), so
  there Auto correctly keeps the pad hidden; set Touch controls to Always to see it.
- **ImGui on a touch screen**: the SDL3 backend takes the pointer position only from motion events, and a tap's
  press arrived before any motion to the new spot, so every tap clicked where the previous one had been (and the
  first clicked nothing). `imgui_process_event` (main.cpp) feeds the press position first. ImGui doesn't scroll
  by dragging content, so `touchpad_drag_scroll()` (called inside the settings window and the wizard page)
  scrolls on a vertical drag, cancelling the press of whatever button the drag started on. The wizard and the
  settings window are fitted to `SDL_GetWindowSafeArea` (the Dynamic Island covered their left edge). Layout: two columns at the safe area's sides (width 0.36 x safe height,
  96-190 pt); the game picture is fitted into the space between them (`touchpad_fit`), and Auto aspect uses that
  space's aspect instead of the window's, so on a phone the picture stays 4:3 in the pillarbox gap and the buttons
  never cover it. Where that gap would make the picture smaller than 80% of the screen height (iPad, a forced wide
  aspect) the picture is drawn bigger, under the translucent buttons. ImGui is scaled 1.5x (1.35x font) with a
  26 pt scrollbar, and windows move only by their title bar, so touch can drive the settings and wizard.
- **Auto resolution** stops at 2x on iOS (a 2048x1024 VRAM target); higher scales are explicit choices.
- **Verified in the iOS 27.2 Simulator (iPhone 18 Pro), 2026-11-16**: first-run wizard driven by touch (it found
  the JP image in Documents), boot from the JP disc, intro, title, attract demo, and the front-end menus at 60
  vblank/s; the touch pad's START, d-pad and Circle drive the title and candy-bar menus; the menu button opens the
  settings, where Touch controls switches the pad; zero GL errors.
- **First test on a phone (iPhone 16 Pro, sideloaded unsigned IPA, 2026-11-16)**: the FPS line and the Autoplay
  watermark went off screen, the picture stayed squeezed 4:3 between the buttons, and it lagged now and then.
  Changes: overlays are placed in the intersection of the picture, the safe area and the space between the
  button columns (`ui0`/`ui1` in main.cpp); setting "Touch layout" (ujl.ini `touch_layout`), default 0 = the
  picture fills the screen as without the pad (Auto aspect follows the window) and the translucent buttons sit
  over it, 1 = the earlier layout between the columns; the gpu_rec fix above; MSAA off by default on iOS; and the
  frame loop paced to 60 Hz on iOS (SDL has no swap interval for UIKit OpenGL and presenting doesn't wait for
  the display, so the loop could draw frames nobody sees). Setting "Performance details" (next to Show FPS, ujl.ini
  `perf_details`) adds the longest frame, the average replay time and recorder drops to the FPS line and writes
  them to ujl.log once a second, which the Files app can share.
- **Measured on the iPhone 16 Pro (2026-11-16)**, Stage 6 from a script with Autoplay (below), 2x:

  | Textures | GPU per frame avg / max | UI fps avg / min | frames > 40 ms |
  |---|---|---|---|
  | Smooth | 20.1 / 61.5 ms | 39 / 10 | 3.8 per s |
  | Smooth, shader decoding each texel once | 18.5 / 74.8 ms | 41 / 10 | 3.7 per s |
  | Original | 1.1 / 1.7 ms | 59.9 / 56 | ~0 |
  | Smooth at 1x | 3.6 / 5.8 ms | 59.3 / 56 | ~0 |

  The Smooth ("vector") filter is the whole cost: up to 32 dependent texel fetches (index, then CLUT) and two
  16-entry local arrays per pixel, which a phone GPU handles badly; decoding the colors once and stopping the
  distinct-color count at 6 (kept, identical output) saved under 10%. iOS defaults to Original. The GPU cost
  follows the shot (5 to 60 ms with the same command counts), so it showed as random drops. Render-pass breaks
  per frame were few (~1 flush, ~1 read-copy refresh, ~0.5 upload/fill/copy). Limiting frames in flight with
  fences changed little and a glFinish per frame made it worse, so neither is on (`UJL_GL_FENCE=n`,
  `UJL_GL_FINISH=1` remain as iOS experiment switches). The first CPU profile also showed host_vblank converting
  the native frame every vblank (~5% of the game thread) while the hardware renderer showed its own picture; it
  now does that only when the native frame is shown (software renderer, 24-bit movies, VRAM viewer).
- **Testing on a phone**: `bash tools/build_ios.sh --device [UDID] [files...]` builds a development-signed copy
  (bundle id `<id>.dev`, "UJL Dev", the first Apple Development identity and the team's wildcard profile) and
  installs it with `devicectl`, next to any sideloaded copy. Files go to its Documents. Launch with environment:
  `xcrun devicectl device process launch --device UDID --terminate-existing --environment-variables
  '{"UJL_FPS_STATS":"1",...}' io.github.waffulzz.UltimateUJL.dev`; fetch the log with `xcrun devicectl device copy
  from ... --domain-type appDataContainer --domain-identifier <id>.dev --source Documents/ujl.log`. A relative
  `UJL_SCRIPT` is read from Documents, so a script copied there drives a run with no hands: Stage 6 =
  `UJL_SCRIPT=stage6.txt UJL_SCRIPT_UNLOCK=1 UJL_FORCE_SCENE=6,0 UJL_AUTOPLAY=1` with the probe's Stage 1 presses
  (15 start, 20 start, 36 down, 38/44/50/53/57/61/65/69 circle). With UJL_FPS_STATS the log gets a line a second
  (UI fps, worst frame, replay time, counts of uploads/fills/copies/read-copy refreshes), a `hitch:` line for every
  frame over 40 ms with the time per step (events, replay, upload, imgui, render, swap, pace), "gpu_rec: game
  waited", and "wide:" lines for margins cleared and new wide twins (also with the Performance details setting). `UJL_GPU_SYNC=1` adds a glFinish after the
  replay and after the UI draw and logs GPU ms per frame. Instruments: Time Profiler works over USB with
  `--all-processes` (attaching to a pid failed); Metal System Trace timed out "waiting for device to boot" (Xcode
  27.1 with iOS 27.2). The connection over Wi-Fi dropped mid-trace and mid-install; USB held. Not verified: a physical device (signing,
  speed), the document picker (needs Files content in the simulator), the pad reappearing when a controller
  disconnects (the Mac's controller was passed through the whole time; the Auto check runs every frame from
  SDL's gamepad list), backgrounding.

## To do
- **Replace the replay system with replay files (done, user request 2026-11-16)**: see "Host replay files" above.
- **Function names (done, tools/recomp/symbols_jp.txt; user request 2026-11-15)**: `gen.py --symbols` reads
  `addr name` (main exe) and `addr@OVERLAY.NAME name` lines. A named function is emitted as `void name(CPU *c)` with
  `/* F_xxxxxxxx */` above it; the old F_/Onn_ name stays as a #define in recomp_funcs.h / recomp_overlays.h, and the
  dispatch tables keep the address (call_indirect unchanged) plus the name string, so `rt_dump_stack` (PROBE_STACK_AT,
  PROBE_WATCH, call_indirect failures) prints `8001e7a0 hiscore_insert`; `rt_func_name()` looks one up. Override
  lines may start with a name, which brings its overlay along (overrides_jp.txt now uses names only). gen.py
  rejects duplicate names, non-C names and names in the runtime's namespaces (F_/Onn_/L_, hle_, hook_, rt_, cpu_,
  gte_, mem_, bios_, g_, ...), and warns about (and skips) a name on an address that isn't a function start. 60
  names so far: everything the notes, hooks and overrides mentioned (45 main exe, 7 COMOD0.MEN, 4 COMOD11.SNG, 4
  COMOD8.SNG); PsyQ functions keep their library names (StSetStream, SpuSetTransferStartAddr, v_wait, ...). These
  notes and the runtime comments use the names; addresses that are only on the USA disc stay as addresses (no
  symbols_us.txt yet). The smoke build runs gen.py without a map. Add a line to symbols_jp.txt whenever a new
  function gets hooked or written up.
- **Setup wizard (done, src/frontend/discs.cpp)**: ImGui pages Welcome -> Japanese disc -> optional discs -> summary,
  shown before boot on the first start (no `setup_done` in ujl.ini), whenever the JP image can't be found, or with
  UJL_SETUP=1; also Settings > Discs > "Manage discs..." in game (changes apply on restart). Never shown under
  UJL_SCRIPT. Files can be picked (.bin/.img/.iso, or the .cue/.ccd next to one) or dragged onto the window; each is
  identified from its contents and put in its slot whichever button picked it:
  - JP: the boot exe's entry point equals this build's (rt_recomp_entry), so it works for the smoke build too.
  - USA: SCUS_944.48 with exe CRC-32 30FFBCE1.
  - Europe (SCES_017.53) and the Jan 22 1999 prototype ("Previewable" image: boots SCPS_180.11, entry 8002def4, exe
    CRC FED6B0F3) are recognized and rejected with an explanation.
  - UJL NOW! CD (CloneCD .img, no SYSTEM.CNF): MOVIE/AM01.IKI + MOVIE/NAMCO.IKI + XA/JOE1.XA.
  - ROM set (MAME "ujlnow"): CRC-32 + size of the 10 ul1* board ROMs, read from a zip's central directory (no
    unzipping) or a folder (or any file in it). The CHD isn't needed.
  ujl.ini keys: disc, usa_disc, arcade_cd, arcade_roms, setup_done. After setup only imported discs count (the USA
  auto-search next to the JP image only runs before setup, e.g. scripted runs on a fresh ini). Each optional feature
  shows as On / Coming soon / "Needs the X disc". Dev: UJL_SETUP_DROP="a;b" drops files at start, UJL_SETUP_SHOTS=1
  saves each page to shots/setup_pN.bmp and quits (=save: saves and boots); setup: lines in ujl.log give each
  identification.
- **JP disc alone is enough to play (done)**. Only the NTSC-J (SCPS_180.11) image is required; boot and the file
  picker only ask for it. Every USA-disc feature (English logo, menu TIMs, tooltip words, memory card text, credits
  song) checks aux_disc_ready() and is skipped without it, and the JP title lettering is only blanked when the
  English logo is available. Settings > Discs shows which discs were found; "Choose..." stores a USA image as
  ujl.ini usa_disc (checked before the auto-search, used from the next start).
- High-res/widescreen/ultrawide GL renderer (FMV stays 4:3 native); texture modes (vectorized HQ / original).
- **Autoplay / Auto-COOL (done, src/runtime/autoplay.c; Settings > Game > Autoplay, not saved; UJL_AUTOPLAY=1/2,
  UJL_AUTOPLAY_LOG=1)**: player 1's presses are injected into rt_pad_state, so the game's own pad driver and judge
  see them. Found from the controller block (pad_update: 0x80078a20 +4 held, +6 pressed, +8 released; raw buffer
  0x80078a68), then the rating meter's writer back to the judge:
  - song time 0x80078af8; slot = time / 24 (stage_frame). The logic runs every second vblank: it advances the time by
    5-7 units, then reads the pad and stamps the press with the new time (player_read_input -> tick_grid_record, tick grid
    0x801a2410, 396 ticks per player, 4 units per tick). So a press is made once time + 6 is within 3 of the slot.
  - chart: 4-byte records {s16 slot, u8 id, u8 note count} sorted by slot, cursor 0x80071df0; ids 1-6 = Triangle,
    Circle, Cross, Square, L1, R1 (0x8005b848). The records are the player's expected notes at the player's own
    timing. Answer window 0x80071ccc..0x80071cce (32767 closed), scored when it closes (answer_window_close -> answer_window_score ->
    phrase_score): +3 per press of a line button within ~8 units of a slot; -count if the first press isn't the first
    note; penalties for other buttons and for pressing more/fewer than the note count (1.5 each); 0 unless every
    line button was pressed; rhythm bonus rhythm_bonus (pairs of slots: odd only / both / even only / empty, a group of
    all four pays 48, table 0x8005b770/0x8005b774). Rating phrase_rate: score >= threshold (0x80071cda, ~40) cool,
    > 0 good, else bad; meter 0x80071dc0.
  - Autoplay presses the chart exactly: 3 per note, meter stays at 4 (GOOD; Stages 1 and 3 checked). Auto-COOL adds
    presses chosen by a search over the window's slot pairs (rhythm_fill): each pair gets one of the four patterns
    the chart allows, maximising the game's score (per-press points and penalties, -count once for a press before
    the first note, 48 per complete group, the partial bonus) plus a musical weight per added press so it stays on
    the beat: the chart's beat is 4 slots; beat +2, "and" +1, the 16th pickup -3, the 16th after a beat -3.5. In
    COOL the teacher leaves and windows have no chart notes: the scorer's no-teacher path takes any button, so the
    same search freestyles with all six buttons in turn (Stage 1: 185 beat, 34 "and", 186 16th presses; the 16ths
    are the two per group the bonus needs). Stages 1, 3 and 7 reach COOL
    and stay there (scores 70-240 per line); the COOL scenes play.
  - Attract demo: 0x80071ddc == 1 (skipped). High scores: hiscore_insert inserts into the top-3 table (from 0x801a5348,
    {name[8], u16 score}) after the name entry; hook_hiscore_insert zeroes its score argument (sp+16) for a song
    played with either on at any point, so nothing is inserted (checked: table unchanged, 3386 dropped).
  - Auto-FEVER (mode 3): Auto-COOL until COOL, then the stage's fever codes. In a COOL window the stage script's
    opcode handler script_set_fever_table copies a fever table pointer from the stage's list into player 1's block +92
    (0x80071d14; 0 outside COOL windows). Six 32-byte records (one per fever colour; Stage 1: COMOD11.SNG file
    0x143ac): {u16 1, u16 100, u16 30, s16 -15, s16 -50, u8 code[8] (0xF0 | button id, 0 = rest), u8 0xFF[8], u16 0,
    u32 next} - the rows the loading screen shows with Unlock all. fever_ring_press files each on-time press under
    floor(time / 24) in a 32-slot ring (block +57) and resets the ring on a press outside tick 1-4; fever_check
    compares the last 8 slots with each code at every slot boundary and on a match sets fever bit k (block +55),
    a 4-phrase timer (block +90) and resets the ring. So a fever press must be stamped in [slot*24, slot*24 + 7]:
    the press goes on the logic frame whose predicted stamp is closest to the middle, slot*24 + 3.5 (aiming at + 2
    filed some presses a slot early). The time steps come in repeating pairs set by the tempo (Stage 1 7/5, Stage 6
    5/8, sometimes 7/6/7), so the next step is predicted as the one before the last ("12 - last" was 1+ units off in
    Stage 6). fever_ring_press also resets the ring when a slot that already holds a press gets another. Codes are played back to back from the window's second beat (a code on the first slot
    missed), a beat of rest before a code that starts with a press (the post-match reset ate it), the fevers not yet
    running first so a miss is retried. Stage 1: all six stack in the first COOL section; in the second a code can
    miss on a jump of the song clock (CD timing, e.g. +31 units at slot 711) with no window left to retry.
  - Players and 2P: each player has its own plan, timing and fever state. Player p's block is 0x80071cb8 + 96p;
    +48 is its input source (player_read_input: 0/1 controller port, 2-4 the game's COM player, 5 nobody). Autoplay drives
    only players on a controller port, on that port; COM partners/opponents and both players of the attract demo
    (source 3) are left alone. Port 2 reports a pad while Autoplay is on (bios.c rt_pad2_present, sio.c), so 2P
    versus/co-op can be chosen. Mode 0x80078aec: 0 solo, 1 versus (a meter per player, 0x80071dc0 + 10p), 2 co-op
    (one meter). The chart (cursor 0x80071df0) is shared by both players. (0x80071ddc, first taken for a demo flag, is
    also 1 in versus: not used.)
  - Lines (line_plan): the scorer needs the first press to be the line's first note, every line button pressed once,
    the right press count; individual chart notes are optional. Dense co-op lines (every slot a note) leave no room
    for "empty"/"odd only" pairs, so Auto-COOL may leave notes out or move them: the search keeps the first note, never
    presses before it, tracks the press count, and covers the whole window (pairs before the first note count as
    empty). A window is scored as its last slot starts, so nothing is planned on that slot. (The older fill pressed
    the first button one slot early on dense lines: an audible wrong note in 2P.)
  - At COOL the scorer's "teacher" argument is the rating (teacher_rating -> player_rating: player struct +2), and rating 0
    takes the no-teacher path: any button counts. PaRappa mode and versus keep giving lines during COOL, so
    Auto-FEVER enters fever codes in any window where the player is COOL and has a fever table, not just empty ones.
  - Checked (probe; 2P by poking player 2's source to 1 and the active mask 0x80071db8 to 3 after load): Stage 1
    Autoplay GOOD, Auto-COOL COOL after two lines (57-243 per line), Auto-FEVER all six fevers from slot 363 to the end
    of the song (also in the real frontend); Stage 3 solo Lammy and PaRappa Auto-COOL; PaRappa solo Autoplay GOOD and
    Auto-FEVER all six; Lammy and PaRappa co-op both players to COOL; versus both players to COOL, player 1 fevers.
    Real 2P through the menus (Stage Select: stage, "Team" row, Second Player Select: MANUAL Controller 2 / AUTO =
    COM / MANUAL Controller 1): in co-op the players take turns (the active mask 0x80071db8 marks whose turn), and the
    game stamps presses at about the decision time, where solo stamped them ~6 units later. A fixed prediction filed
    2P fever presses a slot early (notes still scored, fevers failed). Press timing now calibrates itself: after each
    press it finds the recorded stamp in the player's tick grid (0x801a2410 + 396p) and keeps a running correction
    (settles near -3 solo, -4 in 2P co-op).
  - Fever turns: stage_frame files a press into the fever ring only while its slot is below block +14; block +12/+14
    hold the player's fever range [start, end), 32767 outside it, set as the window opens (+8 bit 0x80 too). In 2P
    the COOL windows open for both players but alternate as fever turns (Stage 6: player 1 368, 432, 496; player 2
    400, 464), so codes entered in the other player's window were lost; Auto-FEVER enters codes only inside the
    range. Each player has its own six fevers (an earlier colour split between the players only worked around the
    lost codes). In versus the player whose turn it isn't rests: its freestyle presses ran up a score lead that ended
    the match early (KO at slot 431). Checked: Stage 6 versus (COMOD6.CMP, COMOD16.CMP) 5-6 fevers each until the
    match ends at slot 527 (was 2 and 0); Stage 6 and Stage 3 co-op both players all six; Stage 1 solo and PaRappa
    Stage 3 solo all six; Stage 1 Autoplay GOOD.
  - After an autoplayed song the system-save and replay-save screens are skipped (hook_menu_screen, pre-hook on the
    menu overlay's screen dispatcher menu_screen_dispatch: the screen loop menu_screen_loop calls it with the screen number and
    it returns the next). Screens: 10 name entry, 30 a step after it, 27 SYSTEM SAVE, 28 REPLAY SAVE, 31/29 replay,
    3 candy-bar main menu, 12/4 Stage Select, 35 stage start. 27/28/29/31 become 3 while armed (armed when the song
    ends, disarmed at the main menu, so Load & Save from the candy bars still works). Name entry still shows (the
    high-score insert is already blocked).
- **Uncensored content toggle (done, src/runtime/censor.c)**: the JP release is the uncensored one, so the setting
  defaults to on; off (needs the USA disc) serves the USA versions of Stages 1, 5 and 6 through the sector patch
  layer, applied from the next stage loaded (ini `uncensored`, UJL_FORCE_CENSORED=1 in the probe). Disc comparison
  (Stages 2, 3, 4 and 7 have no censorship differences; every overlay and archive also differs for English-only
  reasons, so Stage 2 was the control):
  - Song XA: ST1/ST5/ST6.XA same size and layout on both discs; S1 channels 2-4 and 6 differ in 52 sectors each
    (channel 7 = credits song, handled by the credits patch), S5/S6 channels 2-7 in 100-270 sectors. Audio sectors
    are copied from the USA file, headers kept (song clock).
  - Lyrics: one English lyric timeline per song overlay differs in a few records (identical timing): COMOD1.SNG 1,
    COMOD11.SNG 2, COMOD5.SNG 8, COMOD15.SNG 3, COMOD6.SNG 8, COMOD16.SNG 2 (+ the CMP/COP variants). USA overlays
    load at 0x801c5750 (JP 0x801c5110). The USA lines (some are longer) go into a pool at 0x80005000 (kernel RAM the
    HLE BIOS leaves free, 0x5000-0x6FFF) and the records point there. Checked in RAM: the loaded record points into
    the pool, which holds the line.
  - Archives (CMP*.INT): sections = {type, count, data sectors, ?} header + 20-byte {size, name} entries, padded to
    0x2000, then the data; the next section follows. The game has no entry names in its code (index access), so
    each JP index keeps its meaning: same-named entries get the USA data, JP-only entries (MVST*SQ.TIM, the Japanese
    caption sheet) lend their slot to USA-only ones (Stage 6: LA12.TIM, the camo boot), the rest are appended if
    they fit. A section that outgrows its space grows into the 8 KB free at the end of the file and the later
    sections move down (the loader walks the headers: checked in Stage 6 PaRappa and co-op). Contents: S5/S6 voice
    sample banks (S5RR_R/S6RR_R .VB/.VH), Lammy's Stage 6 camo outfit (LA06/09/10/11 + LA12, LA.TMD, *.DAT),
    Stage 1 animations (*_TI.TOD, ON_TO2.TOD). The USA-only face textures (F_LA*.TIM) don't fit and are left out.
  - Stage 6 movie: S6.IKI re-encoded on the USA disc (1964 frames vs 2043, every frame different). The game calls
    StSetStream with end frame -1; the movie loop (movie_loop/movie_next_frame) ends after frame
    `directory size / 20480 - 5` (JP 2043, USA 1964), so the USA stream is served from the JP file's start, its
    end-of-file sector repeats past its end, and the /S6 directory record reports the USA size. Not yet watched in
    game (the probe's menu timing kept landing in the attract loop instead of Stage 6 story).
- Stage 1 versus/PaRappa restoration, UJL NOW! Joe Chin cutscenes.
- A real pause screen, separate from the "AGAIN?" prompt (the JP game reuses AGAIN? when Start is pressed
  mid-song): resume / retry / quit / settings, drawn by the port.
- Setting: play the UJL NOW! arcade attract movies (arcade CD MOVIE/*.IKI) in the attract loop (needs the
  arcade CD image; off/absent = console attract).
- Uncapped framerate: the game logic stays at its native tick. Rendering above 60 fps needs interpolation
  between consecutive frames' primitive lists (match prims frame to frame, lerp vertices), with a fallback
  to plain 60 when matching fails. Research item: may not be clean for every scene.
- SPU noise (reverb and memory card saves are done; Stage 8 has no captions).
- **Score comparison vs personal best (user request 2026-11-15)**: next to the score counter, show how the current
  run compares to the best run at the same point in the song: ahead (improving), on track, or behind
  (underperforming), e.g. a +/- delta coloured green/yellow/red. Needs: read the live score from RAM (player struct
  near 0x80071dbc), sample it against song time (0x80078af8) during play, save the trace of each stage's best run per
  stage/mode/difficulty in a port-side file (not the memory card), and draw the delta as a port overlay beside the
  game's score (pillarbox-aware). Optional: compare against the best run's final score instead of the trace.
- **Discord Rich Presence (user request 2026-11-15)**: show what's being played (menu / stage name / Lammy or
  PaRappa mode, maybe the rating), optional and off-able in settings. Read the state from game RAM (current scene
  entry at 0x801a23ec). Talk to the Discord client over its local IPC pipe (discord-ipc-0) directly, so no SDK
  has to be shipped.
- **UJL NOW! import in the setup wizard + arcade audio (user request 2026-11-15)**: the disc import step also accepts
  the UJL NOW! arcade CD image (CloneCD .img/.ccd) and ROM set. When imported, all of Lammy's guitar sounds and
  PaRappa's voice lines are replaced with the arcade versions (much higher quality than the PS1 samples). Needs:
  find the guitar/voice samples in the arcade data, map each to the PS1 VAB/XA sample it replaces, resample/encode
  as needed at load time (never shipped).
- Credits screen: a big vertical line on the staff roll and a flickering line left of it (user report; not yet
  reproduced: the PROBE_COPY stage-table swap still loads Stage 1, so the menu doesn't pick scenes from that table).
- **Drum kit stands blinking at a far camera (fixed, gte.c NCLIP)**: in Stage 1's wide shots the cymbal and hi-hat
  stands vanished every other frame or so, worst at ultrawide. The game backface-culls on NCLIP's sign; the stands
  are under a native pixel wide there (the widescreen squeeze g_gte_xscale makes them ~1.8x thinner), so the area
  of the integer SXY points is 0 or flips sign as the camera drifts. NCLIP now takes the sign from the points'
  sub-pixel positions (fifo_dx/dy) when all three came from an RTP and the signs disagree. Measured at 3440x1440
  (UJL_FORCE_RATING=1, burst at 76 s): stand pixels per frame alternated ~2200 / 200-900 before, steady 2900-4200
  after. UJL_NCLIP_INT=1 restores the integer test. Note: in BAD the stage's wobble effect draws the stands as
  stair-stepped dashes; that's the game's effect, not the renderer, so check renderer issues at GOOD.
- Same-frame SXY collisions (~100/frame) used to hand vertices another point's sub-pixel fraction. gen.py emits
  gte_store_sxy for SWC2 of SXY0-2/SXYP (store + record the RAM address with the FIFO slot's precise offset; gte.c
  keeps per-slot offsets through the FIFO), mem.c passes each linked-list word's address (g_gpu_word_addr), and
  do_polygon looks offsets up by address (value-checked) before falling back to the SXY value cache: 99.8% of
  Stage 1 vertices resolve by address. UJL_SUBPIX_STATS / UJL_GTE_STATS log coverage, H/SZ and collisions.
  Full-precision projection (offsets > 1 px from MAC/z) exists behind g_gte_precise (off): with value lookups it
  shook the crowd; worth retrying now that lookups are by address.
- Seen once: a segfault around the scripted quit after a fullscreen 3440x1440 burst (exit code 139, log clean, not
  reproduced in the next runs). Likely a shutdown race; watch for it.
- **Crowd jitter (Stage 1)**: "Keep UI pixel-sharp" picked nearest sampling per triangle by UV scale (0.85-1.18).
  The foreground crowd is 3D art at 0.80-0.87 scale, so its triangles flipped between sharp and smoothed as the
  camera drifted, even the two halves of one sprite. Pixel-sharp is now 2D-only (plus GTE-rotated HUD sprites,
  detected by an OFX away from the frame-buffer center).
- **Stage 1 crowd "snapping back" ~once a second = missed vblanks (gpu_rec.c)**: with the hardware renderer on, every
  primitive was still also rasterized in software into g_vram on the game thread (~2.4 ms/vblank on average, spikes
  of 20-35 ms). A spike pushed the frame's work past the next vblank, so the frame took 3 vblanks instead of 2 and
  the crowd's beat-synced bobbing visibly jumped. rec_prim now skips the software raster while recording
  (UJL_SOFT_RASTER=1 keeps it). g_vram still gets uploads/fills/copies; UJL never reads VRAM back to the CPU (gpu.c
  logs any readback). Measured in the real build: late vblanks during Stage 1 went from 2-5 per run to 0.
  UJL_CD_STATS logs late vblanks (with irq/sleep/soft-raster time), CD commands, timer reads, safepoints/vblank.
- **Instant loading (setting "Instant loading (skip loading screens)", ini fast_load, default off; UJL_FAST_LOAD=1 in
  the probe)**. (1) cdrom.c: while XA-ADPCM is off (data reads), each sector arrives FAST_SECTOR (3000 cycles) after
  the game took the previous one (INT acked, queue empty) instead of at 75/150 per second. Movies, title cards and
  songs stream with XA on, so they keep real speed and the song clock is untouched. (2) Loading turbo: every
  fast-loaded data sector sets g_load_turbo_until = now + 0.5 s (emulated); until then (or until a ReadS with XA on)
  clock_update runs time ahead of the host (jump to the next event when idle, +20000 cycles per safepoint) and
  re-anchors host_t0, so the frames a loader spends between files and behind its loading screen pass instantly. The
  frontend holds the last picture and mutes audio while rt_load_turbo_active(). Stage Select -> Stage 1, real time:
  9.36 s -> 2.77 s, which is now almost all the stage title card (kept: it has a voice line). Loading screen anatomy
  (Stage 1): title card sprites (S1.IKI ReadS for its voice), then the purple loading screen drawn by the 2D
  sprite-script thread sprite_script_thread while the loader reads CMP11SNG.INT/S1 files with ~10 frames between files.
- **Green screen after a fast-forwarded load (fixed)**: the turbo outran the UI thread, the recorder dropped its
  backlog and resynced the GPU from g_vram, which has no drawn pixels since the software raster skip, so the
  frame buffers showed stale data. gpu_rec_vblank now waits (up to 250 ms) for the UI thread instead of dropping
  while the software raster is off.
- **Stage 1 crowd "snapping back" ~once a second: SOLVED (patch.c credits swap)**. Chain: the crowd (clut 16155) plays
  32-frame morph clips (anim_update -> morph_clip_apply -> morph_blend, GPF-weighted pose deltas); the clip restarts on stage
  choreography cues (stage1_cue_dispatch <- per-frame event buffer 0x80078330 filled by the timeline thread stage1_timeline_thread ->
  timeline_events_fill, time from stage1_song_time -> 0x80078af8 <- song_clock_update <- raw clock 0x801a2360). That raw clock is written
  by the CD callback cd_getloc_callback from GetlocL's result: the MSF in the header of the last sector read, minus the
  song's start. The credits-song swap copied whole USA sectors, header included, and the USA ST1.XA sits 34 sectors
  earlier, so every 8th sector the clock jumped back ~33 sectors (-33/+35 per vblank). The timeline clamps it to
  max(previous), so cues fired late/early by a frame or two and clips were cut at t=29/30 (pop). Fix: copy only
  subheader + audio (bytes 16..). After: clock steps +1/+2 per vblank; clips end at t=31 apart from rare 1-frame
  cue rounding. Lesson: never substitute sector headers; the game reads positions back.
- Found along the way and kept: the software raster skip while hardware-recording (fewer late vblanks), the
  vblank-latched GetlocL while XA streams, and 2D-only pixel-sharp sampling. The Settings > Debug A/B toggles and
  the morph/anim logging hooks are removed (re-add via overrides: hook_morph_log / hook_anim_log in unlock.c).
- Tried and left off: capping emulated time during game code (UJL_COMPUTE_CLOCK=1) made pacing worse.
- **Song clock (cdrom.c)**: UJL issues GetlocL ~60x/s while the XA song streams. Recompiled code takes no emulated
  time, so the query's phase within the frame followed host speed and the position came back lumpy. GetlocL now
  answers with the header latched at the last vblank while XA streams (cdrom_vblank). The Stage 1 camera does not
  follow it (crowd motion was identical), so this wasn't the crowd jitter. The camera's per-frame steps ripple
  (~1.25/1.50 px alternating) identically run to run: game-side, likely integer camera math.
- Settings > Debug has unsaved A/B toggles for the night's renderer changes (edge clamp, margin snap, 2D-only
  pixel-sharp, wide-buffer seeding, sub-pixel by address). The user tried all: none affects the crowd jitter.
- **Solid thin geometry** (setting, default on; ini thin_fix): 3D triangles under 0.9 native px thick across their
  longest edge (63 of the 253 drum-kit triangles in Stage 1: drum and cymbal stands) are widened to 1 native px.
  At high internal resolution they were drawn a fraction of a pixel wide and broke up/flickered as they moved.
- Frontend scripts: "<sec> burst <n>" takes a shot every vblank for n vblanks. UJL_SCRIPT_HIDDEN=WxH renders offscreen
  behind a hidden window (the user prefers runs on screen, so it's not the default).
- gen.py: branches whose target is outside the function (overlay data swept as code) become tail calls instead of
  undefined gotos (US COMOD0.MEN tooltip tables broke the US build).
