/* Autoplay and Auto-COOL (settings; UJL_AUTOPLAY=1/2 in the probe, UJL_AUTOPLAY_LOG=1 logs presses and results).
 *
 * Both play the stage through the controller: they press buttons at the right moments and the game judges them like
 * any player's presses (rt_pad_state ORs them in, so the game's own pad driver reads them).
 *
 * What the game keeps (main executable, shared by every stage):
 *   song time      0x80078af8 (song_time_get); one chart slot = 24 time units (stage_frame: slot = time / 24). The game
 *                  logic runs every second vblank: it advances the song time by 5-7 units, then reads the pad and
 *                  stamps each press with the new time (player_read_input -> tick_grid_record records it in a tick grid at
 *                  0x801a2410, 4 time units per tick).
 *   chart          4-byte records {s16 slot, u8 button id, u8 note count}, sorted by slot, cursor at 0x80071df0.
 *                  A record is a note the player is expected to play at that slot; flags 0x1000 in the u16 at +2 mark
 *                  non-notes; the first record of a line carries its note count. Ids 1-6 = Triangle, Circle, Cross,
 *                  Square, L1, R1 (table 0x8005b848).
 *   answer window  player 1: slots 0x80071ccc..0x80071cce (32767 = closed). It opens a few slots before the line's
 *                  first note and is scored when it closes (answer_window_close -> answer_window_score -> phrase_score).
 *   phrase score   0x80071cd8, COOL threshold 0x80071cda (only valid at the moment of rating, phrase_rate): no
 *                  presses / <= 0 bad, < threshold good, >= threshold cool; meter 0x80071dc0 (0-2 COOL, 3-6 GOOD,
 *                  7-10 BAD, 11+ AWFUL).
 * The score (phrase_score, tally at 0x801a2750): +3 per press of a button the line uses, within ~8 time units of a slot;
 * minus the note count if the first press isn't the line's first note; a penalty per press of another button;
 * -1.5 per press more or fewer than the line's note count; 0 unless every button of the line was pressed; plus a
 * rhythm bonus (rhythm_bonus): each pair of slots (even, odd) is classed as "odd only", "both", "even only" or "empty"
 * (table 0x8005b770); a group that has shown all four pays 6 + 9 + 15 + 18 = 48 (table 0x8005b774), and groups repeat.
 * An exact copy scores 3 per note, which stays GOOD (the threshold is ~40). Auto-COOL keeps the chart notes and adds
 * presses (with the line's buttons) chosen by rhythm_fill: the most the game would score, preferring the beat. */
#include "psx.h"
#include "arcade_stage1.h"
#include "recomp.h"
#include "replay_files.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int g_autoplay;            /* 0 off, 1 Autoplay (follows the chart, holds GOOD), 2 Auto-COOL, 3 Auto-FEVER */
int g_autoplay_used;       /* the current (or last) song was played with either on at some point: its score isn't kept */

#define SONG_TIME   0x80078af8u
#define CHART_CUR   0x80071df0u   /* shared by both players */
#define PLAY_MODE   0x80078aecu   /* 0 solo, 1 versus (a meter per player), 2 co-op (one shared meter) */
#define ACTIVE_MASK 0x80071db8u   /* bit p: player p takes part */
#define SLOT_UNITS  24
/* Player p's block: 0x80071cb8 + 96p. +20/+22 answer window, +48 input source (0/1 = controller port, 2-4 = the
   game's COM player, 5 = nobody; player_read_input), +55 fever bits, +57 fever ring, +92 fever table. Phrase score at
   0x80071cd8 + 96p. Meter: player struct 0x80071dbc (+4), one per player in versus (+10 each, player_meter). */
#define PBLOCK(p)   (0x80071cb8u + 96u * (uint32_t)(p))
#define WIN_START(p) (PBLOCK(p) + 20u)
#define WIN_END(p)  (PBLOCK(p) + 22u)
#define SOURCE(p)   (PBLOCK(p) + 48u)
static uint32_t meter_addr(int p) { return mem_r32(PLAY_MODE) == 1 ? 0x80071dc0u + 10u * (uint32_t)p : 0x80071dc0u; }
#define NO_WINDOW   32767
#define MAX_PLAN    96

static const uint16_t id_to_pad[7] = {0, 1u << 12, 1u << 13, 1u << 14, 1u << 15, 1u << 10, 1u << 11};

