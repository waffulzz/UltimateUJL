/* CD-ROM controller emulation (register level) + XA-ADPCM decoding.
   Behaviour follows the public controller documentation. */
#include "psx.h"
#include <stdlib.h>
extern uint64_t SDL_GetTicksNS(void); /* timing logs only (the SDL header clashes with "stat" here) */

#define SECTOR_1X (PSX_CLOCK / 75)

static uint8_t idx, ienable, iflag;
static uint8_t param[16], nparam;
static uint8_t resp[16], nresp, resp_pos;
static uint8_t stat = 0x02;   /* motor on */
static uint8_t mode;
static uint8_t filter_file, filter_chan;
int g_xa_filter_chan;
int cd_xa_streaming(void) { return (mode & 0x40) != 0; }   /* XA-ADPCM playback enabled (a song or movie is streaming) */
static uint32_t setloc_lba, read_lba;
static int setloc_pending;
static int reading;           /* 1 ReadN/ReadS active */
static uint8_t sector[2352];   /* last read sector */
static uint8_t databuf[2352];
static uint32_t data_len, data_pos;
static int request_bfrd;

/* pending interrupt queue (one is visible at a time) */
typedef struct { uint8_t type; uint8_t n; uint8_t bytes[16]; uint64_t delay; } CdInt;
static CdInt q[16];
static int qh, qt;

static uint8_t bcd2bin(uint8_t b) { return (uint8_t)((b >> 4) * 10 + (b & 15)); }
static uint8_t bin2bcd(uint8_t b) { return (uint8_t)(((b / 10) << 4) | (b % 10)); }

static void deliver_next(void *ctx);

static void try_deliver(void) {
    if (iflag & 7) return;               /* previous not acked */
    if (qh == qt) return;
    sched_add(g_now + q[qh].delay, deliver_next, NULL, EV_CD_RESP);
}

static void deliver_next(void *ctx) {
    (void)ctx;
    if ((iflag & 7) || qh == qt) return;
    CdInt *e = &q[qh];
    qh = (qh + 1) & 15;
    memcpy(resp, e->bytes, e->n);
    nresp = e->n;
    resp_pos = 0;
    iflag = (iflag & ~7) | e->type;
    LOGV("cd: deliver INT%d [%02x %02x] ien=%02x", e->type, e->n ? e->bytes[0] : 0, e->n > 1 ? e->bytes[1] : 0, ienable);
    if (ienable & iflag & 0x1F) irq_raise(IRQ_CDROM);
}

static void push_int(uint8_t type, uint64_t delay, int n, const uint8_t *b) {
    CdInt *e = &q[qt];
    qt = (qt + 1) & 15;
    e->type = type;
    e->n = (uint8_t)n;
    e->delay = delay;
    if (n) memcpy(e->bytes, b, (size_t)n);
    try_deliver();
}
static void ack_stat(uint8_t type, uint64_t delay) { uint8_t s = stat; push_int(type, delay, 1, &s); }

/* ------------------------------------------------------------ XA ADPCM */
static const int xa_pos[4] = {0, 60, 115, 98};
static const int xa_neg[4] = {0, 0, -52, -55};
static int32_t xa_hist[2][2];

int g_xa_sectors, g_cd_sectors;
static void xa_decode_sector(const uint8_t *s) {
    g_xa_sectors++;
    uint8_t coding = s[16 + 3];
    int stereo = (coding & 3) == 1;
    int rate = (coding & 0x0C) ? 18900 : 37800;
    int bits8 = (coding & 0x30) == 0x10;
    static int16_t out[18 * 8 * 28];
    int nout = 0;
    const uint8_t *g = s + 24;
    for (int grp = 0; grp < 18; grp++, g += 128) {
        int units = bits8 ? 4 : 8;
        for (int u = 0; u < units; u++) {
            uint8_t prm = g[4 + u];
            int shift = prm & 0x0F, filt = (prm >> 4) & 3;
            int ch = stereo ? (u & 1) : 0;
            int32_t *h = xa_hist[ch];
            for (int j = 0; j < 28; j++) {
                int32_t smp;
                if (bits8) smp = (int16_t)((uint16_t)g[16 + j * 4 + u] << 8);
                else {
                    uint8_t b = g[16 + j * 4 + (u >> 1)];
                    smp = (int16_t)((uint16_t)((u & 1) ? (b >> 4) : (b & 15)) << 12);
                }
                smp >>= shift > 12 ? 9 : shift;
                smp += (h[0] * xa_pos[filt] + h[1] * xa_neg[filt] + 32) >> 6;
                if (smp > 32767) smp = 32767;
                if (smp < -32768) smp = -32768;
                h[1] = h[0];
                h[0] = smp;
                if (stereo) {
                    /* interleave: even units L, odd units R -> pair them up */
                    int pair = (u >> 1) * 28 + j;
                    out[(grp * (units / 2) * 28 + pair) * 2 + ch] = (int16_t)smp;
                } else {
                    out[grp * units * 28 + u * 28 + j] = (int16_t)smp;
                }
            }
        }
        nout = stereo ? (grp + 1) * (units / 2) * 28 : (grp + 1) * units * 28;
    }
    spu_xa_push(out, nout, rate, stereo);
}

