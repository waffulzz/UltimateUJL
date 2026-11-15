/* Memory map, MMIO dispatch, interrupt controller, DMA, timers, scheduler. */
#include "psx.h"
#include <stdarg.h>
#include <stdlib.h>

uint8_t *g_ram;
uint8_t g_scratch[1024];
int g_cache_isolated;
int g_log_level = 1;
uint32_t g_istat, g_imask;
uint64_t g_now;
uint32_t g_watch_lo = 0xFFFFFFFFu, g_watch_hi = 0xFFFFFFFFu;
uint32_t g_dma_cur_packet;
uint32_t g_gpu_word_addr = 0xFFFFFFFFu; /* RAM address of the GP0 word being fed by linked-list DMA */
uint32_t g_rwatch_lo = 0xFFFFFFFFu, g_rwatch_hi = 0xFFFFFFFFu;
void rt_rwatch_hit(uint32_t phys) {
    static int hits;
    static uint32_t last_ra;
    if (g_cpu->r[31] == last_ra && hits > 3) return;   /* one report per distinct caller */
    last_ra = g_cpu->r[31];
    if (hits++ >= 8) { g_rwatch_lo = g_rwatch_hi = 0xFFFFFFFFu; return; }
    rt_log("rwatch: read [%06x] (+%x into table)  a0=%08x a1=%08x v0=%08x v1=%08x s0=%08x s1=%08x s2=%08x",
           phys, phys - g_rwatch_lo, g_cpu->r[4], g_cpu->r[5], g_cpu->r[2], g_cpu->r[3], g_cpu->r[16], g_cpu->r[17], g_cpu->r[18]);
    rt_dump_stack("rwatch");
}
static int watch_hits;
void rt_watch_hit(uint32_t phys, uint32_t val, int size) {
    {   /* PROBE_WATCH_SKIPPC=hex: ignore writes from the function containing this return address range (+-0x100) */
        static int init; static uint32_t skip;
        if (!init) { init = 1; const char *e = getenv("PROBE_WATCH_SKIPPC"); if (e) skip = (uint32_t)strtoul(e, NULL, 16); }
        if (skip && g_cpu->r[31] == skip) return;
    }
    {   /* PROBE_WATCH_JUMPS=1: only report writes that aren't the previous value + 1 (up to PROBE_WATCH_N) */
        static int init, jumps, maxn = 6; static uint32_t prev;
        if (!init) { init = 1; jumps = getenv("PROBE_WATCH_JUMPS") != NULL; const char *e = getenv("PROBE_WATCH_N"); if (e) maxn = atoi(e); }
        if (jumps) { uint32_t pv = prev; prev = val; if (val == pv + 1) return; }
        if (watch_hits++ >= maxn) { g_watch_lo = g_watch_hi = 0xFFFFFFFFu; return; }
        rt_log("watch: write%d [%06x] = %08x (prev %08x) ra=%08x", size * 8, phys, val, prev, g_cpu->r[31]);
        if (watch_hits > 3) return;
    }
    if (0) { g_watch_lo = g_watch_hi = 0xFFFFFFFFu; return; }
    rt_log("watch: write%d [%06x] = %08x  a0=%08x a1=%08x a2=%08x a3=%08x s0=%08x s1=%08x s2=%08x s3=%08x", size * 8, phys, val,
           g_cpu->r[4], g_cpu->r[5], g_cpu->r[6], g_cpu->r[7], g_cpu->r[16], g_cpu->r[17], g_cpu->r[18], g_cpu->r[19]);
    rt_dump_stack("watch");
}

static uint32_t memctrl[16];
static uint32_t ram_size_reg;

void rt_log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    fflush(stderr);
}

void rt_fatal(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fputs("FATAL: ", stderr);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    fflush(stderr);
    exit(1);
}

/* ------------------------------------------------------------ scheduler */
typedef struct { uint64_t at; SchedFn fn; void *ctx; int live; } SchedEv;
static SchedEv sched[EV_MAX];

