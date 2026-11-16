/* High-level emulation of the PS1 BIOS kernel services the game uses. */
#include "psx.h"
#include <setjmp.h>
#include <SDL3/SDL.h>

#define SR c->cop0[12]

/* ------------------------------------------------------------ IntRP queues */
static uint32_t intrp_head[4];
static uint32_t custom_exit;       /* guest address of jmp_buf-like struct, 0 = default */

static int rcnt_handlers;
static uint32_t rcnt_clear[4] = {1, 1, 1, 1};

/* exception nesting: ReturnFromException longjmps back here */
static jmp_buf *exc_jb;

static void call_guest(CPU *c, uint32_t fn, uint32_t a0, uint32_t ret_to) {
    c->r[4] = a0;
    c->r[31] = ret_to;
    call_indirect(c, fn);
}

static void exc_thread_switch(CPU *c);
void bios_exception(CPU *c) {
    CPU saved = *c;
    jmp_buf jb, *prev = exc_jb;
    int shadow_depth = g_shadow_n;
    exc_jb = &jb;
    /* push interrupt-enable stack, like the hardware does on exception entry */
    SR = (SR & ~0x3Fu) | ((SR << 2) & 0x3Fu);
    c->cop0[13] = 0; /* cause: interrupt */
    LOGV("exc: enter istat=%03x imask=%03x sp=%08x depth=%d", g_istat, g_imask, saved.r[29], g_shadow_n);
    if (!setjmp(jb)) {
        c->r[29] = 0x8000FF00u; /* kernel exception stack (below the exe) */
        /* queued memory card results (first: the IntRP handlers below may leave via ReturnFromException) */
        { extern void memcard_deliver(CPU *c); memcard_deliver(c); }
        for (int prio = 0; prio < 4; prio++) {
            uint32_t e = intrp_head[prio];
            int guard = 0;
            while (e && guard++ < 64) {
                uint32_t next = mem_r32(e), f2 = mem_r32(e + 4), f1 = mem_r32(e + 8);
                LOGV("exc: intrp prio %d entry %08x f1=%08x f2=%08x", prio, e, f1, f2);
                if (f1) {
                    call_guest(c, f1, 0, 0);
                    uint32_t r = c->r[2];
                    LOGV("exc:   f1 -> %08x", r);
                    if (r && f2) call_guest(c, f2, r, 0);
                }
                e = next;
            }
        }
        /* BIOS root-counter handlers (installed by C0:00): deliver RCnt events, ack if allowed */
        if (rcnt_handlers) {
            static const int bit[4] = {IRQ_TMR0, IRQ_TMR1, IRQ_TMR2, IRQ_VBLANK};
            for (int t = 0; t < 4; t++) {
                if (!(g_istat & g_imask & (1u << bit[t]))) continue;
                bios_deliver_event(c, 0xF2000000u | (uint32_t)t, 0x0002);
                if (rcnt_clear[t]) g_istat &= ~(1u << bit[t]);
            }
        }
        if (custom_exit) {
            uint32_t b = custom_exit;
            c->r[31] = mem_r32(b + 0);
            c->r[29] = mem_r32(b + 4);
            c->r[30] = mem_r32(b + 8);
            for (int i = 0; i < 8; i++) c->r[16 + i] = mem_r32(b + 12 + i * 4);
            c->r[28] = mem_r32(b + 44);
            c->r[2] = 1;
            LOGV("exc: custom exit -> %08x sp=%08x", c->r[31], c->r[29]);
            call_indirect(c, c->r[31]); /* continuation; normally ends in ReturnFromException */
            LOGV("exc: custom exit continuation returned");
        }
    }
    exc_jb = prev;
    LOGV("exc: leave sp=%08x -> %08x", c->r[29], saved.r[29]);
    g_shadow_n = shadow_depth;
    int budget = c->budget;
    *c = saved;
    c->budget = budget;
    /* The real BIOS resumes whichever thread PCB->current TCB points at when it returns from an exception.
       Games switch threads by writing that pointer (UJL's sound engine runs its tick in a thread it
       schedules this way), so honour it here. */
    exc_thread_switch(c);
}

/* ------------------------------------------------------------ events */
typedef struct { uint32_t cls, spec, mode, func, status; } Event;
#define MAX_EVENTS 32
static Event events[MAX_EVENTS];
enum { EV_FREE = 0, EV_DISABLED = 0x1000, EV_BUSY = 0x2000, EV_READY = 0x4000 };