/* Position latched at each vblank. UJL reads its song clock with GetlocL every frame while the XA song streams.
   Recompiled code takes no emulated time; emulated time follows the host clock, so where in the frame the query
   lands depended on how fast the host ran that frame: the position came back lumpy (+2, +0, +2 per vblank instead
   of +1/+1/+1/+2) and the stage camera, which follows the song clock, juddered (Stage 1's crowd showed it most).
   While XA streams, GetlocL answers with the position at the last vblank: a steady cadence, as on hardware where
   the game's per-frame work takes the same time every frame. */
static uint8_t loc_latch[8];
static int loc_latched;
void cdrom_vblank(void) {
    static int off = -1; if (off < 0) off = getenv("UJL_NO_LOC_LATCH") != NULL;
    if (reading && !off) { memcpy(loc_latch, sector + 12, 8); loc_latched = 1; }
    else loc_latched = 0;
}

/* ------------------------------------------------------------ reading */
static void sector_tick(void *ctx);

/* Fast loading (setting): while the game reads data (XA-ADPCM streaming off), each sector arrives as soon as the
   game has taken the previous one instead of at 75/150 sectors a second. Song streaming keeps real speed, so the
   music and the song clock (GetlocL) are untouched. */
int g_cd_fast_load; /* set by the frontend setting (or UJL_FAST_LOAD=1) */
#define FAST_SECTOR 3000ull
/* Loading turbo: after each fast-loaded data sector the emulation may run unthrottled for a while (cpu.c), so the
   frames a loader spends between files (and its loading-screen animation) pass instantly. Song streaming ends it. */
uint64_t g_load_turbo_until;
#define TURBO_TAIL (PSX_CLOCK / 2)
static int fast_data(void) {
    static int env = -1;
    if (env < 0) env = getenv("UJL_FAST_LOAD") != NULL;
    return (g_cd_fast_load || env) && !(mode & 0x40);
}

static uint64_t sector_period(void) {
    if (fast_data()) return FAST_SECTOR;
    return (mode & 0x80) ? SECTOR_1X / 2 : SECTOR_1X;
}

/* sector ticks run on a fixed grid (next += period), not relative to when the previous tick
   happened to fire; otherwise lateness accumulates and streamed XA audio arrives too slowly */
static uint64_t next_sector_at;

static void start_reading(void) {
    if (setloc_pending) { read_lba = setloc_lba; setloc_pending = 0; }
    reading = 1;
    stat = (stat & ~0xC0) | 0x20 | 0x02;
    next_sector_at = g_now + (fast_data() ? FAST_SECTOR : sector_period() * 3);
    sched_add(next_sector_at, sector_tick, NULL, EV_CD_SECTOR);
}

static void sector_tick(void *ctx) {
    (void)ctx;
    if (!reading) return;
    if (fast_data() && ((iflag & 7) || qh != qt)) { /* the game hasn't taken the previous sector yet */
        next_sector_at = g_now + FAST_SECTOR;
        sched_add(next_sector_at, sector_tick, NULL, EV_CD_SECTOR);
        return;
    }
    LOGV("cd: sector %u (iflag=%x q=%d)", read_lba, iflag, (qt - qh) & 15);
    disc_read_sector(read_lba, sector);
    read_lba++;
    g_cd_sectors++;
    uint8_t file = sector[16], chan = sector[17], sub = sector[18];
    int is_audio = (sub & 0x04) && (sub & 0x20); /* audio + form2 */
    int deliver_data = 1;
    { extern int g_spu_log; static uint32_t chmask, nsec; if (g_spu_log && is_audio) { chmask |= 1u << (chan & 31); if (++nsec % 150 == 0) { LOGI("cd: XA channels seen %08x (file %u), playing %u/%u", chmask, file, filter_file, filter_chan); chmask = 0; } } }
    if ((mode & 0x40) && is_audio) {             /* XA-ADPCM enabled */
        if (!(mode & 0x08) || (file == filter_file && chan == filter_chan)) xa_decode_sector(sector);
        deliver_data = 0;
    } else if ((mode & 0x08) && (mode & 0x40)) {
        /* filter on, non-audio sector of other channel: skip */
        if (file != filter_file || chan != filter_chan) deliver_data = 0;
    }
    if (sub & 0x80 && (mode & 0x40)) { /* EOF bit on XA stream */ }
    if (deliver_data) {
        if (mode & 0x20) { memcpy(databuf, sector + 12, 0x924); data_len = 0x924; }
        else { memcpy(databuf, sector + 24, 0x800); data_len = 0x800; }
        data_pos = data_len; /* not loaded until BFRD */
        ack_stat(1, 0);
        if (fast_data()) g_load_turbo_until = g_now + TURBO_TAIL;
    }
    next_sector_at += sector_period();
    if (next_sector_at + sector_period() * 8 < g_now) next_sector_at = g_now; /* after a long stall, don't burst */
    sched_add(next_sector_at, sector_tick, NULL, EV_CD_SECTOR);
}