void sched_add(uint64_t at, SchedFn fn, void *ctx, int id) {
    sched[id].at = at;
    sched[id].fn = fn;
    sched[id].ctx = ctx;
    sched[id].live = 1;
}
void sched_cancel(int id) { sched[id].live = 0; }
uint64_t sched_next(void) {
    uint64_t best = UINT64_MAX;
    for (int i = 1; i < EV_MAX; i++)
        if (sched[i].live && sched[i].at < best) best = sched[i].at;
    return best;
}
void sched_run_due(void) {
    for (;;) {
        int best = -1;
        for (int i = 1; i < EV_MAX; i++)
            if (sched[i].live && sched[i].at <= g_now && (best < 0 || sched[i].at < sched[best].at)) best = i;
        if (best < 0) return;
        sched[best].live = 0;
        sched[best].fn(sched[best].ctx);
    }
}

/* ------------------------------------------------------------ IRQ */
void irq_raise(int n) { g_istat |= 1u << n; }

/* ------------------------------------------------------------ DMA */
typedef struct { uint32_t madr, bcr, chcr; } DmaCh;
static DmaCh dma[7];
static uint32_t dpcr = 0x07654321, dicr;

static void dma_update_master(void) {
    uint32_t en = (dicr >> 16) & 0x7F, fl = (dicr >> 24) & 0x7F;
    int m = (dicr & 0x8000) || ((dicr & 0x800000) && (en & fl));
    int was = (dicr >> 31) & 1;
    dicr = (dicr & 0x7FFFFFFF) | ((uint32_t)m << 31);
    if (m && !was) irq_raise(IRQ_DMA);
}

static void dma_finish(int ch) {
    dma[ch].chcr &= ~0x01000000u;
    if (dicr & (1u << (16 + ch))) {
        dicr |= 1u << (24 + ch);
        dma_update_master();
    }
}

int mdec_out_avail(void);
static int dma_pending[7];
static uint32_t dma_left[7];   /* words still to move for a partially completed block transfer */

static void dma_run(int ch) {
    DmaCh *d = &dma[ch];
    uint32_t chcr = d->chcr;
    int sync = (chcr >> 9) & 3;
    int from_ram = chcr & 1;
    int step = (chcr & 2) ? -4 : 4;
    uint32_t addr = d->madr & 0x1FFFFC;

    if (ch == 6) { /* OTC: reverse clear ordering table */
        uint32_t n = d->bcr & 0xFFFF;
        if (!n) n = 0x10000;
        for (uint32_t i = 0; i < n; i++) {
            uint32_t v = (i == n - 1) ? 0x00FFFFFFu : ((addr - 4) & 0x1FFFFC);
            memcpy(g_ram + addr, &v, 4);
            addr = (addr - 4) & 0x1FFFFC;
        }
        dma_finish(ch);
        return;
    }
    if (sync == 2) { /* linked list (GPU) */
        int guard = 0;
        for (;;) {
            uint32_t hdr;
            memcpy(&hdr, g_ram + addr, 4);
            uint32_t n = hdr >> 24;
            g_dma_cur_packet = addr;
            for (uint32_t i = 0; i < n; i++) {
                uint32_t w;
                addr = (addr + 4) & 0x1FFFFC;
                memcpy(&w, g_ram + addr, 4);
                g_gpu_word_addr = addr;
                gpu_gp0(w);
                g_gpu_word_addr = 0xFFFFFFFFu;
            }
            if (hdr & 0x800000) break;
            addr = hdr & 0x1FFFFC;
            if (++guard > 0x100000) { rt_log("dma: linked list loop?"); break; }
        }
        d->madr = 0x00FFFFFF;
        dma_finish(ch);
        return;
    }
    uint32_t words;
    LOGV("dma%d: madr=%08x bcr=%08x chcr=%08x", ch, d->madr, d->bcr, chcr);
    { static int dumped; if (ch == 3 && d->bcr == 8 && !dumped++ && g_log_level >= 2) rt_dump_stack("dma3 8 words"); }
    if (dma_pending[ch]) words = dma_left[ch];
    else if (sync == 0) {
        words = d->bcr & 0xFFFF;
        if (!words) words = 0x10000;
    } else {
        words = (d->bcr & 0xFFFF) * (d->bcr >> 16);
    }
    if (ch == 1) { /* MDEC out: move what's available, wait for the rest */
        uint32_t moved = 0;
        while (moved < words && mdec_out_avail()) {
            uint32_t w = mdec_dma_out();
            memcpy(g_ram + addr, &w, 4);
            addr = (addr + step) & 0x1FFFFC;
            moved++;
        }
        d->madr = addr;
        if (moved < words) { dma_pending[1] = 1; dma_left[1] = words - moved; return; }
        dma_pending[1] = 0;
        dma_finish(ch);
        return;
    }
    for (uint32_t i = 0; i < words; i++) {
        uint32_t w;
        if (from_ram) {
            memcpy(&w, g_ram + addr, 4);
            switch (ch) {
            case 0: mdec_dma_in(w); break;
            case 2: gpu_gp0(w); break;
            case 4: spu_dma_write(w); break;
            default: break;
            }
        } else {
            switch (ch) {
            case 1: w = mdec_dma_out(); break;
            case 2: w = gpu_read(); break;
            case 3: w = cd_dma_read_word(); break;
            case 4: w = spu_dma_read(); break;
            default: w = 0; break;
            }
            memcpy(g_ram + addr, &w, 4);
        }
        addr = (addr + step) & 0x1FFFFC;
    }
    if (sync == 1) {
        d->madr = addr;
        d->bcr &= 0xFFFF;
    }
    dma_finish(ch);
}