void bios_debug_card_events(uint32_t spec) {
    for (int i = 0; i < MAX_EVENTS; i++)
        if ((events[i].cls & 0xFF000000u) == 0xF4000000u || events[i].cls == 0xF0000011u)
            LOGI("  event %d cls %08x spec %04x mode %04x func %08x status %04x (want spec %04x)", i, events[i].cls,
                 events[i].spec, events[i].mode, events[i].func, events[i].status, spec);
}
void bios_deliver_event(CPU *c, uint32_t cls, uint32_t spec) {
    for (int i = 0; i < MAX_EVENTS; i++) {
        Event *e = &events[i];
        if (e->status != EV_BUSY || e->cls != cls || e->spec != spec) continue;
        if (e->mode == 0x2000) e->status = EV_READY;
        else if (e->mode == 0x1000 && e->func) {
            CPU saved = *c;
            call_guest(c, e->func, 0, 0);
            int b = c->budget;
            *c = saved;
            c->budget = b;
        }
    }
}

/* ------------------------------------------------------------ threads */
/* Guest threads run on host threads; exactly one runs at a time (baton passing).
   Thread state is mirrored into the guest-visible TCB array (ToT[2] at 0x110), because
   PsyQ code writes fields there directly (e.g. a new thread's SR at TCB+0x94). */
#define MAX_THREADS 16
#define TCB_SIZE 0xC0
typedef struct {
    int used;
    uint32_t regs[32], hi, lo, sr;
    SDL_Semaphore *sem;
    SDL_Thread *th;
    uint32_t start_pc;
    int started;              /* host thread has entered guest code at least once */
    int restart;              /* slot reopened while host thread was parked: re-enter at top */
    jmp_buf top;
    uint32_t snap[24]; int nsnap; uint32_t snap_pc; /* debug: shadow stack when switched out */
} GThread;
static GThread threads[MAX_THREADS];
int g_cur_thread;

static uint32_t tcb_addr(int i) {
    uint32_t base = mem_r32(0x110), size = mem_r32(0x114);
    if (!base || (uint32_t)(i + 1) * TCB_SIZE > size) return 0;
    return base + (uint32_t)i * TCB_SIZE;
}
static int tcb_count(void) {
    uint32_t base = mem_r32(0x110), size = mem_r32(0x114);
    int n = base ? (int)(size / TCB_SIZE) : 4;
    return n > MAX_THREADS ? MAX_THREADS : n;
}

static void save_regs(CPU *c, int i) {
    GThread *t = &threads[i];
    memcpy(t->regs, c->r, sizeof t->regs);
    t->hi = c->hi; t->lo = c->lo; t->sr = SR;
    uint32_t tcb = tcb_addr(i);
    if (tcb) {
        for (int r = 0; r < 32; r++) mem_w32(tcb + 8 + r * 4, c->r[r]);
        mem_w32(tcb + 0x8C, c->hi);
        mem_w32(tcb + 0x90, c->lo);
        mem_w32(tcb + 0x94, (SR & ~0x3Fu) | ((SR << 2) & 0x3Fu)); /* as an exception would save it */
    }
}
static void load_regs(CPU *c, int i) {
    GThread *t = &threads[i];
    memcpy(c->r, t->regs, sizeof t->regs);
    c->hi = t->hi; c->lo = t->lo; SR = t->sr;
    uint32_t tcb = tcb_addr(i);
    if (tcb) {
        uint32_t sr = mem_r32(tcb + 0x94);
        SR = (sr & ~0xFu) | ((sr >> 2) & 0xFu); /* rfe */
        mem_w32(mem_r32(0x108), tcb);          /* PCB->current TCB */
    }
}

/* Host threads that run recompiled code get an explicit stack: platform defaults differ (macOS gives secondary
   threads only 512 KB, Windows 1 MB, Linux 8 MB) and deep guest call chains recurse on the host stack. */
