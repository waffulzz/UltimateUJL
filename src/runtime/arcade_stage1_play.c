#include "arcade_stage1.h"
#include "psx.h"
#include <stdlib.h>

void hook_score_attempt_reset(CPU *c, uint32_t fn);
static uint32_t imported_resources[1024], scene_data;
static unsigned active_slot;
static bool player_models_ready, vacuums_shown;
static bool restoring;
static uint32_t sequence_start, sequence_end;
static uint8_t sequence_banks[83], sequence_commands[83];
static uint8_t *console_sound_archive;
static const uint8_t *guitar_vh, *guitar_vb, *system_vh, *system_vb;
static uint32_t guitar_vh_size, guitar_vb_size, system_vh_size, system_vb_size;
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
/* The restored mode playing now (board slot 0..5), or -1 when Stage 1 is the console's (Discord presence). */
int arcade_stage1_active_slot(void) { return restoring ? (int)active_slot : -1; }
static uint32_t read32(const uint8_t *p) { return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t read16(const uint8_t *p) { return p[0] | (uint16_t)p[1] << 8; }

/* Song endings. At song end the shared stage code (F_8001c8cc; the board's copy is identical) leaves solo and versus
   on the singing channel and switches co-op to channel 4/5 (COOL) or 6/7 (GOOD), the odd one when 0x80078b0c is 1
   (PaRappa co-op). Those endings are recorded on the arcade CD's /S1/ST1.XA, which has the console file's 8-channel
   layout sector for sector but its own endings from per-channel sector 1245 on (29 sectors longer). The JP file has
   console Lammy's endings there and the credits song on channel 7, so restored modes heard Lammy's solo line and
   the credits song. Before 1245 the files match except the censored vocal section (arcade = USA take), so the JP
   sectors and the censorship setting stay in charge there. Subheader and audio only: the header is the JP disc's
   sector address, which the game reads back as its song clock. */
bool arcade_disc_ready(void);
bool arcade_disc_find_file(const char *path, uint32_t *lba, uint32_t *size);
bool arcade_disc_read_sector(uint32_t lba, uint8_t out[2352]);
bool disc_find_file(const char *path, uint32_t *lba, uint32_t *size);
int cd_xa_streaming(void);
enum { XA_CHANNELS = 8, XA_ENDING = 1245 };
int arcade_stage1_xa_sector(uint32_t lba, uint8_t sec[2352]) {
    static int state;   /* 0 not checked, 1 usable, -1 not available */
    static uint32_t jp_lba, jp_n, ar_lba, ar_n;
    if (!restoring) return 0;
    if (!state) {
        uint32_t js, as;
        state = -1;
        if (!arcade_disc_ready() || !disc_find_file("S1/ST1.XA", &jp_lba, &js) ||
            !arcade_disc_find_file("S1/ST1.XA", &ar_lba, &as) || as < js) return 0;
        jp_n = js / 2048; ar_n = as / 2048;
        uint8_t a[2352];
        for (uint32_t i = 0; i < 64; i++)   /* same interleave: file 1, channel i % 8, audio */
            if (!arcade_disc_read_sector(ar_lba + i, a) || a[16] != 1 || a[17] != i % XA_CHANNELS || !(a[18] & 0x04)) {
                LOGI("arcade Stage 1: the arcade CD's ST1.XA layout is unexpected; keeping the console endings");
                return 0;
            }
        state = 1;
        LOGI("arcade Stage 1: song endings from the arcade CD (%u sectors, console %u)", ar_n, jp_n);
    }
    uint32_t i = lba - jp_lba;
    if (state < 0 || i >= ar_n || i < XA_ENDING * XA_CHANNELS) return 0;
    /* Past the JP file the drive reads the next file: serve the arcade tail only to the XA stream. */
    if (i >= jp_n && !cd_xa_streaming()) return 0;
    uint8_t a[2352];
    if (!arcade_disc_read_sector(ar_lba + i, a) || !(a[18] & 0x04)) return 0;
    if (i < jp_n && (a[16] != sec[16] || a[17] != sec[17])) return 0;
    memcpy(sec + 16, a + 16, 2352 - 16);
    return 1;
}

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
            } else if (type == 2 && !memcmp(entry + 4, "S1GG_SS.VH", 11)) {
                system_vh = console_sound_archive + at; system_vh_size = bytes;
            } else if (type == 2 && !memcmp(entry + 4, "S1GG_SS.VB", 11)) {
                system_vb = console_sound_archive + at; system_vb_size = bytes;
            }
            at += ((size_t)bytes + 3) & ~(size_t)3;
            if (at > end) goto invalid_bank;
        }
        section = end;
    }
    if (guitar_vh && guitar_vb && guitar_vh_size == 3616 && guitar_vb_size == 98880 &&
        read32(guitar_vh) == 0x56414270 && read16(guitar_vh + 22) == 20 &&
        system_vh && system_vb && system_vh_size == 3104 && system_vb_size == 238816 &&
        read32(system_vh) == 0x56414270 && read16(system_vh + 22) == 7) return true;
