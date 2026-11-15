/* Geometry Transformation Engine (COP2). Written from the public hardware documentation.
   Also hosts the widescreen hook: g_gte_xscale squeezes projected X around OFX (normally the screen center). */
#include "psx.h"
#include <stdlib.h>
#include <math.h>

int32_t g_gte_xscale = 4096;
/* Full-precision projection offsets (beyond the dropped fraction). Off: UJL's crowd is many overlapping copies of the
   same sprite, so different vertices share an integer SXY and the renderer's lookup can pick another copy's offset;
   with corrections of several pixels that made the crowd shake. The dropped fraction (under 1 px) is safe. */
int g_gte_precise = 0;

/* Recently projected screen positions (SXY words), so the GPU layer can tell 3D (GTE-projected) primitives
   from 2D ones drawn with CPU-computed coordinates, and draw them at sub-pixel precision. 4-way set-associative
   cache stamped with the frame: polygons that share a vertex must get the same answer, and with a direct-mapped
   table collisions gave neighbours different sub-pixel positions (visible cracks). */
#define SXY_SETS 16384
#define SXY_WAYS 4
typedef struct { uint32_t val, frame; int16_t ofx, dx, dy; } SxyEnt;
static SxyEnt sxy_tab[SXY_SETS][SXY_WAYS];
extern uint32_t g_gpu_frame;
/* coarse occupancy: 8x8-pixel cells over the GTE screen range, stamped with the frame */
static uint32_t cell_frame[256 * 256];
static inline int sxy_cell(int x, int y) { return (((y + 1024) >> 3) & 255) * 256 + (((x + 1024) >> 3) & 255); }
static inline uint32_t sxy_hash(uint32_t v) { return (v * 2654435761u) >> 18; }
int g_sxy_collisions; /* same frame, same integer SXY, different precise position (UJL_GTE_STATS) */
static void note_sxy(uint32_t v, int16_t ofx, int16_t dx, int16_t dy) {
    SxyEnt *set = sxy_tab[sxy_hash(v) & (SXY_SETS - 1)];
    uint32_t now = g_gpu_frame + 1; /* +1 so 0 means empty */
    int w = 0;
    for (int i = 0; i < SXY_WAYS; i++) {
        if (set[i].frame && set[i].val == v) {
            if (set[i].frame == now && (set[i].dx != dx || set[i].dy != dy)) g_sxy_collisions++;
            w = i; break;
        }
        if (set[i].frame < set[w].frame) w = i; /* oldest (or empty) */
    }
    set[w].val = v; set[w].frame = now; set[w].ofx = ofx; set[w].dx = dx; set[w].dy = dy;
    cell_frame[sxy_cell((int16_t)(v & 0xFFFF), (int16_t)(v >> 16))] = now;
}
static const SxyEnt *sxy_find(uint32_t v) {
    SxyEnt *set = sxy_tab[sxy_hash(v) & (SXY_SETS - 1)];
    for (int i = 0; i < SXY_WAYS; i++)
        if (set[i].frame && set[i].val == v && (g_gpu_frame + 1) - set[i].frame <= 2) return &set[i];
    return NULL;
}
int gte_sxy_near(int x, int y) { /* a projected vertex within ~1 cell of (x, y) in the last 2 frames */
    if (x < -1024 || x > 1023 || y < -1024 || y > 1023) return 0;
    int cx = (x + 1024) >> 3, cy = (y + 1024) >> 3;
    for (int dy = -1; dy <= 1; dy++)
        for (int dx = -1; dx <= 1; dx++) {
            int xx = cx + dx, yy = cy + dy;
            if (xx < 0 || yy < 0 || xx > 255 || yy > 255) continue;
            uint32_t f = cell_frame[yy * 256 + xx];
            if (f && (g_gpu_frame + 1) - f <= 2) return 1;
        }
    return 0;
}
/* OFX of a recently projected vertex, or 0x7FFF when v isn't one. Most 3D uses the screen center; Um Jammer
   Lammy's HUD rotates/flips 2D sprites with RotTransPers around the sprite's own position. */
