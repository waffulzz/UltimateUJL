/* "Confirm with Cross" for in-stage prompts.
   The JP release's in-stage prompt ("AGAIN?", shown when a song is failed or paused) answers with
   Circle = yes, Cross = no, and draws the two button icons as sprites. With g_confirm_cross the icons
   trade art (so the prompt reads Cross = yes, Circle = no, like the US release) and rt_pad_state()
   swaps Circle/Cross while a prompt is on screen. */
#include "psx.h"
#include "gpu.h"

extern uint32_t g_gpu_frame;
uint32_t g_prompt_frame = 0xFFFFFF00u; /* last frame a prompt button icon was drawn */

typedef struct { uint16_t texpage, clut; uint8_t u, v; int16_t w, h; } Icon;
/* pairs: [0] Circle, [1] Cross (texpage 896,256 4bpp; CLUTs 832,284 / 832,285) */
static const Icon k_pairs[][2] = {
    {{542, (284 << 6) | (832 / 16), 92, 125, 32, 29}, {542, (285 << 6) | (832 / 16), 124, 125, 32, 29}},
};

static int icon_eq(const GpuPrim *p, const Icon *ic) {
    return p->type == PRIM_RECT && p->textured && (p->texpage & 0x1FF) == (ic->texpage & 0x1FF) && p->clut == ic->clut &&
           p->v[0].u == ic->u && p->v[0].v == ic->v && p->w == ic->w && p->h == ic->h;
}

/* Returns 1 and fills *out when p is a prompt icon whose art should be swapped. */
int button_swap_filter(const GpuPrim *p, GpuPrim *out) {
    if (!g_confirm_cross || p->type != PRIM_RECT) return 0;
    for (size_t i = 0; i < sizeof k_pairs / sizeof k_pairs[0]; i++)
        for (int k = 0; k < 2; k++)
            if (icon_eq(p, &k_pairs[i][k])) {
                const Icon *o = &k_pairs[i][k ^ 1];
                g_prompt_frame = g_gpu_frame;
                *out = *p;
                out->clut = o->clut;
                out->v[0].u = o->u;
                out->v[0].v = o->v;
                out->w = o->w;
                out->h = o->h;
                return 1;
            }
    return 0;
}
