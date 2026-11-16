/* COP0, syscalls, indirect dispatch, safepoints, clock. */
#include "psx.h"
#include <stdlib.h>
#include <string.h>
#include <SDL3/SDL.h>

CPU *g_cpu;
_Thread_local uint32_t g_shadow[SHADOW_MAX];
_Thread_local int g_shadow_n;
extern int g_in_exception;

void rt_dump_stack(const char *why) {
    rt_log("--- guest stack (%s): pc~%08x ra=%08x sp=%08x gp=%08x depth=%d sr=%08x inexc=%d istat=%03x imask=%03x", why, g_cpu->pc, g_cpu->r[31], g_cpu->r[29], g_cpu->r[28], g_shadow_n, g_cpu->cop0[12], g_in_exception, g_istat, g_imask);
    int n = g_shadow_n < SHADOW_MAX ? g_shadow_n : SHADOW_MAX;
    char buf[32];
    for (int i = n - 1; i >= 0 && i >= n - 24; i--)
        rt_log("   #%d %08x %s", n - 1 - i, g_shadow[i], rt_func_name(g_shadow[i], buf, sizeof buf));
}

/* ------------------------------------------------------------ COP0 */
enum { C0_SR = 12, C0_CAUSE = 13, C0_EPC = 14, C0_PRID = 15 };

uint32_t cpu_mfc0(CPU *c, int r) {
    if (r == C0_PRID) return 0x00000002;
    return c->cop0[r];
}

void cpu_mtc0(CPU *c, int r, uint32_t v) {
    c->cop0[r] = v;
    if (r == C0_SR) {
        g_cache_isolated = (v >> 16) & 1;
        irq_check(c);
    }
}

void cpu_rfe(CPU *c) {
    uint32_t sr = c->cop0[C0_SR];
    c->cop0[C0_SR] = (sr & ~0xFu) | ((sr >> 2) & 0xFu);
}

void cpu_syscall(CPU *c, uint32_t pc) {
    (void)pc;
    switch (c->r[4]) {
    case 1: { /* EnterCriticalSection */
        /* the BIOS edits IEp then rfe's; with no rfe here, act on IEc directly */
        uint32_t sr = c->cop0[C0_SR];
        c->r[2] = (sr & 0x401) == 0x401;
        c->cop0[C0_SR] = sr & ~0x401u;
        break;
    }
    case 2: /* ExitCriticalSection */
        c->cop0[C0_SR] |= 0x401;
        irq_check(c);
        break;
    default:
        LOGI("syscall a0=%u at %08x", c->r[4], pc);
        break;
    }
}

void cpu_break(CPU *c, uint32_t pc) {
    rt_fatal("break at %08x (ra=%08x)", pc, c->r[31]);
}

void cpu_invalid(CPU *c, uint32_t pc, uint32_t word) {
    (void)c;
    rt_fatal("invalid instruction %08x at %08x", word, pc);
}

/* ------------------------------------------------------------ dispatch */
static const RecompFunc *entry_in(const RecompFunc *t, int n, uint32_t addr) {
    int lo = 0, hi = n - 1;
    while (lo <= hi) {
        int mid = (lo + hi) >> 1;
        uint32_t a = t[mid].addr;
        if (a == addr) return &t[mid];
        if (a < addr) lo = mid + 1;
        else hi = mid - 1;
    }
    return NULL;
}

static RecompFn find_in(const RecompFunc *t, int n, uint32_t addr) {
    const RecompFunc *e = entry_in(t, n, addr);
    return e ? e->fn : NULL;
}

static int cur_overlay = -1;

static int overlay_matches(int k) {
    const RecompOverlay *o = &recomp_overlays[k];
    return memcmp(ram_ptr(o->base + o->sig_off), o->sig, 32) == 0;
}

/* which overlay image currently sits in RAM at addr? */
static const RecompOverlay *overlay_for(uint32_t addr) {
    if (cur_overlay >= 0 && overlay_matches(cur_overlay)) {
        const RecompOverlay *o = &recomp_overlays[cur_overlay];
        if (addr >= o->base && addr < o->base + o->size) return o;
    }
    for (int k = 0; k < recomp_num_overlays; k++) {
        const RecompOverlay *o = &recomp_overlays[k];
        if (addr >= o->base && addr < o->base + o->size && overlay_matches(k)) {
            if (cur_overlay != k) LOGI("overlay: %s is loaded", o->name);
            cur_overlay = k;
            return o;
        }
    }
    return NULL;
}