void dma_kick(int ch) {
    if (dma_pending[ch] && (dma[ch].chcr & 0x01000000)) dma_run(ch);
}

void dma_write(uint32_t off, uint32_t v) {
    int ch = (off >> 4) & 7;
    int reg = (off >> 2) & 3;
    if (ch == 7) {
        if (reg == 0) dpcr = v;
        else if (reg == 1) {
            if (g_log_level >= 2) { rt_log("dicr write %08x (was %08x)", v, dicr); rt_dump_stack("dicr"); }
            uint32_t ack = v & 0x7F000000;
            dicr = (dicr & ~0x00FF803Fu & ~ack) | (v & 0x00FF803Fu);
            dicr &= ~ack;
            dma_update_master();
        }
        return;
    }
    if (reg == 0) dma[ch].madr = v & 0xFFFFFF;
    else if (reg == 1) dma[ch].bcr = v;
    else if (reg == 2) {
        if (ch == 6) v = (v & 0x51000000) | 2;
        dma[ch].chcr = v;
        dma_pending[ch] = 0;
        int sync = (v >> 9) & 3;
        int go = (v & 0x01000000) && (sync != 0 || (v & 0x10000000));
        if (go && (dpcr & (8u << (ch * 4)))) dma_run(ch);
    }
}

uint32_t dma_read(uint32_t off) {
    int ch = (off >> 4) & 7;
    int reg = (off >> 2) & 3;
    if (ch == 7) return reg == 0 ? dpcr : reg == 1 ? dicr : 0;
    if (reg == 0) return dma[ch].madr;
    if (reg == 1) return dma[ch].bcr;
    if (reg == 2) return dma[ch].chcr;
    return 0;
}

/* ------------------------------------------------------------ timers */
/* Counters are derived from virtual time; good enough for PsyQ VSync()/RCnt use. */
typedef struct { uint32_t mode, target; uint64_t base; } Timer;
static Timer tmr[3];
#define CYCLES_PER_LINE 2152ull /* 3413 video cycles * 7/11 */