int gte_sxy_ofx(uint32_t v) { const SxyEnt *e = sxy_find(v); return e ? e->ofx : 0x7FFF; }
/* Sub-pixel fraction of a recently projected vertex (x | y << 8, 1/256 px), 0 if v isn't one. Lets the renderer
   draw 3D geometry at the precise projected position instead of the integer one (no vertex jitter). */
int gte_sxy_off(uint32_t v, int16_t *dx, int16_t *dy) {
    const SxyEnt *e = sxy_find(v);
    *dx = e ? e->dx : 0; *dy = e ? e->dy : 0;
    return e != NULL;
}
int gte_sxy_recent(uint32_t v) { return sxy_find(v) != NULL; }

#define D (c->gte.data)
#define C (c->gte.ctrl)
#define FLAG C[31]

static inline int16_t lo16(int32_t v) { return (int16_t)(v & 0xFFFF); }
static inline int16_t hi16(int32_t v) { return (int16_t)((uint32_t)v >> 16); }

/* ---- register views ---- */
#define VX(n) lo16(D[(n) * 2])
#define VY(n) hi16(D[(n) * 2])
#define VZ(n) lo16(D[(n) * 2 + 1])
#define IR0 D[8]
#define IR1 D[9]
#define IR2 D[10]
#define IR3 D[11]
#define MAC0 D[24]
#define MAC1 D[25]
#define MAC2 D[26]
#define MAC3 D[27]

static inline int16_t mat(CPU *c, int m, int r, int col) {
    int i = m * 8 + r * 3 + col; /* 9 halfwords packed in 5 words */
    int32_t w = C[m * 8 + (r * 3 + col) / 2];
    (void)i;
    return ((r * 3 + col) & 1) ? hi16(w) : lo16(w);
}
static inline int32_t vecc(CPU *c, int base, int k) { return C[base + k]; } /* TR / BK / FC */

static uint8_t unr_table[0x101];
static int unr_ready;
static void init_unr(void) {
    for (int i = 0; i < 0x101; i++) {
        int v = (0x40000 / (i + 0x100) + 1) / 2 - 0x101;
        unr_table[i] = (uint8_t)(v < 0 ? 0 : v);
    }
    unr_ready = 1;
}

/* ---- saturation / overflow helpers ---- */
static inline int64_t chkA(CPU *c, int i, int64_t v) {
    if (v >= (1LL << 43)) FLAG |= 1u << (31 - i);       /* 30,29,28 */
    else if (v < -(1LL << 43)) FLAG |= 1u << (28 - i);  /* 27,26,25 */
    return (v << 20) >> 20;
}
static inline int32_t setMAC(CPU *c, int i, int64_t v, int shift) {
    v = chkA(c, i, v);
    int32_t r = (int32_t)(v >> shift);
    D[24 + i] = r;
    return r;
}
static inline int32_t limB(CPU *c, int i, int32_t v, int lm) {
    int32_t lo = lm ? 0 : -0x8000;
    if (v < lo) { FLAG |= 1u << (25 - i); return lo; }
    if (v > 0x7FFF) { FLAG |= 1u << (25 - i); return 0x7FFF; }
    return v;
}
static inline void setIR(CPU *c, int i, int32_t v, int lm) { D[8 + i] = limB(c, i, v, lm); }
static inline int32_t limC(CPU *c, int i, int32_t v) {
    if (v < 0) { FLAG |= 1u << (22 - i); return 0; }
    if (v > 255) { FLAG |= 1u << (22 - i); return 255; }
    return v;
}
static inline int32_t limD(CPU *c, int32_t v) {
    if (v < 0) { FLAG |= 1u << 18; return 0; }
    if (v > 0xFFFF) { FLAG |= 1u << 18; return 0xFFFF; }
    return v;
}
static inline int64_t chkF(CPU *c, int64_t v) {
    if (v > 0x7FFFFFFFLL) FLAG |= 1u << 16;
    else if (v < -0x80000000LL) FLAG |= 1u << 15;
    return v;
}
static inline int32_t limG(CPU *c, int bit, int32_t v) {
    if (v < -0x400) { FLAG |= 1u << bit; return -0x400; }
    if (v > 0x3FF) { FLAG |= 1u << bit; return 0x3FF; }
    return v;
}
static inline int32_t limH(CPU *c, int32_t v) {
    if (v < 0) { FLAG |= 1u << 12; return 0; }
    if (v > 0x1000) { FLAG |= 1u << 12; return 0x1000; }
    return v;
}

