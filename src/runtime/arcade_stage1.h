/* Runtime-only Stage 1 arcade resources. Nothing extracted from a ROM is distributed with the port. */
#pragma once
#include <SDL3/SDL.h>
#ifdef __cplusplus
extern "C" {
#endif
bool arcade_stage1_init(const char *rom_set);
void arcade_stage1_clear(void);
bool arcade_stage1_resources_ready(void);
struct CPU;
int arcade_stage1_menu_screen(struct CPU *c, int screen);
void arcade_stage1_menu_sprite(struct CPU *c);
/* Indices 0..5 select the board's six Stage 1 overlay slots. */
const uint8_t *arcade_stage1_overlay(unsigned mode, size_t *size);
const uint8_t *arcade_stage1_asset(const char *name, size_t *size);
const uint8_t *arcade_stage1_resource(unsigned index, size_t *size);
unsigned arcade_stage1_asset_count(void);
const char *arcade_stage1_asset_name(unsigned index);
typedef struct {
    /* phrases contains solo difficulty profiles; multiplayer uses the same table for both. */
    uint32_t scripts[2], phrases[2], fever, chart;
    size_t size;
    unsigned chart_notes;
} ArcadeStage1Gameplay;
/* Relocate gameplay data into a caller-owned guest buffer. Returns offsets when guest_base is zero. */
bool arcade_stage1_gameplay(unsigned mode, uint8_t *out, size_t capacity, uint32_t guest_base,
                            ArcadeStage1Gameplay *info);
typedef struct {
    uint32_t tables[3];
    unsigned counts[3];
    size_t size;
} ArcadeStage1Scene;
/* Native scene commands use tagged resource IDs so console caption resources remain separate. */
enum { ARCADE_STAGE1_RESOURCE_TAG = 0x4000 };
bool arcade_stage1_scene(unsigned mode, uint8_t *out, size_t capacity, uint32_t guest_base,
                        ArcadeStage1Scene *info);
typedef struct {
    uint32_t table;
    unsigned count;
    size_t size;
} ArcadeStage1Faces;
/* Scene op 7 face texture programs (lip sync, blinks), relocated with tagged resource IDs. */
bool arcade_stage1_faces(unsigned mode, uint8_t *out, size_t capacity, uint32_t guest_base,
                         ArcadeStage1Faces *info);
/* The mode's whole-song note bar grid (header + one u16 per slot), validated; NULL if absent. */
const uint8_t *arcade_stage1_bar_grid(unsigned mode, size_t *size);
/* Stage 1's board command numbers -> the user's console PaRappa VAB sample numbers. */
unsigned arcade_stage1_rap_voice(unsigned command);
bool arcade_stage1_unpack(const uint8_t *src, size_t packed_size, uint8_t *out, size_t size);
#ifdef __cplusplus
}
#endif