#define RT_THREAD_STACK (8u << 20)
SDL_Thread *rt_create_thread(SDL_ThreadFunction fn, const char *name, void *data) {
    SDL_PropertiesID p = SDL_CreateProperties();
    SDL_SetPointerProperty(p, SDL_PROP_THREAD_CREATE_ENTRY_FUNCTION_POINTER, (void *)fn);
    SDL_SetStringProperty(p, SDL_PROP_THREAD_CREATE_NAME_STRING, name);
    SDL_SetPointerProperty(p, SDL_PROP_THREAD_CREATE_USERDATA_POINTER, data);
    SDL_SetNumberProperty(p, SDL_PROP_THREAD_CREATE_STACKSIZE_NUMBER, RT_THREAD_STACK);
    SDL_Thread *t = SDL_CreateThreadWithProperties(p);
    SDL_DestroyProperties(p);
    return t;
}

static int SDLCALL thread_main(void *arg) {
    GThread *t = (GThread *)arg;
    if (setjmp(t->top)) t->restart = 0; /* restarted: we were already signalled */
    else SDL_WaitSemaphore(t->sem);       /* first start: wait for ChangeThread to us */
    t->started = 1;
    g_shadow_n = 0;
    CPU *c = g_cpu;
    call_indirect(c, t->start_pc);
    rt_fatal("guest thread %d returned from its entry %08x", (int)(t - threads), t->start_pc);
    return 0;
}

static uint32_t open_thread(CPU *c, uint32_t pc, uint32_t sp, uint32_t gp) {
    int n = tcb_count();
    for (int i = 1; i < n; i++) {
        if (threads[i].used) continue;
        GThread *t = &threads[i];
        memset(t->regs, 0, sizeof t->regs);
        t->used = 1;
        t->start_pc = pc;
        t->regs[29] = t->regs[30] = sp;
        t->regs[28] = gp;
        t->hi = t->lo = 0;
        t->sr = SR | 0x401;
        uint32_t tcb = tcb_addr(i);
        if (tcb) {
            mem_w32(tcb + 0, 0x4000);
            for (int r = 0; r < 32; r++) mem_w32(tcb + 8 + r * 4, t->regs[r]);
            mem_w32(tcb + 0x88, pc);
            mem_w32(tcb + 0x94, (SR & ~0x3Fu) | 0x404);
        }
        if (!t->sem) t->sem = SDL_CreateSemaphore(0);
        if (!t->th) t->th = rt_create_thread(thread_main, "guest", t);
        else if (t->started) t->restart = 1; /* parked inside old guest code: re-enter at top */
        LOGV("bios: OpenThread %d pc=%08x sp=%08x", i, pc, sp);
        return 0xFF000000u | (uint32_t)i;
    }
    LOGI("bios: OpenThread failed: all %d TCBs in use", n);
    return 0xFFFFFFFFu;
}

static void close_thread(uint32_t h) {
    int i = (int)(h & 0xFFFF);
    if (i <= 0 || i >= MAX_THREADS) return;
    threads[i].used = 0;
    uint32_t tcb = tcb_addr(i);
    if (tcb) mem_w32(tcb, 0x1000);
}

static void change_thread(CPU *c, uint32_t h) {
    int n = (int)(h & 0xFFFF);
    if (n < 0 || n >= MAX_THREADS || !threads[n].used) { c->r[2] = 0; return; }
    if (n == g_cur_thread) { c->r[2] = 1; return; }
    int mi = g_cur_thread;
    GThread *me = &threads[mi], *to = &threads[n];
    c->r[2] = 1;
    me->nsnap = g_shadow_n < 24 ? g_shadow_n : 24;
    for (int i = 0; i < me->nsnap; i++) me->snap[i] = g_shadow[g_shadow_n - 1 - i];
    me->snap_pc = c->pc;
    save_regs(c, mi);
    load_regs(c, n);
    g_cur_thread = n;
    SDL_SignalSemaphore(to->sem);
    SDL_WaitSemaphore(me->sem);
    if (me->restart) longjmp(me->top, 1); /* our slot was closed and reopened with a new entry */
    c->r[2] = 1;
}