invalid_bank:
    free(console_sound_archive); console_sound_archive = NULL;
    guitar_vh = guitar_vb = system_vh = system_vb = NULL;
    guitar_vh_size = guitar_vb_size = system_vh_size = system_vb_size = 0;
    return false;
}

void hook_stage1_bank_load(CPU *c, uint32_t fn) {
    (void)fn;
    const char *overlay = rt_current_overlay_name();
    if (!restoring || active_slot == 0 ||
        mem_r32(0x80078ae0) != 1 || c->r[6] > 1 ||
        !overlay || strcmp(overlay, "COMOD1.SNG")) return;
    if (!read_guitar_bank()) { rt_fatal("arcade Stage 1: could not read the native guitar bank"); return; }
    if (c->r[6] == 0) {
        /* Bank 0 is the stage's system bank. CMP1SNG.INT has PaRappa's (S1RR_SS); Lammy's Stage 1 bank
           (S1GG_SS) is identical except for sample 6, the end-of-song failure line. */
        uint32_t header = rt_asset_alloc(system_vh_size), body = rt_asset_alloc(system_vb_size + 63u);
        if (!header || !body) { rt_fatal("arcade Stage 1: could not allocate the native system bank"); return; }
        memcpy(ram_ptr(header), system_vh, system_vh_size);
        memcpy(ram_ptr(body), system_vb, system_vb_size);
        c->r[4] = header; c->r[5] = body;
        return;
    }
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
    if (restoring && getenv("UJL_STAGE1_SOUND_LOG") && (seq < sequence_start || seq >= sequence_end))
        LOGI("arcade Stage 1 sound: sequence %08x a0 %08x a2 %08x from %08x", seq, c->r[4], c->r[6], c->r[31]);
    if (!restoring || seq < sequence_start || seq >= sequence_end ||
        ((seq - sequence_start) & 31)) return;
    unsigned slot = sequence_banks[(seq - sequence_start) / 32];
    if (getenv("UJL_STAGE1_SOUND_LOG"))
        LOGI("arcade Stage 1 sound: command %u from %08x", sequence_commands[(seq - sequence_start) / 32], c->r[31]);
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
        sequence_commands[made] = (uint8_t)command;
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
        /* VACUUM*.TIM: Lammy's and PaRappa's vacuum cleaner. Same image as Rammy's BL_VA*.TIM (same VRAM),
           with its own palette, so both can be uploaded. */
        bool selected = (pa && !strncmp(name, "PA", 2)) || (lammy && !strncmp(name, "LA", 2)) ||
                        (blue && !strncmp(name, "BL", 2)) || ((pa || lammy) && !strncmp(name, "VACUUM", 6));
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
        /* PaRappa's vacuum cleaner (PAV) starts hidden, as in the board's actor constructor. */
        c->r[4] = root; c->r[5] = 9; c->r[6] = 0;
        call_indirect(c, 0x801ca690);
    }
    player_models_ready = true; vacuums_shown = false;
    *c = saved;
    upload_player_textures();
    LOGI("arcade Stage 1: player models at %08x", root);
}

/* Console Lammy Stage 1 swaps her guitar for the vacuum cleaner from the final section (slot 896, her
   actor's animation 57) to the end of the song. COMOD1.SNG has no such switch; do it for every player:
   Lammy (vacuum 4, guitar 5), Rammy (13, 14) and PaRappa (vacuum 9; he has no guitar). */