static void push_color(CPU *c) {
    D[20] = D[21];
    D[21] = D[22];
    uint32_t code = (uint32_t)D[6] >> 24;
    D[22] = (int32_t)((uint32_t)limC(c, 1, MAC1 >> 4) | ((uint32_t)limC(c, 2, MAC2 >> 4) << 8) |
                      ((uint32_t)limC(c, 3, MAC3 >> 4) << 16) | (code << 24));
}
static void push_sz(CPU *c, int32_t z) {
    D[16] = D[17]; D[17] = D[18]; D[18] = D[19];
    D[19] = limD(c, z);
}
/* precise offsets (1/256 px) of the SXY FIFO entries, valid only for points this GTE projected */
static int16_t fifo_dx[3], fifo_dy[3];
static uint8_t fifo_ok[3];
static void push_sxy(CPU *c, int32_t x, int32_t y) {
    D[12] = D[13]; D[13] = D[14];
    fifo_dx[0] = fifo_dx[1]; fifo_dy[0] = fifo_dy[1]; fifo_ok[0] = fifo_ok[1];
    fifo_dx[1] = fifo_dx[2]; fifo_dy[1] = fifo_dy[2]; fifo_ok[1] = fifo_ok[2];
    fifo_ok[2] = 0;
    D[14] = (int32_t)(((uint32_t)(uint16_t)x) | ((uint32_t)(uint16_t)y << 16));
}

static uint32_t gte_divide(CPU *c, uint32_t h, uint32_t sz3) {
    if (h < sz3 * 2) {
        int z = 0;
        for (uint32_t t = sz3 & 0xFFFF; z < 16 && !(t & 0x8000); t <<= 1) z++;
        uint64_t n = (uint64_t)h << z;
        int64_t d = (int64_t)(sz3 << z);
        int64_t u = unr_table[(d - 0x7FC0) >> 7] + 0x101;
        d = (0x2000080 - d * u) >> 8;
        d = (0x0000080 + d * u) >> 8;
        uint64_t r = (n * (uint64_t)d + 0x8000) >> 16;
        return r > 0x1FFFF ? 0x1FFFF : (uint32_t)r;
    }
    FLAG |= 1u << 17;
    return 0x1FFFF;
}