typedef struct { int16_t slot; uint8_t id; uint8_t done; } Press;
typedef struct {
    Press plan_[MAX_PLAN];
    int nplan_, ws, we, hold_frames, fever_next_;
    uint16_t hold_mask;
    int16_t last_meter;
    int last_fever;
    /* timing calibration: the stamp the game recorded for our last press vs our prediction */
    int pend_slot, pend_id, pend_frames;
    int32_t pend_pred;                 /* predicted stamp */
    float corr;                        /* running correction added to the prediction */
} Player;
static Player players[2];
static Player *P = &players[0];   /* the player being planned */
static int cur_p;                 /* its index */
#define plan  (P->plan_)
#define nplan (P->nplan_)
static int log_on = -1;
static int skip_post_saves;       /* hook_menu_screen: an autoplayed song just ended */

static int in_song(void) {
    const char *ov = rt_current_overlay_name();
    if (!ov) return 0;
    size_t n = strlen(ov);
    if (n < 4 || !strncmp(ov, "COMOD8", 6)) return 0;   /* the staff roll */
    return !strcmp(ov + n - 4, ".SNG") || !strcmp(ov + n - 4, ".CMP") || !strcmp(ov + n - 4, ".COP");
}

static int guest_ptr_ok(uint32_t p) {
    if (p & 3) return 0;
    return (p >= 0x80010000u && p < 0x801FFFF0u) ||
           (g_asset_ram_capacity >= 16 && phys_addr(p) - HOST_ASSET_BASE < g_asset_ram_capacity - 16);
}

static void plan_add(int slot, int id) {
    for (int i = 0; i < nplan; i++) if (plan[i].slot == slot) return;
    if (nplan < MAX_PLAN) plan[nplan++] = (Press){(int16_t)slot, (uint8_t)id, 0};
}

/* Musical weight of an added press: the chart's beat is 4 slots (lines put their notes on multiples of 4, 8ths on
   multiples of 2), so presses on the beat and on the "and" are preferred, the 16th leading into a beat is tolerated,
   the 16th after a beat is avoided unless the rhythm bonus needs it. */
static float beat_weight(int slot) {
    static const float w[4] = {2.0f, -3.5f, 1.0f, -3.0f};
    return w[slot & 3];
}

/* Auto-COOL's added presses for window [ws, we] (chart notes in note_id[], first note `first`, `notes` of them; first
   < 0 = COOL freestyle without chart notes). A search over the slot pairs, each set to one of the scorer's four
   patterns (odd only, both, even only, empty) where the chart notes allow it, maximising what the game would score
   (per added press +3, -1.5 for exceeding the note count in a line; -notes once for any press before the line's first
   note; 48 per group of all four patterns, the partial-group bonus at the end) plus the musical weight above. */