static void update_vacuums(CPU *c) {
    static const struct { unsigned show, hide; } props[] = {{4, 5}, {13, 14}, {9, 0}};
    uint32_t root = mem_r32(0x801e45c4);
    if (!player_models_ready || !root) return;
    bool want = mem_r32(0x80078af8) / 24 >= 896;
    if (want == vacuums_shown) return;
    vacuums_shown = want;
    CPU saved = *c;
    for (unsigned i = 0; i < 3; i++) {
        bool present = i == 0 ? active_slot != 0 : i == 1 ? active_slot == 1 || active_slot == 5
                     : active_slot % 2 == 0;
        if (!present) continue;
        c->r[4] = root; c->r[5] = props[i].show; c->r[6] = want;
        call_indirect(c, 0x801ca690);
        if (props[i].hide) { c->r[4] = root; c->r[5] = props[i].hide; c->r[6] = !want; call_indirect(c, 0x801ca690); }
    }
    *c = saved;
}
void hook_stage1_scene_cues(CPU *c, uint32_t fn) {
    (void)fn;
    if (!restoring || !arcade_stage1_resources_ready()) return;
    uint32_t timeline = mem_r32(0x801dc364);
    if (!timeline) return;
    update_vacuums(c);
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
    /* Scene op 7 starts face texture programs (lip sync, blinks) by number. Without these the arcade
       numbers index COMOD1.SNG's own 43 programs and play the wrong mouth sequences. */
    ArcadeStage1Faces faces;
    if (!arcade_stage1_faces(active_slot, NULL, 0, 0, &faces)) { rt_fatal("arcade Stage 1: %s", SDL_GetError()); return; }
    uint32_t face_buffer = rt_asset_alloc(faces.size);
    if (!face_buffer) { rt_fatal("arcade Stage 1: could not allocate host memory for face programs"); return; }
    if (!arcade_stage1_faces(active_slot, ram_ptr(face_buffer), faces.size, face_buffer, &faces)) {
        rt_fatal("arcade Stage 1: %s", SDL_GetError()); return;
    }
    mem_w32(timeline + 1992, faces.table);
    mem_w32(timeline + 2004, faces.count);
    uint32_t table = mem_r32(timeline + 2244);
    mem_w32(timeline + 128, mem_r32(table + 8));
    mem_w32(timeline + 132, mem_r32(table + 12));
    mem_w32(timeline + 140, 0); mem_w32(timeline + 144, 1); mem_w32(timeline + 148, 0);
    LOGI("arcade Stage 1: scene cues, %zu bytes at %08x; %u face programs; timeline %08x", scene.size, buffer,
         faces.count, timeline);
}
/* Lyric tracks: one per language, descriptor {0, end 0x6600, records}; records {duration, end, text}.
   COMOD1.SNG carries PaRappa's lines. Modes with Lammy sing Lammy's vocal channel, so point each COMOD1.SNG
   track at a copy of the matching COMOD11.SNG track (read through the disc patch layer, so the censored-lyrics
   setting applies; its replacement lines live in guest kernel RAM). */
