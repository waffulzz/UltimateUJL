/* MDEC: macroblock decoder (RLE -> dequant -> IDCT -> YUV->RGB). Written from the public
   hardware description. Decodes as soon as a command's parameters are complete; output is
   drained by DMA1, which waits (stays pending) until output exists. */
#include "psx.h"
#include <math.h>
#include <stdlib.h>

static const uint8_t zigzag[64] = {
    0, 1, 5, 6, 14, 15, 27, 28, 2, 4, 7, 13, 16, 26, 29, 42,
    3, 8, 12, 17, 25, 30, 41, 43, 9, 11, 18, 24, 31, 40, 44, 53,
    10, 19, 23, 32, 39, 45, 52, 54, 20, 22, 33, 38, 46, 51, 55, 60,
    21, 34, 37, 47, 50, 56, 59, 61, 35, 36, 48, 49, 57, 58, 62, 63};
static uint8_t zagzig[64];

static uint8_t qt_y[64], qt_c[64];
static int16_t scale[64];

/* command / parameter intake */
static uint32_t cur_cmd;
static uint32_t words_left;
static uint16_t *in_buf;
static uint32_t in_cap, in_len;    /* halfwords */
static int qt_pos, sc_pos;

/* output FIFO (words) */
static uint32_t *out_buf;
static uint32_t out_cap, out_len, out_pos;

static int enable_in, enable_out;
static int depth, is_signed, bit15;
static int cur_block = 4;

void dma_kick(int ch);

static void out_push(uint32_t w) {
    if (out_len >= out_cap) {
        out_cap = out_cap ? out_cap * 2 : 65536;
        out_buf = (uint32_t *)realloc(out_buf, out_cap * 4);
    }
    out_buf[out_len++] = w;
}

static inline int s10(uint16_t n) { return (int)((int16_t)(n << 6) >> 6); }

/* returns pointer after block, or NULL at end of input */
static const uint16_t *rl_decode(const uint16_t *src, const uint16_t *end, int16_t blk[64], const uint8_t *qt) {
    memset(blk, 0, 64 * sizeof blk[0]);
    uint16_t n;
    do {
        if (src >= end) return NULL;
        n = *src++;
    } while (n == 0xFE00);
    int k = 0;
    int q = (n >> 10) & 0x3F;
    int val = s10(n) * qt[0];
    for (;;) {
        if (q == 0) val = s10(n) * 2;
        if (val < -0x400) val = -0x400;
        if (val > 0x3FF) val = 0x3FF;
        if (q > 0) blk[zagzig[k]] = (int16_t)val;
        else blk[k] = (int16_t)val;
        if (src >= end) break;
        n = *src++;
        k += ((n >> 10) & 0x3F) + 1;
        if (k > 63) break;
        val = (s10(n) * qt[k] * q + 4) / 8;
    }
    return src;
}

static void idct(int16_t blk[64]) {
    int64_t tmp[64];
    for (int x = 0; x < 8; x++)
        for (int y = 0; y < 8; y++) {
            int64_t sum = 0;
            for (int z = 0; z < 8; z++) sum += (int64_t)blk[y + z * 8] * (scale[x + z * 8] / 8);
            tmp[x + y * 8] = (sum + 0xFFF) / 0x2000;
        }
    for (int x = 0; x < 8; x++)
        for (int y = 0; y < 8; y++) {
            int64_t sum = 0;
            for (int z = 0; z < 8; z++) sum += tmp[y + z * 8] * (scale[x + z * 8] / 8);
            int64_t v = (sum + 0xFFF) / 0x2000;
            if (v < -128) v = -128;
            if (v > 127) v = 127;
            blk[x + y * 8] = (int16_t)v;
        }
}

static inline int clamp8s(int v) { return v < -128 ? -128 : v > 127 ? 127 : v; }

static void emit_rgb(const int16_t *cr, const int16_t *cb, int16_t *ys[4]) {
    uint8_t px[16 * 16 * 3];
    for (int b = 0; b < 4; b++) {
        int xx = (b & 1) * 8, yy = (b >> 1) * 8;
        for (int y = 0; y < 8; y++)
            for (int x = 0; x < 8; x++) {
                int ci = ((x + xx) >> 1) + ((y + yy) >> 1) * 8;
                double R = cr[ci], B = cb[ci];
                double G = -0.3437 * B - 0.7143 * R;
                R = 1.402 * R;
                B = 1.772 * B;
                int Y = ys[b][x + y * 8];
                int r = clamp8s((int)lround(Y + R)), g = clamp8s((int)lround(Y + G)), bl = clamp8s((int)lround(Y + B));
                if (!is_signed) { r ^= 0x80; g ^= 0x80; bl ^= 0x80; }
                uint8_t *p = &px[((y + yy) * 16 + (x + xx)) * 3];
                p[0] = (uint8_t)r; p[1] = (uint8_t)g; p[2] = (uint8_t)bl;
            }
    }
    if (depth == 2) { /* 24bpp: pack bytes */
        for (int i = 0; i < 16 * 16 * 3; i += 4)
            out_push((uint32_t)px[i] | (uint32_t)px[i + 1] << 8 | (uint32_t)px[i + 2] << 16 | (uint32_t)px[i + 3] << 24);
    } else { /* 15bpp */
        for (int i = 0; i < 256; i += 2) {
            uint32_t w = 0;
            for (int k = 0; k < 2; k++) {
                const uint8_t *p = &px[(i + k) * 3];
                uint32_t c = (uint32_t)(p[0] >> 3) | (uint32_t)(p[1] >> 3) << 5 | (uint32_t)(p[2] >> 3) << 10 | (bit15 ? 0x8000u : 0);
                w |= c << (16 * k);
            }
            out_push(w);
        }
    }
}

