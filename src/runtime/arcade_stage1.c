#include "arcade_stage1.h"
#include "arcade_media.h"
#include <string.h>

enum { PROGRAM_SIZE = 4194304, OVERLAY_DIRECTORY = 0xc0000, MODE_COUNT = 6 };
typedef struct { uint32_t size, crc; } OverlayInfo;
static const OverlayInfo overlay_info[MODE_COUNT] = {
    {169304, 0x6852b802}, {182296, 0x9b30a10f}, {184744, 0xf8ee85b9},
    {168892, 0x3c4388a9}, {184576, 0x0bd0720d}, {182464, 0xf6463417},
};
static uint8_t *overlays[MODE_COUNT];
typedef struct { char name[17]; uint32_t offset, size, resource; } StageAsset;
static uint8_t *assets;
static size_t assets_size;
static StageAsset catalog[2048];
static unsigned asset_count;
static unsigned resource_count;
typedef struct { uint32_t scripts[2], phrases, fever, chart; unsigned phrase_count; } GameplaySource;
static const GameplaySource gameplay_sources[MODE_COUNT] = {
    {{0x9f00, 0xa608}, 0x9838, 0xae68, 0xe6e4, 31},
    {{0x9cf4, 0xa36c}, 0x9974, 0xad94, 0xe614, 32},
    {{0x9ea4, 0xa51c}, 0x9b24, 0xaf44, 0xe7c4, 32},
    {{0x12ed0, 0x135d8}, 0x12808, 0x13e38, 0x176b4, 31},
    {{0x12e44, 0x134bc}, 0x12ac4, 0x13ee4, 0x17764, 32},
    {{0x12de4, 0x1345c}, 0x12a64, 0x13e84, 0x17704, 32},
};
typedef struct { uint32_t roots[3]; unsigned counts[3]; } SceneSource;
static const SceneSource scene_sources[MODE_COUNT] = {
    {{0x8032c534, 0x8032c08c, 0x803327b4}, {69, 71, 9}},
    {{0x8032dd4c, 0x8032d5dc, 0x80335a44}, {86, 107, 15}},
    {{0x8032dd7c, 0x8032d57c, 0x803363d4}, {85, 141, 15}},
    {{0x8032c9f4, 0x8032c4ec, 0x80332618}, {68, 71, 9}},
    {{0x8032dcd4, 0x8032d4d4, 0x8033632c}, {85, 141, 15}},
    {{0x8032ddf4, 0x8032d684, 0x80335aec}, {86, 107, 15}},
};
static uint32_t le32(const uint8_t *p) {
    return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static uint16_t le16(const uint8_t *p) { return p[0] | (uint16_t)p[1] << 8; }
static void put32(uint8_t *p, uint32_t value) {
    p[0] = value; p[1] = value >> 8; p[2] = value >> 16; p[3] = value >> 24;
}
static bool include_range(size_t at, size_t size, size_t limit, size_t *start, size_t *end) {
    if (at > limit || size > limit - at) return false;
    if (at < *start) *start = at;
    if (at + size > *end) *end = at + size;
    return true;
}
static void put16(uint8_t *p, uint16_t v) { p[0] = v; p[1] = v >> 8; }
unsigned arcade_stage1_rap_voice(unsigned command) {
    if (command == 64 || command == 65) return 1;
    if (command == 66) return 2;
    if (command == 67) return 61;
    if (command >= 68 && command <= 124) return command - 65;
    return command == 125 ? 60 : 0;
}
static unsigned native_mesh(unsigned index) {
    /* The board inserts a mesh at slot 20; subsequent console props are one slot earlier. */
    return index >= 21 && index <= 36 ? index - 1 : index;
}
static bool scene_commands(const uint8_t *source, size_t limit, size_t at, unsigned count,
                           size_t *start, size_t *end, uint8_t *out, size_t origin) {
    if (count > 1024 || !include_range(at, (size_t)count * 8, limit, start, end)) return false;
    for (unsigned i = 0; i < count; i++, at += 8) {
        unsigned op = source[at], target = le16(source + at + 2);
        uint32_t value = le32(source + at + 4);
        if (op > 20) return false;
        if (op == 2 || op == 4 || op == 19 || op == 20) {
            unsigned lo = value & 65535, hi = value >> 16;
            size_t ignored;
            if ((lo && !arcade_stage1_resource(lo, &ignored)) ||
                ((op == 19 || op == 20) && hi && !arcade_stage1_resource(hi, &ignored))) return false;
            if (out) {
                if (lo) lo |= ARCADE_STAGE1_RESOURCE_TAG;
                if ((op == 19 || op == 20) && hi) hi |= ARCADE_STAGE1_RESOURCE_TAG;
                put32(out + at - origin + 4, lo | hi << 16);
                if (op == 4) put16(out + at - origin + 2, (uint16_t)native_mesh(target));
            }
        }
    }
    return true;
}
bool arcade_stage1_scene(unsigned mode, uint8_t *out, size_t capacity, uint32_t guest_base,
                        ArcadeStage1Scene *info) {
    if (!info || mode >= MODE_COUNT || !overlays[mode]) return SDL_SetError("Stage 1 scene is not loaded");
    memset(info, 0, sizeof *info);
    const uint32_t base = 0x80311898;
    const uint32_t *roots = scene_sources[mode].roots;
    const unsigned *counts = scene_sources[mode].counts;
    const uint8_t *source = overlays[mode]; size_t limit = overlay_info[mode].size;
    size_t start = limit, end = 0;
    for (unsigned pass = 0; pass < (out ? 2u : 1u); pass++) {
        uint8_t *dest = pass ? out : NULL;
        for (unsigned table = 0; table < 3; table++) {
            size_t root = roots[table] - base;
            if (!include_range(root, counts[table] * 8u, limit, &start, &end)) goto invalid_scene;
            for (unsigned i = 0; i < counts[table]; i++) {
                size_t row = root + i * 8u;
                uint32_t ptr = le32(source + row), n = le32(source + row + 4);
                if (!ptr && !n) continue;
                if (ptr < base || n > 1024) goto invalid_scene;
                size_t at = ptr - base;
                if (table < 2) {
                    if (!scene_commands(source, limit, at, n, &start, &end, dest, start)) goto invalid_scene;
                } else {
                    if (!include_range(at, n * 12u, limit, &start, &end)) goto invalid_scene;
                    for (unsigned j = 0; j < n; j++) {
                        size_t record = at + j * 12u;
                        uint32_t commands = le32(source + record + 4), count = le32(source + record + 8);
                        if (!commands && !count) continue;
                        if (commands < base || !scene_commands(source, limit, commands - base, count,
                                                               &start, &end, dest, start)) goto invalid_scene;
                        if (dest) put32(dest + record - start + 4, guest_base + commands - base - start);
                    }
                }
                if (dest) put32(dest + row - start, guest_base + at - start);
            }
        }
        if (end < start || end - start > 65536 || guest_base > UINT32_MAX - (end - start)) goto invalid_scene;
        info->size = end - start;
        if (out && !pass) {
            if (capacity < info->size) return SDL_SetError("Stage 1 scene buffer is too small");
            memcpy(out, source + start, info->size);
        }
    }
    for (unsigned i = 0; i < 3; i++) { info->tables[i] = guest_base + roots[i] - base - start; info->counts[i] = counts[i]; }
    return true;
invalid_scene:
    return SDL_SetError("Invalid arcade Stage 1 scene tables");
}
bool arcade_stage1_gameplay(unsigned mode, uint8_t *out, size_t capacity, uint32_t guest_base,
                            ArcadeStage1Gameplay *info) {
    if (!info || mode >= MODE_COUNT || !overlays[mode]) return SDL_SetError("Stage 1 mode is not loaded");
    memset(info, 0, sizeof *info);
    const uint32_t board_base = 0x80311898;
    const uint8_t *src = overlays[mode]; size_t limit = overlay_info[mode].size;
    const GameplaySource *g = gameplay_sources + mode;
    size_t start = limit, end = 0; unsigned fever_count = 0;
    for (unsigned course = 0; course < 2; course++) {
        size_t at = g->scripts[course]; bool stopped = false;
        for (unsigned record = 0; record < 1024; record++, at += 12) {
            if (!include_range(at, 12, limit, &start, &end)) goto invalid;
            uint32_t op = le32(src + at);
            if (op > 14) goto invalid;
            if (op == 11) for (unsigned p = 0; p < 2; p++) {
                unsigned index = le16(src + at + 8 + p * 2);
                if (index >= 128) goto invalid;
                if (index + 1 > fever_count) fever_count = index + 1;
            }
            if (op == 10) { stopped = true; break; }
        }
        if (!stopped) goto invalid;
    }
    // Each phrase has six button alternatives and a seventh optional neutral alternative.
    size_t phrases_bytes = g->phrase_count * 28u;
    // Solo has two difficulty profiles. Multiplayer shares one table between both players.
    unsigned profiles = mode % 3 == 0 ? 2 : 1;
    if (!include_range(g->phrases, phrases_bytes * profiles, limit, &start, &end)) goto invalid;
    for (unsigned i = 0; i < g->phrase_count * 7 * profiles; i++) {
        uint32_t ptr = le32(src + g->phrases + i * 4);
        if (!ptr) continue;
        if (ptr < board_base) goto invalid;
        size_t at = ptr - board_base;
        if (!include_range(at, 12, limit, &start, &end)) goto invalid;
        unsigned count = le16(src + at + 4);
        uint32_t notes = le32(src + at);
        if (count > 1024 || notes < board_base ||
            !include_range(notes - board_base, count * 4u, limit, &start, &end)) goto invalid;
    }
    if (!include_range(g->fever, fever_count * 4u, limit, &start, &end)) goto invalid;
    for (unsigned i = 0; i < fever_count; i++) {
        uint32_t ptr = le32(src + g->fever + i * 4);
        if (!ptr) continue;
        if (ptr < board_base || !include_range(ptr - board_base, 192, limit, &start, &end)) goto invalid;
        for (unsigned k = 0; k < 6; k++) {
            uint32_t descriptor = le32(src + ptr - board_base + k * 32 + 28);
            if (!descriptor) continue;
            if (descriptor < board_base || !include_range(descriptor - board_base, 12, limit, &start, &end)) goto invalid;
            unsigned count = le16(src + descriptor - board_base + 4);
            uint32_t notes = le32(src + descriptor - board_base);
            if (count > 1024 || notes < board_base ||
                !include_range(notes - board_base, count * 4u, limit, &start, &end)) goto invalid;
        }
    }
    unsigned notes = 0; int previous = -1;
    for (; notes < 4096; notes++) {
        size_t at = g->chart + notes * 4u;
        if (!include_range(at, 4, limit, &start, &end)) goto invalid;
        unsigned slot = le16(src + at);
        if (slot == 32767) break;
        if (slot > 32767 || (int)slot < previous) goto invalid;
        previous = slot;
    }
    if (notes == 4096) goto invalid;
    start &= ~(size_t)3; end = (end + 3) & ~(size_t)3;
    if (end > limit || end - start > 65536 || guest_base > UINT32_MAX - (end - start)) goto invalid;
    info->size = end - start; info->chart_notes = notes;
    info->scripts[0] = guest_base + g->scripts[0] - start;
    info->scripts[1] = guest_base + g->scripts[1] - start;
    info->phrases[0] = guest_base + g->phrases - start;
    info->phrases[1] = info->phrases[0] + (profiles == 2 ? phrases_bytes : 0);
    info->fever = guest_base + g->fever - start;
    info->chart = guest_base + g->chart - start;
    if (!out) return true;
    if (capacity < info->size) return SDL_SetError("Stage 1 guest buffer is too small");
    memcpy(out, src + start, info->size);
    // Relocate typed pointer fields only. Script arguments and note records are numeric data.
    for (unsigned i = 0; i < g->phrase_count * 7 * profiles; i++) {
        size_t field = g->phrases + i * 4;
        uint32_t ptr = le32(src + field);
        if (!ptr) continue;
        size_t descriptor = ptr - board_base;
        put32(out + field - start, guest_base + descriptor - start);
        put32(out + descriptor - start, guest_base + le32(src + descriptor) - board_base - start);
    }
    for (unsigned i = 0; i < fever_count; i++) {
        size_t field = g->fever + i * 4;
        uint32_t ptr = le32(src + field);
        if (!ptr) continue;
        put32(out + field - start, guest_base + ptr - board_base - start);
        for (unsigned k = 0; k < 6; k++) {
            size_t voice = ptr - board_base + k * 32 + 28;
            uint32_t descriptor = le32(src + voice);
            if (!descriptor) continue;
            size_t at = descriptor - board_base;
            put32(out + voice - start, guest_base + at - start);
            put32(out + at - start, guest_base + le32(src + at) - board_base - start);
        }
    }
    return true;
invalid:
    return SDL_SetError("Unsupported Stage 1 gameplay table layout");
}

/* The board's overlay container is a length-prefixed LZSS stream with a 4KiB zero-filled dictionary,
   initial cursor 4078, LSB-first literal flags and 12-bit dictionary positions / 3..18-byte matches. */
bool arcade_stage1_unpack(const uint8_t *src, size_t packed_size, uint8_t *out, size_t size) {
    if (!src || !out || packed_size < 8 || le32(src) != size || size > 1048576)
        return SDL_SetError("Invalid arcade overlay size");
    size_t end = le32(src + 4), at = 8, made = 0;
    if (end < 8 || end > packed_size) return SDL_SetError("Truncated arcade overlay");
    uint8_t dictionary[4096] = {0};
    unsigned cursor = 4078, flags = 0;
    while (made < size) {
        flags >>= 1;
        if (!(flags & 256)) {
            if (at >= end) goto invalid;
            flags = src[at++] | 0xff00u;
        }
        if (flags & 1) {
            if (at >= end) goto invalid;
            uint8_t value = src[at++];
            out[made++] = value; dictionary[cursor] = value; cursor = (cursor + 1) & 4095;
        } else {
            if (at >= end || end - at < 2) goto invalid;
            unsigned a = src[at++], b = src[at++];
            unsigned from = a | ((b & 240) << 4), count = (b & 15) + 3;
            if (count > size - made) goto invalid;
            for (unsigned i = 0; i < count; i++) {
                uint8_t value = dictionary[(from + i) & 4095];
                out[made++] = value; dictionary[cursor] = value; cursor = (cursor + 1) & 4095;
            }
        }
    }
    return true;
invalid:
    return SDL_SetError("The arcade overlay stream is damaged");
}
void arcade_stage1_clear(void) {
    for (unsigned i = 0; i < MODE_COUNT; i++) { SDL_free(overlays[i]); overlays[i] = NULL; }
    SDL_free(assets); assets = NULL; assets_size = 0; asset_count = resource_count = 0;
}
bool arcade_stage1_resources_ready(void) {
    for (unsigned i = 0; i < MODE_COUNT; i++) if (!overlays[i]) return false;
    return assets != NULL && asset_count != 0;
}
unsigned arcade_stage1_asset_count(void) { return asset_count; }
const char *arcade_stage1_asset_name(unsigned index) { return index < asset_count ? catalog[index].name : NULL; }
const uint8_t *arcade_stage1_resource(unsigned index, size_t *size) {
    if (size) *size = 0;
    if (!assets || !index) return NULL;
    for (unsigned i = 0; i < asset_count; i++) if (catalog[i].resource == index) {
        if (size) *size = catalog[i].size;
        return assets + catalog[i].offset;
    }
    return NULL;
}
const uint8_t *arcade_stage1_asset(const char *name, size_t *size) {
    if (size) *size = 0;
    if (!name || !assets) return NULL;
    for (unsigned i = 0; i < asset_count; i++) if (!SDL_strcasecmp(catalog[i].name, name)) {
        if (size) *size = catalog[i].size;
        return assets + catalog[i].offset;
    }
    return NULL;
}
static bool catalog_assets(void) {
    size_t section = 0;
    while (section <= assets_size && assets_size - section >= 16) {
        const uint8_t *s = assets + section;
        uint32_t type = le32(s), count = le32(s + 4), sectors = le32(s + 8);
        if (type == UINT32_MAX) return asset_count != 0;
        if ((type != 1 && type != 3) || !count || count > 2048 - asset_count) break;
        size_t header = (16u + count * 20u + 8191u) & ~(size_t)8191u;
        uint64_t length = header + (uint64_t)sectors * 2048;
        if (!sectors || length > assets_size - section) break;
        size_t at = section + header, end = section + (size_t)length;
        for (unsigned i = 0; i < count; i++) {
            const uint8_t *entry = s + 16 + i * 20;
            uint32_t size = le32(entry);
            if (!size || size > end - at) return SDL_SetError("Invalid Stage 1 asset size");
            StageAsset *a = catalog + asset_count;
            memcpy(a->name, entry + 4, 16); a->name[16] = 0;
            if (!a->name[0]) return SDL_SetError("Invalid Stage 1 asset name");
            for (unsigned j = 0; j < asset_count; j++) if (!SDL_strcasecmp(catalog[j].name, a->name))
                return SDL_SetError("Duplicate Stage 1 asset name");
            a->offset = (uint32_t)at; a->size = size; a->resource = type == 3 ? ++resource_count : 0; asset_count++;
            at += ((size_t)size + 3) & ~(size_t)3;
            if (at > end) return SDL_SetError("Invalid Stage 1 asset alignment");
        }
        section = end;
    }
    return SDL_SetError("Invalid Stage 1 asset archive");
}
const uint8_t *arcade_stage1_overlay(unsigned mode, size_t *size) {
    if (size) *size = 0;
    if (mode >= MODE_COUNT || !overlays[mode]) return NULL;
    if (size) *size = overlay_info[mode].size;
    return overlays[mode];
}
bool arcade_stage1_init(const char *path) {
    arcade_stage1_clear();
    if (!path || !*path) return false;
    AssetZip zip = {0}; SDL_PathInfo info;
    if (!SDL_GetPathInfo(path, &info)) return false;
    if (info.type != SDL_PATHTYPE_DIRECTORY && !asset_zip_open(&zip, path)) return false;
    uint8_t *program = SDL_malloc(PROGRAM_SIZE), *lane = SDL_malloc(PROGRAM_SIZE / 2);
    bool ok = false;
    if (!program || !lane) { SDL_OutOfMemory(); goto end; }
    for (unsigned i = 0; i < 2; i++) {
        if (!arcade_rom_read(path, 6 + i, lane, &zip)) goto end;
        for (unsigned at = 0; at < PROGRAM_SIZE / 2; at++) program[at * 2 + i] = lane[at];
    }
    /* Board file 0 is the menu. Files 1..6 are the six Stage 1 variants. */
    for (unsigned i = 0; i < MODE_COUNT; i++) {
        uint32_t start = le32(program + OVERLAY_DIRECTORY + (i + 1) * 4);
        uint32_t end = le32(program + OVERLAY_DIRECTORY + (i + 2) * 4);
        if (start < 0x400 || start >= end || end > PROGRAM_SIZE - OVERLAY_DIRECTORY) {
            SDL_SetError("Invalid arcade overlay directory"); goto end;
        }
        overlays[i] = SDL_malloc(overlay_info[i].size);
        if (!overlays[i]) { SDL_OutOfMemory(); goto end; }
        if (!arcade_stage1_unpack(program + OVERLAY_DIRECTORY + start, end - start, overlays[i], overlay_info[i].size)) goto end;
        if (asset_crc32(0, overlays[i], overlay_info[i].size) != overlay_info[i].crc) {
            SDL_SetError("The decoded Stage 1 overlay failed its CRC check"); goto end;
        }
    }
    /* Board data file 17 contains the shared Stage 1 models, textures and animations. */
    SDL_free(program); program = SDL_malloc(16777216);
    SDL_free(lane); lane = SDL_malloc(8388608);
    if (!program || !lane) { SDL_OutOfMemory(); goto end; }
    for (unsigned i = 0; i < 2; i++) {
        if (!arcade_rom_read(path, i, lane, &zip)) goto end;
        for (unsigned at = 0; at < 8388608; at++) program[at * 2 + i] = lane[at];
    }
    uint32_t start = le32(program + 17 * 4), finish = le32(program + 18 * 4);
    if (start < 0x400 || start >= finish || finish > 16777216) {
        SDL_SetError("Invalid Stage 1 asset directory"); goto end;
    }
    assets_size = finish - start; assets = SDL_malloc(assets_size);
    if (!assets) { SDL_OutOfMemory(); goto end; }
    memcpy(assets, program + start, assets_size);
    if (!catalog_assets()) goto end;
    ok = true;
end:
    SDL_free(lane); SDL_free(program); asset_zip_close(&zip);
    if (!ok) arcade_stage1_clear();
    return ok;
}
