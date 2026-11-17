/* Runtime-internal hardware state and service interfaces. */
#pragma once
#include "recomp.h"
#include <stdio.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------- logging ---------- */
extern int g_log_level;
void rt_log(const char *fmt, ...);
void rt_fatal(const char *fmt, ...);
#define LOGV(...) do { if (g_log_level >= 2) rt_log(__VA_ARGS__); } while (0)
#define LOGI(...) do { if (g_log_level >= 1) rt_log(__VA_ARGS__); } while (0)

/* ---------- time / scheduler ---------- */
/* Virtual time in PS1 CPU cycles (33.8688 MHz). Advanced by the frame pacer. */
#define PSX_CLOCK 33868800ull
extern uint64_t g_now;               /* current virtual cycle */
typedef void (*SchedFn)(void *ctx);
void sched_add(uint64_t at, SchedFn fn, void *ctx, int id);
void sched_cancel(int id);
void sched_run_due(void);
uint64_t sched_next(void);
void rt_idle(CPU *c);
enum { EV_VBLANK = 1, EV_CD_RESP, EV_CD_SECTOR, EV_DMA_DONE, EV_SPU, EV_TIMER0, EV_TIMER1, EV_TIMER2, EV_MDEC, EV_SIO, EV_MAX };

/* ---------- interrupts ---------- */
enum { IRQ_VBLANK = 0, IRQ_GPU, IRQ_CDROM, IRQ_DMA, IRQ_TMR0, IRQ_TMR1, IRQ_TMR2, IRQ_PAD, IRQ_SIO, IRQ_SPU, IRQ_LIGHTPEN };
extern uint32_t g_istat, g_imask;
void irq_raise(int n);
void irq_check(CPU *c);   /* deliver pending interrupt if enabled */
extern CPU *g_cpu;

/* ---------- DMA ---------- */
void dma_write(uint32_t off, uint32_t v);
uint32_t dma_read(uint32_t off);

/* ---------- timers ---------- */
uint32_t timer_read(uint32_t off);
void timer_write(uint32_t off, uint32_t v);
void timers_vblank(void);

/* ---------- GPU ---------- */
void gpu_gp0(uint32_t w);
void gpu_gp1(uint32_t w);
uint32_t gpu_read(void);
uint32_t gpu_stat(void);
void gpu_vblank(void);
extern uint16_t *g_vram; /* 1024x512 16bpp */

/* ---------- CD-ROM ---------- */
uint8_t cd_read(uint32_t reg);
void cd_write(uint32_t reg, uint8_t v);
uint32_t cd_dma_read_word(void);
bool disc_open(const char *path);
bool disc_read_sector(uint32_t lba, uint8_t out[2352]);
bool disc_find_file(const char *path, uint32_t *lba, uint32_t *size);

/* ---------- SPU ---------- */
uint16_t spu_read(uint32_t off);
void spu_write(uint32_t off, uint16_t v);
void spu_dma_write(uint32_t w);
uint32_t spu_dma_read(void);
void spu_xa_push(const int16_t *lr, int frames, int rate, int stereo);

/* ---------- MDEC ---------- */
void mdec_write(uint32_t reg, uint32_t v);
uint32_t mdec_read(uint32_t reg);
void mdec_dma_in(uint32_t w);
uint32_t mdec_dma_out(void);

/* ---------- pad / memory card (SIO0) ---------- */
uint32_t sio_read(uint32_t off, int size);
void sio_write(uint32_t off, uint32_t v, int size);
extern uint16_t g_pad_buttons[2]; /* active-low PS1 button bits */
extern int g_confirm_cross;       /* 1: swap Circle/Cross in menus (Western confirm) */
uint16_t rt_pad_state(int port); /* g_pad_buttons with menu remaps applied */
extern int g_pad2_connected;      /* set by the frontend: a player 2 input source is assigned to port 2 */
int rt_pad2_present(void);       /* bios.c: a pad in port 2 (player 2 connected, or Autoplay on) */

/* ---------- BIOS HLE ---------- */
void bios_init(CPU *c);
void bios_exception(CPU *c);   /* IRQ entry */
void bios_thread_boot(CPU *c); /* initial thread */
void bios_vblank(void);
void bios_deliver_event(CPU *c, uint32_t cls, uint32_t spec);
void clock_init(void);

/* ---------- GTE enhancements ---------- */
extern int32_t g_gte_xscale; /* 4096 = 1.0; <4096 squeezes X for wider aspect ratios */

/* ---------- host frame hook ---------- */
void host_vblank(void);        /* present + input + pacing; implemented by frontend */

#ifdef __cplusplus
}
#endif
