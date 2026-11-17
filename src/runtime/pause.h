/* Host suspension stops the shared guest clock, not just presentation. */
#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
enum { RT_PAUSE_USER = 1, RT_PAUSE_BACKGROUND = 2, RT_PAUSE_STAGE_PICKER = 4 };
void rt_pause_set(int reason, int on); /* UI thread; reasons compose across app suspension */
int rt_pause_requested(void);
int rt_pause_active(void);            /* guest is parked; RAM/VRAM may be read */
uint64_t rt_pause_wait(void);         /* guest clock only: wall time to subtract from its epoch */
#ifdef __cplusplus
}
#endif
