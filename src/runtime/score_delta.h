/* Personal-best score traces and the native in-game score delta. */
#pragma once
#include "gpu.h"
#ifdef __cplusplus
extern "C" {
#endif
void score_delta_init(const char *directory);
void score_delta_tick(void);
void score_delta_draw(const GpuPrim *score_digit, void (*emit)(const GpuPrim *));
#ifdef __cplusplus
}
#endif
