/* SPU: 24 ADPCM voices with ADSR, CD/XA input, mixed at 44.1 kHz on emulated time.
   Written from the public hardware description. Noise is TODO. */
#include "psx.h"
#include <stdlib.h>

#define SPU_RATE 44100
#define CYCLES_PER_SAMPLE (PSX_CLOCK / SPU_RATE) /* 768 */
#define CHUNK 64

static uint16_t regs[0x200];
uint8_t g_spu_ram[512 * 1024];
static uint32_t xfer_addr;
static uint32_t dbg_xfer_start, dbg_xfer_n; static int dbg_xfer_dir;
#define REG(o) regs[(o) >> 1]

void host_audio_push(const int16_t *lr, int frames); /* frontend sink */

/* ------------------------------------------------------------ IRQ */
static int irq_flag;
static uint32_t capture_pos; /* 0..0x1FF halfword index into the 4 capture buffers */

static inline void spu_irq_check(uint32_t addr, uint32_t len) {
    uint16_t cnt = REG(0x1AA);
    if (!(cnt & 0x40) || irq_flag) return;
    uint32_t ia = (uint32_t)REG(0x1A4) << 3;
    if (ia >= addr && ia < addr + len) {
        irq_flag = 1;
        irq_raise(IRQ_SPU);
    }
}

/* ------------------------------------------------------------ voices */
enum { ENV_OFF, ENV_ATTACK, ENV_DECAY, ENV_SUSTAIN, ENV_RELEASE };
typedef struct {
    uint32_t addr;          /* current ADPCM block address (bytes) */
    uint32_t repeat;        /* loop address */
    uint32_t counter;       /* pitch counter, 12-bit fraction */
    int16_t buf[28];        /* decoded block */
    int16_t prev[3];        /* last samples of previous block (interpolation) */
    int32_t h1, h2;         /* ADPCM history */
    int phase;              /* ENV_* */
    int32_t env;            /* 0..0x7FFF */
    int32_t env_cnt;
    int decoded;
    int repeat_set;         /* repeat address written by software since key-on */
} Voice;
static Voice v[24];
int g_spu_log;                         /* debug: log key-ons and per-voice peaks */
/* per-second audio diagnostics in the log: which voices were keyed, which got non-zero / zero volume */
static uint32_t diag_kon, diag_volnz, diag_volz;
static uint32_t diag_samples;
static int32_t diag_rvb_peak;
static int32_t vpeak[24];
static uint64_t kon_count[24];

static const int f_pos[5] = {0, 60, 115, 98, 122};
static const int f_neg[5] = {0, 0, -52, -55, -60};

static void decode_block(Voice *vc) {
    spu_irq_check(vc->addr & 0x7FFF0, 16);
    const uint8_t *b = &g_spu_ram[vc->addr & 0x7FFF0];
    int shift = b[0] & 0x0F, filt = (b[0] >> 4) & 7;
    if (shift > 12) shift = 9;
    if (filt > 4) filt = 4;
    for (int i = 0; i < 28; i++) {
        int nib = (b[2 + i / 2] >> ((i & 1) * 4)) & 0xF;
        int32_t s = (int16_t)(nib << 12) >> shift;
        s += (vc->h1 * f_pos[filt] + vc->h2 * f_neg[filt] + 32) >> 6;
        if (s > 32767) s = 32767;
        if (s < -32768) s = -32768;
        vc->h2 = vc->h1;
        vc->h1 = s;
        vc->buf[i] = (int16_t)s;
    }
    vc->decoded = 1;
}

/* envelope rate step, as described for the hardware ADSR unit */
static void env_tick(Voice *vc, int rate, int decreasing, int exponential) {
    int shift = rate >> 2;
    int step = decreasing ? (-8 + (rate & 3)) : (7 - (rate & 3));
    int cycles = 1 << (shift > 11 ? shift - 11 : 0);
    int32_t inc = step << (shift < 11 ? 11 - shift : 0);
    if (exponential && !decreasing && vc->env > 0x6000) cycles *= 4;
    if (exponential && decreasing) inc = (inc * vc->env) >> 15;
    if (++vc->env_cnt < cycles) return;
    vc->env_cnt = 0;
    vc->env += inc;
    if (vc->env > 0x7FFF) vc->env = 0x7FFF;
    if (vc->env < 0) vc->env = 0;
}