/* ---- RTPS core: transform vector n, project ---- */
static void rtp(CPU *c, int n, int sf, int lm, int last) {
    int shift = sf ? 12 : 0;
    int64_t vx = VX(n), vy = VY(n), vz = VZ(n);
    int64_t x = chkA(c, 1, chkA(c, 1, chkA(c, 1, ((int64_t)C[5] << 12) + mat(c, 0, 0, 0) * vx) + mat(c, 0, 0, 1) * vy) + mat(c, 0, 0, 2) * vz);
    int64_t y = chkA(c, 2, chkA(c, 2, chkA(c, 2, ((int64_t)C[6] << 12) + mat(c, 0, 1, 0) * vx) + mat(c, 0, 1, 1) * vy) + mat(c, 0, 1, 2) * vz);
    int64_t z = chkA(c, 3, chkA(c, 3, chkA(c, 3, ((int64_t)C[7] << 12) + mat(c, 0, 2, 0) * vx) + mat(c, 0, 2, 1) * vy) + mat(c, 0, 2, 2) * vz);
    MAC1 = (int32_t)(x >> shift);
    MAC2 = (int32_t)(y >> shift);
    MAC3 = (int32_t)(z >> shift);
    setIR(c, 1, MAC1, lm);
    setIR(c, 2, MAC2, lm);
    /* IR3 quirk: saturation flag is checked against z>>12 regardless of sf */
    {
        int32_t z12 = (int32_t)(z >> 12);
        int32_t lo = lm ? 0 : -0x8000;
        if (z12 < -0x8000 || z12 > 0x7FFF) FLAG |= 1u << 22;
        int32_t v = MAC3;
        IR3 = v < lo ? lo : v > 0x7FFF ? 0x7FFF : v;
    }
    push_sz(c, (int32_t)(z >> 12));
    uint32_t h = (uint32_t)(uint16_t)C[26];
    int64_t q = gte_divide(c, h, (uint32_t)D[19]);
    int64_t px = q * (int16_t)IR1;
    if (g_gte_xscale != 4096) px = (px * g_gte_xscale) >> 12;
    int64_t sx = chkF(c, px + C[24]);
    int64_t sy = chkF(c, q * (int16_t)IR2 + C[25]);
    push_sxy(c, limG(c, 14, (int32_t)(sx >> 16)), limG(c, 13, (int32_t)(sy >> 16)));
    {   /* UJL_GTE_STATS: pixels per view-space unit (H/SZ). Above ~1, IR1/IR2 integer truncation alone moves
           vertices by more than a pixel, which the sub-pixel fraction can't hide. */
        static int st = -1; static uint32_t lastf; static int n, big; static double sum, mx;
        if (st < 0) st = getenv("UJL_GTE_STATS") != NULL;
        if (st) {
            uint32_t sz = (uint32_t)(uint16_t)D[19];
            double r = sz ? (double)h / sz : 0;
            n++; sum += r; if (r > mx) mx = r; if (r > 1.0) big++;
            if (g_gpu_frame / 120 != lastf) {
                lastf = g_gpu_frame / 120;
                rt_log("gte: %d rtp, H/SZ avg %.3f max %.3f, >1px/unit %d, sxy collisions %d", n, n ? sum / n : 0, mx, big, g_sxy_collisions); g_sxy_collisions = 0;
                n = big = 0; sum = mx = 0;
            }
        }
    }
    {   /* Precise position for the renderer (offset from the stored integer SXY, 1/256 px), only when not saturated.
           Normally the fraction the hardware drops. With sf=1 and no divide overflow, it's the projection of the
           full-precision view position (MAC before the >>12, z with its fraction) instead: geometry close to the
           camera (H/SZ up to ~400 px per view unit in UJL) otherwise jumps by whole pixels as IR1/IR2/SZ truncate.
           Larger corrections than 16 px are left alone (degenerate near-plane cases). */
        int32_t ix = (int32_t)(sx >> 16), iy = (int32_t)(sy >> 16);
        int16_t dx = 0, dy = 0;
        if (ix >= -0x400 && ix <= 0x3FF && iy >= -0x400 && iy <= 0x3FF) {
            dx = (int16_t)((sx >> 8) & 0xFF); dy = (int16_t)((sy >> 8) & 0xFF);
            if (g_gte_precise && sf && z > 0 && !(FLAG & (1u << 17)) && !(FLAG & (3u << 23))) {
                double zv = (double)z, px2 = (double)h * (double)x / zv, py2 = (double)h * (double)y / zv;
                if (g_gte_xscale != 4096) px2 = px2 * g_gte_xscale / 4096.0;
                double fx = (px2 + C[24] / 65536.0 - ix) * 256.0, fy = (py2 + C[25] / 65536.0 - iy) * 256.0;
                if (fx > -4096 && fx < 4096 && fy > -4096 && fy < 4096) { dx = (int16_t)lround(fx); dy = (int16_t)lround(fy); }
            }
        }
        note_sxy((uint32_t)D[14], (int16_t)(C[24] >> 16), dx, dy);
        fifo_dx[2] = dx; fifo_dy[2] = dy; fifo_ok[2] = 1;
    }
    if (last) {
        int64_t m0 = chkF(c, q * (int16_t)C[27] + C[28]);
        MAC0 = (int32_t)m0;
        IR0 = limH(c, (int32_t)(m0 >> 12));
    }
}