/* name of the overlay image in RAM ("COMOD0.MEN" ...), or NULL */
const char *rt_current_overlay_name(void) {
    if (cur_overlay < 0 || !overlay_matches(cur_overlay)) return NULL;
    return recomp_overlays[cur_overlay].name;
}

int rt_current_overlay(uint32_t *base, uint32_t *size) {
    if (cur_overlay < 0 || !overlay_matches(cur_overlay)) return 0;
    *base = recomp_overlays[cur_overlay].base;
    *size = recomp_overlays[cur_overlay].size;
    return 1;
}

static RecompFn find_fn(uint32_t addr) {
    RecompFn f = find_in(recomp_table, recomp_table_size, addr);
    if (f) return f;
    const RecompOverlay *o = overlay_for(addr);
    return o ? find_in(o->table, o->ntable, addr) : NULL;
}

const char *rt_func_name(uint32_t addr, char *buf, size_t n) {
    const RecompFunc *e = entry_in(recomp_table, recomp_table_size, addr);
    if (e) {
        if (e->name) return e->name;
        snprintf(buf, n, "F_%08x", addr);
        return buf;
    }
    const RecompOverlay *o = overlay_for(addr);
    if (o && (e = entry_in(o->table, o->ntable, addr))) {
        if (e->name) return e->name;
        snprintf(buf, n, "O%02d_%08x", (int)(o - recomp_overlays), addr);
        return buf;
    }
    snprintf(buf, n, "?%08x", addr);
    return buf;
}

void call_indirect(CPU *c, uint32_t addr) {
    uint32_t a = addr & 0x1FFFFFFF;
    if (a == 0xA0 || a == 0xB0 || a == 0xC0) {
        bios_call(c, a);
        return;
    }
    RecompFn f = find_fn(a | 0x80000000u); /* KUSEG/KSEG0/KSEG1 aliases -> KSEG0 */
    if (f) { f(c); return; }
    rt_dump_stack("call_indirect");
    if ((a & 0x1FFFFF) < 0x1FFFC0) {
        char line[256]; int n = 0;
        for (int i = 0; i < 12; i++) n += snprintf(line + n, sizeof line - (size_t)n, "%08x ", mem_r32(addr + i * 4));
        rt_log("RAM at %08x: %s", addr, line);
    }
    rt_fatal("call_indirect: no function at %08x (ra=%08x) -- add it to the seeds file", addr, c->r[31]);
}

/* ------------------------------------------------------------ clock & safepoints */
int g_sp_count, g_idle_count; /* UJL_CD_STATS */
static uint64_t g_sleep_ns, g_max_oversleep;
extern uint32_t g_gpu_frame;
static uint32_t g_gpu_frame_dbg(void) { return g_gpu_frame; }
static uint64_t host_t0;
static uint64_t vblank_at;
#define NTSC_CYCLES_PER_FRAME 566107ull  /* 33868800 / 59.826 Hz */

static void on_vblank(void *ctx) {
    (void)ctx;
    {   /* UJL_CD_STATS: vblanks serviced late in host time (the game thread was busy or stalled) */
        static int st = -1; static uint64_t last; static int sp0; static int n;
        if (st < 0) st = getenv("UJL_CD_STATS") != NULL;
        if (st) {
            uint64_t now = SDL_GetTicksNS();
            extern uint64_t g_irq_ns, g_soft_ns; static uint64_t irq0, soft0;
            if (last && now - last > 25000000ull && n < 200) {
                n++;
                rt_log("vblank: serviced %.1f ms after the previous one (%d safepoints, irq %.2f ms, slept %.2f ms, worst oversleep %.2f ms, soft raster %.2f ms) at frame %u",
                       (now - last) / 1e6, g_sp_count - sp0, (g_irq_ns - irq0) / 1e6, g_sleep_ns / 1e6, g_max_oversleep / 1e6, (g_soft_ns - soft0) / 1e6, g_gpu_frame_dbg());
            }
            static uint64_t soft_acc, soft_n; soft_acc += g_soft_ns - soft0; soft_n++;
            if (soft_n % 600 == 0) { rt_log("soft raster: avg %.2f ms per vblank", soft_acc / 1e6 / 600); soft_acc = 0; }
            last = now; sp0 = g_sp_count; irq0 = g_irq_ns; g_sleep_ns = 0; g_max_oversleep = 0; soft0 = g_soft_ns;
        }
    }
    irq_raise(IRQ_VBLANK);
    gpu_vblank();
    timers_vblank();
    bios_vblank();
    { extern void cdrom_vblank(void); cdrom_vblank(); }
    {   static int st = -1, n; static int hist[64]; static int lastsp;
        if (st < 0) st = getenv("UJL_CD_STATS") != NULL;
        if (st) { int d = g_sp_count - lastsp; lastsp = g_sp_count; hist[d > 63 * 50 ? 63 : d / 50]++;
            if (++n % 600 == 0) { char b[512]; int k = 0; for (int i = 0; i < 64; i++) if (hist[i]) k += snprintf(b + k, sizeof b - (size_t)k, " %d:%d", i * 50, hist[i]);
                rt_log("safepoints per vblank (bucket:count):%s", b); memset(hist, 0, sizeof hist); } } }
    host_vblank();
    { extern void memcard_tick(void); memcard_tick(); }
    { extern void unlock_tick(void); unlock_tick(); }
    { extern void score_delta_tick(void); score_delta_tick(); }
    vblank_at += NTSC_CYCLES_PER_FRAME;
    sched_add(vblank_at, on_vblank, NULL, EV_VBLANK);
}