/* switch to the thread whose TCB the guest made current, keeping every register of the interrupted code */
static void exc_thread_switch(CPU *c) {
    uint32_t pcb = mem_r32(0x108);
    if (!pcb) return;
    uint32_t cur = mem_r32(pcb), mine = tcb_addr(g_cur_thread);
    if (!cur || cur == mine) return;
    int n = -1;
    for (int i = 0; i < tcb_count(); i++)
        if (tcb_addr(i) == cur) { n = i; break; }
    if (n < 0 || !threads[n].used) { mem_w32(pcb, mine); return; } /* not one of ours: ignore */
    int mi = g_cur_thread;
    GThread *me = &threads[mi], *to = &threads[n];
    me->nsnap = g_shadow_n < 24 ? g_shadow_n : 24;
    for (int i = 0; i < me->nsnap; i++) me->snap[i] = g_shadow[g_shadow_n - 1 - i];
    me->snap_pc = c->pc;
    save_regs(c, mi);
    load_regs(c, n);
    g_cur_thread = n;
    SDL_SignalSemaphore(to->sem);
    SDL_WaitSemaphore(me->sem);
    if (me->restart) longjmp(me->top, 1);
}

void bios_thread_boot(CPU *c) {
    (void)c;
    threads[0].used = 1;
    threads[0].started = 1;
    threads[0].sem = SDL_CreateSemaphore(0);
    g_cur_thread = 0;
}

/* ------------------------------------------------------------ pads (BIOS driver) */
static uint32_t pad_buf[2], pad_len[2];
static int pad_started;
uint16_t g_pad_buttons[2] = {0xFFFF, 0xFFFF};
int g_confirm_cross = 0;
const char *rt_current_overlay_name(void);

/* The JP release confirms with Circle. With g_confirm_cross, Circle and Cross are swapped in menus
   (a COMOD*.MEN overlay is loaded) and while an in-stage prompt is on screen (button_swap.c saw its
   icons within the last few frames). In songs both are note buttons and stay put. When the swap ends,
   whichever of the two is still held is masked until released, so a held answer can't turn into a
   note press. */
extern uint32_t g_prompt_frame, g_gpu_frame;
/* Port 2 has a pad while Autoplay is on, so 2P versus/co-op can be chosen and Autoplay plays player 2 there. */
int rt_pad2_present(void) { extern int g_autoplay; return g_autoplay != 0; }

uint16_t rt_pad_state(int port) {
    static int was_swapped[2];
    static uint16_t mask[2]; /* active-high bits forced released */
    port &= 1;
    uint16_t b = g_pad_buttons[port];
    int swap = 0;
    if (g_confirm_cross) {
        const char *ov = rt_current_overlay_name();
        size_t n = ov ? strlen(ov) : 0;
        swap = (n >= 4 && !strcmp(ov + n - 4, ".MEN")) || g_gpu_frame - g_prompt_frame <= 4;
    }
    if (was_swapped[port] && !swap) mask[port] = (uint16_t)(~b & 0x6000u);
    was_swapped[port] = swap;
    mask[port] &= (uint16_t)~b;          /* released buttons leave the mask */
    if (swap) {
        uint16_t circle = (b >> 13) & 1, cross = (b >> 14) & 1;
        b = (uint16_t)((b & ~0x6000u) | cross << 13 | circle << 14);
    }
    b = (uint16_t)(b | mask[port]);
    { uint16_t autoplay_buttons(int port); b &= (uint16_t)~autoplay_buttons(port); }   /* autoplay.c */
    return b;
}

static void pad_vblank_update(void) {
    if (!pad_started) return;
    for (int p = 0; p < 2; p++) {
        if (!pad_buf[p]) continue;
        uint32_t b = pad_buf[p];
        if (p == 0 || rt_pad2_present()) {
            mem_w8(b + 0, 0x00);       /* ok */
            mem_w8(b + 1, 0x41);       /* digital pad, 1 halfword */
            mem_w8(b + 2, rt_pad_state(p) & 0xFF);
            mem_w8(b + 3, rt_pad_state(p) >> 8);
        } else {
            mem_w8(b + 0, 0xFF);       /* no controller */
        }
    }
}

/* ------------------------------------------------------------ memory card (memcard.c) */
void memcard_info(CPU *c, uint32_t port);
void memcard_sector(CPU *c, int write, uint32_t port, uint32_t sector, uint32_t buf);
int memcard_open(uint32_t name, uint32_t mode);
int memcard_is_fd(uint32_t fd);
int memcard_rw(CPU *c, int write, uint32_t fd, uint32_t buf, uint32_t len);
int memcard_seek(uint32_t fd, int32_t off, int whence);
int memcard_close(uint32_t fd);
uint32_t memcard_firstfile(uint32_t pattern, uint32_t dirent);
uint32_t memcard_nextfile(uint32_t dirent);
int memcard_erase(uint32_t name);
int memcard_rename(uint32_t a, uint32_t b);
/* UJL_CARD_LOG=1: log the game's card file calls with the caller (ra) */
static int card_log = -1;
#define CARD_LOG(fmt, ...) do { if (card_log < 0) card_log = getenv("UJL_CARD_LOG") != NULL; \
    if (card_log) rt_log("card: " fmt "  ra=%08x", __VA_ARGS__, c->r[31]); } while (0)