/* matrix * vector (+ translation), generic MVMVA style */
static void mvmva(CPU *c, int sf, int lm, int mx, int v, int cv) {
    int shift = sf ? 12 : 0;
    int64_t vx, vy, vz;
    if (v == 3) { vx = (int16_t)IR1; vy = (int16_t)IR2; vz = (int16_t)IR3; }
    else { vx = VX(v); vy = VY(v); vz = VZ(v); }
    int16_t m[3][3];
    if (mx == 3) { /* garbage matrix */
        int16_t r = (int16_t)(((uint32_t)(uint8_t)D[6]) << 4);
        m[0][0] = -r; m[0][1] = r; m[0][2] = (int16_t)IR0;
        m[1][0] = m[1][1] = m[1][2] = mat(c, 0, 0, 2);
        m[2][0] = m[2][1] = m[2][2] = mat(c, 0, 1, 1);
    } else {
        for (int r = 0; r < 3; r++)
            for (int k = 0; k < 3; k++) m[r][k] = mat(c, mx, r, k);
    }
    int base = cv == 0 ? 5 : cv == 1 ? 13 : 21;
    for (int r = 0; r < 3; r++) {
        int64_t t = cv == 3 ? 0 : ((int64_t)vecc(c, base, r) << 12);
        if (cv == 2) { /* FC bug: first column result is discarded except flags */
            chkA(c, r + 1, chkA(c, r + 1, t + m[r][0] * vx) );
            int64_t a = chkA(c, r + 1, chkA(c, r + 1, m[r][1] * vy) + m[r][2] * vz);
            setMAC(c, r + 1, a, shift);
            setIR(c, r + 1, D[25 + r], lm);
            continue;
        }
        int64_t a = chkA(c, r + 1, chkA(c, r + 1, chkA(c, r + 1, t + m[r][0] * vx) + m[r][1] * vy) + m[r][2] * vz);
        setMAC(c, r + 1, a, shift);
        setIR(c, r + 1, D[25 + r], lm);
    }
}

static void mac_to_ir(CPU *c, int lm) {
    setIR(c, 1, MAC1, lm);
    setIR(c, 2, MAC2, lm);
    setIR(c, 3, MAC3, lm);
}

/* MAC = MAC + (FC - MAC) * IR0, used by depth-cue ops. Input MACs already prepared (<<12 scale). */
static void interp_fc(CPU *c, int64_t m1, int64_t m2, int64_t m3, int sf, int lm) {
    int shift = sf ? 12 : 0;
    int64_t in[3] = {m1, m2, m3};
    for (int i = 0; i < 3; i++) {
        int64_t fc = (int64_t)C[21 + i] << 12;
        int32_t d = (int32_t)(chkA(c, i + 1, fc - in[i]) >> shift);
        int32_t ir = limB(c, i + 1, d, 0);
        int64_t r = chkA(c, i + 1, (int64_t)ir * (int16_t)IR0 + in[i]);
        D[25 + i] = (int32_t)(r >> shift);
    }
    mac_to_ir(c, lm);
}

static void ncs_core(CPU *c, int n, int sf, int lm, int mode /*0 NC,1 NCC,2 NCD*/) {
    int shift = sf ? 12 : 0;
    /* light matrix */
    mvmva(c, sf, lm, 1, n, 3);
    /* color matrix + background */
    mvmva(c, sf, lm, 2, 3, 1);
    if (mode == 0) { push_color(c); return; }
    uint32_t rgbc = (uint32_t)D[6];
    int64_t r = (int64_t)(rgbc & 0xFF), g = (int64_t)((rgbc >> 8) & 0xFF), b = (int64_t)((rgbc >> 16) & 0xFF);
    int64_t m1 = (r * (int16_t)IR1) << 4, m2 = (g * (int16_t)IR2) << 4, m3 = (b * (int16_t)IR3) << 4;
    if (mode == 1) {
        setMAC(c, 1, m1, shift); setMAC(c, 2, m2, shift); setMAC(c, 3, m3, shift);
        mac_to_ir(c, lm);
    } else {
        interp_fc(c, m1, m2, m3, sf, lm);
    }
    push_color(c);
}

