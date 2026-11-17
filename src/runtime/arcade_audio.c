/* High-resolution arcade PCM sources, feeding the existing SPU envelope/capture/effects path.
   The native ADPCM decoder still runs: pitch, ENDX, IRQs, mute timing and loops remain guest-controlled. */
#include "arcade_audio.h"
#include "arcade_media.h"
#include <stdatomic.h>
#include <string.h>

typedef struct { uint32_t crc, length; uint16_t wave; int16_t offset; int32_t gain; } SampleMap;
static const SampleMap sample_map[] = {
#include "arcade_sample_map.inc"
};
typedef struct { uint32_t start, length; uint16_t flags; } Wave;
static uint8_t *wave_bytes;
static Wave waves[1820];
static struct { const SampleMap *map; uint32_t start; } voices[24];
static atomic_bool enabled = true;
static bool log_notes;
static uint16_t le16(const uint8_t *p) { return p[0] | (uint16_t)p[1] << 8; }

static bool load_rom(const char *path, const ArcadeRomFile *rom, uint8_t *out, AssetZip *zip) {
    if (zip->io) {
        AssetZipEntry e;
        if (!asset_zip_find(zip, rom->name, &e) || e.size != rom->size || e.crc != rom->crc ||
            !asset_zip_read(zip, &e, out, rom->size)) return false;
    } else {
        int count = 0;
        char **names = SDL_GlobDirectory(path, rom->name, SDL_GLOB_CASEINSENSITIVE, &count);
        bool ok = false;
        if (names && count == 1) {
            char *file = NULL;
            SDL_asprintf(&file, "%s/%s", path, names[0]);
            SDL_IOStream *io = file ? SDL_IOFromFile(file, "rb") : NULL;
            ok = io && SDL_GetIOSize(io) == rom->size && SDL_ReadIO(io, out, rom->size) == rom->size;
            if (io) SDL_CloseIO(io);
            SDL_free(file);
        }
        SDL_free(names);
        if (!ok) return SDL_SetError("Could not read arcade sound ROM %s", rom->name);
    }
    if (asset_crc32(0, out, rom->size) != rom->crc)
        return SDL_SetError("Arcade sound ROM %s failed its CRC check", rom->name);
    return true;
}
void arcade_audio_reset(void) { memset(voices, 0, sizeof voices); }
bool arcade_audio_init(const char *path) {
    arcade_audio_reset();
    SDL_free(wave_bytes); wave_bytes = NULL;
    if (!path || !*path) return false;
    AssetZip zip = {0};
    SDL_PathInfo info;
    if (!SDL_GetPathInfo(path, &info)) return false;
    if (info.type != SDL_PATHTYPE_DIRECTORY && !asset_zip_open(&zip, path)) return false;
    uint8_t *mcu = SDL_malloc(524288), *pcm = SDL_malloc(16777216);
    bool ok = false;
    if (!mcu || !pcm) { SDL_OutOfMemory(); goto end; }
    if (!load_rom(path, arcade_rom_files + 5, mcu, &zip) ||
        !load_rom(path, arcade_rom_files + 8, pcm, &zip) ||
        !load_rom(path, arcade_rom_files + 9, pcm + 8388608, &zip)) goto end;
    for (size_t i = 0; i < 524288; i += 2) { uint8_t a = mcu[i]; mcu[i] = mcu[i + 1]; mcu[i + 1] = a; }
    uint16_t table = le16(mcu + 0x8002);
    if (table != 0x96ee || le16(mcu + table) != table + 1820 * 2) {
        SDL_SetError("The arcade sound ROM wave table is unsupported"); goto end;
    }
    memset(waves, 0, sizeof waves);
    for (size_t i = 0; i < sizeof sample_map / sizeof sample_map[0]; i++) {
        unsigned id = sample_map[i].wave;
        if (id >= 1820) { SDL_SetError("Invalid arcade wave mapping"); goto end; }
        const uint8_t *d = mcu + le16(mcu + table + id * 2);
        Wave w = {(uint32_t)le16(d + 2) * 65536 + le16(d + 6),
                  (uint16_t)(le16(d + 8) - le16(d + 6)) + 1u, le16(d + 4)};
        if ((w.flags != 0 && w.flags != 8) || w.start > 16777216 || w.length > 16777216 - w.start) {
            SDL_SetError("The arcade wave descriptor is unsupported"); goto end;
        }
        waves[id] = w;
    }
    wave_bytes = pcm; pcm = NULL; ok = true;
    log_notes = SDL_getenv("UJL_ARCADE_AUDIO_LOG") != NULL;
end:
    SDL_free(mcu); SDL_free(pcm); asset_zip_close(&zip);
    return ok;
}
bool arcade_audio_ready(void) { return wave_bytes != NULL; }
void arcade_audio_set_enabled(bool value) { atomic_store(&enabled, value); }
bool arcade_audio_enabled(void) { return atomic_load(&enabled); }

