/* The delta is drawn with the game's score digits and punctuation, through the PS1 GPU stream.
   Traces contain only times/scores, never disc data. Only completed, unassisted human runs become PBs. */
#include "psx.h"
#include "score_delta.h"
#include <SDL3/SDL.h>
#include <stdlib.h>

#define SCENE       0x80078ae0u
#define SONG_TIME   0x80078af8u
#define PLAYER(p)   (0x80071cb8u + 96u * (p))
#define REPLAY_MODE 0x80071dd0u /* 0 live, 1 replay, 2 attract */
#define MAX_POINTS  4096
#define MAX_TIME    (60 * 60 * 300) /* reject corrupt clocks/files; well above every song */
extern int g_autoplay, g_autoplay_used, g_uncensored;

typedef struct { int32_t time, score; } Point;
typedef struct { uint32_t stage, variant, mode, inputs, profile, censored; } Key;
typedef struct { Point points[MAX_POINTS]; uint32_t count; int final_score, end_time; } Trace;
static struct {
    int active, pending, assisted, overflow, last_time;
    Key key;
    Trace run[2], best[2];
    int human[2];
} state;
static char trace_dir[2048];
static int log_on;

static int song_context(void) {
    const char *ov = rt_current_overlay_name();
    if (!ov || !strncmp(ov, "COMOD8", 6)) return 0;
    size_t n = strlen(ov);
    return n >= 4 && (!strcmp(ov + n - 4, ".SNG") || !strcmp(ov + n - 4, ".CMP") || !strcmp(ov + n - 4, ".COP"));
}
static int score(int p) { return (int16_t)mem_r16(0x80071dbcu + (state.key.mode == 1 ? 10u * (uint32_t)p : 0)); }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static void wr32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static int trace_path(char *path, size_t n, int p) {
    const Key *k = &state.key;
    return SDL_snprintf(path, n, "%s/s%u-v%u-m%u-i%u-n%u-c%u-p%d.bin", trace_dir,
                        k->stage, k->variant, k->mode, k->inputs, k->profile, k->censored, p + 1) < (int)n;
}

/* Versioned, little-endian header (48 bytes), then {time,score} points. */
static void load_best(int p) {
    Trace *t = &state.best[p];
    t->count = 0;
    char path[2304];
    if (!trace_dir[0] || !trace_path(path, sizeof path, p)) return;
    SDL_IOStream *io = SDL_IOFromFile(path, "rb");
    if (!io) return;
    uint8_t h[48];
    if (SDL_ReadIO(io, h, sizeof h) != sizeof h || memcmp(h, "UJLPB001", 8)) goto out;
    uint32_t count = rd32(h + 8), end = rd32(h + 12);
    int32_t final = (int32_t)rd32(h + 16);
    uint32_t keys[] = {state.key.stage, state.key.variant, state.key.mode, state.key.inputs, state.key.profile, state.key.censored, (uint32_t)p};
    for (int i = 0; i < 7; i++) if (rd32(h + 20 + i * 4) != keys[i]) goto out;
    if (!count || count > MAX_POINTS || end > MAX_TIME || final < -32768 || final > 32767 ||
        SDL_GetIOSize(io) != (Sint64)(sizeof h + count * 8)) goto out;
    for (uint32_t i = 0; i < count; i++) {
        uint8_t b[8];
        if (SDL_ReadIO(io, b, sizeof b) != sizeof b) goto out;
        int32_t tm = (int32_t)rd32(b), sc = (int32_t)rd32(b + 4);
        if (tm < 0 || (uint32_t)tm > end || (i && tm <= t->points[i - 1].time) || sc < -32768 || sc > 32767) goto out;
        t->points[i] = (Point){tm, sc};
    }
    if (t->points[0].time != 0 || t->points[count - 1].score != final) goto out;
    t->count = count; t->end_time = (int)end; t->final_score = final;
    if (log_on) LOGI("score delta: loaded %s (%u points, final %d)", path, count, final);
out:
    SDL_CloseIO(io);
}