static void env_step(int n, Voice *vc) {
    uint16_t a1 = REG(n * 16 + 8), a2 = REG(n * 16 + 10);
    switch (vc->phase) {
    case ENV_ATTACK:
        env_tick(vc, (a1 >> 8) & 0x7F, 0, (a1 >> 15) & 1);
        if (vc->env >= 0x7FFF) { vc->env = 0x7FFF; vc->phase = ENV_DECAY; }
        break;
    case ENV_DECAY: {
        int32_t sl = ((a1 & 0xF) + 1) * 0x800;
        env_tick(vc, ((a1 >> 4) & 0xF) << 2, 1, 1);
        if (vc->env <= sl) { vc->env = sl > 0x7FFF ? 0x7FFF : sl; vc->phase = ENV_SUSTAIN; }
        break;
    }
    case ENV_SUSTAIN:
        env_tick(vc, (a2 >> 6) & 0x7F, (a2 >> 14) & 1, (a2 >> 15) & 1);
        break;
    case ENV_RELEASE:
        env_tick(vc, (a2 & 0x1F) << 2, 1, (a2 >> 5) & 1);
        if (vc->env <= 0) { vc->env = 0; vc->phase = ENV_OFF; }
        break;
    default: break;
    }
    REG(n * 16 + 12) = (uint16_t)vc->env;
}

static void key_on(int n) {
    Voice *vc = &v[n];
    diag_kon |= 1u << n;
    if (g_spu_log) {
        if (kon_count[n]) LOGI("spu: v%02d previous note peak %d", n, vpeak[n]);
        kon_count[n]++;
        vpeak[n] = 0;
        LOGI("spu: KON v%02d addr %05x pitch %04x vol %04x/%04x adsr %04x %04x  main %04x/%04x cnt %04x eon %04x%04x non %04x%04x pmon %04x%04x",
             n, REG(n * 16 + 6) << 3, REG(n * 16 + 4), REG(n * 16 + 0), REG(n * 16 + 2), REG(n * 16 + 8), REG(n * 16 + 10),
             REG(0x180), REG(0x182), REG(0x1AA), REG(0x19A), REG(0x198), REG(0x196), REG(0x194), REG(0x192), REG(0x190));
    }
    vc->addr = (uint32_t)REG(n * 16 + 6) << 3;
    if (!vc->repeat_set) vc->repeat = vc->addr;
    vc->counter = 0;
    vc->h1 = vc->h2 = 0;
    vc->prev[0] = vc->prev[1] = vc->prev[2] = 0;
    vc->decoded = 0;
    vc->phase = ENV_ATTACK;
    vc->env = 0;
    vc->env_cnt = 0;
    vc->repeat_set = 0;
    REG(0x19C + (n >= 16 ? 2 : 0)) &= (uint16_t)~(1u << (n & 15));
}

static void key_off(int n) {
    if (v[n].phase != ENV_OFF) v[n].phase = ENV_RELEASE;
}

static inline int16_t vol_reg(uint16_t r) {
    if (r & 0x8000) return 0x3FFF; /* sweep mode: treat as full until sweeps are implemented */
    return (int16_t)(r << 1) >> 1;
}

/* fetch next output sample of voice n (advances pitch counter) */
static int32_t voice_sample(int n) {
    Voice *vc = &v[n];
    if (!vc->decoded) decode_block(vc);
    int idx = (int)(vc->counter >> 12);
    int frac = (int)(vc->counter & 0xFFF);
    int32_t s0 = vc->buf[idx], s1 = idx + 1 < 28 ? vc->buf[idx + 1] : s0;
    int32_t out = s0 + (((s1 - s0) * frac) >> 12);
    uint32_t pitch = REG(n * 16 + 4);
    if (pitch > 0x3FFF) pitch = 0x3FFF;
    vc->counter += pitch;
    while ((vc->counter >> 12) >= 28) {
        vc->counter -= 28 << 12;
        const uint8_t *b = &g_spu_ram[vc->addr & 0x7FFF0];
        uint8_t flags = b[1];
        if (flags & 4) vc->repeat = vc->addr;           /* loop start */
        if (flags & 1) {                                 /* loop end */
            REG(0x19C + (n >= 16 ? 2 : 0)) |= (uint16_t)(1u << (n & 15));
            vc->addr = vc->repeat;
            if (!(flags & 2)) { vc->phase = ENV_OFF; vc->env = 0; } /* no repeat: stop */
        } else {
            vc->addr = (vc->addr + 16) & 0x7FFF0;
        }
        decode_block(vc);
    }
    return out;
}