static void rhythm_fill(int ws, int we, const uint8_t *note_id, int first, int notes) {
    enum { MAXP = 260, NS = 32 };
    static const int tbl[4] = {6, 9, 15, 18};
    static float val[MAXP + 1][NS];
    static int8_t from[MAXP + 1][NS], pick[MAXP + 1][NS];
    const int freestyle = first < 0;
    const int p0 = ws & ~1, np = (we - p0) / 2 + 1;
    if (np > MAXP) return;
    for (int i = 0; i <= np; i++) for (int s = 0; s < NS; s++) val[i][s] = -1e9f;
    val[0][0] = 0;
    for (int i = 0; i < np; i++) {
        int ev = p0 + 2 * i, od = ev + 1;
        int ev_ok = ev >= ws && ev < we, od_ok = od < we;   /* the window is scored as its last slot starts: a press there is too late */
        int fe = ev_ok && note_id[ev - ws], fo = od_ok && note_id[od - ws];
        for (int k = 0; k < 4; k++) {
            int wantE = k == 1 || k == 2, wantO = k == 0 || k == 1;
            if ((fe && !wantE) || (fo && !wantO) || (wantE && !ev_ok) || (wantO && !od_ok)) continue;
            int addE = wantE && !fe, addO = wantO && !fo, pre = 0;
            float gain = 0;
            if (addE) { gain += (freestyle ? 3.0f : 1.5f) + beat_weight(ev); pre |= !freestyle && ev < first; }
            if (addO) { gain += (freestyle ? 3.0f : 1.5f) + beat_weight(od); pre |= !freestyle && od < first; }
            for (int s = 0; s < NS; s++) {
                if (val[i][s] <= -1e8f) continue;
                int bits = (s & 15) | 1 << k, was_pre = s >> 4;
                float g = gain - (pre && !was_pre ? (float)notes : 0.0f);
                if (bits == 15) { g += 48; bits = 0; }
                int ns = bits | (was_pre | pre) << 4;
                if (val[i][s] + g > val[i + 1][ns]) { val[i + 1][ns] = val[i][s] + g; from[i + 1][ns] = (int8_t)s; pick[i + 1][ns] = (int8_t)k; }
            }
        }
    }
    int best = -1;
    float bestv = -1e9f;
    for (int s = 0; s < NS; s++) {
        if (val[np][s] <= -1e8f) continue;
        int n = 0, b = 0;
        for (int k = 0; k < 3; k++) if (s >> k & 1) { b += tbl[k]; n++; }
        float v = val[np][s] + (n >= 2 ? (float)(b + (n == 3 ? tbl[3] : 0)) : 0.0f);
        if (v > bestv) { bestv = v; best = s; }
    }
    if (best < 0) return;
    static int8_t chosen[MAXP];
    for (int i = np, s = best; i > 0; i--) { chosen[i - 1] = pick[i][s]; s = from[i][s]; }
    /* buttons: in a line, the most recent chart note's (a button the line uses); in the freestyle, all six in turn */
    int cur = freestyle ? 0 : note_id[first - ws], btn = 0;
    for (int i = 0; i < np; i++) {
        int ev = p0 + 2 * i, od = ev + 1, k = chosen[i];
        if (ev >= ws && note_id[ev - ws]) cur = note_id[ev - ws];
        if ((k == 1 || k == 2) && !(ev >= ws && note_id[ev - ws])) plan_add(ev, freestyle ? 1 + btn++ % 6 : cur);
        if (od <= we && note_id[od - ws]) cur = note_id[od - ws];
        if ((k == 0 || k == 1) && !(od <= we && note_id[od - ws])) plan_add(od, freestyle ? 1 + btn++ % 6 : cur);
    }
}

/* Auto-FEVER: in a COOL window the stage script hands player 1 a fever table (script_set_fever_table copies it from the stage's
   list into 0x80071d14 = block +92; 0 outside COOL windows). It holds six 32-byte records, one per fever level
   (colour): {u16 1, u16 100, u16 30, s16 -15, s16 -50, u8 code[8], u8 0xff[8], u16 0, u32 next} with code bytes
   0xF0 | button id and 0 for a rest; they're the rows the loading screen shows with "Unlock all". Each on-time press
   writes its button id into a 32-slot ring (block +57, slot mod 32; fever_ring_press), and at every slot boundary
   fever_check compares the last 8 slots with each code (low nibbles; a rest needs an empty slot). A match sets fever
   bit k (block +55), refreshes its 4-phrase timer (block +90) and clears the ring. So a code is 8 consecutive slots,
   one step per 16th note. The codes are played back to back from the window's first beat, cycling through all six
   so the effects stack; any slots left over get the rhythm fill. */
