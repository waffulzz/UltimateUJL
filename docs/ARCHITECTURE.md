# UJL-PC architecture (working notes)

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

## Status (2026-11-14)
Boots from the JP disc and runs the full attract loop: logo, intro FMV, lobby FMV, Stage 1 story FMV with
subtitles, title movie, title screen ("NEW GAME / MENU"), LOADING screen, and the Stage 3 gameplay demo
(COMOD3.COP, about 1000+ prims per frame through the software rasterizer). Then it loops. Sound output works
(SPU voices + XA through SDL). No reverb or noise yet, and no high-res/widescreen renderer yet.
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
  - Captions: **the JP release has its own LANGUAGE option** (main menu: ENGLISH / Japanese) and ships the
    final English subtitles and lyrics. That's why every JP overlay carries English timelines byte-identical to
    the USA ones. The setting is the word at 0x801a5338 (settings block 0x801a5310 + 0x28): 5 = Japanese,
    0 = English. The boot settings init F_8001e498 stores 5, then calls F_8001e42c. `hook_default_language`
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
    the intro. The JP code calls the same renderer (JP F_8002623c / F_800261f8) with a NULL text field and
    shows the pre-rendered Japanese sheet instead.
  - JP equivalents of the USA renderer: F_800261f8 (=US 800260b0), F_80025e74 (=US 80025d2c), F_8002623c
    (=US 800260f4). Overlay caller JP 0x801cb4d4 (=US 0x801cbf04) draws obj[1016] (sheet) or obj[1012].
    USA loads its caption timeline in overlay F_801cb868 (JP F_801cae58) from a pointer variable (US 0x801d7c88,
    JP 0x801db348). On JP that variable ends up pointing at a different (sequence) table. The branch that makes
    that choice isn't identified yet. Next: read-watch/write-watch that variable and the scene setup
    (JP F_801cad58 picks directory entry 0 at file 0x17b64), or, failing that, hook JP F_800261f8 and feed it the
    English line for the current time from the JP-resident timeline.
  - Tooling: the recompiler supports `hook_*` overrides (call a runtime hook, then run the original). The probe
    supports a debug write-watch (`PROBE_WATCH_CLUT`/`PROBE_WATCH_AT`) and prim dumps (`PROBE_DUMP_AT`).
- **Title logo (src/runtime/title_logo.c)**: the USA logo is an 80-triangle mesh (`title_logo_mesh.h`) over three
  USA-only 4bpp images (tail of USA S0/CMP0.INT, CLUTs 752,0/2/3). At the GPU-primitive level the JP lettering
  (tpage 512,0 4bpp, CLUT 560,7/8) is replaced by that mesh and the JP flames (8bpp tpage 960,0, CLUT 576,511)
  are dropped. The images are read at runtime from the user's USA disc, which the frontend locates next to the
  JP disc (`search_usa_disc`). Without it, the title keeps the shared emblem.
- **Function discovery**: overlay code reached only through pointers (e.g. COMOD11.SNG 0x801c6de0, the Stage 1
  crash) is accepted when it looks like a function start: it follows `jr ra`/`j`/an unconditional branch (+ nop
  padding), or it opens with `addiu sp,sp,-N`.
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
- **Unlock all (src/runtime/unlock.c)**: ORs progress bits into 0x801a5310 every vblank: +0x07 stage mask,
  +0x08/09, +0x0A..0F and +0x1A..1F per-stage flags (0x3f), +0x10..18 mode flags. Saving with it on keeps them.
- **Sound engine thread / guitar audio**: the game's sound engine is an object whose tick (vtable entry,
  F_8004cca4 -> F_80051784 flushes pending key-off/key-on masks) runs in its own guest thread (body
  F_800543bc). It's scheduled by writing PCB->current TCB (*(0x108)) directly (F_8005430c) and relying on the
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
  22050 Hz. Two things were missing: (1) libspu's SpuSetTransferStartAddr (F_8004e554) rejects addresses
  below 0x1010; the game nops that check (bnez at 0x8004e56c) at init, which recompiled code can't see, so
  hle_spu_set_transfer_addr skips the check once the word there is no longer 0x1440000b. (2) spu.c had no
  voice 1/3 capture and no reverb. Reverb is now the public formula (full IIR/comb/APF, writes gated by
  SPUCNT bit 7), with the buffer position locked to the capture counter (the game times its writes by it).
  Measured: output = voice 1 delayed ~17 ms, correlation 0.84. UJL_RVB_DUMP=file writes reverb out / v1 / v3.
- **Rating (debug)**: player struct 0x80071dbc (+2 rating, +4 meter); rating = table[meter] (F_80019580, table
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
  The menu overlay submits each word through O00_801cbc4c (sprite struct: +8 x, +10 y, +12 u, +13 v, +14 clut,
  +16 w, +18 h, +24 draw mode); a pre-hook (overlay-scoped override "801cbc4c@COMOD0.MEN", new in gen.py)
  retargets sheet sprites to the US sheet: the Stage Select builder (O00_801cd2a0: 38 messages x 5 slots of x
  and u/v/w tables at 0x801c5848..) gets its tables rewritten from the US overlay's (US builder O00_801cdcbc,
  tables at 0x801c6198.., overlay base 0x801c5750), so only v/clut/page/height change; the other seven builders
  keep JP tables and each JP word rect maps to its US word via tt_words (coordinates only), with x shifted by
  the accumulated width difference. Unknown words stay Japanese (logged once as "tooltip: unmapped JP word").
  UJL_JP_TOOLTIPS=1 disables it.
