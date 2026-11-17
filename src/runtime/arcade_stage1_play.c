#include "arcade_stage1.h"
#include "psx.h"
#include <stdlib.h>

void hook_score_attempt_reset(CPU *c, uint32_t fn);
static uint32_t imported_resources[1024], scene_data;
static unsigned active_slot;
static bool player_models_ready;
static bool restoring;
static uint32_t sequence_start, sequence_end;
static uint8_t sequence_banks[83];
static uint8_t *console_sound_archive;
static const uint8_t *guitar_vh, *guitar_vb;
static uint32_t guitar_vh_size, guitar_vb_size;
static uint32_t loaded_guitar_header, stage_assets_end;
bool disc_read_file(const char *path, uint8_t **data, uint32_t *size);
void hook_force_scene(CPU *c, uint32_t fn);

void hook_stage1_scene_route(CPU *c, uint32_t fn) {
    hook_force_scene(c, fn);
    restoring = false;
    if (mem_r32(0x80078ae0) != 1) return;
    const char *path = getenv("UJL_ARCADE_ROMS");
    if (!arcade_stage1_resources_ready() && path && !arcade_stage1_init(path)) {
        rt_fatal("arcade Stage 1: %s", SDL_GetError()); return;
    }
    unsigned submode = mem_r32(0x80078ae4);
    if (!arcade_stage1_resources_ready() || !submode || submode > 5) return;
    static const unsigned slots[] = {3, 5, 1, 0, 2, 4};
    active_slot = slots[submode]; restoring = true;
    unsigned scene = submode == 1 || submode == 5 ? 11 : 1;
    unsigned column = submode == 1 || submode == 4 ? 1 : submode == 2 || submode == 5 ? 2 : 0;
    uint32_t entry = 0x8005b3b0 + scene * 48 + column * 16;
    /* Reuse the console's unfinished rap scene engine and its real Stage 1 voice banks.
       Gameplay, models and choreography then come from the user's arcade ROM set. */
    if (entry != 0x8005b3e0) for (unsigned i = 0; i < 4; i++)
        mem_w32(entry + i * 4, mem_r32(0x8005b3e0 + i * 4));
    LOGI("arcade Stage 1: selected submode %u, source slot %u", submode, active_slot);
}
static uint32_t read32(const uint8_t *p) { return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t read16(const uint8_t *p) { return p[0] | (uint16_t)p[1] << 8; }

static bool read_guitar_bank(void) {
    if (guitar_vh && guitar_vb) return true;
    uint32_t size;
    if (!disc_read_file("/S1/CMP11SNG.INT", &console_sound_archive, &size)) return false;
    size_t section = 0;
    while (section <= size && size - section >= 16) {
        const uint8_t *s = console_sound_archive + section;
        uint32_t type = read32(s), count = read32(s + 4), sectors = read32(s + 8);
        if (type == UINT32_MAX) break;
        if (!count || count > 2048) break;
        size_t header = (16u + count * 20u + 8191u) & ~(size_t)8191u;
        uint64_t length = header + (uint64_t)sectors * 2048u;
        if (!sectors || length > size - section) break;
        size_t at = section + header, end = section + (size_t)length;
        for (unsigned i = 0; i < count; i++) {
            const uint8_t *entry = s + 16 + i * 20;
            uint32_t bytes = read32(entry);
            if (!bytes || bytes > end - at) goto invalid_bank;
            if (type == 2 && !memcmp(entry + 4, "S1GG_G.VH", 10)) {
                guitar_vh = console_sound_archive + at; guitar_vh_size = bytes;
            } else if (type == 2 && !memcmp(entry + 4, "S1GG_G.VB", 10)) {
                guitar_vb = console_sound_archive + at; guitar_vb_size = bytes;
            }
            at += ((size_t)bytes + 3) & ~(size_t)3;
            if (at > end) goto invalid_bank;
        }
        section = end;
    }
    if (guitar_vh && guitar_vb && guitar_vh_size == 3616 && guitar_vb_size == 98880 &&
        read32(guitar_vh) == 0x56414270 && read16(guitar_vh + 22) == 20) return true;
invalid_bank:
    free(console_sound_archive); console_sound_archive = NULL;
    guitar_vh = guitar_vb = NULL; guitar_vh_size = guitar_vb_size = 0;
    return false;
}

void hook_stage1_bank_load(CPU *c, uint32_t fn) {
    (void)fn;
    const char *overlay = rt_current_overlay_name();
    if (!restoring || active_slot == 0 ||
        mem_r32(0x80078ae0) != 1 || c->r[6] != 1 ||
        !overlay || strcmp(overlay, "COMOD1.SNG")) return;
    if (!read_guitar_bank()) { rt_fatal("arcade Stage 1: could not read the native guitar bank"); return; }
    uint32_t header = rt_asset_alloc(guitar_vh_size), body = rt_asset_alloc(guitar_vb_size + 63u);
    if (!header || !body) { rt_fatal("arcade Stage 1: could not allocate the native guitar bank"); return; }
    memcpy(ram_ptr(header), guitar_vh, guitar_vh_size);
    memcpy(ram_ptr(body), guitar_vb, guitar_vb_size);
    c->r[4] = header; c->r[5] = body;
    loaded_guitar_header = header;
    stage_assets_end = rt_asset_mark();
}

void hook_stage1_sound_bank(CPU *c, uint32_t fn) {
    (void)fn;
    uint32_t seq = c->r[5];
    if (!restoring || seq < sequence_start || seq >= sequence_end ||
        ((seq - sequence_start) & 31)) return;
    unsigned slot = sequence_banks[(seq - sequence_start) / 32];
    int bank = (int16_t)mem_r16(0x801a23f0 + slot * 2);
    if (bank < 0 || bank >= 10) { rt_fatal("arcade Stage 1: missing sound bank"); return; }
    c->r[6] = mem_r32(0x801c50c0 + (unsigned)bank * 4);
}

static bool bind_stage_sounds(uint32_t guitar_header) {
    /* The console already supplies the Stage 1 PaRappa voice bank. The board's MCU commands
       have their own numbering; build a small MIDI sequence per command from that bank's tone
       metadata instead of passing command numbers to the console's sequence-pointer table. */
    uint32_t bank = mem_r32(0x80078b20 + 3 * 4);
    if (!bank || mem_r32(bank) != 0x56414270 || mem_r16(bank + 18) != 4 || mem_r16(bank + 22) != 61)
        return false;
    uint32_t buffer = rt_asset_alloc(128 * 4 + 83 * 32);
    if (!buffer) return false;
    sequence_start = buffer + 128 * 4; sequence_end = sequence_start;
    unsigned made = 0;
    for (unsigned command = 32; command <= 125; command++) {
        bool guitar = command <= 52;
        if ((guitar && !guitar_header) || (command > 52 && command < 64)) continue;
        uint32_t header = guitar ? guitar_header : bank;
        unsigned sample = guitar ? (command == 32 ? 1 : command - 32) : arcade_stage1_rap_voice(command), tone = 0;
        unsigned tones = mem_r16(header + 18) * 16;
        for (; tone < tones; tone++) if (mem_r16(header + 32 + 128 * 16 + tone * 32 + 22) == sample) break;
        if (tone == tones) return false;
        uint32_t attr = header + 32 + 128 * 16 + tone * 32;
        unsigned program = mem_r16(attr + 20), note = mem_r8(attr + 6);
        if (program >= 128 || note >= 128) return false;
        uint32_t seq = sequence_start + made * 32;
        sequence_banks[made++] = guitar ? 1 : 2;
        uint8_t *p = ram_ptr(seq);
        /* SEQ header: version 1, 480 ticks/quarter, 500000 us/quarter, 4/4. MIDI note-on,
           a long release deadline (the SPU sample ends first), note-off and end-of-track. */
        p[0] = 'p'; p[1] = 'Q'; p[2] = 'E'; p[3] = 'S'; p[7] = 1;
        p[8] = 1; p[9] = 224; p[10] = 7; p[11] = 161; p[12] = 32; p[13] = 4; p[14] = 2;
        p[16] = 0xc0; p[17] = (uint8_t)program; p[19] = 0x90;
        p[20] = (uint8_t)note; p[21] = 120; p[22] = 0x9e; p[24] = 0x90;
        p[25] = (uint8_t)note; p[28] = 0xff; p[29] = 0x2f;
        mem_w32(buffer + command * 4, seq);
    }
    sequence_end = sequence_start + made * 32;
    for (unsigned i = 0; i < 4; i++) mem_w32(0x80071ca8 + 224 + i * 4, buffer);
    LOGI("arcade Stage 1: mapped %u sound commands to native instrument banks", made);
    return true;
}

static void upload_player_textures(void) {
    for (unsigned i = 0; i < arcade_stage1_asset_count(); i++) {
        const char *name = arcade_stage1_asset_name(i);
        size_t n = strlen(name), size;
        bool pa = active_slot == 0 || active_slot == 2 || active_slot == 4;
        bool lammy = active_slot != 0;
        bool blue = active_slot == 1 || active_slot == 5;
        bool selected = (pa && !strncmp(name, "PA", 2)) || (lammy && !strncmp(name, "LA", 2)) ||
                        (blue && !strncmp(name, "BL", 2));
        if (n < 6 || !selected || strcmp(name + n - 4, ".TIM")) continue;
        const uint8_t *data = arcade_stage1_asset(name, &size);
        if (!data || size < 8 || read32(data) != 16) { rt_fatal("Invalid arcade character texture"); return; }
        size_t at = 8;
        unsigned blocks = read32(data + 4) & 8 ? 2 : 1;
        for (unsigned b = 0; b < blocks; b++) {
            if (at > size || size - at < 12) { rt_fatal("Truncated arcade character texture"); return; }
            uint32_t length = read32(data + at);
            unsigned w = read16(data + at + 8), h = read16(data + at + 10);
            size_t bytes = (size_t)w * h * 2;
            if (!w || !h || w > 1024 || h > 512 || length < 12 || length > size - at || bytes > length - 12) {
                rt_fatal("Invalid arcade character texture dimensions"); return;
            }
            gpu_gp0(0xa0000000); gpu_gp0(read32(data + at + 4)); gpu_gp0(w | h << 16);
            for (size_t p = 0; p < bytes; p += 4) {
                uint32_t word = read16(data + at + 12 + p);
                if (bytes - p >= 4) word |= (uint32_t)read16(data + at + 14 + p) << 16;
                gpu_gp0(word);
            }
            at += length;
        }
    }
}

static uint32_t copy_resource(CPU *c, unsigned index) {
    (void)c;
    size_t size;
    const uint8_t *data = arcade_stage1_resource(index, &size);
    if (!data) return 0;
    uint32_t address = rt_asset_alloc(size);
    if (!address) return 0;
    memcpy(ram_ptr(address), data, size);
    return address;
}

int filter_stage1_asset_lookup(CPU *c, uint32_t fn) {
    (void)fn;
    static unsigned logged;
    const char *overlay = rt_current_overlay_name();
    if (restoring && overlay && !strcmp(overlay, "COMOD1.SNG") &&
        c->r[4] >= ARCADE_STAGE1_RESOURCE_TAG && c->r[4] < ARCADE_STAGE1_RESOURCE_TAG + 1024) {
        unsigned index = c->r[4] - ARCADE_STAGE1_RESOURCE_TAG;
        if (!index || index >= 1024) { rt_fatal("arcade Stage 1: invalid scene resource %u", index); return 1; }
        if (!imported_resources[index]) imported_resources[index] = copy_resource(c, index);
        if (!imported_resources[index]) rt_fatal("arcade Stage 1: could not allocate host memory for resource %u", index);
        c->r[2] = imported_resources[index];
        if (getenv("UJL_STAGE1_ASSET_LOG") && logged++ < 600)
            LOGI("arcade resource %u -> %08x from %08x", index, c->r[2], c->r[31]);
        return 1;
    }
    if (getenv("UJL_STAGE1_ASSET_LOG") && overlay && !strcmp(overlay, "COMOD1.SNG") && logged < 600) {
        LOGI("Stage 1 resource lookup %u from %08x", c->r[4], c->r[31]);
        logged++;
    }
    return 0;
}

static void add_player_mesh(CPU *c, uint32_t root, unsigned mesh, unsigned resource, uint32_t parent) {
    mem_w32(c->r[29] + 16, parent);
    c->r[4] = root; c->r[5] = mesh; c->r[6] = ARCADE_STAGE1_RESOURCE_TAG + resource; c->r[7] = 2;
    call_indirect(c, 0x801ca530);
    c->r[4] = root; c->r[5] = mesh;
    call_indirect(c, 0x801ca718);
}

static void add_guitar_player(CPU *c, uint32_t root, bool blue) {
    unsigned body = blue ? 12 : 3, resource = blue ? 372 : 379;
    add_player_mesh(c, root, body, resource, 0);
    /* The extra channel blends the guitar player's body independently of the base animation. */
    c->r[4] = root; c->r[5] = blue ? 16 : 7; c->r[6] = body;
    call_indirect(c, 0x801ca828);
    c->r[4] = root; c->r[5] = body;
    call_indirect(c, 0x801ca7a0);
    uint32_t parent = c->r[2];
    add_player_mesh(c, root, body + 2, resource + 1, parent);
    add_player_mesh(c, root, body + 1, resource + 2, parent);
    c->r[4] = root; c->r[5] = body + 1; c->r[6] = 0;
    call_indirect(c, 0x801ca690);
}

void hook_stage1_player_models(CPU *c, uint32_t fn) {
    (void)fn;
    if (!restoring || player_models_ready || !arcade_stage1_resources_ready()) return;
    uint32_t root = mem_r32(0x801e45c4);
    if (!root || mem_r32(root + 84) < 10) return;
    if (!mem_r32(root + 80)) return;
    CPU saved = *c;
    c->r[29] -= 64;
    if (mem_r32(root) < 0xc000) {
        c->r[4] = root; c->r[5] = 0;
        call_indirect(c, 0x801c9250);
        c->r[4] = root; c->r[5] = 0xc000;
        call_indirect(c, 0x801c91d4);
    }
    if (active_slot != 0) add_guitar_player(c, root, false);
    if (active_slot == 1 || active_slot == 5) add_guitar_player(c, root, true);
    else if (active_slot != 3) {
        add_player_mesh(c, root, 8, 384, 0);
        c->r[4] = root; c->r[5] = 8;
        call_indirect(c, 0x801ca7a0);
        add_player_mesh(c, root, 9, 385, c->r[2]);
        /* Expression meshes start hidden, as in the board's actor constructor. */
        c->r[4] = root; c->r[5] = 9; c->r[6] = 0;
        call_indirect(c, 0x801ca690);
    }
    player_models_ready = true;
    *c = saved;
    upload_player_textures();
    LOGI("arcade Stage 1: player models at %08x", root);
}

void hook_stage1_scene_cues(CPU *c, uint32_t fn) {
    (void)fn;
    if (!restoring || !arcade_stage1_resources_ready()) return;
    uint32_t timeline = mem_r32(0x801dc364);
    if (!timeline) return;
    if (scene_data) {
        /* The arcade loop ends a human versus match when the song and its final scene cues
           are exhausted. The unfinished console Stage 1 loop only notices a KO there. */
        uint32_t flags = mem_r32(0x80071ca8 + 300), count = mem_r32(timeline + 132);
        if (mem_r32(0x80078aec) == 1 && (flags & 4) && count && mem_r32(timeline + 136) >= count)
            mem_w32(0x80071ca8 + 300, flags | 8);
        return;
    }
    ArcadeStage1Scene scene;
    if (!arcade_stage1_scene(active_slot, NULL, 0, 0, &scene)) {
        rt_fatal("arcade Stage 1: %s", SDL_GetError()); return;
    }
    uint32_t buffer = rt_asset_alloc(scene.size);
    if (!buffer) {
        rt_fatal("arcade Stage 1: could not allocate host memory for scene cues"); return;
    }
    if (!arcade_stage1_scene(active_slot, ram_ptr(buffer), scene.size, buffer, &scene)) {
        rt_fatal("arcade Stage 1: %s", SDL_GetError()); return;
    }
    scene_data = buffer;
    for (unsigned i = 0; i < 3; i++) {
        mem_w32(timeline + 2220 + i * 12, scene.tables[i]);
        mem_w32(timeline + 2224 + i * 12, scene.counts[i]);
        mem_w32(timeline + 2228 + i * 12, i == 1);
    }
    uint32_t table = mem_r32(timeline + 2244);
    mem_w32(timeline + 128, mem_r32(table + 8));
    mem_w32(timeline + 132, mem_r32(table + 12));
    mem_w32(timeline + 140, 0); mem_w32(timeline + 144, 1); mem_w32(timeline + 148, 0);
    LOGI("arcade Stage 1: scene cues, %zu bytes at %08x", scene.size, buffer);
}
/* Bind the board's charts to the native rhythm engine after its attempt initialization. */
void hook_song_attempt_reset(CPU *c, uint32_t fn) {
    hook_score_attempt_reset(c, fn);
    if (!restoring || mem_r32(0x80078ae0) != 1) return;
    const char *overlay = rt_current_overlay_name();
    if (!overlay || (strcmp(overlay, "COMOD11.SNG") && strcmp(overlay, "COMOD1.SNG"))) return;
    /* Banks are stage scoped: load them before the native allocation checkpoint. Charts and
       scene cues are attempt scoped and rewind after the persistent sound data on AGAIN?. */
    if (c->r[31] == (!strcmp(overlay, "COMOD11.SNG") ? 0x801c67e8u : 0x801c6410u)) {
        rt_asset_reset(); stage_assets_end = loaded_guitar_header = sequence_start = sequence_end = 0;
        return;
    }
    if (c->r[31] != (!strcmp(overlay, "COMOD11.SNG") ? 0x801c6954u : 0x801c657cu)) return;
    unsigned mode = active_slot;
    if (mode >= 6) return;
    active_slot = mode;
    bool rap_overlay = !strcmp(overlay, "COMOD1.SNG");
    memset(imported_resources, 0, sizeof imported_resources); scene_data = 0; player_models_ready = false;
    sequence_start = sequence_end = 0;
    rt_asset_rewind(stage_assets_end);
    if (!arcade_stage1_resources_ready() && !arcade_stage1_init(getenv("UJL_ARCADE_ROMS"))) {
        rt_fatal("arcade Stage 1: %s", SDL_GetError()); return;
    }
    uint32_t guitar_header = mode && rap_overlay ? loaded_guitar_header : 0;
    if (mode && rap_overlay && !guitar_header) { rt_fatal("arcade Stage 1: could not restore the native guitar bank"); return; }
    ArcadeStage1Gameplay data;
    if (!arcade_stage1_gameplay(mode, NULL, 0, 0, &data)) {
        rt_fatal("arcade Stage 1: %s", SDL_GetError()); return;
    }
    uint32_t buffer = rt_asset_alloc(data.size);
    if (!buffer) {
        rt_fatal("arcade Stage 1: could not allocate host memory for charts"); return;
    }
    if (!arcade_stage1_gameplay(mode, ram_ptr(buffer), data.size, buffer, &data)) {
        rt_fatal("arcade Stage 1: %s", SDL_GetError()); return;
    }
    uint32_t core = 0x80071ca8;
    mem_w32(core + 4, data.scripts[0]); mem_w32(core + 8, data.scripts[1]);
    mem_w32(core + 12, data.fever);
    unsigned profile = mode % 3 == 0 && mem_r32(0x80078af0) == 1;
    mem_w32(core + 16, data.phrases[profile]);
    mem_w32(core + 112, mode % 3 ? data.phrases[0] : 0);
    /* Preserve the original multiplayer options, including a COM opponent. Debug scene
       forcing bypasses those options and starts with the story mode's absent player 2. */
    if (mode % 3 && mem_r32(core + 160) == 5) mem_w32(core + 160, 1);
    mem_w32(core + 324, data.chart); mem_w32(core + 328, data.chart);
    if (rap_overlay && !bind_stage_sounds(guitar_header)) rt_fatal("arcade Stage 1: native sound command mapping failed");
    LOGI("arcade Stage 1: chart slot %u, %u notes, %zu bytes at %08x", mode, data.chart_notes, data.size, buffer);
}
