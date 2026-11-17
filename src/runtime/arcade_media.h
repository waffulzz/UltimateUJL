/* Imports only known board ROMs and the raw arcade CD from user-owned archives. */
#pragma once
#include "asset_zip.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct { const char *name; uint32_t size, crc; } ArcadeRomFile;
extern const ArcadeRomFile arcade_rom_files[10];
/* destination must not exist. Failure removes only files created by this call. */
bool arcade_media_import(const char *archive, const char *destination, bool *has_cd,
                         AssetZipProgress progress, void *user);
#ifdef __cplusplus
}
#endif