void gte_cmd(CPU *c, uint32_t cmd) {
    if (!unr_ready) init_unr();
    int sf = (cmd >> 19) & 1, lm = (cmd >> 10) & 1;
    int shift = sf ? 12 : 0;
    int mx = (cmd >> 17) & 3, v = (cmd >> 15) & 3, cv = (cmd >> 13) & 3;
    FLAG = 0;
    switch (cmd & 0x3F) {
    case 0x01: rtp(c, 0, sf, lm, 1); break; /* RTPS */
    case 0x30: rtp(c, 0, sf, lm, 0); rtp(c, 1, sf, lm, 0); rtp(c, 2, sf, lm, 1); break; /* RTPT */
    case 0x06: { /* NCLIP */
        int64_t x0 = lo16(D[12]), y0 = hi16(D[12]), x1 = lo16(D[13]), y1 = hi16(D[13]), x2 = lo16(D[14]), y2 = hi16(D[14]);
        MAC0 = (int32_t)chkF(c, x0 * y1 + x1 * y2 + x2 * y0 - x0 * y2 - x1 * y0 - x2 * y1);
        break;
    }
    case 0x0C: { /* OP */
        int64_t d1 = mat(c, 0, 0, 0), d2 = mat(c, 0, 1, 1), d3 = mat(c, 0, 2, 2);
        int64_t i1 = (int16_t)IR1, i2 = (int16_t)IR2, i3 = (int16_t)IR3;
        setMAC(c, 1, i3 * d2 - i2 * d3, shift);
        setMAC(c, 2, i1 * d3 - i3 * d1, shift);
        setMAC(c, 3, i2 * d1 - i1 * d2, shift);
        mac_to_ir(c, lm);
        break;
    }
    case 0x10: { /* DPCS */
        uint32_t rgbc = (uint32_t)D[6];
        interp_fc(c, (int64_t)(rgbc & 0xFF) << 16, (int64_t)((rgbc >> 8) & 0xFF) << 16, (int64_t)((rgbc >> 16) & 0xFF) << 16, sf, lm);
        push_color(c);
        break;
    }
    case 0x2A: /* DPCT */
        for (int i = 0; i < 3; i++) {
            uint32_t rgb = (uint32_t)D[20];
            interp_fc(c, (int64_t)(rgb & 0xFF) << 16, (int64_t)((rgb >> 8) & 0xFF) << 16, (int64_t)((rgb >> 16) & 0xFF) << 16, sf, lm);
            push_color(c);
        }
        break;
    case 0x11: /* INTPL */
        interp_fc(c, (int64_t)(int16_t)IR1 << 12, (int64_t)(int16_t)IR2 << 12, (int64_t)(int16_t)IR3 << 12, sf, lm);
        push_color(c);
        break;
    case 0x12: mvmva(c, sf, lm, mx, v, cv); break; /* MVMVA */
    case 0x13: ncs_core(c, 0, sf, lm, 2); break;   /* NCDS */
    case 0x16: for (int i = 0; i < 3; i++) ncs_core(c, i, sf, lm, 2); break; /* NCDT */
    case 0x1B: ncs_core(c, 0, sf, lm, 1); break;   /* NCCS */
    case 0x3F: for (int i = 0; i < 3; i++) ncs_core(c, i, sf, lm, 1); break; /* NCCT */
    case 0x1E: ncs_core(c, 0, sf, lm, 0); break;   /* NCS */
    case 0x20: for (int i = 0; i < 3; i++) ncs_core(c, i, sf, lm, 0); break; /* NCT */
    case 0x14: { /* CDP */
        mvmva(c, sf, lm, 2, 3, 1);
        uint32_t rgbc = (uint32_t)D[6];
        int64_t r = rgbc & 0xFF, g = (rgbc >> 8) & 0xFF, b = (rgbc >> 16) & 0xFF;
        interp_fc(c, (r * (int16_t)IR1) << 4, (g * (int16_t)IR2) << 4, (b * (int16_t)IR3) << 4, sf, lm);
        push_color(c);
        break;
    }
    case 0x1C: { /* CC */
        mvmva(c, sf, lm, 2, 3, 1);
        uint32_t rgbc = (uint32_t)D[6];
        int64_t r = rgbc & 0xFF, g = (rgbc >> 8) & 0xFF, b = (rgbc >> 16) & 0xFF;
        setMAC(c, 1, (r * (int16_t)IR1) << 4, shift);
        setMAC(c, 2, (g * (int16_t)IR2) << 4, shift);
        setMAC(c, 3, (b * (int16_t)IR3) << 4, shift);
        mac_to_ir(c, lm);
        push_color(c);
        break;
    }
    case 0x29: { /* DCPL */
        uint32_t rgbc = (uint32_t)D[6];
        int64_t r = rgbc & 0xFF, g = (rgbc >> 8) & 0xFF, b = (rgbc >> 16) & 0xFF;
        interp_fc(c, (r * (int16_t)IR1) << 4, (g * (int16_t)IR2) << 4, (b * (int16_t)IR3) << 4, sf, lm);
        push_color(c);
        break;
    }
    case 0x28: /* SQR */
        setMAC(c, 1, (int64_t)(int16_t)IR1 * (int16_t)IR1, shift);
        setMAC(c, 2, (int64_t)(int16_t)IR2 * (int16_t)IR2, shift);
        setMAC(c, 3, (int64_t)(int16_t)IR3 * (int16_t)IR3, shift);
        mac_to_ir(c, lm);
        break;
    case 0x2D: { /* AVSZ3 */
        int64_t s = (int64_t)(int16_t)C[29] * ((uint16_t)D[17] + (uint16_t)D[18] + (uint16_t)D[19]);
        MAC0 = (int32_t)chkF(c, s);
        D[7] = limD(c, (int32_t)(s >> 12));
        break;
    }
    case 0x2E: { /* AVSZ4 */
        int64_t s = (int64_t)(int16_t)C[30] * ((uint16_t)D[16] + (uint16_t)D[17] + (uint16_t)D[18] + (uint16_t)D[19]);
        MAC0 = (int32_t)chkF(c, s);
        D[7] = limD(c, (int32_t)(s >> 12));
        break;
    }
    case 0x3D: /* GPF */
        setMAC(c, 1, (int64_t)(int16_t)IR0 * (int16_t)IR1, shift);
        setMAC(c, 2, (int64_t)(int16_t)IR0 * (int16_t)IR2, shift);
        setMAC(c, 3, (int64_t)(int16_t)IR0 * (int16_t)IR3, shift);
        mac_to_ir(c, lm);
        push_color(c);
        break;
    case 0x3E: /* GPL */
        setMAC(c, 1, ((int64_t)MAC1 << shift) + (int64_t)(int16_t)IR0 * (int16_t)IR1, shift);
        setMAC(c, 2, ((int64_t)MAC2 << shift) + (int64_t)(int16_t)IR0 * (int16_t)IR2, shift);
        setMAC(c, 3, ((int64_t)MAC3 << shift) + (int64_t)(int16_t)IR0 * (int16_t)IR3, shift);
        mac_to_ir(c, lm);
        push_color(c);
        break;
    default:
        LOGI("gte: unknown command %07x", cmd);
        break;
    }
    if (FLAG & 0x7F87E000) FLAG |= 0x80000000u;
}

