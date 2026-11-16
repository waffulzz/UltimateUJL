/* Core ABI shared by generated code and the runtime. */
#pragma once
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct GTE {
    int32_t data[32];
    int32_t ctrl[32];
} GTE;

typedef struct CPU {
    uint32_t r[32];
    uint32_t hi, lo;
    uint32_t cop0[32];
    GTE gte;
    int32_t budget;      /* safepoint countdown */
    uint32_t pc;         /* last safepoint address (debug) */
} CPU;

typedef void (*RecompFn)(CPU *c);
typedef struct { uint32_t addr; RecompFn fn; const char *name; } RecompFunc; /* name: symbol map, or NULL */
extern const RecompFunc recomp_table[];
extern const int recomp_table_size;

/* overlays: code images loaded at runtime (COMOD*.* at 0x801c5110). Identified by 32 code bytes. */
typedef struct {
    const char *name;
    uint32_t base, size;
    uint32_t sig_off;
    uint8_t sig[32];
    const RecompFunc *table;
    int ntable;
} RecompOverlay;
extern const RecompOverlay recomp_overlays[];
extern const int recomp_num_overlays;


/* ---------------- memory ---------------- */
#define PSX_RAM_SIZE (2u * 1024u * 1024u)
extern uint8_t *g_ram;          /* 2MB main RAM */
extern uint8_t g_scratch[1024]; /* D-cache scratchpad 0x1F800000 */
extern int g_cache_isolated;
/* debug write-watch (probe only): when armed, stores into [g_watch_lo, g_watch_hi) call rt_watch_hit */
extern uint32_t g_watch_lo, g_watch_hi;
void rt_watch_hit(uint32_t phys, uint32_t val, int size);
extern uint32_t g_rwatch_lo, g_rwatch_hi;
void rt_rwatch_hit(uint32_t phys);
#define WATCH(p, v, sz) do { if ((p) - g_watch_lo < g_watch_hi - g_watch_lo) rt_watch_hit((p), (v), (sz)); } while (0)

uint32_t mmio_read(uint32_t phys, int size);
void mmio_write(uint32_t phys, uint32_t val, int size);

static inline uint32_t phys_addr(uint32_t a) { return a & 0x1FFFFFFFu; }

static inline uint32_t mem_r8(uint32_t a) {
    uint32_t p = phys_addr(a);
    if (p < 0x00800000u) return g_ram[p & 0x1FFFFFu];
    if ((p & 0xFFFFFC00u) == 0x1F800000u) return g_scratch[p & 0x3FF];
    return mmio_read(p, 1) & 0xFF;
}
static inline uint32_t mem_r16(uint32_t a) {
    uint32_t p = phys_addr(a); uint16_t v;
    if (p < 0x00800000u) { memcpy(&v, g_ram + (p & 0x1FFFFEu), 2); return v; }
    if ((p & 0xFFFFFC00u) == 0x1F800000u) { memcpy(&v, g_scratch + (p & 0x3FE), 2); return v; }
    return mmio_read(p, 2) & 0xFFFF;
}
static inline uint32_t mem_r32(uint32_t a) {
    uint32_t p = phys_addr(a), v;
    if ((p & 0x1FFFFCu) - g_rwatch_lo < g_rwatch_hi - g_rwatch_lo && p < 0x00800000u) rt_rwatch_hit(p & 0x1FFFFCu);
    if (p < 0x00800000u) { memcpy(&v, g_ram + (p & 0x1FFFFCu), 4); return v; }
    if ((p & 0xFFFFFC00u) == 0x1F800000u) { memcpy(&v, g_scratch + (p & 0x3FC), 4); return v; }
    return mmio_read(p, 4);
}
static inline void mem_w8(uint32_t a, uint32_t v) {
    uint32_t p = phys_addr(a);
    if (g_cache_isolated) return;
    WATCH(p & 0x1FFFFFu, v, 1);
    if (p < 0x00800000u) { g_ram[p & 0x1FFFFFu] = (uint8_t)v; return; }
    if ((p & 0xFFFFFC00u) == 0x1F800000u) { g_scratch[p & 0x3FF] = (uint8_t)v; return; }
    mmio_write(p, v & 0xFF, 1);
}
static inline void mem_w16(uint32_t a, uint32_t v) {
    uint32_t p = phys_addr(a); uint16_t h = (uint16_t)v;
    if (g_cache_isolated) return;
    WATCH(p & 0x1FFFFEu, v, 2);
    if (p < 0x00800000u) { memcpy(g_ram + (p & 0x1FFFFEu), &h, 2); return; }
    if ((p & 0xFFFFFC00u) == 0x1F800000u) { memcpy(g_scratch + (p & 0x3FE), &h, 2); return; }
    mmio_write(p, h, 2);
}
static inline void mem_w32(uint32_t a, uint32_t v) {
    uint32_t p = phys_addr(a);
    if (g_cache_isolated) return;
    WATCH(p & 0x1FFFFCu, v, 4);
    if (p < 0x00800000u) { memcpy(g_ram + (p & 0x1FFFFCu), &v, 4); return; }
    if ((p & 0xFFFFFC00u) == 0x1F800000u) { memcpy(g_scratch + (p & 0x3FC), &v, 4); return; }
    mmio_write(p, v, 4);
}

/* host pointer into RAM for a guest address (RAM only) */
static inline void *ram_ptr(uint32_t a) { return g_ram + (phys_addr(a) & 0x1FFFFFu); }

