/* "Unlock all songs and variants": the system data block (0x801a5310, the same 0x600 bytes the game saves to
   the memory card) holds the progress fields. Found by diffing a fresh card against a completed US save:
     +0x07        stage unlock bitmask (fresh 0x01, all regular stages 0x3f)
     +0x09        set on a completed save
     +0x0A..0x0F  per-stage clear/rank flags (Lammy)
     +0x10..0x18  per-stage/mode flags (0/1)
     +0x1A..0x1F  per-stage flags (PaRappa / variants)
   While the setting is on, the unlock bits are ORed in every vblank, so they stick across card loads. Saving
   with it on writes the unlocks to the card. */
#include "psx.h"

int g_unlock_all = 0;
#define PROGRESS 0x801a5310u

static void or8(uint32_t a, uint8_t v) { uint8_t o = mem_r8(a); if ((o | v) != o) mem_w8(a, (uint8_t)(o | v)); }

void unlock_tick(void) {
    if (!g_unlock_all) return;
    or8(PROGRESS + 0x07, 0xFF);
    or8(PROGRESS + 0x08, 0x01);
    or8(PROGRESS + 0x09, 0x01);
    for (int i = 0x0A; i <= 0x0F; i++) or8(PROGRESS + i, 0x3F);
    for (int i = 0x10; i <= 0x18; i++) or8(PROGRESS + i, 0x01);
    for (int i = 0x1A; i <= 0x1F; i++) or8(PROGRESS + i, 0x3F);
}

/* Debug: UJL_FORCE_RATING=0..3 pins the rating (0 = COOL) by feeding the meter-to-rating lookup
   (F_80019580, table at 0x80010b0c: meter 0-2 COOL, 3-6 GOOD, 7-10 BAD, 11+ AWFUL) a fixed meter.
   Used to reach fever mode headless. */
#include <stdlib.h>
#include <stdio.h>
void hook_force_rating(CPU *c, uint32_t fn) {
    (void)fn;
    static int forced = -2;
    if (forced == -2) { const char *e = getenv("UJL_FORCE_RATING"); forced = e ? atoi(e) : -1; }
    if (forced < 0) return;
    c->r[4] = (uint32_t)(forced == 0 ? 0 : forced * 4 - 1);
    mem_w16(0x80071dc0u, (uint16_t)c->r[4]); /* player 1 meter (struct 0x80071dbc +4) */
}

/* Debug: UJL_FORCE_SCENE=stage[,sub] rewrites the stage/sub pair (0x80078ae0/e4) once, when the scene loader
   F_80013e8c is about to start Stage 1 story (1,0). It maps them to the scene (F_800190d8: stage+10 for sub 0/1/5,
   else stage) and the SNG/CMP/COP column (F_800190fc: 0 for sub 0/3/4). UJL_FORCE_SCENE=8,3 = staff roll. */
void hook_force_scene(CPU *c, uint32_t fn) {
    (void)c; (void)fn;
    static int stage = -2, sub;
    if (stage == -2) { const char *e = getenv("UJL_FORCE_SCENE"); stage = -1; if (e && sscanf(e, "%d,%d", &stage, &sub) < 1) stage = -1; }
    if (stage < 0 || mem_r32(0x80078ae0u) != 1 || mem_r32(0x80078ae4u) != 0) return;
    rt_log("force scene: stage %d sub %d", stage, sub);
    mem_w32(0x80078ae0u, (uint32_t)stage);
    mem_w32(0x80078ae4u, (uint32_t)sub);
    stage = -1;
}

/* PROBE_MORPH=hexdest (debug): log the morph blend calls (F_8003d944: a0 dest verts, a1 delta verts, a2 count,
   a3 weight) that write the given destination array, one line each, with the GPU frame. */
void hook_morph_log(CPU *c, uint32_t fn) {
    (void)fn;
    static int st = -1; static uint32_t dest; static int n;
    if (st < 0) { const char *e = getenv("PROBE_MORPH"); st = e != NULL; if (e) dest = (uint32_t)strtoul(e, NULL, 16); }
    uint32_t end = c->r[4] + c->r[6] * 8;
    if (!st || dest < c->r[4] || dest >= end || n > 4000) return;
    extern uint32_t g_gpu_frame;
    n++;
    rt_log("morph: f %u dst %08x src %08x n %u w %d", g_gpu_frame, c->r[4], c->r[5], c->r[6], (int32_t)c->r[7]);
}

/* PROBE_ANIM=f0,f1 (debug): log F_80023b0c(anim, time, obj, idx) calls between GPU frames f0..f1. */
void hook_anim_log(CPU *c, uint32_t fn) {
    (void)fn;
    static int st = -1; static unsigned f0, f1;
    if (st < 0) { const char *e = getenv("PROBE_ANIM"); st = e && sscanf(e, "%u,%u", &f0, &f1) == 2; }
    extern uint32_t g_gpu_frame;
    if (!st || g_gpu_frame < f0 || g_gpu_frame > f1) return;
    uint32_t o = c->r[16];
    rt_log("anim: f %u anim %08x t %d obj %08x idx %d ra %08x | owner %08x start %d dur %d left %d len %u", g_gpu_frame, c->r[4], (int32_t)c->r[5], c->r[6], (int32_t)c->r[7], c->r[31],
           o, (int32_t)mem_r32(o + 20), (int32_t)mem_r32(o + 24), (int32_t)mem_r32(o + 32), mem_r16(c->r[4] + 10));
}
