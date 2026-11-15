/* GPU command recorder: the game thread records every primitive and VRAM change of a frame so a
   hardware renderer on another thread can replay it at high resolution. The software rasterizer
   still runs, keeping g_vram correct for CPU readbacks and native-resolution output (FMV). */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "gpu.h"

#ifdef __cplusplus
extern "C" {
#endif

enum { REC_PRIM = 1, REC_UPLOAD, REC_FILL, REC_COPY, REC_FRAME };

typedef struct { uint32_t type, size; } RecHdr;             /* size = payload bytes */
typedef struct { int16_t x, y, w, h; } RecRect;              /* REC_UPLOAD: followed by w*h uint16 */
typedef struct { int16_t x, y, w, h; uint32_t rgb; } RecFill;
typedef struct { int16_t sx, sy, dx, dy, w, h; } RecCopy;
/* REC_PRIM: GpuPrim.  REC_FRAME: GpuDisplay (state at vblank) */

void gpu_rec_install(void);              /* backend = software rasterizer + recorder */
void gpu_rec_set_enabled(int on);        /* any thread; takes effect at the next vblank */
void gpu_rec_request_resync(void);       /* re-send all of VRAM (e.g. after renderer resize) */
void gpu_rec_vblank(void);               /* game thread, at every vblank: marks the frame, publishes */
/* UI thread: take everything recorded since the last call. Returns bytes; *buf stays owned by the
   recorder until the next take. */
size_t gpu_rec_take(const uint8_t **buf);

#ifdef __cplusplus
}
#endif