void clock_init(void) {
    host_t0 = SDL_GetTicksNS();
    g_now = 0;
    vblank_at = NTSC_CYCLES_PER_FRAME;
    sched_add(vblank_at, on_vblank, NULL, EV_VBLANK);
}

/* Virtual time follows the host clock, but only catches up with it while the game waits (rt_idle: VSync).
   While game code runs, each safepoint advances it at most COMPUTE_STEP cycles (and never past the host clock).
   Recompiled code takes no emulated time, so with time tied to the host clock throughout, a host hiccup
   mid-frame (preemption, another program's load) let the next vblank fire before the frame's work was done:
   a missed vblank, a 3-vblank frame, and the stage animations hitched (Stage 1's crowd snapped back about once a
   second). Busy-waits on timers/CD still see time pass. If the host falls badly behind (debugger, window drag),
   skip ahead rather than fast-forwarding through a backlog. Experimental, off: UJL_COMPUTE_CLOCK=1 (it made
   frame pacing worse in tests: one 22-vblank stall, more 3-vblank frames). */
#define COMPUTE_STEP 2000ull
/* Loading turbo (cdrom.c sets g_load_turbo_until while fast loading reads data): time runs ahead of the host
   clock, so frames the game spends waiting between files pass instantly. The host clock is re-anchored to the
   emulated one, so nothing jumps when it ends. */
extern uint64_t g_load_turbo_until;
extern int g_cd_fast_load;
int rt_load_turbo_active(void) { return g_now < g_load_turbo_until; }
static void turbo_advance(uint64_t to) {
    if (to <= g_now) return;
    g_now = to;
    host_t0 = SDL_GetTicksNS() - (uint64_t)((double)g_now * (1e9 / PSX_CLOCK));
}

static void clock_update(int idle) {
    if (g_now < g_load_turbo_until) {
        uint64_t next = sched_next();
        if (idle && next != UINT64_MAX) turbo_advance(next);
        else turbo_advance(g_now + 20000);
        return;
    }
    static int host_clock = -1; /* default: host clock throughout (UJL_COMPUTE_CLOCK=1 tries the capped mode) */
    if (host_clock < 0) host_clock = getenv("UJL_COMPUTE_CLOCK") == NULL;
    uint64_t ns = SDL_GetTicksNS() - host_t0;
    uint64_t t = (uint64_t)((double)ns * (PSX_CLOCK / 1e9));
    if (idle || host_clock) {
        if (t > g_now + PSX_CLOCK / 4) {
            uint64_t skip = t - g_now - NTSC_CYCLES_PER_FRAME;
            host_t0 += (uint64_t)((double)skip * (1e9 / PSX_CLOCK));
            t = g_now + NTSC_CYCLES_PER_FRAME;
        }
    } else if (t > g_now + COMPUTE_STEP) {
        t = g_now + COMPUTE_STEP;
    }
    if (t > g_now) g_now = t;
}

uint64_t clock_next_event(void);