static const SampleMap *find_sample(uint32_t crc, uint32_t length) {
    size_t lo = 0, hi = sizeof sample_map / sizeof sample_map[0];
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        const SampleMap *m = sample_map + mid;
        if (m->crc < crc || (m->crc == crc && m->length < length)) lo = mid + 1;
        else hi = mid;
    }
    if (lo < sizeof sample_map / sizeof sample_map[0] && sample_map[lo].crc == crc && sample_map[lo].length == length)
        return sample_map + lo;
    return NULL;
}
void arcade_audio_key_on(int voice, const uint8_t *ram, uint32_t start) {
    if (voice < 0 || voice >= 24) return;
    voices[voice].map = NULL;
    if (!wave_bytes || !arcade_audio_enabled() || start < 0x1000 || (start & 15)) return;
    for (uint32_t at = start; at + 16 <= 524288; at += 16) {
        if ((ram[at] >> 4) > 4 || ram[at + 1] > 7) return;
        if (!(ram[at + 1] & 1)) continue;
        uint32_t length = at + 16 - start;
        const SampleMap *map = find_sample(asset_crc32(0, ram + start, length), length);
        if (map) {
            voices[voice].map = map; voices[voice].start = start;
            if (log_notes) SDL_Log("arcade audio: voice %d wave %u native bytes %u", voice, map->wave, length);
        }
        return;
    }
}
static int32_t decode_pcm(uint8_t b, uint16_t flags) {
    if (!flags) return (int32_t)(int8_t)b * 256;
    unsigned a = b & 127;
    int32_t magnitude = a < 16 ? a : a < 24 ? 16 + 2 * (a - 16) : a < 48 ? 32 + 4 * (a - 24) :
                        a < 100 ? 128 + 8 * (a - 48) : 544 + 16 * (a - 100);
    magnitude *= 32;
    return b & 128 ? -magnitude - 32 : magnitude;
}
int32_t arcade_audio_sample(int voice, uint32_t address, uint32_t counter, int32_t original) {
    if (voice < 0 || voice >= 24 || !wave_bytes || !arcade_audio_enabled()) return original;
    const SampleMap *map = voices[voice].map;
    if (!map) return original;
    uint32_t bytes = (address - voices[voice].start) & 0x7ffff;
    if (bytes >= map->length) return original;
    /* Raw arcade sources run at 28kHz (guitars 14kHz); console sources at 22.05kHz (11.025kHz).
       Keep every source byte and interpolate directly into the 44.1kHz SPU mix, using the guest's pitch. */
    int64_t position = ((int64_t)(bytes / 16) * 28 + map->offset) * 4096 + counter;
    if (position < 0) return 0;
    position = position * 80 / 63;
    Wave *w = waves + map->wave;
    uint64_t frame = (uint64_t)position >> 12;
    if (frame >= w->length) return 0;
    const uint8_t *pcm = wave_bytes + w->start;
    int32_t a = decode_pcm(pcm[frame], w->flags);
    int32_t b = frame + 1 < w->length ? decode_pcm(pcm[frame + 1], w->flags) : 0;
    int32_t value = a + (int32_t)(((int64_t)(b - a) * (position & 4095)) / 4096);
    value = (int32_t)(((int64_t)value * map->gain) / 4096);
    return value < -32768 ? -32768 : value > 32767 ? 32767 : value;
}
