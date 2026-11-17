/* Extend the original candy-bar grid using its own renderer, portraits and word sheets. */
#include "arcade_stage1.h"
#include "psx.h"

typedef struct { uint8_t column, row, mode, source, y_source; } StageBar;
static const StageBar added[] = {
    {0, 1, 2, 7, 7},    /* Lammy/Rammy co-op, below Lammy solo */
    {7, 0, 1, 13, 13},  /* Lammy/Rammy versus, on the top half */
    {11, 0, 3, 19, 19}, /* PaRappa solo */
    {11, 1, 5, 25, 25}, /* Lammy/PaRappa co-op */
    {18, 0, 4, 31, 31}, /* Lammy/PaRappa versus, on the top half */
};
enum { FIRST_BAR = 38, BAR_COUNT = 5 };
extern int g_unlock_all;
static bool drawing_added, tooltip_active, bars_submitted;
static uint32_t added_stack;

static void enable_bars(bool initialize) {
    uint32_t cells = mem_r32(0x801daf34);
    for (unsigned i = 0; i < BAR_COUNT; i++) {
        uint32_t id = FIRST_BAR + i;
        uint32_t cell = cells + (added[i].row * 22u + added[i].column) * 2u;
        mem_w8(cell, id);
        mem_w8(cell + 1, 0x80u | added[i].mode * 8u | 1u);
        /* Native status arrays have 48 slots, including padding beyond the original 38 entries. */
        mem_w8(0x801dea58 + id, 1);
        if (initialize) {
            mem_w8(0x801dea28 + id, 0);
            mem_w8(0x801de9f8 + id, 0);
        }
        /* "Unlock all" shows every native bar as cleared (clear flag = its availability value; COOL only on
           Lammy's solo stages). Mark the restored variants the same way. */
        if (g_unlock_all) mem_w8(0x801dea28 + id, 1);
    }
}
int arcade_stage1_menu_screen(CPU *c, int screen) {
    (void)c;
    if ((screen == 4 || screen == 12) && arcade_stage1_resources_ready()) enable_bars(true);
    return screen;
}

/* Insert between the native pointer and wrapper submissions in the same ordering bucket.
   Raising the bars to a separate bucket would incorrectly cover the selection pointer. */
static void draw_added_bars(CPU *c) {
    enable_bars(false);
    CPU before = *c;
    uint32_t frame = before.r[29];
    int scroll = (int32_t)before.r[19];
    int pitch = (int16_t)mem_r16(frame + 44) - (int16_t)mem_r16(frame + 40);
    drawing_added = true;
    for (unsigned i = 0; i < BAR_COUNT; i++) {
        const StageBar *bar = added + i;
        *c = before;
        c->r[29] -= 64;
        c->r[5] = bar->source;
        c->r[6] = mem_r32(0x801dea8c) == FIRST_BAR + i ? bar->source : UINT32_MAX;
        int x = i ? (int16_t)mem_r16(frame + 40 + bar->source * 4) - pitch : (int16_t)mem_r16(frame + 40);
        c->r[7] = (uint32_t)(x - scroll);
        mem_w32(c->r[29] + 16, mem_r16(frame + 42 + bar->y_source * 4));
        mem_w32(c->r[29] + 20, 1);
        mem_w32(c->r[29] + 24, mem_r8(0x801dea28 + FIRST_BAR + i));
        mem_w32(c->r[29] + 28, mem_r8(0x801de9f8 + FIRST_BAR + i));
        mem_w32(c->r[29] + 32, mem_r32(0x801dea88) & 1u);
        added_stack = c->r[29] - 528;
        call_indirect(c, 0x801d5048);
    }
    added_stack = 0;
    drawing_added = false;
    *c = before;
}

int filter_stage1_menu_move(CPU *c, uint32_t fn) {
    (void)fn;
    if (!arcade_stage1_resources_ready() || c->r[4] != 0x801daf30 || mem_r16(c->r[4]) || !(c->r[5] & 0x4000)) return 0;
    /* Stock code jumps diagonally around the absent Stage 1 co-op card. With that card
       restored, use the game's regular vertical navigation and its normal move sound. */
    c->r[6] = 5; c->r[7] = 0;
    call_indirect(c, 0x801c64e8);
    return 1;
}

void arcade_stage1_menu_sprite(CPU *c) {
    /* The first wrapper may be conditional on unlock progress; the final strip is unconditional. */
    uint32_t caller = c->r[31];
    if (!drawing_added && !bars_submitted && arcade_stage1_resources_ready() &&
        (caller == 0x801d8c98 || caller == 0x801d8d78 || caller == 0x801d8ea0 ||
         caller == 0x801d8fc8 || caller == 0x801d90bc)) {
        bars_submitted = true;
        draw_added_bars(c);
    }
    if (!drawing_added || !added_stack) return;
    /* Both native number sprites use consecutive 22-pixel digits; borrow Stage 2's art with digit 1. */
    if (c->r[5] == added_stack + 80 || c->r[5] == added_stack + 164)
        mem_w8(c->r[5] + 12, mem_r8(c->r[5] + 12) - 22u);
}

int filter_stage1_menu_tooltip(CPU *c, uint32_t fn) {
    if (!tooltip_active && c->r[31] == 0x801d8b08) bars_submitted = false;
    unsigned id = c->r[5];
    if (tooltip_active || !arcade_stage1_resources_ready() || id < FIRST_BAR || id >= FIRST_BAR + BAR_COUNT) return 0;
    CPU before = *c;
    unsigned source = added[id - FIRST_BAR].source + 1;
    uint32_t number = 0x801c5a38 + source * 3;
    uint8_t saved[3];
    for (unsigned k = 0; k < 3; k++) {
        saved[k] = mem_r8(number + k);
        mem_w8(number + k, mem_r8(0x801c5a38 + 3 + k));
    }
    tooltip_active = true;
    c->r[5] = source;
    call_indirect(c, fn);
    tooltip_active = false;
    for (unsigned k = 0; k < 3; k++) mem_w8(number + k, saved[k]);
    /* Match the original return convention, retaining the caller's argument register. */
    c->r[5] = before.r[5];
    return 1;
}