static uint32_t timer_value(int i) {
    uint64_t el = g_now - tmr[i].base;
    int src = (tmr[i].mode >> 8) & 3;
    uint64_t v;
    if (i == 1 && (src & 1)) v = el / CYCLES_PER_LINE;
    else if (i == 2 && (src & 2)) v = el / 8;
    else v = el;
    uint32_t t = tmr[i].target ? tmr[i].target : 0x10000;
    if (tmr[i].mode & 8) v %= (t + 1);
    return (uint32_t)(v & 0xFFFF);
}

uint32_t timer_read(uint32_t off) {
    int i = (off >> 4) & 3;
    if (i > 2) return 0;
    {   /* UJL_CD_STATS: root counter reads per second (which clocks the game times itself with) */
        static int st = -1, cnt[3][4]; static uint64_t last;
        if (st < 0) st = getenv("UJL_CD_STATS") != NULL;
        if (st) {
            cnt[i][(off >> 2) & 3]++;
            { static int dumps; extern uint32_t g_gpu_frame; if (i == 2 && g_gpu_frame > 5000 && dumps < 3 && cnt[2][0] == 5000 * (dumps + 1)) { dumps++; rt_dump_stack("timer2 poll"); } }
            if (g_now - last > PSX_CLOCK) {
                last = g_now;
                rt_log("timers: reads/s t0 %d/%d t1 %d/%d t2 %d/%d (value/mode)", cnt[0][0], cnt[0][1], cnt[1][0], cnt[1][1], cnt[2][0], cnt[2][1]);
                memset(cnt, 0, sizeof cnt);
            }
        }
    }
    switch ((off >> 2) & 3) {
    case 0: return timer_value(i);
    case 1: { uint32_t m = tmr[i].mode; tmr[i].mode &= ~0x1800u; return m | 0x400; }
    case 2: return tmr[i].target;
    }
    return 0;
}

void timer_write(uint32_t off, uint32_t v) {
    int i = (off >> 4) & 3;
    { static int n; if (n++ < 40) LOGI("timer%d write reg%u = %04x", i, (off >> 2) & 3, v); }
    if (i > 2) return;
    switch ((off >> 2) & 3) {
    case 0: tmr[i].base = g_now; break; /* value write: reset (approx) */
    case 1: tmr[i].mode = v & 0x3FF; tmr[i].base = g_now; break;
    case 2: tmr[i].target = v & 0xFFFF; break;
    }
}

void timers_vblank(void) {
    /* hblank counter (timer1 in hblank mode) is reset each frame by most games' VSync logic */
}

/* ------------------------------------------------------------ MMIO dispatch */
static int unk_reads, unk_writes;

static uint32_t mmio_read32(uint32_t p);
static void mmio_write32(uint32_t p, uint32_t v);

/* 32-bit register blocks (memctrl, IRQ, DMA, timers, GPU, MDEC) accessed with lb/lh/sb/sh */
static int is_reg32(uint32_t p) {
    return (p >= 0x1F801000 && p < 0x1F801040) || (p >= 0x1F801060 && p < 0x1F801130) ||
           (p >= 0x1F801810 && p < 0x1F801830);
}

uint32_t mmio_read(uint32_t p, int size) {
    if (is_reg32(p)) {
        uint32_t w = mmio_read32(p & ~3u);
        return size == 4 ? w : (w >> ((p & 3) * 8));
    }
    return mmio_read32(p | (size == 4 ? 0 : 0x80000000u)) ;
}