void rt_safepoint(CPU *c) {
    c->budget = 2000;
    g_sp_count++;
    {   /* UJL_WAIT_STATS: emulated time consumed per safepoint PC (which busy-waits eat the frame budget) */
        static int st = -1; static uint64_t prev, lastrep; static uint32_t pcs[64]; static uint64_t acc[64]; static int npc;
        if (st < 0) st = getenv("UJL_WAIT_STATS") != NULL;
        if (st) {
            uint64_t before = g_now;
            clock_update(0);
            uint64_t dt = g_now - before;
            int k = 0; while (k < npc && pcs[k] != c->pc) k++;
            if (k == npc && npc < 64) { pcs[npc] = c->pc; acc[npc] = 0; npc++; }
            if (k < 64) acc[k] += dt;
            if (g_now - lastrep > PSX_CLOCK * 2) {
                lastrep = g_now; char b[1024]; int n = 0;
                for (int r = 0; r < 6; r++) { int bi = -1; for (int j = 0; j < npc; j++) if (acc[j] && (bi < 0 || acc[j] > acc[bi])) bi = j;
                    if (bi < 0) break; n += snprintf(b + n, sizeof b - (size_t)n, " %08x:%.1fms", pcs[bi], acc[bi] * 1000.0 / PSX_CLOCK); acc[bi] = 0; }
                rt_log("waits (2 s, emulated time per safepoint pc):%s", b);
                for (int j = 0; j < npc; j++) acc[j] = 0;
            }
            (void)prev;
            sched_run_due(); irq_check(c); return;
        }
    }
    clock_update(0);
    sched_run_due();
    irq_check(c);
}

/* Wait (without burning CPU) until the next scheduled event is due, then service it. */
void rt_idle(CPU *c) {
    g_idle_count++;
    clock_update(1);
    uint64_t next = sched_next();
    if (next != UINT64_MAX && next > g_now && g_now >= g_load_turbo_until) {
        uint64_t ns = (uint64_t)((double)(next - g_now) * (1e9 / PSX_CLOCK));
        if (ns > 2000000) ns = 2000000;
        if (ns > 50000) {
            uint64_t t0 = SDL_GetTicksNS();
            SDL_DelayPrecise(ns);
            uint64_t over = SDL_GetTicksNS() - t0;
            over = over > ns ? over - ns : 0;
            g_sleep_ns += ns; if (over > g_max_oversleep) g_max_oversleep = over;
        }
    }
    c->budget = 2000;
    clock_update(1);
    sched_run_due();
    irq_check(c);
}

/* libetc VSync wait: block until the vblank counter reaches a0. The original's timeout is an
   iteration count (a1 << 15 loops) that a native host burns through in microseconds; on hardware
   each iteration costs ~18 cycles, so reproduce that as emulated time. On timeout the original
   calls ChangeClearPad(0) and ChangeClearRCnt(3, 0); do the same through the exe's own stubs. */
uint64_t g_irq_ns; /* host time spent in interrupt handlers (UJL_CD_STATS) */
static int vs_stats = -1;
static uint64_t vs_t_ret, vs_irq0;
static int32_t vs_vb_ret;
static int vs_sp0;
void hle_vsync_wait(CPU *c, uint32_t counter_addr, uint32_t fn_clearpad, uint32_t fn_clearrcnt) {
    int32_t target = (int32_t)c->r[4];
    if (vs_stats < 0) vs_stats = getenv("UJL_CD_STATS") != NULL;
    {   /* UJL_FLIP_STATS: how far ahead the game waits (target - counter at entry), i.e. the frame rate it asks for */
        static int st = -1, hist[6], n;
        if (st < 0) st = getenv("UJL_FLIP_STATS") != NULL;
        if (st) {
            int32_t ahead = target - (int32_t)mem_r32(counter_addr);
            hist[ahead < 0 ? 0 : ahead > 4 ? 5 : ahead + 0]++;
            if (++n % 600 == 0) { rt_log("vsync waits: target-counter <=0:%d 1:%d 2:%d 3:%d 4:%d 5+:%d (a1=%u)", hist[0], hist[1], hist[2], hist[3], hist[4], hist[5], c->r[5]); memset(hist, 0, sizeof hist); }
        }
    }
    if (vs_stats && vs_t_ret) { /* frames whose work (between VSync waits) spanned 2+ vblanks */
        int32_t vb = (int32_t)mem_r32(counter_addr);
        if (vb - vs_vb_ret >= 2)
            rt_log("vsync: work took %.2f ms host (irq %.2f ms, %d safepoints), counter advanced %d during work (target %d)",
                   (SDL_GetTicksNS() - vs_t_ret) / 1e6, (g_irq_ns - vs_irq0) / 1e6, g_sp_count - vs_sp0, vb - vs_vb_ret, target);
    }
    uint64_t iters = (uint64_t)c->r[5] << 15; /* a1 == 0: give up at once if not reached */
    uint64_t start = g_now, limit = iters * 18;
    for (;;) {
        if ((int32_t)mem_r32(counter_addr) >= target) break;
        if (g_now - start >= limit) {
            LOGV("hle_vsync_wait: timeout (target %d, counter %d)", target, (int32_t)mem_r32(counter_addr));
            uint32_t ra = c->r[31], s0 = c->r[16];
            c->r[4] = 0;
            call_indirect(c, fn_clearpad);
            c->r[4] = 3; c->r[5] = 0;
            call_indirect(c, fn_clearrcnt);
            c->r[31] = ra; c->r[16] = s0;
            break;
        }
        rt_idle(c);
    }
    if (vs_stats) { vs_t_ret = SDL_GetTicksNS(); vs_irq0 = g_irq_ns; vs_vb_ret = (int32_t)mem_r32(counter_addr); vs_sp0 = g_sp_count; }
}