#define FEVER_TABLE(p) (PBLOCK(p) + 92u)
#define FEVER_BITS(p)  (PBLOCK(p) + 55u)
#define fever_next (P->fever_next_)
static int fever_fill(int ws, int we) {
    uint32_t t = mem_r32(FEVER_TABLE(cur_p));
    if (!guest_ptr_ok(t)) return 0;
    /* The player's fever turn: block +12/+14 hold the slot range [start, end) in which its presses are filed into the
       fever ring (stage_frame calls fever_ring_press only for a slot below +14), 32767 outside it. Set when the window opens.
       In versus the COOL windows alternate between the players (Stage 6: player 1 368, 432, 496; player 2 400, 464),
       so codes entered in the other player's window were lost. */
    int f0 = (int16_t)mem_r16(PBLOCK(cur_p) + 12u), f1 = (int16_t)mem_r16(PBLOCK(cur_p) + 14u);
    if (f0 == NO_WINDOW || f1 == NO_WINDOW) return 0;
    if (we > f1 - 1) we = f1 - 1;
    if (ws < f0) ws = f0;
    if (we - ws < 8) return 0;
    uint8_t codes[6][16];
    int len[6];
    for (int k = 0; k < 6; k++) {
        len[k] = 0;
        for (int i = 0; i < 16; i++) {
            uint8_t b = mem_r8(t + 32u * (uint32_t)k + 10u + (uint32_t)i);
            if (b == 0xFF) break;
            codes[k][i] = b & 0x0F;
            len[k] = i + 1;
        }
        if (len[k] == 0) return 0;   /* not a fever table */
        for (int i = 0; i < len[k]; i++) if (codes[k][i] > 6) return 0;
    }
    /* From the window's second beat: a code starting on the window's first slot can miss (its leading rest isn't read
       as empty when the window opens; seen with Stage 1's L1 code at slot 320). The first beat gets one press. */
    /* Order: the fevers not running yet first (bits at block +55; a code that missed, e.g. on a jump of the song clock,
       comes back in the next window), then the rest in turn, which refreshes their timers. */
    /* Each player has its own six fevers (versus and co-op: both reach all six). An earlier split of the colours
       between the players worked around codes lost outside the player's fever turn (see above). */
    int order[6], n = 0, active = mem_r8(FEVER_BITS(cur_p));
    for (int j = 0; j < 6; j++) { int k = (fever_next + j) % 6; if (!(active >> k & 1)) order[n++] = k; }
    for (int j = 0; j < 6; j++) { int k = (fever_next + j) % 6; if (active >> k & 1) order[n++] = k; }
    int s = (ws + 3) & ~3;
    if (s - ws < 2) { plan_add(s, codes[order[0]][1] ? codes[order[0]][1] : 1); s += 4; }
    int prev_end = -100;
    for (int j = 0; j < n; j++) {
        int k = order[j];
        /* After a match the game resets the ring during the next slot, so a code that starts with a press (not a
           rest) right after another code can lose that press: give it a beat of rest first. */
        if (codes[k][0] && s == prev_end) s += 4;
        if (s + len[k] - 1 > we) break;
        for (int i = 0; i < len[k]; i++) if (codes[k][i]) plan_add(s + i, codes[k][i]);
        s += len[k];
        prev_end = s;
        fever_next = (k + 1) % 6;
    }
    return nplan > 0;
}

/* Auto-COOL's plan for a line (window [ws, we] with chart notes; replaces the chart notes in the plan). The scorer
   (phrase_score) doesn't require each chart note: it wants the first press to be the line's first note (slot and
   button), every button the line uses pressed at least once, the right number of presses (-1.5 for each one more or
   fewer than the line's note count), +3 per press of a line button on time, and pays the rhythm bonus per group of
   slot pairs showing all four patterns. Lines that fill every slot with notes leave no room for "empty" or "odd only"
   pairs, so the search may leave notes out or move them. It keeps the first note, presses nothing before it, and
   maximises the game's score plus the beat weights, a little for each chart note kept (+0.5) and a little against
   each one left out (-0.5). Buttons: a chart note's own where one is kept, else the line's most recent one; then any
   line button left unpressed is given to a press that was going to repeat another, so every one appears. */