/* ------------------------------------------------------------ XA ring */
#define XA_RING 65536
int16_t g_xa_ring[XA_RING * 2];
volatile uint32_t g_xa_w, g_xa_r;
int g_xa_rate = 37800;
static uint32_t xa_frac;

void spu_xa_push(const int16_t *s, int frames, int rate, int stereo) {
    g_xa_rate = rate;
    for (int i = 0; i < frames; i++) {
        if (g_xa_w - g_xa_r >= XA_RING - 1) break; /* overflow: drop */
        uint32_t w = g_xa_w & (XA_RING - 1);
        g_xa_ring[w * 2] = stereo ? s[i * 2] : s[i];
        g_xa_ring[w * 2 + 1] = stereo ? s[i * 2 + 1] : s[i];
        g_xa_w++;
    }
}

/* XA sectors arrive in ~53 ms bursts; on hardware the decoder buffers a sector, so playback is
   continuous. We start (or restart after an underrun) only once ~30 ms is queued, interpolate
   between source samples, and fade out instead of cutting to silence when the ring runs dry. */
#define XA_START_FRAMES 1200
static int xa_playing;
int g_xa_underruns;
static int32_t xa_last_l, xa_last_r;

static void xa_sample(int32_t *l, int32_t *r) {
    uint32_t avail = g_xa_w - g_xa_r;
    if (!xa_playing) {
        if (avail >= XA_START_FRAMES) { xa_playing = 1; xa_frac = 0; }
    } else if (avail < 2) {
        xa_playing = 0; /* underrun: rebuffer */
        g_xa_underruns++;
    }
    if (!xa_playing) {
        /* decay the last output towards zero (no click) */
        xa_last_l = xa_last_l * 31 / 32;
        xa_last_r = xa_last_r * 31 / 32;
        *l = xa_last_l;
        *r = xa_last_r;
        return;
    }
    uint32_t i0 = g_xa_r & (XA_RING - 1), i1 = (g_xa_r + 1) & (XA_RING - 1);
    int32_t f = (int32_t)((xa_frac << 12) / SPU_RATE); /* 0..4095 */
    *l = g_xa_ring[i0 * 2] + (((g_xa_ring[i1 * 2] - g_xa_ring[i0 * 2]) * f) >> 12);
    *r = g_xa_ring[i0 * 2 + 1] + (((g_xa_ring[i1 * 2 + 1] - g_xa_ring[i0 * 2 + 1]) * f) >> 12);
    xa_last_l = *l;
    xa_last_r = *r;
    xa_frac += (uint32_t)g_xa_rate;
    while (xa_frac >= SPU_RATE) { xa_frac -= SPU_RATE; g_xa_r++; }
}

/* ------------------------------------------------------------ mixing */
static inline int16_t sat16(int32_t x) { return (int16_t)(x > 32767 ? 32767 : x < -32768 ? -32768 : x); }

/* ------------------------------------------------------------ reverb
   The public reverb formula, run at 22050 Hz on the work area mBASE..0x7FFFF. Buffer writes only happen
   while SPUCNT bit 7 (reverb master enable) is set; the output stage always reads the buffer. Um Jammer
   Lammy relies on that: its guitar feedback stream (fbstream.c) writes processed 22 kHz PCM into a
   512-byte work area with every reverb register 0 and the master enable off, so the output is just
   ring[pos] * vLOUT. The game times those writes by the capture-buffer IRQ, so the buffer position runs
   off the same sample counter as the capture position. */
static uint32_t rvb_pos;          /* byte offset of the current halfword within the work area */
static int32_t rvb_inl, rvb_inr;  /* input accumulated over the two 44.1 kHz samples of one step */
static int32_t rvb_prevl, rvb_prevr, rvb_outl, rvb_outr;
#define RS(o) ((int32_t)(int16_t)REG(o))
#define RA(o) ((int32_t)REG(o) << 3)

static inline uint32_t rvb_addr(int32_t ofs) {
    uint32_t base = (uint32_t)REG(0x1A2) << 3, size = 0x80000u - base;
    if (!size) return base;
    int64_t a = ((int64_t)rvb_pos + ofs) % (int64_t)size;
    if (a < 0) a += size;
    return (base + (uint32_t)a) & 0x7FFFE;
}
static inline int32_t rvb_rd(int32_t ofs) { int16_t x; memcpy(&x, &g_spu_ram[rvb_addr(ofs)], 2); return x; }
static inline void rvb_wr(int32_t ofs, int32_t val) {
    if (!(REG(0x1AA) & 0x80)) return;
    uint32_t a = rvb_addr(ofs);
    spu_irq_check(a, 2);
    int16_t x = sat16(val);
    memcpy(&g_spu_ram[a], &x, 2);
}
#define M(a, b) (((a) * (b)) >> 15)