/* ------------------------------------------------------------ commands */
static void command(uint8_t cmd) {
    uint64_t d1 = 25000;          /* first response delay */
    uint64_t d2 = PSX_CLOCK / 100;
    LOGV("cd: cmd %02x params %d [%02x %02x %02x]", cmd, nparam, param[0], param[1], param[2]);
    {   /* UJL_CD_STATS: which commands the game issues (e.g. how it reads the song position) */
        static int st = -1, cnt[32]; static uint64_t last;
        if (st < 0) st = getenv("UJL_CD_STATS") != NULL;
        if (st) {
            cnt[cmd & 31]++;
            if (g_now - last > PSX_CLOCK * 2) {
                last = g_now;
                char b[256]; int n = 0;
                for (int i = 0; i < 32; i++) if (cnt[i]) n += snprintf(b + n, sizeof b - (size_t)n, " %02x:%d", i, cnt[i]);
                rt_log("cd: commands in 2 s:%s", b);
                memset(cnt, 0, sizeof cnt);
            }
        }
    }
    switch (cmd) {
    case 0x01: ack_stat(3, d1); break;                                 /* Getstat */
    case 0x02: /* Setloc */
        setloc_lba = (uint32_t)((bcd2bin(param[0]) * 60 + bcd2bin(param[1])) * 75 + bcd2bin(param[2])) - 150;
        setloc_pending = 1;
        ack_stat(3, d1);
        break;
    case 0x03: /* Play (CD-DA): this disc has no audio tracks; acknowledge as playing */
        LOGI("cd: Play (track param %02x, n=%d)", nparam ? param[0] : 0, nparam);
        reading = 0; sched_cancel(EV_CD_SECTOR);
        if (setloc_pending) { read_lba = setloc_lba; setloc_pending = 0; }
        stat = (stat & ~0x60) | 0x80 | 0x02;
        ack_stat(3, d1);
        break;
    case 0x06: case 0x1B: /* ReadN / ReadS */
        if (mode & 0x40) g_load_turbo_until = 0; /* XA streaming (song, movie): real time from here */
        { static int st = -1; if (st < 0) st = getenv("UJL_CD_STATS") != NULL;
          extern uint32_t g_gpu_frame; if (st) rt_log("cd: read%s lba %u mode %02x at vblank %u host %llu ms", cmd == 6 ? "N" : "S", setloc_pending ? setloc_lba : read_lba, mode, g_gpu_frame, (unsigned long long)(SDL_GetTicksNS() / 1000000)); }
        ack_stat(3, d1);
        start_reading();
        break;
    case 0x07: stat |= 0x02; ack_stat(3, d1); ack_stat(2, d2); break; /* MotorOn */
    case 0x08: reading = 0; sched_cancel(EV_CD_SECTOR); stat = 0; ack_stat(3, d1); ack_stat(2, d2); stat = 0x00; break;
    case 0x09: /* Pause */
        ack_stat(3, d1);
        reading = 0;
        sched_cancel(EV_CD_SECTOR);
        stat &= ~0xE0;
        ack_stat(2, reading ? PSX_CLOCK / 15 : 7000);
        break;
    case 0x0A: /* Init */
        ack_stat(3, d1);
        mode = 0; reading = 0; sched_cancel(EV_CD_SECTOR);
        stat = 0x02;
        ack_stat(2, d2);
        break;
    case 0x0B: case 0x0C: ack_stat(3, d1); break;                      /* Mute / Demute */
    case 0x0D: filter_file = param[0]; filter_chan = param[1]; g_xa_filter_chan = param[1]; LOGI("cd: XA channel -> %u (file %u)", param[1], param[0]); ack_stat(3, d1); break;
    case 0x0E: mode = param[0]; ack_stat(3, d1); break;                /* Setmode */
    case 0x0F: { uint8_t r[5] = {stat, mode, 0, filter_file, filter_chan}; push_int(3, d1, 5, r); break; }
    case 0x10: { uint8_t r[8]; memcpy(r, (loc_latched && (mode & 0x40)) ? loc_latch : sector + 12, 8); push_int(3, d1, 8, r); /* GetlocL */
        { static int st = -1, n; if (st < 0) st = getenv("UJL_CD_STATS") != NULL;
          extern uint32_t g_gpu_frame;
          if (st && g_gpu_frame >= 5160 && n < 1300) { n++; rt_log("getlocl: vb %u lba %u", g_gpu_frame, read_lba - 1); } }
        break; }
    case 0x11: { /* GetlocP */
        uint32_t l = read_lba + 150, rel = read_lba;
        uint8_t r[8] = {1, 1, bin2bcd((uint8_t)(rel / 75 / 60)), bin2bcd((uint8_t)(rel / 75 % 60)), bin2bcd((uint8_t)(rel % 75)),
                        bin2bcd((uint8_t)(l / 75 / 60)), bin2bcd((uint8_t)(l / 75 % 60)), bin2bcd((uint8_t)(l % 75))};
        push_int(3, d1, 8, r);
        break;
    }
    case 0x13: { uint8_t r[3] = {stat, 1, 1}; push_int(3, d1, 3, r); break; } /* GetTN */
    case 0x14: { uint8_t r[3] = {stat, 0, 2}; push_int(3, d1, 3, r); break; } /* GetTD (approx) */
    case 0x15: case 0x16: /* SeekL / SeekP */
        ack_stat(3, d1);
        if (setloc_pending) { read_lba = setloc_lba; setloc_pending = 0; }
        reading = 0; sched_cancel(EV_CD_SECTOR);
        stat = (stat & ~0xE0) | 0x02;
        ack_stat(2, d2);
        break;
    case 0x19: /* Test */
        if (param[0] == 0x20) { uint8_t r[4] = {0x94, 0x09, 0x19, 0xC0}; push_int(3, d1, 4, r); }
        else ack_stat(3, d1);
        break;
    case 0x1A: { /* GetID: licensed data disc, region from SCEx */
        ack_stat(3, d1);
        uint8_t r[8] = {0x02, 0x00, 0x20, 0x00, 'S', 'C', 'E', 'I'};
        push_int(2, d2, 8, r);
        break;
    }
    case 0x1E: ack_stat(3, d1); ack_stat(2, PSX_CLOCK / 2); break;      /* ReadTOC */
    default:
        LOGI("cd: unhandled command %02x", cmd);
        { uint8_t r[2] = {(uint8_t)(stat | 1), 0x40}; push_int(5, d1, 2, r); }
    }
    nparam = 0;
}