/* unaligned load/store helpers (little endian R3000) */
static inline uint32_t cpu_lwl(uint32_t rt, uint32_t a) {
    uint32_t w = mem_r32(a & ~3u);
    switch (a & 3) {
    case 0: return (rt & 0x00FFFFFFu) | (w << 24);
    case 1: return (rt & 0x0000FFFFu) | (w << 16);
    case 2: return (rt & 0x000000FFu) | (w << 8);
    default: return w;
    }
}
static inline uint32_t cpu_lwr(uint32_t rt, uint32_t a) {
    uint32_t w = mem_r32(a & ~3u);
    switch (a & 3) {
    case 0: return w;
    case 1: return (rt & 0xFF000000u) | (w >> 8);
    case 2: return (rt & 0xFFFF0000u) | (w >> 16);
    default: return (rt & 0xFFFFFF00u) | (w >> 24);
    }
}
static inline void cpu_swl(uint32_t rt, uint32_t a) {
    uint32_t al = a & ~3u, w = mem_r32(al);
    switch (a & 3) {
    case 0: w = (w & 0xFFFFFF00u) | (rt >> 24); break;
    case 1: w = (w & 0xFFFF0000u) | (rt >> 16); break;
    case 2: w = (w & 0xFF000000u) | (rt >> 8); break;
    default: w = rt; break;
    }
    mem_w32(al, w);
}
static inline void cpu_swr(uint32_t rt, uint32_t a) {
    uint32_t al = a & ~3u, w = mem_r32(al);
    switch (a & 3) {
    case 0: w = rt; break;
    case 1: w = (w & 0x000000FFu) | (rt << 8); break;
    case 2: w = (w & 0x0000FFFFu) | (rt << 16); break;
    default: w = (w & 0x00FFFFFFu) | (rt << 24); break;
    }
    mem_w32(al, w);
}

static inline void cpu_div(CPU *c, uint32_t s, uint32_t t) {
    int32_t n = (int32_t)s, d = (int32_t)t;
    if (d == 0) { c->hi = s; c->lo = n >= 0 ? 0xFFFFFFFFu : 1u; }
    else if (s == 0x80000000u && d == -1) { c->hi = 0; c->lo = 0x80000000u; }
    else { c->lo = (uint32_t)(n / d); c->hi = (uint32_t)(n % d); }
}
static inline void cpu_divu(CPU *c, uint32_t s, uint32_t t) {
    if (t == 0) { c->hi = s; c->lo = 0xFFFFFFFFu; }
    else { c->lo = s / t; c->hi = s % t; }
}

/* ---------------- runtime services ---------------- */
uint32_t cpu_mfc0(CPU *c, int r);
void cpu_mtc0(CPU *c, int r, uint32_t v);
void cpu_rfe(CPU *c);
void cpu_syscall(CPU *c, uint32_t pc);
void cpu_break(CPU *c, uint32_t pc);
void cpu_invalid(CPU *c, uint32_t pc, uint32_t word);
void call_indirect(CPU *c, uint32_t addr);
void bios_call(CPU *c, uint32_t vector);
void rt_safepoint(CPU *c);

uint32_t gte_read(CPU *c, int reg);       /* 0..31 data, 32..63 control */
void gte_write(CPU *c, int reg, uint32_t v);
void gte_cmd(CPU *c, uint32_t cmd);
void gte_store_sxy(CPU *c, uint32_t ea, int reg); /* SWC2 of SXY0-2/SXYP: store + remember the address */

/* native replacements for library functions (see tools/recomp/overrides_*.txt) */
void hook_trace_text(CPU *c, uint32_t fn);
void hook_caption_clock(CPU *c, uint32_t fn);
void hook_default_language(CPU *c, uint32_t fn);
void hle_vsync_wait(CPU *c, uint32_t counter_addr, uint32_t fn_clearpad, uint32_t fn_clearrcnt);
void hle_spu_set_transfer_addr(CPU *c);
void hook_force_rating(CPU *c, uint32_t fn);
void hook_force_scene(CPU *c, uint32_t fn);
void hook_credits_hold(CPU *c, uint32_t fn);
void hook_morph_log(CPU *c, uint32_t fn);
void hook_anim_log(CPU *c, uint32_t fn);
void hook_tooltip_sprite(CPU *c, uint32_t fn);
void hook_hiscore_insert(CPU *c, uint32_t fn);
void hook_menu_screen(CPU *c, uint32_t fn);
int filter_replay_directory(CPU *c, uint32_t fn);
int filter_replay_slots(CPU *c, uint32_t fn);
int filter_replay_save(CPU *c, uint32_t fn);
const char *rt_current_overlay_name(void);

#define SAFEPOINT(c, a) do { (c)->pc = (a); if (--(c)->budget <= 0) rt_safepoint(c); } while (0)

/* shadow call stack (debug): which recompiled functions are active on this host thread */
#define SHADOW_MAX 1024
extern _Thread_local uint32_t g_shadow[SHADOW_MAX];
extern _Thread_local int g_shadow_n;
typedef int ShadowGuard;
static inline ShadowGuard shadow_push(uint32_t a) { if (g_shadow_n < SHADOW_MAX) g_shadow[g_shadow_n] = a; return g_shadow_n++; }
static inline void shadow_pop(ShadowGuard *g) { g_shadow_n = *g; }
#define TRACE_FN(a) ShadowGuard _sg __attribute__((cleanup(shadow_pop))) = shadow_push(a)
void rt_dump_stack(const char *why);
/* function name for a guest address: the symbol map's ("hiscore_insert"), else F_8001e7a0 / O00_801c5f44 (the
   overlay loaded now), else "?8001e7a0"; unnamed ones are formatted into buf */
const char *rt_func_name(uint32_t addr, char *buf, size_t n);

#ifdef __cplusplus
}
#endif