static uint32_t mmio_read32(uint32_t p) {
    int size = 4;
    if (p & 0x80000000u) { p &= 0x7FFFFFFF; size = 2; }
    if (p >= 0x1F801000 && p < 0x1F801024) return memctrl[(p - 0x1F801000) >> 2];
    if (p == 0x1F801060) return ram_size_reg;
    if (p >= 0x1F801040 && p < 0x1F801060) return sio_read(p - 0x1F801040, size);
    if (p == 0x1F801070) {
        extern void sio_poll_ack(void);
        sio_poll_ack();
        return g_istat;
    }
    if (p == 0x1F801074) return g_imask;
    if (p >= 0x1F801080 && p < 0x1F801100) return dma_read(p - 0x1F801080);
    if (p >= 0x1F801100 && p < 0x1F801130) return timer_read(p - 0x1F801100);
    if (p >= 0x1F801800 && p < 0x1F801804) return cd_read(p & 3);
    if (p == 0x1F801810) return gpu_read();
    if (p == 0x1F801814) return gpu_stat();
    if (p == 0x1F801820 || p == 0x1F801824) return mdec_read(p & 4);
    if (p >= 0x1F801C00 && p < 0x1F802000) {
        uint32_t lo = spu_read(p - 0x1F801C00);
        if (size == 4) lo |= (uint32_t)spu_read(p - 0x1F801C00 + 2) << 16;
        return lo;
    }
    if (p >= 0x1F000000 && p < 0x1F800000) return 0xFFFFFFFF; /* expansion 1: nothing */
    if (p >= 0x1FC00000 && p < 0x1FC80000) { static int n; if (n++ < 10) LOGI("mem: BIOS ROM read %08x", p); return 0; } /* BIOS ROM: not present (HLE) */
    if (p == 0x1FFE0130) return 0;                            /* cache control */
    if (unk_reads++ < 64) LOGI("mmio: unhandled read%d %08x", size * 8, p);
    return 0;
}

void mmio_write(uint32_t p, uint32_t v, int size) {
    if (is_reg32(p) && size != 4) {
        uint32_t al = p & ~3u, sh = (p & 3) * 8;
        uint32_t mask = (size == 1 ? 0xFFu : 0xFFFFu) << sh;
        uint32_t cur;
        if (al == 0x1F8010F4) cur = dma_read(0x74) & 0x00FFFFFFu; /* never ack flags by accident */
        else cur = mmio_read32(al);
        mmio_write32(al, (cur & ~mask) | ((v << sh) & mask));
        return;
    }
    if (is_reg32(p)) { mmio_write32(p, v); return; }
    mmio_write32(p | (size == 1 ? 0x40000000u : size == 2 ? 0x80000000u : 0), v);
}

static void mmio_write32(uint32_t p, uint32_t v) {
    int size = 4;
    if (p & 0x40000000u) { p &= 0x3FFFFFFF; size = 1; }
    else if (p & 0x80000000u) { p &= 0x7FFFFFFF; size = 2; }
    if (p >= 0x1F801000 && p < 0x1F801024) { memctrl[(p - 0x1F801000) >> 2] = v; return; }
    if (p == 0x1F801060) { ram_size_reg = v; return; }
    if (p >= 0x1F801040 && p < 0x1F801060) { sio_write(p - 0x1F801040, v, size); return; }
    if (p == 0x1F801070) { g_istat &= v; return; }
    if (p == 0x1F801074) { g_imask = v & 0x7FF; return; }
    if (p >= 0x1F801080 && p < 0x1F801100) { dma_write(p - 0x1F801080, v); return; }
    if (p >= 0x1F801100 && p < 0x1F801130) { timer_write(p - 0x1F801100, v); return; }
    if (p >= 0x1F801800 && p < 0x1F801804) { cd_write(p & 3, (uint8_t)v); return; }
    if (p == 0x1F801810) { gpu_gp0(v); return; }
    if (p == 0x1F801814) { gpu_gp1(v); return; }
    if (p == 0x1F801820 || p == 0x1F801824) { mdec_write(p & 4, v); return; }
    if (p >= 0x1F801C00 && p < 0x1F802000) {
        spu_write(p - 0x1F801C00, (uint16_t)v);
        if (size == 4) spu_write(p - 0x1F801C00 + 2, (uint16_t)(v >> 16));
        return;
    }
    if (p >= 0x1F802000 && p < 0x1F802100) return; /* expansion 2 (debug LEDs etc.) */
    if (p == 0x1FFE0130) return;
    if (unk_writes++ < 64) {
        LOGI("mmio: unhandled write%d %08x = %08x", size * 8, p, v);
        if (unk_writes < 12) rt_dump_stack("unhandled mmio write");
    }
}