static void line_plan(int ws, int we, const uint8_t *note_id, int first, int notes) {
    enum { MAXP = 130, MAXT = 64 };
    static const int tbl[4] = {6, 9, 15, 18};
    static float val[MAXP + 1][16][MAXT];
    static int8_t pick[MAXP + 1][16][MAXT];
    static uint8_t from_bits[MAXP + 1][16][MAXT], from_t[MAXP + 1][16][MAXT];
    const int p0 = ws & ~1, np = (we - p0) / 2 + 1;   /* the whole window: pairs before the first note count as empty */
    if (np > MAXP || np <= 0) return;
    for (int i = 0; i <= np; i++) for (int b = 0; b < 16; b++) for (int t = 0; t < MAXT; t++) val[i][b][t] = -1e9f;
    val[0][0][0] = 0;
    for (int i = 0; i < np; i++) {
        int ev = p0 + 2 * i, od = ev + 1;
        int ev_ok = ev >= first && ev < we, od_ok = od >= first && od < we;   /* none on the last slot (scored as it starts) */
        int ce = ev >= ws && ev <= we && note_id[ev - ws], co = od <= we && note_id[od - ws];
        for (int k = 0; k < 4; k++) {
            int wantE = k == 1 || k == 2, wantO = k == 0 || k == 1;
            if ((wantE && !ev_ok) || (wantO && !od_ok)) continue;
            if ((ev == first && !wantE) || (od == first && !wantO)) continue;   /* the first note stays */
            float gain = 0;
            int add = 0;
            if (wantE) { gain += 3.0f + beat_weight(ev) + (ce ? 0.5f : 0.0f); add++; } else if (ce) gain -= 0.5f;
            if (wantO) { gain += 3.0f + beat_weight(od) + (co ? 0.5f : 0.0f); add++; } else if (co) gain -= 0.5f;
            for (int b = 0; b < 16; b++) for (int t = 0; t + add < MAXT; t++) {
                if (val[i][b][t] <= -1e8f) continue;
                int nb = b | 1 << k;
                float g = gain;
                if (nb == 15) { g += 48; nb = 0; }
                float v = val[i][b][t] + g;
                if (v > val[i + 1][nb][t + add]) {
                    val[i + 1][nb][t + add] = v; pick[i + 1][nb][t + add] = (int8_t)k;
                    from_bits[i + 1][nb][t + add] = (uint8_t)b; from_t[i + 1][nb][t + add] = (uint8_t)t;
                }
            }
        }
    }
    int bb = -1, bt = 0;
    float bestv = -1e9f;
    for (int b = 0; b < 16; b++) for (int t = 0; t < MAXT; t++) {
        if (val[np][b][t] <= -1e8f) continue;
        int n = 0, s = 0;
        for (int k = 0; k < 3; k++) if (b >> k & 1) { s += tbl[k]; n++; }
        int d = t > notes ? t - notes : notes - t;
        float v = val[np][b][t] + (n >= 2 ? (float)(s + (n == 3 ? tbl[3] : 0)) : 0.0f) - (float)(d + (d + 1) / 2);
        if (v > bestv) { bestv = v; bb = b; bt = t; }
    }
    if (bb < 0) return;
    static int8_t chosen[MAXP];
    for (int i = np, b = bb, t = bt; i > 0; i--) {
        chosen[i - 1] = pick[i][b][t];
        int pb = from_bits[i][b][t], pt = from_t[i][b][t];
        b = pb; t = pt;
    }
    nplan = 0;
    int cur = note_id[first - ws];
    for (int i = 0; i < np; i++) {
        int ev = p0 + 2 * i, od = ev + 1, k = chosen[i];
        int ce = ev >= ws && note_id[ev - ws], co = od <= we && note_id[od - ws];
        if (ce) cur = note_id[ev - ws];
        if (k == 1 || k == 2) plan_add(ev, ce ? note_id[ev - ws] : cur);
        if (co) cur = note_id[od - ws];
        if (k == 0 || k == 1) plan_add(od, co ? note_id[od - ws] : cur);
    }
    /* every button of the line at least once */
    int used = 0, need = 0;
    for (int i = 0; i < we - ws + 1; i++) if (note_id[i]) need |= 1 << note_id[i];
    for (int i = 0; i < nplan; i++) used |= 1 << plan[i].id;
    for (int id = 1; id <= 6; id++) {
        if (!(need >> id & 1) || (used >> id & 1)) continue;
        for (int i = 0; i < nplan; i++) {   /* a press whose button appears again elsewhere, not the first note */
            if (plan[i].slot == first) continue;
            int dup = 0;
            for (int j = 0; j < nplan; j++) if (j != i && plan[j].id == plan[i].id) dup = 1;
            if (dup) { plan[i].id = (uint8_t)id; used |= 1 << id; break; }
        }
    }
}