static void reverb_step(int32_t inl, int32_t inr, int32_t *outl, int32_t *outr) {
    int32_t lin = M(RS(0x1FC), inl), rin = M(RS(0x1FE), inr);
    int32_t viir = RS(0x1C4), vwall = RS(0x1CE);
    int32_t lsame2 = rvb_rd(RA(0x1D4) - 2), rsame2 = rvb_rd(RA(0x1D6) - 2);
    int32_t ldiff2 = rvb_rd(RA(0x1E4) - 2), rdiff2 = rvb_rd(RA(0x1E6) - 2);
    rvb_wr(RA(0x1D4), M(lin + M(rvb_rd(RA(0x1E0)), vwall) - lsame2, viir) + lsame2);
    rvb_wr(RA(0x1D6), M(rin + M(rvb_rd(RA(0x1E2)), vwall) - rsame2, viir) + rsame2);
    rvb_wr(RA(0x1E4), M(lin + M(rvb_rd(RA(0x1F2)), vwall) - ldiff2, viir) + ldiff2);
    rvb_wr(RA(0x1E6), M(rin + M(rvb_rd(RA(0x1F0)), vwall) - rdiff2, viir) + rdiff2);
    int32_t l = M(RS(0x1C6), rvb_rd(RA(0x1D8))) + M(RS(0x1C8), rvb_rd(RA(0x1DC))) +
                M(RS(0x1CA), rvb_rd(RA(0x1E8))) + M(RS(0x1CC), rvb_rd(RA(0x1EC)));
    int32_t r = M(RS(0x1C6), rvb_rd(RA(0x1DA))) + M(RS(0x1C8), rvb_rd(RA(0x1DE))) +
                M(RS(0x1CA), rvb_rd(RA(0x1EA))) + M(RS(0x1CC), rvb_rd(RA(0x1EE)));
    int32_t vapf1 = RS(0x1D0), vapf2 = RS(0x1D2), dapf1 = RA(0x1C0), dapf2 = RA(0x1C2);
    int32_t t;
    t = rvb_rd(RA(0x1F4) - dapf1); l = sat16(l - M(vapf1, t)); rvb_wr(RA(0x1F4), l); l = M(l, vapf1) + t;
    t = rvb_rd(RA(0x1F6) - dapf1); r = sat16(r - M(vapf1, t)); rvb_wr(RA(0x1F6), r); r = M(r, vapf1) + t;
    t = rvb_rd(RA(0x1F8) - dapf2); l = sat16(l - M(vapf2, t)); rvb_wr(RA(0x1F8), l); l = M(l, vapf2) + t;
    t = rvb_rd(RA(0x1FA) - dapf2); r = sat16(r - M(vapf2, t)); rvb_wr(RA(0x1FA), r); r = M(r, vapf2) + t;
    *outl = M(sat16(l), RS(0x184));
    *outr = M(sat16(r), RS(0x186));
}

/* one 44.1 kHz sample: feed input, run a reverb step every other sample, return interpolated output */
static void reverb_sample(int32_t inl, int32_t inr, int32_t *outl, int32_t *outr) {
    rvb_inl += inl; rvb_inr += inr;
    if (((capture_pos - 1) & 0x1FF) & 1) { /* capture_pos has already advanced past this sample */
        rvb_prevl = rvb_outl; rvb_prevr = rvb_outr;
        reverb_step(rvb_inl >> 1, rvb_inr >> 1, &rvb_outl, &rvb_outr);
        rvb_inl = rvb_inr = 0;
        uint32_t size = 0x80000u - ((uint32_t)REG(0x1A2) << 3);
        rvb_pos = size ? (rvb_pos + 2) % size : 0;
        *outl = rvb_outl; *outr = rvb_outr;
    } else {
        *outl = (rvb_prevl + rvb_outl) >> 1; *outr = (rvb_prevr + rvb_outr) >> 1;
    }
}