- **English memory card messages (patch.c english_mc_messages / mc_text_filter)**: the JP menus draw each of 12
  messages as one pre-rendered image (AL_*.TIM) via O00_801cc188(msg): msg -> sprite through a 13-byte table
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
  scene 8 (staff roll) at 0x80059a14. F_80016720(entry) stores the current entry at 0x801a23ec.
  PROBE_COPY=dst,src,len,sec copies guest memory once (e.g. scene 8's entry over Stage 1's).

## To do
- **Setup wizard (first run)**: imports the user's discs and gates features on what was imported.
  Required: JP disc (SCPS_180.11). Optional: USA disc (English title logo), arcade UJL NOW! CD/ROMs
  (attract movies, Joe Chin cutscenes, Stage 1 versus/PaRappa content), and maybe Europe/prototype later. Each disc is verified
  (SYSTEM.CNF / file hashes), remembered in ujl.ini, and each optional feature shows as locked with
  "needs the X disc" until imported. Styled like the game's own menus (the MilkCan candy-bar cards), not
  ImGui. The first screen (before any disc) is drawn procedurally in that style. Once the JP disc is imported,
  later steps use the real menu art read from it at runtime (never shipped). Also reachable from settings
  ("Manage discs").
- **JP disc alone must be enough to play.** Only the NTSC-J (SCPS_180.11) image is required. Features that use
  USA-only assets (English title logo, any later USA re-localization) turn on when a USA disc is also found and
  are simply skipped without it. English captions already come from the JP disc itself. Current gap: with no
  USA disc the patch layer still blanks the JP title lettering, so the title shows only the emblem. Without a
  USA disc it should leave the JP logo (and flames) untouched. Also: never require the USA disc in boot or the
  file picker, and show which optional discs were found in the settings overlay.
- High-res/widescreen/ultrawide GL renderer (FMV stays 4:3 native); texture modes (vectorized HQ / original).
- Autoplay + Auto-COOL, both watermarking gameplay.
- Uncensor toggle (JP S6/S7 differences), Stage 1 versus/PaRappa restoration, UJL NOW! Joe Chin cutscenes.
- Check other ○/✕ prompts (results, save/load, menus' button hints) for the Confirm-with-Cross icon swap.
- A real pause screen, separate from the "AGAIN?" prompt (the JP game reuses AGAIN? when Start is pressed
  mid-song): resume / retry / quit / settings, drawn by the port.
- Setting: play the UJL NOW! arcade attract movies (arcade CD MOVIE/*.IKI) in the attract loop (needs the
  arcade CD image; off/absent = console attract).
- Uncapped framerate: the game logic stays at its native tick. Rendering above 60 fps needs interpolation
  between consecutive frames' primitive lists (match prims frame to frame, lerp vertices), with a fallback
  to plain 60 when matching fails. Research item: may not be clean for every scene.
- SPU reverb/noise, memory card saves, Stage 8 captions check.
- **Credits song cut off (user report 2026-11-15)**: the JP staff roll after Stage 7 (scene 8) is shorter than the US
  vocal credits song now swapped in (ST1.XA channel 7), so the song stops early. Options: extend the JP staff roll's
  timeline/end condition to the US song's length (the US COMOD8.SNG is larger, likely a longer roll), or end on the
  song's own end (XA EOF), or fade out. Needs a look at how scene 8 decides it's finished.
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
- Drumset (Stage 1) jitter, fix in (needs a check in play): same-frame SXY collisions (~100/frame) handed vertices
  another point's sub-pixel fraction. Now gen.py emits gte_store_sxy for SWC2 of SXY0-2/SXYP (store + record the
  RAM address with the FIFO slot's precise offset; gte.c keeps per-slot offsets through the FIFO), mem.c passes each
  linked-list word's address (g_gpu_word_addr), and do_polygon looks offsets up by address (value-checked) before
  falling back to the SXY value cache: 99.8% of Stage 1 vertices resolve by address. UJL_SUBPIX_STATS /
  UJL_GTE_STATS log coverage, H/SZ and collisions. Full-precision projection (offsets > 1 px from MAC/z) exists
  behind g_gte_precise (off): with value lookups it shook the crowd; worth retrying now that lookups are by address.
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
  sprite-script thread F_80021940 while the loader reads CMP11SNG.INT/S1 files with ~10 frames between files.
- **Green screen after a fast-forwarded load (fixed)**: the turbo outran the UI thread, the recorder dropped its
  backlog and resynced the GPU from g_vram, which has no drawn pixels since the software raster skip, so the
  frame buffers showed stale data. gpu_rec_vblank now waits (up to 250 ms) for the UI thread instead of dropping
  while the software raster is off.
- **Stage 1 crowd "snapping back" ~once a second: SOLVED (patch.c credits swap)**. Chain: the crowd (clut 16155) plays
  32-frame morph clips (F_80023b0c -> F_80023340 -> F_8003d944, GPF-weighted pose deltas); the clip restarts on stage
  choreography cues (O02_801cb550 <- per-frame event buffer 0x80078330 filled by the timeline thread O02_801c6c94 ->
  F_8002b7d4, time from O02_801cd4c4 -> 0x80078af8 <- F_8001a954 <- raw clock 0x801a2360). That raw clock is written
  by the CD callback F_80014c54 from GetlocL's result: the MSF in the header of the last sector read, minus the
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