static int save_best(int p) {
    Trace *t = &state.run[p];
    char path[2304], tmp[2368];
    if (!trace_dir[0] || !trace_path(path, sizeof path, p) || !SDL_CreateDirectory(trace_dir)) return 0;
    SDL_snprintf(tmp, sizeof tmp, "%s.tmp-%llu", path, (unsigned long long)SDL_GetTicksNS());
    uint8_t *data = (uint8_t *)SDL_calloc(1, 48 + t->count * 8);
    if (!data) return 0;
    memcpy(data, "UJLPB001", 8);
    wr32(data + 8, t->count); wr32(data + 12, (uint32_t)state.last_time); wr32(data + 16, (uint32_t)score(p));
    uint32_t keys[] = {state.key.stage, state.key.variant, state.key.mode, state.key.inputs, state.key.profile, state.key.censored, (uint32_t)p};
    for (int i = 0; i < 7; i++) wr32(data + 20 + i * 4, keys[i]);
    for (uint32_t i = 0; i < t->count; i++) {
        wr32(data + 48 + i * 8, (uint32_t)t->points[i].time);
        wr32(data + 52 + i * 8, (uint32_t)t->points[i].score);
    }
    int ok = SDL_SaveFile(tmp, data, 48 + t->count * 8) && SDL_RenamePath(tmp, path);
    if (!ok) LOGI("score delta: couldn't save %s: %s", path, SDL_GetError());
    else LOGI("score delta: saved PB %s (final %d, %u points)", path, score(p), t->count);
    SDL_RemovePath(tmp);
    SDL_free(data);
    return ok;
}

void score_delta_init(const char *directory) {
    memset(&state, 0, sizeof state);
    const char *override = getenv("UJL_SCORE_TRACE_DIR");
    SDL_strlcpy(trace_dir, override ? override : directory ? directory : "", sizeof trace_dir);
    log_on = getenv("UJL_SCORE_LOG") != NULL;
}

static void begin_run(void) {
    memset(&state, 0, sizeof state);
    Key *k = &state.key;
    k->stage = mem_r32(SCENE); k->variant = mem_r32(SCENE + 4); k->mode = mem_r32(SCENE + 12);
    k->inputs = mem_r8(PLAYER(0) + 48) | mem_r8(PLAYER(1) + 48) << 4;
    k->profile = mem_r32(0x801a5334u);
    k->censored = !g_uncensored && (k->stage == 1 || k->stage == 5 || k->stage == 6);
    if (k->stage < 1 || k->stage > 7 || k->variant > 5 || k->mode > 2 || k->profile > 1) return;
    state.active = state.pending = 1;
    for (int p = 0; p < 2; p++) {
        state.human[p] = mem_r8(PLAYER(p) + 48) <= 1 && (p == 0 || k->mode == 1);
        if (p == 0 && k->mode == 2) state.human[p] |= mem_r8(PLAYER(1) + 48) <= 1;
        state.run[p].points[0] = (Point){0, score(p)}; state.run[p].count = 1;
        if (state.human[p]) load_best(p);
    }
    if (log_on) LOGI("score delta: start stage %u variant %u mode %u inputs %u profile %u censored %u", k->stage, k->variant, k->mode, k->inputs, k->profile, k->censored);
}

static void sample_scores(int tm) {
    state.last_time = tm;
    state.assisted |= g_autoplay || g_autoplay_used || mem_r32(REPLAY_MODE) != 0;
    for (int p = 0; p < 2; p++) {
        if (!state.human[p]) continue;
        Trace *t = &state.run[p];
        Point *last = &t->points[t->count - 1];
        int sc = score(p);
        if (sc == last->score) continue;
        if (tm == last->time) last->score = sc;
        else if (t->count < MAX_POINTS) t->points[t->count++] = (Point){tm, sc};
        else state.overflow = 1;
        if (log_on) LOGI("score delta: P%d t=%d score=%d", p + 1, tm, sc);
    }
}

void score_delta_tick(void) {
    if (!state.active) return;
    if (!song_context()) { state.active = 0; return; }
    int tm = (int32_t)mem_r32(SONG_TIME);
    if (tm < 0 || tm > MAX_TIME) return;
    if (tm < state.last_time || mem_r32(SCENE) != state.key.stage || mem_r32(SCENE + 4) != state.key.variant) {
        state.active = state.pending = 0; return; /* retry/new song: next counter starts a new trace */
    }
    sample_scores(tm);
}

