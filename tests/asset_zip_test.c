/* Driver for synthetic archive fixtures and optional local, user-owned media checks. */
#include "arcade_media.h"
#include "arcade_audio.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
void host_vblank(void) {}
void host_audio_push(const int16_t *lr, int frames) { (void)lr; (void)frames; }

static bool cancel(void *user, uint64_t done, uint64_t total) {
    (void)user; (void)total;
    return done < 32768;
}
static bool discard(void *user, const uint8_t *bytes, size_t size) {
    (void)user; (void)bytes; (void)size;
    return true;
}
int main(int argc, char **argv) {
    bool ok = false;
    if (argc == 3 && !strcmp(argv[1], "audio")) {
        ok = arcade_audio_init(argv[2]);
    } else if (argc == 5 && !strcmp(argv[1], "import")) {
        bool has_cd;
        ok = arcade_media_import(argv[2], argv[3], &has_cd, NULL, NULL);
        if (ok && has_cd != (atoi(argv[4]) != 0)) return 2;
    } else if (argc == 4 && !strcmp(argv[1], "cancel-import")) {
        bool has_cd;
        ok = arcade_media_import(argv[2], argv[3], &has_cd, cancel, NULL);
    } else if (argc >= 4) {
        AssetZip z; AssetZipEntry e;
        if (asset_zip_open(&z, argv[2])) {
            ok = asset_zip_find(&z, argv[3], &e);
            if (ok && !strcmp(argv[1], "extract") && argc == 5)
                ok = asset_zip_write_file(&z, &e, argv[4], NULL, NULL);
            else if (ok)
                ok = asset_zip_extract(&z, &e, discard, NULL, !strcmp(argv[1], "cancel") ? cancel : NULL, NULL);
            asset_zip_close(&z);
        }
    } else return 2;
    if (!ok) fprintf(stderr, "%s\n", SDL_GetError());
    return ok ? 0 : 1;
}