/* The presses for answer window [ws, we]: the chart's notes, plus (Auto-COOL) the rhythm fill. */
static void make_plan(int ws, int we) {
    nplan = 0;
    uint32_t r = mem_r32(CHART_CUR);
    if (!guest_ptr_ok(r)) return;
    int first = -1, notes = 0;
    static uint8_t note_id[512];   /* button of the chart note at slot (ws + i), 0 = none */
    int span = we - ws + 1;
    if (span <= 0 || span > 512) return;
    memset(note_id, 0, (size_t)span);
    for (int guard = 0; guard < 1024; guard++, r += 4) {
        int16_t s = (int16_t)mem_r16(r);
        uint16_t code = mem_r16(r + 2);
        if (s > we) break;
        if (s < ws || (code & 0xF000) == 0x1000) continue;
        uint8_t id = (uint8_t)code;
        if (id < 1 || id > 6) continue;
        plan_add(s, id);
        note_id[s - ws] = id;
        if (first < 0) first = s;
        notes++;
    }
    if (g_autoplay < 2) return;
    /* COOL freestyle (no chart notes): the teacher has left and the scorer's no-teacher path takes any button (+3 per
       press, no note-count penalty, the rhythm bonus; presses >= window length + 3 void it). Only in COOL: outside
       it an empty window would score every press as a wrong button. */
    int cool = (int16_t)mem_r16(meter_addr(cur_p)) <= 2;
    /* Versus COOL turns: the windows open for both players, but only the player whose turn it is has a fever range
       (block +12). The other one rests: its freestyle presses ran up a score lead that ended Stage 6 versus early. */
    if (mem_r32(PLAY_MODE) == 1 && first < 0 && (int16_t)mem_r16(PBLOCK(cur_p) + 12u) == NO_WINDOW
        && (int16_t)mem_r16(PBLOCK(cur_p ^ 1) + 12u) != NO_WINDOW) return;
    if (first < 0 && !cool) return;
    /* Fever codes: the fever check runs whenever the game has handed the player a fever table, and at COOL the scorer
       takes its no-teacher path (any button counts, phrase_score with rating 0), so the codes can replace a line's
       notes. Needed in PaRappa mode and versus, where the teacher keeps giving lines during COOL. */
    if (g_autoplay == 3 && cool && guest_ptr_ok(mem_r32(FEVER_TABLE(cur_p)))) {
        Press keep[MAX_PLAN];
        int nkeep = nplan;
        memcpy(keep, plan, sizeof keep);
        nplan = 0;
        if (fever_fill(ws, we)) return;
        nplan = nkeep;
        memcpy(plan, keep, sizeof keep);
    }
    if (first >= 0) line_plan(ws, we, note_id, first, notes);
    else rhythm_fill(ws, we, note_id, first, notes);
}

/* One player's buttons for this vblank (active high): plans each answer window when it opens, then presses on time.
   Press timing: the game logic runs every second vblank; it advances the song time, then reads the pad and stamps a
   press with the new time. The steps come in repeating pairs whose sum follows the tempo (Stage 1 7, 5; Stage 6 5, 8;
   now and then 7, 6, 7), so the next step is predicted as the one before the last (12 minus the last was a unit or
   more off in Stage 6). The scorer rounds (tick of t + 12) and accepts about +-8 units around a slot, but the fever ring files a
   press under floor(t / 24) and resets itself on a press outside tick 1-4: a fever press must be stamped in
   [slot * 24, slot * 24 + 7]. So the press goes on the logic frame whose predicted stamp is closest to the middle of
   that range, slot * 24 + 3.5. */
