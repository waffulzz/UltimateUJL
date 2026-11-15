/* Decoded GPU primitives handed to the renderer backend. */
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int16_t x, y;          /* screen coords after draw offset */
    uint8_t r, g, b;
    uint8_t u, v;
    int16_t fx, fy;        /* precise position minus x/y (1/256 px) from the GTE's projection; 0 for 2D */
} GpuVertex;

enum { PRIM_TRI = 0, PRIM_LINE, PRIM_RECT, PRIM_FILL };

typedef struct {
    int type;
    int nverts;            /* 3 for tri (quads are split), 2 for line, 1 for rect */
    GpuVertex v[3];
    int16_t w, h;          /* rect size */
    uint8_t textured, raw, semi, gouraud;
    uint16_t clut;         /* CLUT word (x/16, y) */
    uint16_t texpage;      /* GP0(E1)-style texpage bits incl. semi mode and depth */
    uint32_t texwindow;
    int16_t clip_x0, clip_y0, clip_x1, clip_y1; /* draw area (inclusive) */
    uint8_t mask_set, mask_check;
    uint8_t gte3d;         /* vertices came from GTE projection (3D world geometry), not CPU 2D coords */
    int16_t gte_ofx;       /* gte3d: projection center X in VRAM coords (OFX + draw offset), 0x7FFF unknown */
} GpuPrim;

typedef struct {
    int disp_x, disp_y;            /* VRAM origin of the display */
    int hres, vres;                /* 256/320/368/512/640, 240/480 */
    int depth24;
    int enabled;
    int x1, x2, y1, y2;            /* horizontal/vertical display range */
} GpuDisplay;

typedef struct GpuBackend {
    void (*prim)(const GpuPrim *p);
    void (*vram_write)(int x, int y, int w, int h);       /* region of g_vram changed by upload/fill/copy */
    void (*vram_copy)(int sx, int sy, int dx, int dy, int w, int h);
    void (*fill)(int x, int y, int w, int h, uint32_t rgb);
    void (*vram_read)(int x, int y, int w, int h);        /* CPU is about to read this region */
} GpuBackend;

extern GpuBackend g_gpu_backend;
extern GpuDisplay g_gpu_display;
extern uint32_t g_gpu_prims_frame;
extern uint32_t g_gpu_uploads, g_gpu_upload_px;

#ifdef __cplusplus
}
#endif
