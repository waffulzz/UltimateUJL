/* Default the JP release's own LANGUAGE setting to English.
   The JP game ships English subtitles/lyrics and a LANGUAGE menu (ENGLISH / Japanese). Its settings init
   (F_8001e498) clears the settings block at 0x801a5310 and stores language = 5 (Japanese) at +0x28;
   0 is English. Right after that store it calls F_8001e42c from 0x8001e4f0 (ra 0x8001e4f4), so a
   pre-hook there switches the boot default to English. The in-game menu still changes it freely. */
#include "psx.h"
#include "recomp.h"
#include <stdlib.h>

int g_default_english = 1;

#define SETTINGS_LANGUAGE 0x801a5338u
enum { LANG_ENGLISH = 0, LANG_JAPANESE = 5 };

void hook_default_language(CPU *c, uint32_t fn) {
    (void)fn;
    if (c->r[31] != 0x8001e4f4u) return; /* only the boot-time settings init */
    if (!g_default_english || getenv("UJL_JAPANESE")) return;
    if (mem_r32(SETTINGS_LANGUAGE) == LANG_JAPANESE) {
        mem_w32(SETTINGS_LANGUAGE, LANG_ENGLISH);
        LOGI("language: defaulting to English");
    }
}