static uint8_t *lammy_overlay;
static uint32_t lammy_overlay_size;
static bool lyric_text(uint32_t ptr, const uint8_t **text, size_t *len) {
    const uint32_t base = 0x801c5110;
    const uint8_t *s;
    size_t limit;
    if (ptr >= base && ptr - base < lammy_overlay_size) { s = lammy_overlay + (ptr - base); limit = lammy_overlay_size - (ptr - base); }
    else if ((ptr & 0x1fffff) < 0x10000 && (ptr >> 24) == 0x80) { s = ram_ptr(ptr); limit = 0x10000 - (ptr & 0x1fffff); }
    else return false;
    for (size_t i = 0; i < limit && i < 200; i++) {
        if (!s[i]) { *text = s; *len = i; return true; }
        if ((s[i] < 0x20 && s[i] != '\n') || s[i] >= 0x7f) return false;
    }
    return false;
}
static void bind_lammy_lyrics(void) {
    static const uint32_t pa_tracks[] = {0x801deed4, 0x801df87c, 0x801e0178, 0x801e0a98, 0x801e13b8, 0x801e2720};
    static const uint32_t la_tracks[] = {0x801e1af4, 0x801e238c, 0x801e2b50, 0x801e3338, 0x801e3b20, 0x801e4d10};
    const uint32_t base = 0x801c5110;
    if (!lammy_overlay && !disc_read_file("/S1/COMOD11.SNG", &lammy_overlay, &lammy_overlay_size)) {
        rt_fatal("arcade Stage 1: could not read the native Lammy lyrics"); return;
    }
    for (unsigned t = 0; t < 6; t++) {
        uint32_t pa = pa_tracks[t], la = la_tracks[t] - base;
        if (la + 12 > lammy_overlay_size || read32(lammy_overlay + la) || read32(lammy_overlay + la + 4) != 0x6600 ||
            mem_r32(pa) || mem_r32(pa + 4) != 0x6600) { rt_fatal("arcade Stage 1: unexpected lyric layout"); return; }
        uint32_t records = read32(lammy_overlay + la + 8) - base;
        unsigned n = 0; size_t text_bytes = 0;
        for (;; n++) {
            size_t at = records + n * 12u, len;
            const uint8_t *text;
            if (n == 200 || at + 12 > lammy_overlay_size || !lyric_text(read32(lammy_overlay + at + 8), &text, &len)) {
                rt_fatal("arcade Stage 1: invalid Lammy lyrics"); return;
            }
            text_bytes += (len + 4) & ~(size_t)3;
            if (read32(lammy_overlay + at + 4) >= 0x6600) { n++; break; }
        }
        uint32_t out = rt_asset_alloc(n * 12u + text_bytes);
        if (!out) { rt_fatal("arcade Stage 1: could not allocate Lammy lyrics"); return; }
        uint32_t text_at = out + n * 12u;
        for (unsigned i = 0; i < n; i++) {
            const uint8_t *record = lammy_overlay + records + i * 12u, *text;
            size_t len;
            lyric_text(read32(record + 8), &text, &len);
            mem_w32(out + i * 12, read32(record)); mem_w32(out + i * 12 + 4, read32(record + 4));
            mem_w32(out + i * 12 + 8, text_at);
            memcpy(ram_ptr(text_at), text, len); mem_w8(text_at + len, 0);
            text_at += (len + 4) & ~(size_t)3;
        }
        mem_w32(pa + 8, out);
    }
}
/* The note bar does not draw the scoring chart. It shows a static whole-song grid from the overlay,
   whose header also names the two players' characters (icons and line colours). COMOD1.SNG's own grid is
   an unfinished placeholder: one button for every note, PaRappa against the teacher. Replace it in place. */
static void bind_bar_grid(CPU *c, unsigned mode) {
    enum { COMOD1_BAR_GRID = 0x801e29ac, BAR_LINE = 0x801acaa0 };
    size_t size;
    const uint8_t *grid = arcade_stage1_bar_grid(mode, &size);
    if (!grid || mem_r16(COMOD1_BAR_GRID) != 0x0b02 || mem_r16(COMOD1_BAR_GRID + 2) != read16(grid + 2)) {
        rt_fatal("arcade Stage 1: note bar layout does not match"); return;
    }
    memcpy(ram_ptr(COMOD1_BAR_GRID), grid, size);
    /* Installing the line resolves the header's character sets; redo it if the stage already has. */
    if (mem_r32(BAR_LINE) == COMOD1_BAR_GRID) {
        CPU saved = *c;
        c->r[4] = COMOD1_BAR_GRID;
        call_indirect(c, 0x800294c8);
        *c = saved;
    }
    LOGI("arcade Stage 1: note bar for slot %u, characters %u/%u", mode, read16(grid + 4), read16(grid + 6));
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
    /* AUTO (COM) players: the board files carry no usable COM performance (placeholder or solo filler
       lines), and COMOD1.SNG installs its schedules for player 1 only. Use the native chart player instead
       (source 3, as in the attract demo): it plays the notes the chart tags with that player's number. */
    for (unsigned player = 0; player < 2; player++) {
        uint32_t source = core + 64 + player * 96;
        if (mode % 3 && mem_r32(source) >= 2 && mem_r32(source) <= 4) mem_w32(source, 3);
    }
    mem_w32(core + 324, data.chart); mem_w32(core + 328, data.chart);
    if (rap_overlay && !bind_stage_sounds(guitar_header)) rt_fatal("arcade Stage 1: native sound command mapping failed");
    if (rap_overlay) bind_bar_grid(c, mode);
    if (rap_overlay && mode != 0) bind_lammy_lyrics();
    LOGI("arcade Stage 1: chart slot %u, %u notes, %zu bytes at %08x", mode, data.chart_notes, data.size, buffer);
}