static void mix(int frames) {
    static int16_t out[CHUNK * 2];
    uint16_t cnt = REG(0x1AA);
    int spu_on = (cnt >> 15) & 1, unmuted = (cnt >> 14) & 1, cd_on = cnt & 1;
    for (int f = 0; f < frames; f++) {
        int32_t L = 0, R = 0;
        int32_t rinl = 0, rinr = 0; /* reverb input: voices with reverb enabled (EON) */
        int32_t cap1 = 0, cap3 = 0; /* voice 1/3 output after ADSR, before volume (capture buffers) */
        if (spu_on) {
            for (int n = 0; n < 24; n++) {
                Voice *vc = &v[n];
                if (vc->phase == ENV_OFF) continue;
                env_step(n, vc);
                int32_t s = voice_sample(n);
                s = (s * vc->env) >> 15;
                if (n == 1) cap1 = s; else if (n == 3) cap3 = s;
                int32_t vl = (s * vol_reg(REG(n * 16 + 0))) >> 15, vr = (s * vol_reg(REG(n * 16 + 2))) >> 15;
                if (g_spu_log) {
                    int32_t a = (s * vol_reg(REG(n * 16 + 0))) >> 15;
                    if (a < 0) a = -a;
                    if (a > vpeak[n]) vpeak[n] = a;
                }
                L += vl;
                R += vr;
                if ((n < 16 ? REG(0x198) >> n : REG(0x19A) >> (n - 16)) & 1) { rinl += vl; rinr += vr; }
            }
        }
        int32_t xl, xr;
        xa_sample(&xl, &xr);
        {   /* capture buffers: CD L/R at 0x000/0x400, voice 1/3 at 0x800/0xC00. The game's guitar
               feedback stream reads voices 1/3 from here (src/runtime/fbstream.c). */
            int16_t cl = (int16_t)xl, cr = (int16_t)xr, c1 = sat16(cap1), c3 = sat16(cap3);
            memcpy(&g_spu_ram[0x000 + capture_pos * 2], &cl, 2);
            memcpy(&g_spu_ram[0x400 + capture_pos * 2], &cr, 2);
            memcpy(&g_spu_ram[0x800 + capture_pos * 2], &c1, 2);
            memcpy(&g_spu_ram[0xC00 + capture_pos * 2], &c3, 2);
            spu_irq_check(0x000 + capture_pos * 2, 2);
            spu_irq_check(0x400 + capture_pos * 2, 2);
            spu_irq_check(0x800 + capture_pos * 2, 2);
            spu_irq_check(0xC00 + capture_pos * 2, 2);
            capture_pos = (capture_pos + 1) & 0x1FF;
        }
        if (cd_on) {
            int32_t cl = (xl * (int16_t)REG(0x1B0)) >> 15, cr = (xr * (int16_t)REG(0x1B2)) >> 15;
            L += cl;
            R += cr;
            if (cnt & 4) { rinl += cl; rinr += cr; } /* CD reverb enable */
        }
        {
            int32_t ol, orr;
            reverb_sample(rinl, rinr, &ol, &orr);
            L += ol;
            R += orr;
            if (ol > diag_rvb_peak) diag_rvb_peak = ol; else if (-ol > diag_rvb_peak) diag_rvb_peak = -ol;
            { static FILE *dump; static int init; if (!init) { init = 1; const char *e = getenv("UJL_RVB_DUMP"); if (e) dump = fopen(e, "wb"); }
              if (dump) { int16_t x[3] = { sat16(ol), sat16(cap1), sat16(cap3) }; fwrite(x, 2, 3, dump); } }
        }
        if (!unmuted) L = R = 0;
        L = (L * vol_reg(REG(0x180))) >> 14;
        R = (R * vol_reg(REG(0x182))) >> 14;
        out[f * 2] = sat16(L);
        out[f * 2 + 1] = sat16(R);
    }
    host_audio_push(out, frames);
    diag_samples += (uint32_t)frames;
    if (diag_samples >= SPU_RATE) {
        diag_samples -= SPU_RATE;
        if (diag_kon) {
            extern int g_xa_filter_chan;
            LOGI("audio: keyed %06x  vol>0 %06x  vol=0 %06x  xa chan %d  reverb peak %d", diag_kon, diag_volnz, diag_volz, g_xa_filter_chan, diag_rvb_peak);
        }
        diag_kon = diag_volnz = diag_volz = 0;
        diag_rvb_peak = 0;
    }
}

static uint64_t next_mix;
static void on_mix(void *ctx) {
    (void)ctx;
    mix(CHUNK);
    next_mix += (uint64_t)CHUNK * CYCLES_PER_SAMPLE;
    sched_add(next_mix, on_mix, NULL, EV_SPU);
}

void spu_init(void) {
    next_mix = g_now + (uint64_t)CHUNK * CYCLES_PER_SAMPLE;
    sched_add(next_mix, on_mix, NULL, EV_SPU);
}