/* stage_results_apply(result): positive = completed; zero/negative = failed, quit or retry. This runs
   before the menu/name-entry decision, so even a first run below the memory card's high scores is captured. */
void hook_score_run_finish(CPU *c, uint32_t fn) {
    (void)fn;
    if (!state.pending) return;
    score_delta_tick();
    /* Cleanup can run between vblanks: include the final score even if no further HUD frame was drawn. */
    if (state.pending) sample_scores(state.last_time);
    if ((int32_t)c->r[4] > 0 && state.pending && !state.assisted && !state.overflow && state.last_time > 0) {
        for (int p = 0; p < 2; p++)
            if (state.human[p] && (!state.best[p].count || score(p) > state.best[p].final_score)) save_best(p);
    } else if (log_on) LOGI("score delta: run not recorded (result %d, assisted %d)", (int)c->r[4], state.assisted);
    state.active = state.pending = 0;
}

static int reference_score(int p, int time) {
    const Trace *t = &state.best[p];
    uint32_t lo = 0, hi = t->count;
    while (lo + 1 < hi) { uint32_t m = (lo + hi) / 2; if (t->points[m].time <= time) lo = m; else hi = m; }
    return t->points[lo].score;
}

/* Native 12x12 S_PLUS0 / S_MINUS0 sprites are resident next to the score digits in every song.
   They share the digits' palette, so the same modulation gives a consistent blue/red tint. */
static void draw_sign(const GpuPrim *base, int positive, int x, void (*emit)(const GpuPrim *)) {
    GpuPrim q = *base;
    q.w = q.h = 12;
    q.v[0].x = (int16_t)x;
    q.v[0].u = positive ? 196 : 172;
    q.v[0].v = 12;
    emit(&q);
}

void score_delta_draw(const GpuPrim *p, void (*emit)(const GpuPrim *)) {
    /* Match the ones digit of the native score counter, not unrelated numbers or menu text. */
    if (p->type != PRIM_RECT || !p->textured || p->w != 10 || p->h != 12 ||
        (p->texpage & 0x19F) != 0x1E || p->v[0].v || p->v[0].u < 32 || p->v[0].u > 122 ||
        (p->v[0].u - 32) % 10 || p->v[0].y - p->clip_y0 != 170 || !song_context() || mem_r32(REPLAY_MODE) == 2) return;
    int x = p->v[0].x - p->clip_x0;
    if (x != 52 && x != 292) return;
    if (!state.active) begin_run();
    if (!state.active) return;
    score_delta_tick();
    if (!state.active) { begin_run(); if (!state.active) return; score_delta_tick(); }
    int player = x == 292;
    if (!state.human[player]) return;
    if (!state.best[player].count) return; /* first run: capture silently until a reference exists */
    int delta = score(player) - reference_score(player, state.last_time);
    char digits[16];
    SDL_snprintf(digits, sizeof digits, "%d", delta < 0 ? -delta : delta);
    int width = 11 + (int)strlen(digits) * 9 + 1;
    x = p->v[0].x + p->w + 8;
    if (player) { int n = 1, v = abs(score(player)); while (v >= 10) { n++; v /= 10; } x = p->v[0].x - (n - 1) * 9 - 8 - width; }
    GpuPrim q = *p;
    q.raw = q.semi = q.gouraud = q.gte3d = 0;
    q.v[0].fx = q.v[0].fy = 0;
    q.v[0].r = q.v[0].g = q.v[0].b = 128;
    if (delta > 0) { q.v[0].r = 32; q.v[0].g = 64; }
    if (delta < 0) { q.v[0].g = q.v[0].b = 32; }
    draw_sign(&q, delta >= 0, x, emit);
    x += 11;
    for (const char *s = digits; *s; s++, x += 9) {
        q.v[0].x = (int16_t)x; q.v[0].u = (uint8_t)(32 + (*s - '0') * 10);
        emit(&q);
    }
}