/* ------------------------------------------------------------ dispatch */
static int unk_bios;

static void bios_a0(CPU *c, uint32_t fn) {
    { static int n; if ((fn == 0xAB || fn == 0xAC || fn == 0x70 || fn == 0x71) && n++ < 200)
        LOGI("bios: card A0:%02x a0=%08x ra=%08x", fn, c->r[4], c->r[31]); }
    switch (fn) {
    case 0x39: break; /* InitHeap */
    case 0x44: break; /* FlushCache */
    case 0x49: gpu_gp0(c->r[4]); break; /* GPU_cw */
    case 0x70: break; /* _bu_init */
    case 0x72: break; /* CdRemove */
    case 0xAB: /* _card_info(port) */
    case 0xAC: /* _card_load(port) */
        memcard_info(c, c->r[4]);
        break;
    default:
        if (unk_bios++ < 64) LOGI("bios: unhandled A0:%02x", fn);
        c->r[2] = 0;
    }
}

/* Fake kernel area: PsyQ's _patch_* routines look up BIOS code via the A0/B0/C0 tables and
   patch instructions in it. With an HLE BIOS that code doesn't exist, so the tables point
   into zero-filled dummy RAM where those patches land harmlessly. */
#define KTAB_A0 0x00000200u
#define KTAB_B0 0x00000874u
#define KTAB_C0 0x00000674u
#define KDUMMY  0x00001000u  /* 0x1000..0x7FFF dummy kernel code */
static uint32_t kheap = 0x00008000u; /* alloc_kernel_memory, up to 0xE000 */

/* Device table (ToT entry 10: 0x150 = DCB array, 0x154 = size). PsyQ's memory card library walks it to find
   the "bu" device and hooks its firstfile entry (DCB+0x34); the HLE BIOS serves bu00: itself, so the entry
   only needs to exist. One 0x50-byte DCB; its function pointers point at harmless dummy kernel RAM. */
#define KDCB 0x00007000u
static void device_table_init(void) {
    const char *name = "bu";
    for (int i = 0; name[i] || i == 2; i++) mem_w8(KDCB + 0x100 + i, (uint8_t)name[i]);
    mem_w8(KDCB + 0x102, 0);
    mem_w32(KDCB + 0x00, KDCB + 0x100);   /* name */
    mem_w32(KDCB + 0x04, 0x14);           /* flags: block device */
    mem_w32(KDCB + 0x08, 0x80);           /* sector size */
    mem_w32(KDCB + 0x0C, KDCB + 0x100);   /* description */
    for (int f = 0x10; f < 0x50; f += 4) mem_w32(KDCB + f, KDCB + 0x200 + (uint32_t)f);
    mem_w32(0x150, KDCB);
    mem_w32(0x154, 0x50);
}

static void kernel_tables_init(void) {
    memset(g_ram, 0, 0x10000);
    device_table_init();
    for (int i = 0; i < 0xC0; i++) mem_w32(KTAB_A0 + i * 4, KDUMMY + 0x0000 + i * 0x20);
    for (int i = 0; i < 0x60; i++) mem_w32(KTAB_B0 + i * 4, KDUMMY + 0x1800 + i * 0x40);
    for (int i = 0; i < 0x20; i++) mem_w32(KTAB_C0 + i * 4, KDUMMY + 0x3000 + i * 0x80);
}