static int32_t t_now;
static int step_last = 6, step_prev = 6;
static uint32_t g_gpu_frame_now;
static uint16_t player_buttons(int p, int port) {
    P = &players[p];
    cur_p = p;
    int ws = (int16_t)mem_r16(WIN_START(p)), we = (int16_t)mem_r16(WIN_END(p));
    if (log_on) {
        int16_t m = (int16_t)mem_r16(meter_addr(p));
        if (m != P->last_meter) rt_log("autoplay: P%d meter %d", p + 1, m);
        P->last_meter = m;
        int fb = mem_r8(FEVER_BITS(p));
        if (fb != P->last_fever) rt_log("autoplay: P%d fever bits %02x at slot %d", p + 1, fb, t_now / SLOT_UNITS);
        P->last_fever = fb;
    }
    if (ws != NO_WINDOW && (ws != P->ws || we != P->we)) {
        P->ws = ws; P->we = we;
        make_plan(ws, we);
        if (log_on) {
            char b[512]; int k = 0; b[0] = 0;
            for (int i = 0; i < nplan && k < 480; i++) k += snprintf(b + k, sizeof b - (size_t)k, " %d:%d", plan[i].slot, plan[i].id);
            rt_log("autoplay: P%d window %d..%d, %d presses:%s", p + 1, ws, we, nplan, b);
        }
    }
    /* Calibration: find the stamp the game recorded for the last press in this player's tick grid (0x801a2410 +
       396p, 6 + ((stamp + 12) / 4) mod 384 holds the button id; tick_grid_record) and fold the difference from the
       prediction into a running correction. Solo stamps came out ~6 units after the decision (the next logic step),
       real 2P co-op stamps at about the decision time, so a fixed prediction filed 2P fever presses a slot early. */
    if (P->pend_frames > 0) {
        uint32_t grid = 0x801a2410u + 396u * (uint32_t)p;
        int e = (((P->pend_slot * SLOT_UNITS + 12) >> 2) % 384) + 6, best = -1;
        for (int d = 0; d <= 6 && best < 0; d++)
            for (int sg = -1; sg <= 1 && best < 0; sg += 2) {
                int k = e + sg * d;
                if (k >= 6 && k < 390 && (mem_r8(grid + (uint32_t)k) & 0x3F) == P->pend_id) best = k;
            }
        if (best >= 0) {
            int32_t stamp = (best - 6) * 4 - 12 + 2;                 /* the middle of the 4-unit tick */
            int32_t pred = ((P->pend_pred % 1536) + 1536) % 1536;
            int32_t diff = ((stamp - pred) % 1536 + 1536 + 768) % 1536 - 768;
            if (diff > -16 && diff < 16) P->corr += 0.3f * (float)diff;
            if (P->corr < -12) P->corr = -12;
            if (P->corr > 6) P->corr = 6;
            if (log_on > 1) rt_log("autoplay: P%d slot %d stamped %+d vs prediction, correction %.1f", p + 1, P->pend_slot, diff, P->corr);
            P->pend_frames = 0;
        } else P->pend_frames--;
    }
    if (P->hold_frames > 0) { P->hold_frames--; return P->hold_mask; }
    if (ws == NO_WINDOW) return 0;
    int est = step_prev;
    if (est < 4) est = 4;
    if (est > 9) est = 9;
    int32_t corr = (int32_t)(P->corr + (P->corr >= 0 ? 0.5f : -0.5f));
    int32_t stamp_now = t_now + est + corr, stamp_after = stamp_now + step_last; (void)port;
    for (int i = 0; i < nplan; i++) {
        Press *q = &plan[i];
        if (q->done) continue;
        int32_t target = q->slot * SLOT_UNITS + 2;
        if (t_now > target + SLOT_UNITS) { q->done = 1; continue; }   /* missed (joined mid-window) */
        /* Distances in half units to the middle of the fever ring's range (slot * 24 + 3.5): the prediction is good to
           about +-2 units, so aiming at + 2 filed some presses a slot early (Stage 6 versus: stamped 9092 for slot
           379, which starts at 9096, breaking the code). */
        int32_t mid2 = 2 * q->slot * SLOT_UNITS + 7;
        int32_t dn = 2 * stamp_now - mid2, da = 2 * stamp_after - mid2;
        if (dn < 0) dn = -dn;
        if (da < 0) da = -da;
        if (2 * stamp_now < mid2 && da < dn) continue;                /* the next logic frame lands closer */
        if (stamp_now < target - SLOT_UNITS / 2) continue;
        q->done = 1;
        if (log_on > 1) rt_log("autoplay: P%d press slot %d at t %d (vblank %u)", p + 1, q->slot, t_now, (unsigned)g_gpu_frame_now);
        P->pend_slot = q->slot; P->pend_id = q->id; P->pend_pred = stamp_now; P->pend_frames = 10;
        P->hold_mask = id_to_pad[q->id];
        P->hold_frames = 1;                  /* this vblank and the next: one logic step sees it */
        return P->hold_mask;
    }
    return 0;
}

/* Called from rt_pad_state for each controller port; the work is done once per vblank. Each player whose input
   source is a controller port (0 or 1) is played on that port; players the game drives itself (its COM player,
   source 2-4: versus/co-op against the computer, and both players of the attract demo) and absent players (5) are
   left alone. Returns the buttons to hold on `port` (active high). */