static void emit_mono(const int16_t *y) {
    uint8_t v[64];
    for (int i = 0; i < 64; i++) {
        int s = clamp8s(y[i]);
        if (!is_signed) s ^= 0x80;
        v[i] = (uint8_t)s;
    }
    if (depth == 1) {
        for (int i = 0; i < 64; i += 4) out_push((uint32_t)v[i] | v[i + 1] << 8 | v[i + 2] << 16 | (uint32_t)v[i + 3] << 24);
    } else {
        for (int i = 0; i < 64; i += 8) {
            uint32_t w = 0;
            for (int k = 0; k < 8; k++) w |= (uint32_t)(v[i + k] >> 4) << (4 * k);
            out_push(w);
        }
    }
}

static void decode_all(void) {
    const uint16_t *src = in_buf, *end = in_buf + in_len;
    int16_t cr[64], cb[64], y0[64], y1[64], y2[64], y3[64];
    int16_t *ys[4] = {y0, y1, y2, y3};
    out_len = out_pos = 0;
    if (depth >= 2) {
        for (;;) {
            if (!(src = rl_decode(src, end, cr, qt_c))) break;
            idct(cr);
            if (!(src = rl_decode(src, end, cb, qt_c))) break;
            idct(cb);
            int ok = 1;
            for (int b = 0; b < 4 && ok; b++) {
                if (!(src = rl_decode(src, end, ys[b], qt_y))) ok = 0;
                else idct(ys[b]);
            }
            if (!ok) break;
            emit_rgb(cr, cb, ys);
        }
    } else {
        while ((src = rl_decode(src, end, y0, qt_y))) {
            idct(y0);
            emit_mono(y0);
        }
    }
    LOGV("mdec: decoded %u halfwords -> %u words", in_len, out_len);
}

static void param_word(uint32_t w) {
    uint32_t op = cur_cmd >> 29;
    if (op == 1) {
        if (in_len + 2 > in_cap) {
            in_cap = in_cap ? in_cap * 2 : 65536;
            in_buf = (uint16_t *)realloc(in_buf, in_cap * 2);
        }
        in_buf[in_len++] = (uint16_t)w;
        in_buf[in_len++] = (uint16_t)(w >> 16);
    } else if (op == 2) {
        for (int k = 0; k < 4; k++, qt_pos++) {
            uint8_t b = (uint8_t)(w >> (8 * k));
            if (qt_pos < 64) qt_y[qt_pos] = b;
            else if (qt_pos < 128) qt_c[qt_pos - 64] = b;
        }
    } else if (op == 3) {
        if (sc_pos < 64) scale[sc_pos++] = (int16_t)w;
        if (sc_pos < 64) scale[sc_pos++] = (int16_t)(w >> 16);
    }
    if (--words_left == 0) {
        if (op == 1) {
            decode_all();
            if (out_len) dma_kick(1);
        }
        cur_cmd = 0;
    }
}

static void command_word(uint32_t w) {
    cur_cmd = w;
    uint32_t op = w >> 29;
    depth = (w >> 27) & 3;
    is_signed = (w >> 26) & 1;
    bit15 = (w >> 25) & 1;
    switch (op) {
    case 1: words_left = w & 0xFFFF; in_len = 0; break;
    case 2: words_left = (w & 1) ? 32 : 16; qt_pos = 0; break;
    case 3: words_left = 32; sc_pos = 0; break;
    default: words_left = 0; cur_cmd = 0; break;
    }
    if (op == 1 && words_left == 0) cur_cmd = 0;
}

void mdec_dma_in(uint32_t w) {
    if (cur_cmd && words_left) param_word(w);
    else command_word(w);
}

uint32_t mdec_dma_out(void) {
    return out_pos < out_len ? out_buf[out_pos++] : 0;
}

int mdec_out_avail(void) { return out_pos < out_len; }

void mdec_write(uint32_t reg, uint32_t v) {
    if (reg == 0) { mdec_dma_in(v); return; }
    if (v & 0x80000000u) { /* reset */
        cur_cmd = 0; words_left = 0; out_len = out_pos = 0; in_len = 0;
        cur_block = 4;
    }
    enable_in = (v >> 30) & 1;
    enable_out = (v >> 29) & 1;
}

uint32_t mdec_read(uint32_t reg) {
    if (reg == 0) return mdec_dma_out();
    uint32_t s = 0;
    if (out_pos >= out_len) s |= 1u << 31;          /* out FIFO empty */
    if (cur_cmd && words_left) s |= 1u << 29;        /* busy receiving */
    if (enable_in) s |= 1u << 28;
    if (enable_out && out_pos < out_len) s |= 1u << 27;
    s |= (uint32_t)depth << 25 | (uint32_t)is_signed << 24 | (uint32_t)bit15 << 23;
    s |= (uint32_t)cur_block << 16;
    s |= (cur_cmd && words_left) ? ((words_left - 1) & 0xFFFF) : 0xFFFF;
    return s;
}

void mdec_init(void) {
    for (int i = 0; i < 64; i++) zagzig[zigzag[i]] = (uint8_t)i;
}