/* ------------------------------------------------------------ interrupts */
int g_in_exception;

void irq_check(CPU *c) {
    if (g_in_exception) return;
    uint32_t sr = c->cop0[C0_SR];
    if (!(sr & 1) || !(sr & 0x400)) return;
    if (!(g_istat & g_imask)) return;
    g_in_exception = 1;
    uint64_t t0 = vs_stats > 0 ? SDL_GetTicksNS() : 0;
    bios_exception(c);
    if (vs_stats > 0) g_irq_ns += SDL_GetTicksNS() - t0;
    g_in_exception = 0;
}

/* Analysis hook (USA tracing build): stats about a text string passed to the font renderer.
   Deliberately does not print the text itself. */
void hook_trace_text(CPU *c, uint32_t fn) {
    static int n;
    if (n++ >= 40) return;
    uint32_t p = c->r[4];
    int len8 = 0, len16 = 0;
    while (len8 < 512 && mem_r8(p + len8)) len8++;
    while (len16 < 256 && mem_r16(p + len16 * 2)) len16++;
    uint32_t mn = 0xFFFF, mx = 0;
    for (int i = 0; i < len8; i++) { uint32_t b = mem_r8(p + i); if (b < mn) mn = b; if (b > mx) mx = b; }
    rt_log("text-hook %08x: ptr=%08x a1=%u  bytes-to-nul=%d (min %02x max %02x)  halfwords-to-0=%d  ra=%08x s0=%08x",
           fn, p, c->r[5], len8, mn, mx, len16, c->r[31], c->r[16]);
    if (getenv("PROBE_OBJ_DUMP") && c->r[16]) {
        /* list words in the caller's object that point into the overlay (timeline/text region) */
        static int dumps;
        if (dumps++ % 8 == 0 && dumps < 80) {
            char line[1024]; int k = 0; line[0] = 0;
            for (uint32_t o = 0; o < 1100; o += 4) {
                uint32_t v = mem_r32(c->r[16] + o);
                if (v >= 0x801c5110u && v < 0x80200000u && k < 1000) k += snprintf(line + k, sizeof line - (size_t)k, " +%u=%08x", o, v);
            }
            rt_log("objdump obj=%08x:%s", c->r[16], line);
        }
    }
    {   /* PROBE_WATCH_OBJ: arm a write-watch on the caller's caption object text fields (+1012..+1020) */
        static int armed;
        if (!armed && getenv("PROBE_WATCH_OBJ") && c->r[16]) {
            armed = 1;
            g_watch_lo = (c->r[16] + 1012) & 0x1FFFFF;
            g_watch_hi = g_watch_lo + 8;
            rt_log("watch: armed on caption object fields %06x", g_watch_lo);
        }
    }
}

/* Analysis hook: log caption-line changes as (vblank count, record index) for a timeline whose first
   record sits at PROBE_TL (guest address). Never logs text. */
void hook_caption_clock(CPU *c, uint32_t fn) {
    static uint32_t last = 1;
    uint32_t p = c->r[4];
    if (p == last) return;
    last = p;
    const char *tl = getenv("PROBE_TL"), *ctr = getenv("PROBE_VBC");
    uint32_t base = tl ? (uint32_t)strtoul(tl, NULL, 16) : 0, vbc = ctr ? mem_r32((uint32_t)strtoul(ctr, NULL, 16)) : 0;
    int idx = -1;
    for (int i = 0; base && i < 64; i++) if (mem_r32(base + i * 12 + 8) == p) { idx = i; break; }
    uint32_t end = idx >= 0 ? mem_r32(base + idx * 12 + 4) : 0, dur = idx >= 0 ? mem_r32(base + idx * 12) : 0;
    rt_log("capclock fn=%08x vblank=%u rec=%d dur=%u end=%u null=%d", fn, vbc, idx, dur, end, p == 0);
}