uint16_t autoplay_buttons(int port) {
    static uint32_t last_frame = UINT32_MAX;
    static uint16_t out[2];
    extern uint32_t g_gpu_frame;
    port &= 1;
    if (g_gpu_frame == last_frame) return out[port];
    g_gpu_frame_now = g_gpu_frame;
    last_frame = g_gpu_frame;
    out[0] = out[1] = 0;
    if (log_on < 0) {
        const char *e = getenv("UJL_AUTOPLAY");
        if (e) g_autoplay = atoi(e);
        { const char *l = getenv("UJL_AUTOPLAY_LOG"); log_on = l ? atoi(l) > 1 ? 2 : 1 : 0; }
    }
    int song = in_song();
    {   /* a new song starts: forget whether the last one was autoplayed (the flag outlives the song for its results
           and name entry, where hook_hiscore_insert reads it) */
        static int was_in_song;
        if (song && !was_in_song) g_autoplay_used = 0;
        if (!song && was_in_song && g_autoplay_used) skip_post_saves = 1;   /* hook_menu_screen */
        was_in_song = song;
    }
    if (!g_autoplay || !song || !guest_ptr_ok(mem_r32(CHART_CUR))) {
        for (int p = 0; p < 2; p++) { players[p].ws = players[p].we = -1; players[p].nplan_ = 0; players[p].hold_frames = 0; players[p].corr = 0; players[p].pend_frames = 0; }
        return 0;
    }
    static int32_t t_last = INT32_MIN;
    t_now = (int32_t)mem_r32(SONG_TIME);
    if (t_now != t_last) {
        if (t_last != INT32_MIN && t_now > t_last && t_now - t_last < 16) { step_prev = step_last; step_last = t_now - t_last; }
        t_last = t_now;
    }
    uint32_t active = mem_r32(ACTIVE_MASK);
    for (int p = 0; p < 2; p++) {
        uint8_t src = mem_r8(SOURCE(p));
        if (src > 1 || !(active >> p & 1)) continue;
        g_autoplay_used = 1;
        out[src] |= player_buttons(p, src);
    }
    return out[port];
}

/* High scores: hiscore_insert(stage, ?, ?, name, score) inserts a score into the stage's top-3 table (system data block,
   entries {name[8], u16 score} from 0x801a5348), called after the name entry. Its insert only happens when an
   entry's score is lower than the new one, so a song played with Autoplay/Auto-COOL gets score 0 here (the fifth
   argument, on the caller's stack at sp+16) and nothing is inserted; the name entry itself still runs. */
void hook_hiscore_insert(CPU *c, uint32_t fn) {
    (void)fn;
    if (!g_autoplay_used) return;
    uint32_t sp = c->r[29];
    rt_log("autoplay: high score %u not recorded (song played with Autoplay/Auto-COOL)", mem_r16(sp + 16));
    mem_w32(sp + 16, 0);
}

/* After a song played with Autoplay/Auto-COOL/Auto-FEVER, the system-save and replay-save prompts are skipped and the
   menu goes straight back to the candy bars. The menu overlay (COMOD0.MEN) runs its screens as a loop: menu_screen_loop
   calls menu_screen_dispatch(screen), which dispatches through the handler table at 0x801db0f0 and returns the next screen
   (-1 ends). This pre-hook swaps those screens for the main menu while the skip is armed; the skip is armed when an
   autoplayed song ends and disarmed once the main menu shows, so Load & Save from the candy bars is untouched.
   UJL_AUTOPLAY_LOG logs every screen number. */
/* Screen numbers (from the log, Stage 1 clear): 10 name entry, 30 a step after it, 27 SYSTEM SAVE, 28 REPLAY SAVE,
   31 and 29 the replay steps (the REPLAY slot screen), 3 the candy-bar main menu, 12/4 Stage Select, 35 stage start. */
#define SCR_SYSTEM_SAVE  27
#define SCR_REPLAY_SAVE  28
#define SCR_REPLAY       29
#define SCR_REPLAY_STEP  31
#define SCR_MAIN_MENU    3
void hook_menu_screen(CPU *c, uint32_t fn) {
    (void)fn;
    int id = replay_files_menu_screen((int)c->r[4]);
    id = arcade_stage1_menu_screen(c, id);
    c->r[4] = (uint32_t)id;
    if (log_on > 0) rt_log("autoplay: menu screen %d%s", id, skip_post_saves ? " (skipping saves)" : "");
    if (!skip_post_saves) return;
    if (id == SCR_SYSTEM_SAVE || id == SCR_REPLAY_SAVE || id == SCR_REPLAY || id == SCR_REPLAY_STEP) {
        rt_log("autoplay: skipped save screen %d after an autoplayed song", id);
        c->r[4] = (uint32_t)SCR_MAIN_MENU;
        id = SCR_MAIN_MENU;
    }
    if (id == SCR_MAIN_MENU) skip_post_saves = 0;
}

/* For the frontend: Autoplay/Auto-COOL is playing the current song (shows the watermark). */
int autoplay_active(void) { return g_autoplay && in_song(); }