static void bios_b0(CPU *c, uint32_t fn) {
    uint32_t a0 = c->r[4], a1 = c->r[5], a2 = c->r[6], a3 = c->r[7];
    { static int n; if (((fn >= 0x32 && fn <= 0x45 && !((fn == 0x35 || fn == 0x34) && a0 < 2)) || (fn >= 0x4A && fn <= 0x5C && fn != 0x5B && fn != 0x56 && fn != 0x57)) && n++ < 300)
        LOGI("bios: card B0:%02x a0=%08x a1=%08x a2=%08x ra=%08x", fn, a0, a1, a2, c->r[31]); }
    switch (fn) {
    case 0x00: { /* alloc_kernel_memory(size) */
        uint32_t sz = (a0 + 3) & ~3u;
        if (kheap + sz > 0xE000) { c->r[2] = 0; break; }
        c->r[2] = 0xA0000000u | kheap;
        kheap += sz;
        break;
    }
    case 0x01: c->r[2] = 0; break; /* free_kernel_memory */
    case 0x56: c->r[2] = KTAB_C0; break; /* GetC0Table */
    case 0x57: c->r[2] = KTAB_B0; break; /* GetB0Table */
    case 0x07: bios_deliver_event(c, a0, a1); break;
    case 0x08: { /* OpenEvent(class, spec, mode, func) */
        c->r[2] = 0xFFFFFFFF;
        for (int i = 0; i < MAX_EVENTS; i++)
            if (events[i].status == EV_FREE) {
                events[i] = (Event){a0, a1, a2, a3, EV_DISABLED};
                c->r[2] = 0xF1000000u | (uint32_t)i;
                break;
            }
        LOGV("bios: OpenEvent %08x %04x mode %04x func %08x -> %08x", a0, a1, a2, a3, c->r[2]);
        break;
    }
    case 0x09: events[a0 & 0xFF].status = EV_FREE; c->r[2] = 1; break;
    case 0x0A: { /* WaitEvent */
        Event *e = &events[a0 & 0xFF];
        int guard = 0;
        while (e->status != EV_READY && e->status == EV_BUSY && guard++ < 10000000) rt_safepoint(c);
        if (e->status == EV_READY) e->status = EV_BUSY;
        c->r[2] = 1;
        break;
    }
    case 0x0B: { /* TestEvent */
        Event *e = &events[a0 & 0xFF];
        if (e->status == EV_READY) { e->status = EV_BUSY; c->r[2] = 1; }
        else c->r[2] = 0;
        break;
    }
    case 0x0C: if (events[a0 & 0xFF].status != EV_FREE) events[a0 & 0xFF].status = EV_BUSY; c->r[2] = 1; break;
    case 0x0D: if (events[a0 & 0xFF].status != EV_FREE) events[a0 & 0xFF].status = EV_DISABLED; c->r[2] = 1; break;
    case 0x0E: c->r[2] = open_thread(c, a0, a1, a2); break;
    case 0x0F: close_thread(a0); c->r[2] = 1; break;
    case 0x10: change_thread(c, a0); break;
    case 0x12: /* InitPad(buf1, siz1, buf2, siz2) */
        pad_buf[0] = a0; pad_len[0] = a1; pad_buf[1] = a2; pad_len[1] = a3;
        c->r[2] = 2;
        break;
    case 0x13: pad_started = 1; SR |= 0x401; c->r[2] = 1; break; /* StartPad (also enables IRQs) */
    case 0x14: pad_started = 0; break;                           /* StopPad */
    case 0x17: /* ReturnFromException */
        if (exc_jb) longjmp(*exc_jb, 1);
        LOGI("bios: ReturnFromException outside exception");
        break;
    case 0x18: custom_exit = 0; break;
    case 0x19: custom_exit = a0; LOGV("bios: SetCustomExitFromException %08x", a0); break;
    case 0x32: c->r[2] = (uint32_t)memcard_open(a0, a1); LOGI("bios: open(\"%s\", %x) -> %d", (char *)ram_ptr(a0), a1, (int)c->r[2]); break;
    case 0x33: c->r[2] = (uint32_t)memcard_seek(a0, (int32_t)a1, (int)a2); CARD_LOG("lseek(%d, %d, %d) -> %d", a0, a1, a2, c->r[2]); break;
    case 0x34: c->r[2] = (uint32_t)memcard_rw(c, 0, a0, a1, a2); CARD_LOG("read(%d, %08x, %x) -> %d", a0, a1, a2, c->r[2]); break;
    case 0x35: /* write: memory card file, or the game's debug text on stdout */
        if (memcard_is_fd(a0)) { c->r[2] = (uint32_t)memcard_rw(c, 1, a0, a1, a2); CARD_LOG("write(%d, %08x, %x) -> %d", a0, a1, a2, c->r[2]); }
        else c->r[2] = a2;
        break;
    case 0x36: c->r[2] = (uint32_t)memcard_close(a0); CARD_LOG("close(%d)", a0); break;
    case 0x42: c->r[2] = memcard_firstfile(a0, a1); CARD_LOG("firstfile(\"%s\") -> %s", (char *)ram_ptr(a0), c->r[2] ? (char *)ram_ptr(a1) : "-"); break;
    case 0x43: c->r[2] = memcard_nextfile(a0); CARD_LOG("nextfile -> %s", c->r[2] ? (char *)ram_ptr(a0) : "-"); break;
    case 0x44: c->r[2] = (uint32_t)memcard_rename(a0, a1); CARD_LOG("rename(\"%s\", \"%s\") -> %d", (char *)ram_ptr(a0), (char *)ram_ptr(a1), c->r[2]); break;
    case 0x45: c->r[2] = (uint32_t)memcard_erase(a0); CARD_LOG("erase(\"%s\") -> %d", (char *)ram_ptr(a0), c->r[2]); break;
    case 0x4A: case 0x4B: case 0x4C: case 0x50: c->r[2] = 1; break;
    case 0x4E: memcard_sector(c, 1, a0, a1, a2); break; /* write_card_sector */
    case 0x4F: memcard_sector(c, 0, a0, a1, a2); break; /* read_card_sector */
    case 0x5B: break;                         /* ChangeClearPad */
    case 0x5C: c->r[2] = 1; break;            /* get_card_status: ready */
    default:
        if (unk_bios++ < 64) LOGI("bios: unhandled B0:%02x (a0=%08x a1=%08x)", fn, a0, a1);
        c->r[2] = 0;
    }
}

