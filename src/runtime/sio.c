/* SIO0: controller + memory card serial port, emulated at register level
   (the game's pad library talks to it directly from the vblank handler). */
#include "psx.h"
#include <stdlib.h>

static uint16_t mode, ctrl, baud;
static uint8_t rx;
static int rx_full;
static int ack_low;          /* /ACK line asserted by device */
static int irq_flag;

/* device state per transfer */
enum { DEV_NONE, DEV_PAD, DEV_CARD };
static int dev, step;
int g_sio_pad_polls;

/* ------------------------------------------------------------ memory card */
#define CARD_SIZE (128 * 1024)
uint8_t g_card[2][CARD_SIZE];
int g_card_present[2] = {1, 0};
int g_card_dirty[2];
static uint8_t card_flag = 0x08; /* "new card / not yet written" flag */
static int card_cmd;
static uint16_t card_sector;
static uint8_t card_chk;
static int card_slot;

static uint8_t card_byte(uint8_t in, int *ack) {
    uint8_t *mc = g_card[card_slot];
    *ack = 1;
    if (step == 0) { step++; return 0xFF; }                 /* 0x81 address byte */
    if (step == 1) { card_cmd = in; step++; return card_flag; }
    switch (card_cmd) {
    case 'R':
        switch (step++) {
        case 2: return 0x5A;
        case 3: return 0x5D;
        case 4: card_sector = (uint16_t)(in << 8); return 0x00;
        case 5: card_sector |= in; card_sector &= 0x3FF; card_chk = (uint8_t)((card_sector >> 8) ^ (card_sector & 0xFF)); { static int n; if (n++ < 40) LOGI("sio card: %c sector %u", card_cmd, card_sector); } return in;
        case 6: return 0x5C;
        case 7: return 0x5D;
        case 8: return (uint8_t)(card_sector >> 8);
        case 9: return (uint8_t)card_sector;
        default:
            if (step - 1 >= 10 && step - 1 < 10 + 128) {
                uint8_t b = mc[card_sector * 128 + (step - 1 - 10)];
                card_chk ^= b;
                return b;
            }
            if (step - 1 == 10 + 128) return card_chk;
            *ack = 0;
            return 0x47; /* 'G' good */
        }
    case 'W':
        switch (step++) {
        case 2: return 0x5A;
        case 3: return 0x5D;
        case 4: card_sector = (uint16_t)(in << 8); return 0x00;
        case 5: card_sector |= in; card_sector &= 0x3FF; card_chk = (uint8_t)((card_sector >> 8) ^ (card_sector & 0xFF)); { static int n; if (n++ < 40) LOGI("sio card: %c sector %u", card_cmd, card_sector); } return in;
        default:
            if (step - 1 >= 6 && step - 1 < 6 + 128) {
                mc[card_sector * 128 + (step - 1 - 6)] = in;
                card_chk ^= in;
                return in;
            }
            if (step - 1 == 6 + 128) return 0x00;                /* checksum in */
            if (step - 1 == 7 + 128) return 0x5C;
            if (step - 1 == 8 + 128) return 0x5D;
            card_flag &= ~0x08;
            g_card_dirty[card_slot] = 1;
            *ack = 0;
            return 0x47;
        }
    case 'S': { /* get ID */
        static const uint8_t id[] = {0x5A, 0x5D, 0x5C, 0x5D, 0x04, 0x00, 0x00, 0x80};
        int k = step++ - 2;
        if (k >= 7) *ack = 0;
        return k < 8 ? id[k] : 0xFF;
    }
    default:
        *ack = 0;
        return 0xFF;
    }
}

/* ------------------------------------------------------------ digital pad */
static uint8_t pad_byte(int port, uint8_t in, int *ack) {
    (void)in;
    uint16_t b = rt_pad_state(port);
    *ack = 1;
    switch (step++) {
    case 0: return 0xFF;
    case 1: return 0x41;     /* digital pad id */
    case 2: return 0x5A;
    case 3: return (uint8_t)(b & 0xFF);
    default: *ack = 0; return (uint8_t)(b >> 8);
    }
}

static int ack_pending;
static void ack_irq(void *ctx) {
    (void)ctx;
    if (!ack_pending) return;
    ack_pending = 0;
    if (ctrl & 0x1000) {
        irq_flag = 1;
        irq_raise(IRQ_PAD);
    }
}

void sio_poll_ack(void) { if (ack_pending) ack_irq(NULL); }

static void tx(uint8_t v) {
    int port = (ctrl >> 13) & 1;
    int ack = 0;
    uint8_t r = 0xFF;
    if (!(ctrl & 2)) { rx = 0xFF; rx_full = 1; return; } /* /JOY not selected */
    if (dev == DEV_NONE) {
        step = 0;
        if (v == 0x01 && (port == 0 || rt_pad2_present())) { dev = DEV_PAD; g_sio_pad_polls++; }
        else if (v == 0x81 && g_card_present[port]) { dev = DEV_CARD; card_slot = port; }
    }
    if (dev == DEV_PAD) r = pad_byte(port, v, &ack);
    else if (dev == DEV_CARD) r = card_byte(v, &ack);
    rx = r;
    rx_full = 1;
    if (ack) {
        /* Real devices pulse /ACK within a few microseconds. Games poll for it with short
           timeouts that can expire before the next scheduler tick, so raise it right away. */
        /* the driver clears I_STAT bit 7 right after writing the byte, then polls for the ack,
           so assert it on the next I_STAT read (or after ~20us of emulated time as a fallback) */
        ack_low = 1;
        ack_pending = 1;
        sched_add(g_now + 600, ack_irq, NULL, EV_SIO);
    } else {
        ack_low = 0;
        dev = DEV_NONE;
    }
}

int g_sio_trace;
uint32_t sio_read_impl(uint32_t off);
uint32_t sio_read(uint32_t off, int size) {
    (void)size;
    uint32_t v = sio_read_impl(off);
    if (g_sio_trace > 0) { g_sio_trace--; rt_log("sio:  read +%x = %x", off, v); }
    return v;
}
uint32_t sio_read_impl(uint32_t off) {
    switch (off) {
    case 0x0: { uint8_t v = rx_full ? rx : 0xFF; rx_full = 0; return v; }
    case 0x4: {
        uint32_t s = 0x1 | 0x4;                 /* TX ready, TX finished */
        if (rx_full) s |= 0x2;
        /* /ACK is a ~2us pulse on real hardware; by the time software polls it has
           ended, so the line always reads as released (bit7 = 0). */
        if (irq_flag) s |= 0x200;
        return s;
    }
    case 0x8: return mode;
    case 0xA: return ctrl;
    case 0xE: return baud;
    }
    return 0;
}

void sio_write(uint32_t off, uint32_t v, int size) {
    (void)size;
    if (g_sio_trace > 0) { g_sio_trace--; rt_log("sio:  write +%x = %x", off, v); }
    switch (off) {
    case 0x0: tx((uint8_t)v); break;
    case 0x8: mode = (uint16_t)v; break;
    case 0xA:
        ctrl = (uint16_t)v;
        if (v & 0x10) { irq_flag = 0; }          /* acknowledge */
        if (v & 0x40) { ctrl = 0; dev = DEV_NONE; rx_full = 0; irq_flag = 0; ack_low = 0; ack_pending = 0; } /* reset */
        if (!(v & 2)) { dev = DEV_NONE; ack_low = 0; ack_pending = 0; } /* deselect ends transfer */
        break;
    case 0xE: baud = (uint16_t)v; break;
    }
}
