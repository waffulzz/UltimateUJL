#include "arcade_stage1.h"
#include "recomp.h"
#include "psx.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
void host_vblank(void) {}
void host_audio_push(const int16_t *lr, int frames) { (void)lr; (void)frames; }
static uint32_t word(const uint8_t *p) {
    return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static void check_commands(const uint8_t *data, size_t size, uint32_t ptr, unsigned count, uint32_t base) {
    assert(ptr >= base && ptr - base <= size && count * 8u <= size - (ptr - base));
    for (unsigned i = 0; i < count; i++) {
        const uint8_t *cmd = data + ptr - base + i * 8u;
        if (cmd[0] == 2 || cmd[0] == 4 || cmd[0] == 19 || cmd[0] == 20) {
            unsigned value = word(cmd + 4);
            assert(!(value & 65535) || (value & 65535) >= ARCADE_STAGE1_RESOURCE_TAG);
            if (cmd[0] == 19 || cmd[0] == 20) assert(!(value >> 16) || value >> 16 >= ARCADE_STAGE1_RESOURCE_TAG);
        }
    }
}

int main(int argc, char **argv) {
    unsigned voice_seen[62] = {0};
    for (unsigned command = 65; command <= 125; command++) {
        unsigned voice = arcade_stage1_rap_voice(command);
        assert(voice >= 1 && voice <= 61 && !voice_seen[voice]++);
    }
    assert(arcade_stage1_rap_voice(64) == 1 && arcade_stage1_rap_voice(67) == 61);
    assert(arcade_stage1_rap_voice(122) == 57 && arcade_stage1_rap_voice(125) == 60);
    assert(!arcade_stage1_rap_voice(63) && !arcade_stage1_rap_voice(126));
    g_ram = SDL_calloc(1, PSX_RAM_SIZE);
    assert(g_ram);
    mem_w32(0x80000100, 0x12345678);
    assert(mem_r32(0x80200100) == 0x12345678);
    uint32_t asset = rt_asset_alloc(33);
    assert(asset && !(asset & 15));
    mem_w32(asset, 0x89abcdef);
    mem_w16(asset + 4, 0x1234);
    mem_w8(asset + 6, 0x56);
    assert(mem_r32(asset) == 0x89abcdef && mem_r16(asset + 4) == 0x1234 && mem_r8(asset + 6) == 0x56);
    assert(*(uint8_t *)ram_ptr(asset + 6) == 0x56);
    assert(rt_asset_alloc(9 * 1024 * 1024)); // Growth exceeds the original RAM and preserves guest addresses.
    assert(mem_r32(asset) == 0x89abcdef && mem_r32(0x80000100) == 0x12345678);
    assert(mem_r32(asset | 0x20000000) == 0x89abcdef);
    assert(rt_asset_contains(asset, 33) && !rt_asset_contains(asset, SIZE_MAX));
    /* Audio DMA accepts both directions and guest aliases beyond the console address width. */
    spu_write(0x1a6, 0x400);
    dma_write(0x70, 8u << (4 * 4));
    dma_write(0x40, asset);
    dma_write(0x44, 2);
    dma_write(0x48, 0x11000001);
    spu_write(0x1a6, 0x400);
    assert(spu_dma_read() == 0x89abcdef && spu_dma_read() == 0x00561234);
    spu_write(0x1a6, 0x400);
    dma_write(0x40, (asset + 16) | 0x20000000);
    dma_write(0x44, 2);
    dma_write(0x48, 0x11000000);
    assert(mem_r32(asset + 16) == 0x89abcdef && mem_r32(asset + 20) == 0x00561234);
    uint32_t bank_end = rt_asset_mark(), attempt = rt_asset_alloc(64);
    assert(attempt && rt_asset_contains(attempt, 64));
    mem_w32(attempt, 0xfeed1234);
    rt_asset_rewind(bank_end);
    assert(!rt_asset_contains(attempt, 1) && mem_r32(asset) == 0x89abcdef);
    assert(rt_asset_alloc(64) == attempt && mem_r32(attempt) == 0);
    assert(!rt_asset_alloc(SIZE_MAX) && !rt_asset_alloc(0));
    rt_asset_reset();
    assert(rt_asset_alloc(33) == asset && mem_r32(asset) == 0);
    SDL_free(g_ram); g_ram = NULL;
    rt_asset_reset();
    uint8_t out[64];
    const uint8_t literal[] = {8,0,0,0,17,0,0,0,255,1,2,3,4,5,6,7,8};
    assert(arcade_stage1_unpack(literal, sizeof literal, out, 8));
    for (int i = 0; i < 8; i++) assert(out[i] == i + 1);
    const uint8_t zero[] = {18,0,0,0,11,0,0,0,0,0,15};
    memset(out, 255, sizeof out);
    assert(arcade_stage1_unpack(zero, sizeof zero, out, 18));
    for (int i = 0; i < 18; i++) assert(out[i] == 0);
    // Overlapping matches see newly written dictionary bytes, including across its wrap boundary.
    const uint8_t repeated[] = {20,0,0,0,13,0,0,0,5,42,238,255,42};
    assert(arcade_stage1_unpack(repeated, sizeof repeated, out, 20));
    for (int i = 0; i < 20; i++) assert(out[i] == 42);
    assert(!arcade_stage1_unpack(literal, sizeof literal - 1, out, 8));
    assert(!arcade_stage1_unpack(repeated, sizeof repeated, out, 19));
    uint8_t damaged[sizeof zero]; memcpy(damaged, zero, sizeof damaged);
    damaged[4] = 10;
    assert(!arcade_stage1_unpack(damaged, sizeof damaged, out, 18));
    damaged[4] = 11; damaged[0] = 17;
    memset(out, 255, sizeof out);
    assert(!arcade_stage1_unpack(damaged, sizeof damaged, out, 17));
    assert(out[17] == 255);
    assert(!arcade_stage1_init(NULL));
    if (argc == 2) {
        if (!arcade_stage1_init(argv[1])) { fprintf(stderr, "%s\n", SDL_GetError()); return 1; }
        assert(arcade_stage1_resources_ready());
        for (unsigned i = 0; i < 6; i++) {
            ArcadeStage1Scene mode_scene;
            if (!arcade_stage1_scene(i, NULL, 0, 0, &mode_scene)) {
                fprintf(stderr, "scene slot %u: %s\n", i, SDL_GetError()); return 1;
            }
            size_t size = 0;
            assert(arcade_stage1_overlay(i, &size) && size > 128000);
            ArcadeStage1Gameplay gameplay;
            if (!arcade_stage1_gameplay(i, NULL, 0, 0, &gameplay)) { fprintf(stderr, "mode %u: %s\n", i, SDL_GetError()); return 1; }
            assert(gameplay.chart_notes == 68 && gameplay.size < 65536);
            uint8_t *buffer = SDL_malloc(gameplay.size);
            assert(buffer);
            assert(arcade_stage1_gameplay(i, buffer, gameplay.size, 0x80100000, &gameplay));
            assert(gameplay.chart >= 0x80100000 && gameplay.chart < 0x80100000 + gameplay.size);
            for (unsigned course = 0; course < 2; course++) {
                size_t at = gameplay.scripts[course] - 0x80100000;
                for (; word(buffer + at) != 10; at += 12) {
                    if (word(buffer + at) != 11) continue;
                    for (unsigned player = 0; player < 2; player++) {
                        unsigned index = (word(buffer + at + 8) >> (player * 16)) & 65535;
                        uint32_t table = word(buffer + gameplay.fever - 0x80100000 + index * 4);
                        if (!table) continue;
                        assert(table >= 0x80100000 && table - 0x80100000 + 192 <= gameplay.size);
                        for (unsigned k = 0; k < 6; k++) {
                            uint32_t descriptor = word(buffer + table - 0x80100000 + k * 32 + 28);
                            if (!descriptor) continue;
                            assert(descriptor >= 0x80100000 && descriptor - 0x80100000 + 12 <= gameplay.size);
                            const uint8_t *voice = buffer + descriptor - 0x80100000;
                            uint32_t notes = word(voice), count = word(voice + 4) & 65535;
                            assert(notes >= 0x80100000 && notes - 0x80100000 + count * 4u <= gameplay.size);
                        }
                    }
                }
            }
            SDL_free(buffer);
        }
        size_t size = 1;
        assert(!arcade_stage1_overlay(6, &size) && size == 0);
        assert(arcade_stage1_asset_count() == 1184);
        assert(arcade_stage1_asset("PA.TMD", &size) && size == 10552);
        assert(arcade_stage1_asset("pav.tmd", &size) && size == 632);
        assert(!arcade_stage1_asset("missing.tmd", &size) && size == 0);
        ArcadeStage1Scene scene;
        assert(arcade_stage1_scene(0, NULL, 0, 0, &scene));
        size_t scene_size = scene.size;
        uint8_t *scene_data = SDL_malloc(scene_size);
        assert(scene_data);
        assert(!arcade_stage1_scene(0, scene_data, scene_size - 1, 0x80100000, &scene));
        assert(!arcade_stage1_scene(0, NULL, 0, UINT32_MAX - 16, &scene));
        assert(arcade_stage1_scene(0, scene_data, scene_size, 0x80100000, &scene));
        for (unsigned table = 0; table < 3; table++) {
            assert(scene.tables[table] >= 0x80100000 && scene.tables[table] - 0x80100000 < scene_size);
            for (unsigned i = 0; i < scene.counts[table]; i++) {
                const uint8_t *row = scene_data + scene.tables[table] - 0x80100000 + i * 8;
                uint32_t ptr = word(row), count = word(row + 4);
                if (!ptr && !count) continue;
                if (table < 2) check_commands(scene_data, scene_size, ptr, count, 0x80100000);
                else {
                    assert(ptr >= 0x80100000 && ptr - 0x80100000 <= scene_size && count * 12u <= scene_size - (ptr - 0x80100000));
                    for (unsigned j = 0; j < count; j++) {
                        const uint8_t *record = scene_data + ptr - 0x80100000 + j * 12;
                        if (word(record + 4) || word(record + 8))
                            check_commands(scene_data, scene_size, word(record + 4), word(record + 8), 0x80100000);
                    }
                }
            }
        }
        SDL_free(scene_data);
        arcade_stage1_clear(); assert(!arcade_stage1_resources_ready());
    }
    puts("PASS: arcade Stage 1 overlay decoding and validation");
    return 0;
}
