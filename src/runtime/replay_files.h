/* Host replay exchange. Only the game thread touches guest RAM. SDL atomics publish requests/results. */
#pragma once
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
int replay_files_enabled(void);
void replay_files_set_enabled(int enabled);
int replay_files_busy(void);
/* Game thread: redirect the return from a host-file replay to the candy bars. */
int replay_files_menu_screen(int screen);
/* UI thread: take one request (1 save, 2 open), then complete it with a UTF-8 path (NULL cancels).
   On failure complete returns 0 and republishes the request, allowing another choice. */
int replay_files_take_request(const char **default_name);
int replay_files_complete(const char *path);
const char *replay_files_error(void);
#ifdef __cplusplus
}
#endif