uint8_t cd_read(uint32_t reg) {
    switch (reg) {
    case 0: {
        uint8_t s = idx;
        if (nparam == 0) s |= 0x08;
        if (nparam < 16) s |= 0x10;
        if (resp_pos < nresp) s |= 0x20;
        if (data_pos < data_len) s |= 0x40;
        return s;
    }
    case 1: return resp_pos < nresp ? resp[resp_pos++] : 0;
    case 2: return data_pos < data_len ? databuf[data_pos++] : 0;
    case 3: return (idx & 1) ? (uint8_t)(iflag | 0xE0) : (uint8_t)(ienable | 0xE0);
    }
    return 0;
}

void cd_write(uint32_t reg, uint8_t v) {
    if (reg == 0) { idx = v & 3; return; }
    switch (reg * 4 + idx) {
    case 1 * 4 + 0: command(v); break;
    case 2 * 4 + 0: if (nparam < 16) param[nparam++] = v; break;
    case 2 * 4 + 1: ienable = v & 0x1F; break;
    case 3 * 4 + 0: /* request */
        LOGV("cd: request %02x (data_len=%u)", v, data_len);
        /* BFRD 0->1 loads the sector into the data FIFO; re-writing 1 keeps the read position */
        if (v & 0x80) { if (!request_bfrd) data_pos = 0; }
        else data_pos = data_len;
        request_bfrd = (v >> 7) & 1;
        break;
    case 3 * 4 + 1: /* ack */
        LOGV("cd: ack %02x", v);
        iflag &= ~(v & 0x1F);
        if (v & 0x40) nparam = 0;
        if (!(iflag & 7)) { nresp = resp_pos = 0; try_deliver(); }
        break;
    default: break; /* volume / sound map registers */
    }
}

uint32_t cd_dma_read_word(void) {
    uint32_t w = 0;
    for (int i = 0; i < 4; i++) {
        uint32_t b = data_pos < data_len ? databuf[data_pos++] : 0;
        w |= b << (i * 8);
    }
    return w;
}