/* ------------------------------------------------------------ registers */
uint16_t spu_read(uint32_t off) {
    off &= 0x3FE;
    switch (off) {
    case 0x1AE: /* SPUSTAT: mode bits mirror SPUCNT, bit6 = IRQ flag, never busy */
        return (uint16_t)((REG(0x1AA) & 0x3F) | (irq_flag << 6) | ((capture_pos & 0x100) << 3));
    case 0x1A6: return (uint16_t)(xfer_addr >> 3);
    default: return REG(off);
    }
}

int g_spu_wlog;   /* debug: log every register write while set */
void spu_write(uint32_t off, uint16_t val) {
    off &= 0x3FE;
    if (g_spu_wlog) LOGI("spu: w %03x = %04x", off, val);
    if (g_spu_wlog && off == 0x010) { static int n; if (n++ < 2) rt_dump_stack("spu v1 vol write"); }
    if (off < 0x180 && (off & 0xF) == 0xC) return; /* current ADSR volume is read-only */
    if (off < 0x180 && ((off & 0xF) == 0 || (off & 0xF) == 2)) { if (val) diag_volnz |= 1u << (off >> 4); else diag_volz |= 1u << (off >> 4); }
    REG(off) = val;
    if (off < 0x180 && (off & 0xF) == 0xE) v[off >> 4].repeat = (uint32_t)val << 3, v[off >> 4].repeat_set = 1;
    switch (off) {
    case 0x188: for (int n = 0; n < 16; n++) if (val & (1 << n)) key_on(n); break;
    case 0x18A: for (int n = 0; n < 8; n++) if (val & (1 << n)) key_on(16 + n); break;
    case 0x18C: for (int n = 0; n < 16; n++) if (val & (1 << n)) key_off(n); break;
    case 0x18E: for (int n = 0; n < 8; n++) if (val & (1 << n)) key_off(16 + n); break;
    case 0x1AA: if (!(val & 0x40)) irq_flag = 0; break; /* clearing IRQ enable acks */
    case 0x1A2: { /* reverb work area base: restart in step with the capture position (see reverb) */
        uint32_t size = 0x80000u - ((uint32_t)val << 3);
        rvb_pos = size ? ((capture_pos >> 1) * 2) % size : 0;
        break; }
    case 0x1A6: { extern int g_spu_wlog; if (g_spu_wlog && dbg_xfer_n) { LOGI("spu: dma %s start %05x bytes %u", dbg_xfer_dir == 1 ? "write" : "read", dbg_xfer_start, dbg_xfer_n * 4); dbg_xfer_n = 0; } } xfer_addr = (uint32_t)val << 3; break;
    case 0x1A8: spu_irq_check(xfer_addr & 0x7FFFE, 2); memcpy(&g_spu_ram[xfer_addr & 0x7FFFE], &val, 2); xfer_addr = (xfer_addr + 2) & 0x7FFFF; break;
    default: break;
    }
}

static void dbg_xfer(int dir) {
    extern int g_spu_wlog;
    if (!g_spu_wlog) return;
    if (dbg_xfer_dir != dir || dbg_xfer_n == 0) {
        if (dbg_xfer_n) LOGI("spu: dma %s start %05x bytes %u", dbg_xfer_dir == 1 ? "write" : "read", dbg_xfer_start, dbg_xfer_n * 4);
        dbg_xfer_start = xfer_addr; dbg_xfer_n = 0; dbg_xfer_dir = dir;
    }
    dbg_xfer_n++;
}
void spu_dma_write(uint32_t w) {
    dbg_xfer(1);
    spu_irq_check(xfer_addr & 0x7FFFC, 4);
    memcpy(&g_spu_ram[xfer_addr & 0x7FFFC], &w, 4);
    xfer_addr = (xfer_addr + 4) & 0x7FFFF;
}

uint32_t spu_dma_read(void) {
    uint32_t w;
    dbg_xfer(2);
    spu_irq_check(xfer_addr & 0x7FFFC, 4);
    memcpy(&w, &g_spu_ram[xfer_addr & 0x7FFFC], 4);
    xfer_addr = (xfer_addr + 4) & 0x7FFFF;
    return w;
}

void spu_debug_dump(void) {
    for (int n = 0; n < 24; n++)
        if (kon_count[n]) LOGI("spu: v%02d keyed %llu times, last note peak %d, phase %d env %d", n,
                               (unsigned long long)kon_count[n], vpeak[n], v[n].phase, v[n].env);
}