/* ---- precise positions keyed by RAM address ----
   UJL stores projected points straight into its GPU packets (SWC2 SXY0-2). Recording each store's address lets
   the GPU layer find a vertex's precise position by the packet word's address instead of by its integer value:
   ~100 vertices a frame share an integer SXY with a different point (overlapping parts like Stage 1's drumset),
   and the value lookup handed them the other point's fraction, which changed every frame (jitter). */
typedef struct { uint32_t val, frame; int16_t dx, dy; uint8_t ok; } AddrEnt;
static AddrEnt *addr_tab; /* one per RAM word (2 MB) */
void gte_store_sxy(CPU *c, uint32_t ea, int r) {
    uint32_t v = gte_read(c, r);
    mem_w32(ea, v);
    uint32_t a = ea & 0x1FFFFFFFu;
    if (a >= 0x800000u) return;
    if (!addr_tab) { addr_tab = (AddrEnt *)calloc(0x80000, sizeof(AddrEnt)); if (!addr_tab) return; }
    AddrEnt *e = &addr_tab[(a & 0x1FFFFC) >> 2];
    int slot = r == 15 ? 2 : r - 12;
    e->val = v; e->frame = g_gpu_frame; e->dx = fifo_dx[slot]; e->dy = fifo_dy[slot]; e->ok = fifo_ok[slot];
}
int gte_addr_off(uint32_t phys, uint32_t val, int16_t *dx, int16_t *dy) {
    if (!addr_tab || phys >= 0x200000u) return 0;
    const AddrEnt *e = &addr_tab[(phys & 0x1FFFFC) >> 2];
    if (!e->ok || e->val != val || g_gpu_frame - e->frame > 2) return 0; /* stale: rewritten since by other means */
    *dx = e->dx; *dy = e->dy;
    return 1;
}