static void bios_c0(CPU *c, uint32_t fn) {
    uint32_t a0 = c->r[4], a1 = c->r[5];
    switch (fn) {
    case 0x02: /* SysEnqIntRP(prio, struct) */
        mem_w32(a1, intrp_head[a0 & 3]);
        intrp_head[a0 & 3] = a1;
        c->r[2] = 1;
        break;
    case 0x03: { /* SysDeqIntRP */
        uint32_t *pp = &intrp_head[a0 & 3];
        if (*pp == a1) *pp = mem_r32(a1);
        else {
            uint32_t e = *pp;
            while (e && mem_r32(e) != a1) e = mem_r32(e);
            if (e) mem_w32(e, mem_r32(a1));
        }
        c->r[2] = 1;
        break;
    }
    case 0x00: rcnt_handlers = 1; c->r[2] = 1; break; /* EnqueueTimerAndVblankIrqs */
    case 0x01: c->r[2] = 1; break;                    /* EnqueueSyscallHandler */
    case 0x08: c->r[2] = 0; break;                    /* SysInitMemory */
    case 0x0A: c->r[2] = rcnt_clear[a0 & 3]; rcnt_clear[a0 & 3] = a1; break; /* ChangeClearRCnt */
    case 0x0C: c->r[2] = 1; break;                    /* InitDefInt */
    default:
        if (unk_bios++ < 64) LOGI("bios: unhandled C0:%02x", fn);
        c->r[2] = 0;
    }
}

void bios_call(CPU *c, uint32_t vector) {
    uint32_t fn = c->r[9];
    LOGV("bios %02x:%02x a0=%08x a1=%08x ra=%08x", vector, fn, c->r[4], c->r[5], c->r[31]);
    if (vector == 0xA0) bios_a0(c, fn);
    else if (vector == 0xB0) bios_b0(c, fn);
    else bios_c0(c, fn);
}

void bios_vblank(void) { pad_vblank_update(); }

void bios_dump_threads(void) {
    for (int i = 0; i < MAX_THREADS; i++) {
        GThread *t = &threads[i];
        if (!t->used || i == g_cur_thread || !t->th) continue;
        char line[512]; int n = 0;
        for (int k = 0; k < t->nsnap && n < 480; k++) n += snprintf(line + n, sizeof line - (size_t)n, " %08x", t->snap[k]);
        rt_log("  thread %d (entry %08x) parked at pc~%08x:%s", i, t->start_pc, t->snap_pc, line);
    }
}

void bios_init(CPU *c) {
    kernel_tables_init();
    memset(intrp_head, 0, sizeof intrp_head);
    custom_exit = 0;
    bios_thread_boot(c);
}