/* ---- register file access ---- */
uint32_t gte_read(CPU *c, int r) {
    if (r < 32) {
        switch (r) {
        case 1: case 3: case 5: case 8: case 9: case 10: case 11:
            return (uint32_t)(int32_t)(int16_t)D[r];
        case 7: case 16: case 17: case 18: case 19:
            return (uint32_t)(uint16_t)D[r];
        case 15: return (uint32_t)D[14];
        case 28: case 29: {
            int32_t rr = (int16_t)IR1 >> 7, gg = (int16_t)IR2 >> 7, bb = (int16_t)IR3 >> 7;
            rr = rr < 0 ? 0 : rr > 0x1F ? 0x1F : rr;
            gg = gg < 0 ? 0 : gg > 0x1F ? 0x1F : gg;
            bb = bb < 0 ? 0 : bb > 0x1F ? 0x1F : bb;
            return (uint32_t)(rr | (gg << 5) | (bb << 10));
        }
        case 31: {
            uint32_t v = (uint32_t)D[30];
            if ((int32_t)v < 0) v = ~v;
            int n = 0;
            while (n < 32 && !(v & 0x80000000u)) { v <<= 1; n++; }
            return (uint32_t)n;
        }
        default: return (uint32_t)D[r];
        }
    }
    r -= 32;
    switch (r) {
    case 4: case 12: case 20: case 26: case 27: case 29: case 30:
        return (uint32_t)(int32_t)(int16_t)C[r];
    default: return (uint32_t)C[r];
    }
}

void gte_write(CPU *c, int r, uint32_t v) {
    if (r < 32) {
        switch (r) {
        case 15: push_sxy(c, (int16_t)(v & 0xFFFF), (int16_t)(v >> 16)); return;
        case 12: case 13: case 14: fifo_ok[r - 12] = 0; D[r] = (int32_t)v; return;
        case 28:
            D[28] = (int32_t)(v & 0x7FFF);
            IR1 = (int32_t)((v & 0x1F) << 7);
            IR2 = (int32_t)(((v >> 5) & 0x1F) << 7);
            IR3 = (int32_t)(((v >> 10) & 0x1F) << 7);
            return;
        case 29: case 31: return;
        case 7: case 16: case 17: case 18: case 19: D[r] = (int32_t)(v & 0xFFFF); return;
        case 1: case 3: case 5: case 8: case 9: case 10: case 11: D[r] = (int16_t)(v & 0xFFFF); return;
        default: D[r] = (int32_t)v; return;
        }
    }
    r -= 32;
    switch (r) {
    case 4: case 12: case 20: case 26: case 27: case 29: case 30: C[r] = (int16_t)(v & 0xFFFF); return;
    case 31:
        C[31] = (int32_t)(v & 0x7FFFF000u);
        if (C[31] & 0x7F87E000) C[31] |= (int32_t)0x80000000u;
        return;
    default: C[r] = (int32_t)v; return;
    }
}
