// High-resolution OpenGL renderer.
//
// VRAM lives in an RGBA8 texture at `scale` x native size (row 0 = VRAM line 0). A raw 16-bit VRAM word
// is stored losslessly as r8=r5<<3|r5>>2 (same for g, b) and a = bit 15, so CLUT indices and texel words
// can be recovered in the shader. Primitives are drawn into that texture in true color (no dithering).
// Textures are read with texelFetch from a second copy ("read" texture), which is refreshed from the draw
// texture whenever a primitive samples a VRAM area written since the last refresh (render-to-texture).
//
// Semi-transparent textured primitives are drawn in two passes, as on hardware only texels with bit 15 set
// blend: pass 1 draws the opaque texels, pass 2 the blended ones.
#include "hw_renderer.h"
#include "gl_funcs.h"
extern "C" {
#include "gpu_rec.h"
}
#include <vector>
#include <string>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <cmath>

#define GLF(name) ujl_gl##name

namespace {

struct Vtx {
    float x, y;
    uint8_t r, g, b, a;
    float u, v;
    uint32_t tp, clut, flags, tw;
    uint16_t box[4];             // texel bounding box of the primitive (umin, vmin, umax, vmax)
};

enum { BLEND_NONE = 0, BLEND_HALF, BLEND_ADD, BLEND_SUB, BLEND_QUARTER };
enum { F_TEX = 1, F_RAW = 2, F_PASS_OPAQUE = 4, F_PASS_SEMI = 8, F_MASK = 16, F_HIRES = 32, F_NEAREST = 64, F_WIDESRC = 128 };

struct Batch {
    int blend;
    int target;             // -1 = VRAM, else wide frame target index
    int src;                // wide target sampled by F_WIDESRC prims, or -1
    int sx, sy, sw, sh;     // scissor in target pixels
    int first, count;
};

// Widescreen: each displayed frame buffer gets a wide twin. 3D primitives are drawn there with X un-squeezed
// (the GTE squeezes it by k so the game draws a wider view), 2D primitives centered (or stretched).
struct Wide {
    int x = 0, y = 0, w = 0, h = 0;     // VRAM rect of the frame buffer it shadows
    int pw = 0, ph = 0;                 // texture size in pixels
    GLuint tex_disp = 0, fbo_disp = 0, rb = 0, fbo_draw = 0, tex_read = 0, fbo_read = 0;
    bool dirty = false;
    std::vector<uint8_t> drows;         // rows written since the last read-copy refresh
    bool wide_drawn = false;            // widened drawing reached the margins since it was last shown
    bool last_wide = false;
    bool drew3d = false, last3d = false; // GTE 3D geometry drawn since the last check / the time before
    bool touched = false;               // anything drawn into it since the last margin check             // ...and did so the time before (a 3D scene, not a 2D screen)
    uint32_t seen = 0;
};
Wide wide[2];
void free_wide(Wide &W);
bool g_subpixel = true;
bool g_thin_fix = true;
// Debug A/B toggles (settings > Debug) for tonight's renderer changes
int g_dbg_edge_clamp = 1, g_dbg_margin_snap = 1, g_dbg_sharp_2d_only = 1, g_dbg_seed_wide = 1;          // widen sub-pixel-thin 3D triangles to one native pixel (no flicker)         // draw 3D vertices at the GTE's sub-pixel position (no vertex jitter)
float g_wide_k = 1.0f;          // horizontal squeeze (1 = off); aspect = (4/3) / k
bool g_stretch_ui = false;
uint32_t frame_no;
int st_wide_batches, st_replays, st_glerr;
double st_replay_ms;
double perf_replay_ms; int perf_replays;   // since the last hw_take_replay_ms (performance details)
// Operations that end a render pass on Apple's OpenGL ES (it runs on Metal: a texture upload or blit needs a blit
// encoder, a target switch a new render pass, and a tile GPU loads and stores the whole target for each pass).
// Counted since the last hw_take_counts (performance details / UJL_FPS_STATS).
int pc_upload, pc_fill, pc_copy, pc_sync_read, pc_sync_wide, pc_flush, pc_wide_copy;
int st_last_drops;
extern "C" int g_gpu_rec_drops;
int st_flush, st_sync_read, st_sync_wide, st_alloc_wide, st_upload; // per-second stats

int S = 1;                       // internal scale
int NS = 1;                      // MSAA samples (effective)
int want_ns = 4;                 // requested samples
bool have_sample_mask;           // GLSL 4.00: sprite silhouettes get MSAA coverage too
GLuint fbo_draw, rb_draw;        // render target: MSAA renderbuffer, or tex_disp itself when NS == 1
GLuint tex_disp, fbo_disp;       // single-sample display copy (resolved)
GLuint tex_read, fbo_read, tex_stage;
int disp_x0, disp_y0, disp_x1, disp_y1 = -1; // display rect to resolve (native, inclusive)
GLuint prog_draw, prog_copy;
GLint u_draw_edge_clamp;
GLint u_draw_vram, u_draw_scale, u_draw_smooth, u_draw_samples, u_draw_size, u_draw_wide;
int g_smooth = 1;
int g_ui_sharp = 1;              // unscaled 2D art (UI) keeps nearest-neighbour pixels
GLint u_copy_src, u_copy_off, u_copy_div, u_copy_dst;
GLuint vao, vbo, vao_empty;
std::vector<Vtx> vert_lists[3];      // per target, so each target's batches stay contiguous and merge
std::vector<Vtx> vert_all;           // concatenated at flush
int cur_vt = -1;                     // target the next add_vertex() goes to
static inline std::vector<Vtx> &vlist(int target) { return vert_lists[target + 1]; }
static inline bool verts_empty() { return vert_lists[0].empty() && vert_lists[1].empty() && vert_lists[2].empty(); }
static inline void clear_verts() { for (auto &l : vert_lists) l.clear(); }
std::vector<Batch> batch_lists[3];
static inline std::vector<Batch> &blist(int target) { return batch_lists[target + 1]; }
static inline void clear_batches() { for (auto &l : batch_lists) l.clear(); }
uint64_t dmask[512];             // per VRAM row: 16-pixel column groups written since the last read-copy refresh
bool rendered[8][16];            // cells holding GPU-rendered content (frame buffers), not uploaded textures
bool any_dirty;

const char *k_vs = R"(#version 330 core
layout(location=0) in vec2 a_pos;
layout(location=1) in vec4 a_col;
layout(location=2) in vec2 a_uv;
layout(location=3) in uvec4 a_tex;
layout(location=4) in uvec4 a_box;
uniform vec2 u_size;    // target size in native pixels
out vec3 v_col;
noperspective out vec2 v_uv; // not centroid: the two triangles of a quad must sample identically along the shared diagonal
flat out uvec4 v_tex;
flat out uvec4 v_box;
void main() {
    v_col = a_col.rgb;
    v_uv = a_uv;
    v_tex = a_tex;
    v_box = a_box;
    gl_Position = vec4(a_pos / u_size * 2.0 - 1.0, 0.0, 1.0);
}
)";

const char *k_fs = R"(#version 330 core
uniform sampler2D u_vram;
uniform sampler2D u_wide;   // wide frame target (F_WIDESRC: screen copies in widescreen)
uniform int u_scale;
uniform int u_smooth;
uniform int u_edge_clamp;
uniform int u_samples;
in vec3 v_col;
noperspective in vec2 v_uv;
flat in uvec4 v_tex;
flat in uvec4 v_box;
out vec4 o_col;
uint fetch16(int x, int y) {
    vec4 c = texelFetch(u_vram, ivec2((x & 1023) * u_scale + u_scale / 2, (y & 511) * u_scale + u_scale / 2), 0);
    uvec4 b = uvec4(c * 255.0 + 0.5);
    return (b.r >> 3) | ((b.g >> 3) << 5) | ((b.b >> 3) << 10) | ((b.a >> 7) << 15);
}
// raw 16-bit texel at page texel coords (unwrapped; clamped to the primitive's texel box)
uint texel(ivec2 uv, bool clampbox) {
    if (clampbox) uv = clamp(uv, ivec2(v_box.xy), ivec2(v_box.zw));
    uv &= 255;
    uint tw = v_tex.w;
    int mx = int(tw & 31u) * 8, my = int((tw >> 5) & 31u) * 8;
    int ox = int((tw >> 10) & 31u) * 8, oy = int((tw >> 15) & 31u) * 8;
    uv.x = (uv.x & ~mx) | (ox & mx);
    uv.y = (uv.y & ~my) | (oy & my);
    uint tp = v_tex.x;
    int tpx = int(tp & 15u) * 64, tpy = int((tp >> 4) & 1u) * 256;
    uint depth = (tp >> 7) & 3u;
    int cx = int(v_tex.y & 63u) * 16, cy = int((v_tex.y >> 6) & 511u);
    if (depth == 0u) {
        uint w = fetch16(tpx + (uv.x >> 2), tpy + uv.y);
        return fetch16(cx + int((w >> (uint(uv.x & 3) * 4u)) & 15u), cy);
    } else if (depth == 1u) {
        uint w = fetch16(tpx + (uv.x >> 1), tpy + uv.y);
        return fetch16(cx + int((w >> (uint(uv.x & 1) * 8u)) & 255u), cy);
    }
    return fetch16(tpx + uv.x, tpy + uv.y);
}
// colors closer than 2/31 per channel count as one (dithered / near-duplicate shades); transparent is its own
bool same_color(uint a, uint b) {
    if (a == b) return true;
    if (a == 0u || b == 0u) return false;
    ivec3 d = abs(ivec3(int(a & 31u), int((a >> 5) & 31u), int((a >> 10) & 31u)) -
                  ivec3(int(b & 31u), int((b >> 5) & 31u), int((b >> 10) & 31u)));
    return max(d.x, max(d.y, d.z)) <= 2;
}
vec3 rgb15(uint t) { return vec3(float(t & 31u), float((t >> 5) & 31u), float((t >> 10) & 31u)) / 31.0; }
vec4 catmull(float t) {
    float t2 = t * t, t3 = t2 * t;
    return vec4(-0.5 * t3 + t2 - 0.5 * t, 1.5 * t3 - 2.5 * t2 + 1.0, -1.5 * t3 + 2.0 * t2 + 0.5 * t, 0.5 * t3 - 0.5 * t2);
}
void main() {
    vec2 footprint = fwidth(v_uv);           // texels per pixel (before any discard)
    float cover = 1.0;                        // sprite-edge coverage (MSAA sample mask)
#ifdef HAVE_SAMPLE_MASK
    bool edge_aa = u_samples > 1;
#else
    bool edge_aa = false;
#endif
    uint flags = v_tex.z;
    vec3 col = v_col;
    uint semibit = 0u;
    if ((flags & 1u) != 0u) {
        uint depth = (v_tex.x >> 7) & 3u;
        uint t;
        vec3 tc;
        if ((flags & 128u) != 0u) {
            // copy from a wide frame target: uv is in its native pixel coordinates
            ivec2 sz = textureSize(u_wide, 0);
            vec4 cc = texelFetch(u_wide, clamp(ivec2(floor(v_uv * float(u_scale))), ivec2(0), sz - 1), 0);
            uvec4 bb = uvec4(cc * 255.0 + 0.5);
            t = (bb.r >> 3) | ((bb.g >> 3) << 5) | ((bb.b >> 3) << 10) | ((bb.a >> 7) << 15);
            if (t == 0u) discard;
            tc = cc.rgb;
        } else if ((flags & 32u) != 0u) {
            // GPU-rendered source: read the upscaled pixels at sub-texel precision
            ivec2 ti = ivec2(floor(v_uv + 0.0001));
            ivec2 sub = clamp(ivec2((v_uv - vec2(ti)) * float(u_scale)), ivec2(0), ivec2(u_scale - 1));
            ti &= 255;
            uint tw = v_tex.w;
            int mx = int(tw & 31u) * 8, my = int((tw >> 5) & 31u) * 8;
            int ox = int((tw >> 10) & 31u) * 8, oy = int((tw >> 15) & 31u) * 8;
            ti.x = (ti.x & ~mx) | (ox & mx);
            ti.y = (ti.y & ~my) | (oy & my);
            int tpx = int(v_tex.x & 15u) * 64, tpy = int((v_tex.x >> 4) & 1u) * 256;
            vec4 cc = texelFetch(u_vram, ivec2(((tpx + ti.x) & 1023) * u_scale + sub.x, ((tpy + ti.y) & 511) * u_scale + sub.y), 0);
            uvec4 bb = uvec4(cc * 255.0 + 0.5);
            t = (bb.r >> 3) | ((bb.g >> 3) << 5) | ((bb.b >> 3) << 10) | ((bb.a >> 7) << 15);
            if (t == 0u) discard;
            tc = cc.rgb;
        } else if (u_smooth != 0 && (flags & 64u) == 0u) {
            // "vector" smoothing: per candidate color, a Catmull-Rom-smoothed coverage field over the 4x4
            // neighbourhood; the strongest field wins. Edges become smooth curves, 1-texel lines survive.
            vec2 q = v_uv - 0.5;
            ivec2 b = ivec2(floor(q));
            vec2 f = q - vec2(b);
            vec4 wx = catmull(f.x), wy = catmull(f.y);
            // Fast path: inside a flat area the 4 nearest texels match; nothing to smooth (most pixels).
            uint q00 = texel(b, true), q10 = texel(b + ivec2(1, 0), true);
            uint q01 = texel(b + ivec2(0, 1), true), q11 = texel(b + ivec2(1, 1), true);
            if (q00 == q10 && q00 == q01 && q00 == q11) {
                t = q00;
                if (t == 0u) discard;
                tc = rgb15(t);
            } else {
            uint c[16];
            ivec3 rgb[16];   // each texel's color decoded once (same_color unpacked both texels on every comparison)
            for (int j = 0; j < 4; j++)
                for (int i = 0; i < 4; i++) {
                    uint t4 = (j == 1 || j == 2) && (i == 1 || i == 2) ? (j == 1 ? (i == 1 ? q00 : q10) : (i == 1 ? q01 : q11))
                                                                      : texel(b + ivec2(i - 1, j - 1), true);
                    c[j * 4 + i] = t4;
                    rgb[j * 4 + i] = ivec3(int(t4 & 31u), int((t4 >> 5) & 31u), int((t4 >> 10) & 31u));
                }
            // same_color() on two of them, by index
#define SAME(m, n) (c[m] == c[n] || (c[m] != 0u && c[n] != 0u && all(lessThanEqual(abs(rgb[m] - rgb[n]), ivec3(2)))))
            // How busy is the neighbourhood? Flat cartoon art has a few colors; shading/gradients many. Only "more
            // than 5" matters, so counting stops there: the busy areas, the costly case, used to run all 120 pairs.
            int distinct = 0;
            for (int n = 0; n < 16 && distinct <= 5; n++) {
                bool dup = false;
                for (int m = 0; m < n && !dup; m++) dup = SAME(m, n);
                if (!dup) distinct++;
            }
            int cand[4] = int[4](5, 6, 9, 10);
            if (distinct > 5 && c[5] != 0u && c[6] != 0u && c[9] != 0u && c[10] != 0u) {
                // gradient / shaded area: smooth bilinear blend of the central 2x2 (all opaque)
                vec3 top = mix(rgb15(c[5]), rgb15(c[6]), f.x), bot = mix(rgb15(c[9]), rgb15(c[10]), f.x);
                tc = mix(top, bot, f.y);
                t = c[(f.y < 0.5 ? 5 : 9) + (f.x < 0.5 ? 0 : 1)];   // nearest texel: semi bit
            } else {
                float best = -1e9, second = -1e9;
                uint cb = c[5], cs = c[5];
                for (int k = 0; k < 4; k++) {
                    int ck = cand[k];
                    uint C = c[ck];
                    bool seen = false;
                    for (int m = 0; m < k && !seen; m++) seen = SAME(cand[m], ck);
                    if (seen) continue;
                    float F = 0.0;
                    for (int n = 0; n < 16; n++)
                        if (SAME(n, ck)) F += wx[n & 3] * wy[n >> 2];
                    if (F > best) { second = best; cs = cb; best = F; cb = C; }
                    else if (F > second) { second = F; cs = C; }
                }
                float fp = max(max(footprint.x, footprint.y), 1e-3);
                float a = (second > -1e8) ? clamp(0.5 + (best - second) / (2.0 * fp), 0.5, 1.0) : 1.0;
                if (cb == 0u) {
                    // transparent wins here; near the silhouette the runner-up still partly covers the pixel
                    if (!edge_aa || cs == 0u || second <= -1e8) discard;
                    t = cs;
                    tc = rgb15(cs);
                    cover = 1.0 - a;
                } else {
                    t = cb;
                    tc = rgb15(t);
                    if (cs != 0u && !same_color(cs, cb) && second > -1e8) tc = mix(rgb15(cs), tc, a); // color AA
                    else if (cs == 0u && second > -1e8 && edge_aa) cover = a;                      // silhouette AA
                }
            }
            } // not flat
        } else {
            t = texel(ivec2(floor(v_uv + 0.0001)), u_edge_clamp != 0); // clamped: no neighbouring atlas column at sprite edges
            if (t == 0u) discard;
            tc = rgb15(t);
        }
        semibit = t >> 15;
        if ((flags & 4u) != 0u && semibit != 0u) discard;
        if ((flags & 8u) != 0u && semibit == 0u) discard;
        col = ((flags & 2u) != 0u) ? tc : min(tc * v_col * (255.0 / 128.0), vec3(1.0));
    }
#ifdef HAVE_SAMPLE_MASK
    // once gl_SampleMask is written anywhere it is undefined on paths that skip it: always write it
    int mask = -1;
    if (edge_aa && cover < 1.0) {
        int n = int(cover * float(u_samples) + 0.5);
        if (n <= 0) discard;
        mask = (1 << n) - 1;
    }
    gl_SampleMask[0] = mask;
#endif
    o_col = vec4(col, ((flags & 16u) != 0u || semibit != 0u) ? 1.0 : 0.0);
}
)";

// fullscreen triangle; copies texels from u_src into the scissored destination
const char *k_copy_vs = R"(#version 330 core
void main() {
    vec2 p = vec2((gl_VertexID == 1) ? 3.0 : -1.0, (gl_VertexID == 2) ? 3.0 : -1.0);
    gl_Position = vec4(p, 0.0, 1.0);
}
)";
const char *k_copy_fs = R"(#version 330 core
uniform sampler2D u_src;
uniform ivec2 u_off;   // source origin (in source texels)
uniform int u_div;     // destination pixels per source texel
uniform ivec2 u_dst;   // destination origin (pixels)
out vec4 o_col;
void main() {
    ivec2 p = ivec2(gl_FragCoord.xy) - u_dst;
    o_col = texelFetch(u_src, u_off + p / u_div, 0);
}
)";

GLuint compile(GLenum type, const char *src) {
    GLuint s = GLF(CreateShader)(type);
    GLF(ShaderSource)(s, 1, &src, nullptr);
    GLF(CompileShader)(s);
    GLint ok = 0;
    GLF(GetShaderiv)(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048];
        GLF(GetShaderInfoLog)(s, sizeof log, nullptr, log);
        fprintf(stderr, "hw: shader compile failed:\n%s\n", log);
    }
    return s;
}

GLuint link(const char *vs, const char *fs) {
    GLuint p = GLF(CreateProgram)();
    GLF(AttachShader)(p, compile(GL_VERTEX_SHADER, vs));
    GLF(AttachShader)(p, compile(GL_FRAGMENT_SHADER, fs));
    GLF(LinkProgram)(p);
    GLint ok = 0;
    GLF(GetProgramiv)(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[2048];
        GLF(GetProgramInfoLog)(p, sizeof log, nullptr, log);
        fprintf(stderr, "hw: program link failed:\n%s\n", log);
    }
    return p;
}

GLuint make_target(GLuint *fbo, int w, int h, GLint filter) {
    GLuint t;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    if (fbo) {
        GLF(GenFramebuffers)(1, fbo);
        GLF(BindFramebuffer)(GL_FRAMEBUFFER, *fbo);
        GLF(FramebufferTexture2D)(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t, 0);
        if (GLF(CheckFramebufferStatus)(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
            fprintf(stderr, "hw: framebuffer incomplete (%dx%d)\n", w, h);
        glClearColor(0, 0, 0, 0);
        glClear(GL_COLOR_BUFFER_BIT);
    }
    return t;
}

void alloc_targets() {
    if (tex_disp) {
        glDeleteTextures(1, &tex_disp);
        glDeleteTextures(1, &tex_read);
        if (fbo_draw != fbo_disp) GLF(DeleteFramebuffers)(1, &fbo_draw);
        GLF(DeleteFramebuffers)(1, &fbo_disp);
        GLF(DeleteFramebuffers)(1, &fbo_read);
        if (rb_draw) GLF(DeleteRenderbuffers)(1, &rb_draw);
        rb_draw = 0;
    }
    int W = 1024 * S, H = 512 * S;
    // MSAA: cap memory (~768 MB for the multisampled target) and the driver limit
    GLint max_samples = 1;
    glGetIntegerv(0x8D57 /* GL_MAX_SAMPLES */, &max_samples);
    NS = std::max(1, std::min(want_ns, (int)max_samples));
    while (NS > 1 && (double)W * H * 4 * NS > 768.0 * 1024 * 1024) NS /= 2;

    tex_disp = make_target(&fbo_disp, W, H, GL_LINEAR); // LINEAR: for display sampling
    // alpha holds the PS1 mask bit, not coverage: the display must read it as opaque. (Only the display
    // samples tex_disp; the renderer reads bit 15 from tex_read, and blits ignore swizzles.)
    glTexParameteri(GL_TEXTURE_2D, 0x8E45 /* GL_TEXTURE_SWIZZLE_A */, GL_ONE);
    if (NS > 1) {
        GLF(GenRenderbuffers)(1, &rb_draw);
        GLF(BindRenderbuffer)(0x8D41 /* GL_RENDERBUFFER */, rb_draw);
        GLF(RenderbufferStorageMultisample)(0x8D41, NS, GL_RGBA8, W, H);
        GLF(GenFramebuffers)(1, &fbo_draw);
        GLF(BindFramebuffer)(GL_FRAMEBUFFER, fbo_draw);
        GLF(FramebufferRenderbuffer)(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, 0x8D41, rb_draw);
        if (GLF(CheckFramebufferStatus)(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            fprintf(stderr, "hw: %dx MSAA target incomplete, anti-aliasing off\n", NS);
            GLF(DeleteFramebuffers)(1, &fbo_draw);
            GLF(DeleteRenderbuffers)(1, &rb_draw);
            rb_draw = 0;
            NS = 1;
        } else {
            glClearColor(0, 0, 0, 0);
            glClear(GL_COLOR_BUFFER_BIT);
        }
    }
    if (NS == 1) fbo_draw = fbo_disp;
    tex_read = make_target(&fbo_read, W, H, GL_NEAREST);
    for (Wide &w : wide) free_wide(w);
    memset(dmask, 0, sizeof dmask);
    memset(rendered, 0, sizeof rendered);
    any_dirty = false;
    fprintf(stderr, "hw: targets %dx%d, MSAA %dx%s\n", W, H, NS, have_sample_mask ? " (+sprite edges)" : "");
}

// resolve the displayed area of the MSAA target into tex_disp
void resolve_display() {
    if (NS > 1)
        for (Wide &W : wide)
            if (W.tex_disp && W.fbo_draw != W.fbo_disp && W.x == disp_x0 && W.y == disp_y0) {
                GLF(BindFramebuffer)(GL_READ_FRAMEBUFFER, W.fbo_draw);
                GLF(BindFramebuffer)(GL_DRAW_FRAMEBUFFER, W.fbo_disp);
                glDisable(GL_SCISSOR_TEST);
                GLF(BlitFramebuffer)(0, 0, W.pw, W.ph, 0, 0, W.pw, W.ph, GL_COLOR_BUFFER_BIT, GL_NEAREST);
            }
    if (NS == 1 || disp_y1 < disp_y0) return;
    GLF(BindFramebuffer)(GL_READ_FRAMEBUFFER, fbo_draw);
    GLF(BindFramebuffer)(GL_DRAW_FRAMEBUFFER, fbo_disp);
    glDisable(GL_SCISSOR_TEST);
    int ax = disp_x0 * S, ay = disp_y0 * S, bx = (disp_x1 + 1) * S, by = (disp_y1 + 1) * S;
    GLF(BlitFramebuffer)(ax, ay, bx, by, ax, ay, bx, by, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    GLF(BindFramebuffer)(GL_FRAMEBUFFER, fbo_draw);
}

static inline uint64_t colmask(int x0, int x1) { // bits for 16-pixel groups x0>>4 .. x1>>4
    int c0 = x0 >> 4, c1 = x1 >> 4;
    uint64_t hi = (c1 >= 63) ? ~0ull : ((1ull << (c1 + 1)) - 1);
    return hi & ~((1ull << c0) - 1);
}

void mark_dirty(int x0, int y0, int x1, int y1) { // native, inclusive; wraps ignored (clamped)
    x0 = std::max(x0, 0); y0 = std::max(y0, 0);
    x1 = std::min(x1, 1023); y1 = std::min(y1, 511);
    if (x0 > x1 || y0 > y1) return;
    uint64_t m = colmask(x0, x1);
    for (int y = y0; y <= y1; y++) dmask[y] |= m;
    any_dirty = true;
}

bool region_dirty(int x0, int y0, int x1, int y1) {
    if (!any_dirty) return false;
    x0 = std::max(x0, 0); y0 = std::max(y0, 0);
    x1 = std::min(x1, 1023); y1 = std::min(y1, 511);
    if (x0 > x1 || y0 > y1) return false;
    uint64_t m = colmask(x0, x1);
    for (int y = y0; y <= y1; y++)
        if (dmask[y] & m) return true;
    return false;
}

void mark_rendered(int x0, int y0, int x1, int y1, bool on) {
    x0 = std::max(x0, 0); y0 = std::max(y0, 0);
    x1 = std::min(x1, 1023); y1 = std::min(y1, 511);
    if (x0 > x1 || y0 > y1) return;
    for (int cy = y0 >> 6; cy <= y1 >> 6; cy++)
        for (int cx = x0 >> 6; cx <= x1 >> 6; cx++) {
            if (on) rendered[cy][cx] = true;
            // an upload only clears cells it covers completely
            else if (x0 <= cx * 64 && x1 >= cx * 64 + 63 && y0 <= cy * 64 && y1 >= cy * 64 + 63) rendered[cy][cx] = false;
        }
}

bool region_rendered(int x0, int y0, int x1, int y1) {
    x0 = std::max(x0, 0); y0 = std::max(y0, 0);
    x1 = std::min(x1, 1023); y1 = std::min(y1, 511);
    for (int cy = y0 >> 6; cy <= y1 >> 6; cy++)
        for (int cx = x0 >> 6; cx <= x1 >> 6; cx++)
            if (rendered[cy][cx]) return true;
    return false;
}

void flush();
void sync_read();
void copy_into_wide(int wi, GLuint src, int src_x, int src_y, int div, int dx, int dy, int w, int h);

void free_wide(Wide &W) {
    if (!W.tex_disp) return;
    glDeleteTextures(1, &W.tex_disp);
    glDeleteTextures(1, &W.tex_read);
    if (W.fbo_draw != W.fbo_disp) GLF(DeleteFramebuffers)(1, &W.fbo_draw);
    GLF(DeleteFramebuffers)(1, &W.fbo_disp);
    GLF(DeleteFramebuffers)(1, &W.fbo_read);
    if (W.rb) GLF(DeleteRenderbuffers)(1, &W.rb);
    W = Wide();
}

float wide_native_w(const Wide &W) { return (float)W.w / g_wide_k; }

void alloc_wide(Wide &W) {
    st_alloc_wide++;
    W.pw = (int)std::lround(wide_native_w(W) * S);
    W.ph = W.h * S;
    W.tex_disp = make_target(&W.fbo_disp, W.pw, W.ph, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, 0x8E45 /* GL_TEXTURE_SWIZZLE_A */, GL_ONE);
    W.fbo_draw = W.fbo_disp;
    if (NS > 1) {
        GLF(GenRenderbuffers)(1, &W.rb);
        GLF(BindRenderbuffer)(0x8D41, W.rb);
        GLF(RenderbufferStorageMultisample)(0x8D41, NS, GL_RGBA8, W.pw, W.ph);
        GLF(GenFramebuffers)(1, &W.fbo_draw);
        GLF(BindFramebuffer)(GL_FRAMEBUFFER, W.fbo_draw);
        GLF(FramebufferRenderbuffer)(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, 0x8D41, W.rb);
        glClearColor(0, 0, 0, 0);
        glClear(GL_COLOR_BUFFER_BIT);
    }
    W.tex_read = make_target(&W.fbo_read, W.pw, W.ph, GL_NEAREST);
    W.dirty = false;
    W.drows.assign(W.h, 0);
}

// A 2D-only screen (menus, title) never draws into the margins, so they'd keep stale pixels from earlier
// screens: when a shown twin got nothing widened since it was last shown, clear its margins to black.
// UJL_FPS_STATS: log the widescreen events that look like the picture dropping to 4:3 without the view changing
static bool g_hw_log;   // hw_set_log: the performance-details setting
static bool wide_log() {
    static int on = -1;
    if (on < 0) on = getenv("UJL_FPS_STATS") != nullptr;
    return on != 0 || g_hw_log;
}
void clear_stale_margins(const GpuDisplay &d) {
    for (Wide &W : wide) {
        if (!W.tex_disp || W.x != d.disp_x || W.y != d.disp_y) continue;
        if (!W.touched) continue; // same finished frame shown again (30 fps games show each frame twice)
        W.touched = false;
        if (!W.wide_drawn) {
            static int nlog;
            if (W.last_wide && wide_log() && nlog++ < 300)
                fprintf(stderr, "wide: margins cleared (no 3D reached them) at frame %u, twin %d,%d\n", frame_no, W.x, W.y);
            flush();
            int m = (int)std::floor((wide_native_w(W) - W.w) * 0.5f * S);
            GLF(BindFramebuffer)(GL_FRAMEBUFFER, W.fbo_draw);
            glEnable(GL_SCISSOR_TEST);
            glClearColor(0, 0, 0, 0);
            glScissor(0, 0, m, W.ph);
            glClear(GL_COLOR_BUFFER_BIT);
            glScissor(W.pw - m, 0, m, W.ph);
            glClear(GL_COLOR_BUFFER_BIT);
            glDisable(GL_SCISSOR_TEST);
        }
        W.last_wide = W.wide_drawn;
        W.wide_drawn = false;
        W.last3d = W.drew3d;
        W.drew3d = false;
    }
}

// register the displayed frame buffer as a wide region (2 slots, least recently seen is replaced)
void note_display(const GpuDisplay &d) {
    if (g_wide_k >= 0.999f || d.depth24 || d.hres <= 0 || d.vres <= 0) return;
    int x = d.disp_x, y = d.disp_y, w = std::min(d.hres, 1024 - d.disp_x), h = std::min(d.vres, 512 - d.disp_y);
    for (Wide &W : wide)
        if (W.tex_disp && W.x == x && W.y == y && W.w == w && W.h == h) { W.seen = frame_no; return; }
    Wide *slot = &wide[0];
    for (Wide &W : wide) if (!W.tex_disp || W.seen < slot->seen) slot = &W;
    {
        static int nlog;
        if (wide_log() && nlog++ < 300)
            fprintf(stderr, "wide: new twin for display %d,%d %dx%d at frame %u (replacing %d,%d)\n", x, y, w, h, frame_no,
                    slot->x, slot->y);
    }
    free_wide(*slot);
    slot->x = x; slot->y = y; slot->w = w; slot->h = h; slot->seen = frame_no;
    alloc_wide(*slot);
    // Start from what the frame buffer already shows (centered), not black: a screen drawn once and then only
    // partly redrawn (the loading screen's backdrop under the animated head) would otherwise keep black holes
    // when the twin is created mid-screen (switching to widescreen, or a new display buffer).
    if (g_dbg_seed_wide) {
        sync_read();
        copy_into_wide((int)(slot - wide), tex_read, x * S, y * S, 1, x, y, w, h);
    }
}

int wide_for_rect(int x0, int y0, int x1, int y1) { // a wide region containing this VRAM rect
    if (g_wide_k >= 0.999f) return -1;
    for (int i = 0; i < 2; i++) {
        const Wide &W = wide[i];
        if (W.tex_disp && x0 >= W.x && y0 >= W.y && x1 <= W.x + W.w - 1 && y1 <= W.y + W.h - 1) return i;
    }
    return -1;
}

void wide_mark(Wide &W, int y0, int y1) { // local rows, inclusive
    y0 = std::max(y0, 0); y1 = std::min(y1, W.h - 1);
    for (int y = y0; y <= y1; y++) W.drows[y] = 1;
    W.touched = true;
    if (y0 <= y1) W.dirty = true;
}

void sync_wide(int i, int y0, int y1) { // refresh the read copy if rows y0..y1 (local) were written
    Wide &W = wide[i];
    if (!W.dirty) return;
    y0 = std::max(y0, 0); y1 = std::min(y1, W.h - 1);
    bool hit = false;
    for (int y = y0; y <= y1 && !hit; y++) hit = W.drows[y] != 0;
    if (!hit) return;
    st_sync_wide++;
    pc_sync_wide++;
    flush();
    GLF(BindFramebuffer)(GL_READ_FRAMEBUFFER, W.fbo_draw);
    GLF(BindFramebuffer)(GL_DRAW_FRAMEBUFFER, W.fbo_read);
    glDisable(GL_SCISSOR_TEST);
    GLF(BlitFramebuffer)(0, 0, W.pw, W.ph, 0, 0, W.pw, W.ph, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    W.dirty = false;
    std::fill(W.drows.begin(), W.drows.end(), 0);
}

// copy draw -> read for the bounding box of everything written since the last refresh
void sync_read() {
    if (!any_dirty) return;
    st_sync_read++;
    pc_sync_read++;
    flush();
    int y0 = 512, y1 = -1;
    uint64_t cols = 0;
    for (int y = 0; y < 512; y++)
        if (dmask[y]) { cols |= dmask[y]; y0 = std::min(y0, y); y1 = y; }
    if (y1 >= 0) {
        int c0 = __builtin_ctzll(cols), c1 = 63 - __builtin_clzll(cols);
        GLF(BindFramebuffer)(GL_READ_FRAMEBUFFER, fbo_draw);
        GLF(BindFramebuffer)(GL_DRAW_FRAMEBUFFER, fbo_read);
        glDisable(GL_SCISSOR_TEST);
        int ax = c0 * 16 * S, ay = y0 * S, bx = (c1 + 1) * 16 * S, by = (y1 + 1) * S;
        GLF(BlitFramebuffer)(ax, ay, bx, by, ax, ay, bx, by, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        GLF(BindFramebuffer)(GL_FRAMEBUFFER, fbo_draw);
    }
    memset(dmask, 0, sizeof dmask);
    any_dirty = false;
}

void set_blend(int b) {
    if (b == BLEND_NONE) { glDisable(GL_BLEND); return; }
    glEnable(GL_BLEND);
    switch (b) {
    case BLEND_HALF:
        GLF(BlendColor)(0.5f, 0.5f, 0.5f, 0.5f);
        GLF(BlendEquationSeparate)(GL_FUNC_ADD, GL_FUNC_ADD);
        GLF(BlendFuncSeparate)(GL_CONSTANT_COLOR, GL_CONSTANT_COLOR, GL_ONE, GL_ZERO);
        break;
    case BLEND_ADD:
        GLF(BlendEquationSeparate)(GL_FUNC_ADD, GL_FUNC_ADD);
        GLF(BlendFuncSeparate)(GL_ONE, GL_ONE, GL_ONE, GL_ZERO);
        break;
    case BLEND_SUB:
        GLF(BlendEquationSeparate)(GL_FUNC_REVERSE_SUBTRACT, GL_FUNC_ADD);
        GLF(BlendFuncSeparate)(GL_ONE, GL_ONE, GL_ONE, GL_ZERO);
        break;
    case BLEND_QUARTER:
        GLF(BlendColor)(0.25f, 0.25f, 0.25f, 0.25f);
        GLF(BlendEquationSeparate)(GL_FUNC_ADD, GL_FUNC_ADD);
        GLF(BlendFuncSeparate)(GL_CONSTANT_COLOR, GL_ONE, GL_ONE, GL_ZERO);
        break;
    }
}

void flush() {
    if (verts_empty()) { clear_batches(); return; }
    int base[3];
    vert_all.clear();
    for (int i = 0; i < 3; i++) { base[i] = (int)vert_all.size(); vert_all.insert(vert_all.end(), vert_lists[i].begin(), vert_lists[i].end()); }
    st_flush++;
    pc_flush++;
    GLF(UseProgram)(prog_draw);
    GLF(ActiveTexture)(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex_read);
    GLF(Uniform1i)(u_draw_vram, 0);
    GLF(Uniform1i)(u_draw_wide, 1);
    GLF(Uniform1i)(u_draw_scale, S);
    GLF(Uniform1i)(u_draw_smooth, g_smooth);
    GLF(Uniform1i)(u_draw_edge_clamp, g_dbg_edge_clamp);
    GLF(Uniform1i)(u_draw_samples, NS);
    GLF(BindVertexArray)(vao);
    GLF(BindBuffer)(GL_ARRAY_BUFFER, vbo);
    GLF(BufferData)(GL_ARRAY_BUFFER, (ptrdiff_t)(vert_all.size() * sizeof(Vtx)), vert_all.data(), GL_STREAM_DRAW);
    glEnable(GL_SCISSOR_TEST);
    int cur_blend = -1, cur_target = -2, cur_src = -2;
    for (int li = 0; li < 3; li++)
    for (const Batch &b : batch_lists[li]) {
        if (b.target != cur_target) {
            cur_target = b.target;
            if (b.target < 0) {
                GLF(BindFramebuffer)(GL_FRAMEBUFFER, fbo_draw);
                glViewport(0, 0, 1024 * S, 512 * S);
                GLF(Uniform2f)(u_draw_size, 1024.f, 512.f);
            } else {
                const Wide &W = wide[b.target];
                GLF(BindFramebuffer)(GL_FRAMEBUFFER, W.fbo_draw);
                glViewport(0, 0, W.pw, W.ph);
                GLF(Uniform2f)(u_draw_size, (float)W.pw / S, (float)W.h);
            }
        }
        if (b.src != cur_src) {
            cur_src = b.src;
            GLF(ActiveTexture)(GL_TEXTURE0 + 1);
            glBindTexture(GL_TEXTURE_2D, b.src >= 0 ? wide[b.src].tex_read : tex_read);
            GLF(ActiveTexture)(GL_TEXTURE0);
        }
        if (b.blend != cur_blend) { set_blend(b.blend); cur_blend = b.blend; }
        glScissor(b.sx, b.sy, b.sw, b.sh);
        glDrawArrays(GL_TRIANGLES, base[li] + b.first, b.count);
    }
    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
    clear_verts();
    clear_batches();
}

void push_batch(int blend, int target, int src, int sx, int sy, int sw, int sh, int nv) {
    int first = (int)vlist(target).size() - nv;
    std::vector<Batch> &batches = blist(target);
    if (!batches.empty()) {
        Batch &l = batches.back();
        if (l.blend == blend && l.target == target && l.src == src && l.sx == sx && l.sy == sy && l.sw == sw &&
            l.sh == sh && l.first + l.count == first) {
            l.count += nv;
            return;
        }
    }
    if (target >= 0) st_wide_batches++;
    batches.push_back({blend, target, src, sx, sy, sw, sh, first, nv});
}

void texture_source(const GpuPrim *p, int *x0, int *y0, int *x1, int *y1, int *cx0, int *cy, int *cx1) {
    int tpx = (p->texpage & 15) * 64, tpy = ((p->texpage >> 4) & 1) * 256, depth = (p->texpage >> 7) & 3;
    int wpx = depth == 0 ? 64 : depth == 1 ? 128 : 256;
    *x0 = tpx; *y0 = tpy; *x1 = tpx + wpx - 1; *y1 = tpy + 255;
    *cx0 = (p->clut & 63) * 16; *cy = (p->clut >> 6) & 511;
    *cx1 = *cx0 + (depth == 0 ? 15 : 255);
}

uint16_t cur_box[4];
void add_vertex(const GpuVertex &v, float u, float vv, uint32_t tp, uint32_t clut, uint32_t flags, uint32_t tw,
                float x, float y) {
    vlist(cur_vt).push_back({x, y, v.r, v.g, v.b, 255, u, vv, tp, clut, flags, tw,
                     {cur_box[0], cur_box[1], cur_box[2], cur_box[3]}});
}

// Is the texture drawn at its original size (texel = screen pixel)? Sprites always are; a polygon is when the
// UV->screen mapping has unit scale on both axes (rotation allowed). That's the 2D UI, which stays pixel-sharp.
bool unscaled(const GpuPrim *p) {
    if (p->type == PRIM_RECT) return true;
    if (p->type != PRIM_TRI) return false;
    float du1 = p->v[1].u - p->v[0].u, dv1 = p->v[1].v - p->v[0].v;
    float du2 = p->v[2].u - p->v[0].u, dv2 = p->v[2].v - p->v[0].v;
    float dx1 = p->v[1].x - p->v[0].x, dy1 = p->v[1].y - p->v[0].y;
    float dx2 = p->v[2].x - p->v[0].x, dy2 = p->v[2].y - p->v[0].y;
    float det = du1 * dv2 - du2 * dv1;
    if (std::fabs(det) < 1.0f) return false;
    // screen = M * uv ; M = [dx/du dx/dv; dy/du dy/dv]
    float a = (dx1 * dv2 - dx2 * dv1) / det, b = (dx2 * du1 - dx1 * du2) / det;
    float c = (dy1 * dv2 - dy2 * dv1) / det, d = (dy2 * du1 - dy1 * du2) / det;
    float su = std::sqrt(a * a + c * c), sv = std::sqrt(b * b + d * d), area = std::fabs(a * d - b * c);
    // PS1 quads often map w-1 texels across w pixels: allow a little slack
    return su > 0.85f && su < 1.18f && sv > 0.85f && sv < 1.18f && area > 0.8f && area < 1.3f;
}

void draw_prim(const GpuPrim *p) {
    if (p->clip_x1 < p->clip_x0 || p->clip_y1 < p->clip_y0) return;
    int bx0, by0, bx1, by1; // bounding box (native)
    if (p->type == PRIM_TRI) {
        bx0 = std::min({p->v[0].x, p->v[1].x, p->v[2].x}); bx1 = std::max({p->v[0].x, p->v[1].x, p->v[2].x});
        by0 = std::min({p->v[0].y, p->v[1].y, p->v[2].y}); by1 = std::max({p->v[0].y, p->v[1].y, p->v[2].y});
        if (bx1 - bx0 >= 1024 || by1 - by0 >= 512) return;
    } else if (p->type == PRIM_RECT) {
        bx0 = p->v[0].x; by0 = p->v[0].y; bx1 = bx0 + p->w - 1; by1 = by0 + p->h - 1;
        if (p->w <= 0 || p->h <= 0) return;
    } else if (p->type == PRIM_LINE) {
        bx0 = std::min(p->v[0].x, p->v[1].x); bx1 = std::max(p->v[0].x, p->v[1].x);
        by0 = std::min(p->v[0].y, p->v[1].y); by1 = std::max(p->v[0].y, p->v[1].y);
        if (bx1 - bx0 >= 1024 || by1 - by0 >= 512) return;
    } else return;

    bool tex = p->textured && p->type != PRIM_LINE;
    if (tex) {
        int x0, y0, x1, y1, cx0, cy, cx1;
        texture_source(p, &x0, &y0, &x1, &y1, &cx0, &cy, &cx1);
        // narrow the page to the texels this primitive can actually sample (in-place screen effects read one
        // row while earlier strips wrote others: they must not force a sync each)
        int umin, vmin, umax, vmax;
        if (p->type == PRIM_RECT) { umin = p->v[0].u; vmin = p->v[0].v; umax = umin + p->w - 1; vmax = vmin + p->h - 1; }
        else {
            umin = std::min({p->v[0].u, p->v[1].u, p->v[2].u}); umax = std::max({p->v[0].u, p->v[1].u, p->v[2].u});
            vmin = std::min({p->v[0].v, p->v[1].v, p->v[2].v}); vmax = std::max({p->v[0].v, p->v[1].v, p->v[2].v});
        }
        if (p->texwindow == 0 && umax <= 255 && vmax <= 255) {
            int depth = (p->texpage >> 7) & 3, sh = depth == 0 ? 2 : depth == 1 ? 1 : 0;
            x1 = x0 + (umax >> sh); x0 = x0 + (umin >> sh);
            y1 = y0 + vmax; y0 = y0 + vmin;
        }
        if (region_dirty(x0, y0, x1, y1) || region_dirty(cx0, cy, cx1, cy)) sync_read();
    }
    // 16bpp texture read from GPU-rendered VRAM (screen copies: the wavy BAD/AWFUL effect, AGAIN? background):
    // sample the high-resolution pixels directly and don't smooth them.
    bool hires = false;
    if (tex && ((p->texpage >> 7) & 3) >= 2) {
        int tpx = (p->texpage & 15) * 64, tpy = ((p->texpage >> 4) & 1) * 256;
        int umin, vmin, umax, vmax;
        if (p->type == PRIM_RECT) { umin = p->v[0].u; vmin = p->v[0].v; umax = umin + p->w - 1; vmax = vmin + p->h - 1; }
        else {
            umin = std::min({p->v[0].u, p->v[1].u, p->v[2].u}); umax = std::max({p->v[0].u, p->v[1].u, p->v[2].u});
            vmin = std::min({p->v[0].v, p->v[1].v, p->v[2].v}); vmax = std::max({p->v[0].v, p->v[1].v, p->v[2].v});
        }
        hires = region_rendered(tpx + umin, tpy + vmin, tpx + std::min(umax, 255), tpy + std::min(vmax, 255));
    }
    int cx0 = std::max(bx0, (int)p->clip_x0), cy0 = std::max(by0, (int)p->clip_y0);
    int cx1 = std::min(bx1, (int)p->clip_x1), cy1 = std::min(by1, (int)p->clip_y1);
    mark_dirty(cx0, cy0, cx1, cy1);
    mark_rendered(cx0, cy0, cx1, cy1, true);

    int mode = (p->texpage >> 5) & 3;
    int semi_blend = BLEND_HALF + mode;
    // Pixel-sharp only for 2D art (and HUD sprites the game rotates with the GTE around their own center). 3D scenery
    // drawn near 1:1 (Stage 1's foreground crowd sits at 0.80-0.87 scale, right on the threshold) flipped between
    // sharp and smoothed from frame to frame as the camera drifted, even between the two halves of one sprite.
    bool gte_hud = p->gte3d && p->gte_ofx != 0x7FFF && std::abs(p->gte_ofx - (p->clip_x0 + p->clip_x1 + 1) * 0.5f) > 2.0f;
    bool ui = tex && !hires && g_ui_sharp && (!p->gte3d || gte_hud || !g_dbg_sharp_2d_only) && unscaled(p);
    uint32_t base = (tex ? F_TEX : 0) | (tex && p->raw ? F_RAW : 0) | (p->mask_set ? F_MASK : 0) | (hires ? F_HIRES : 0) |
                    (ui ? F_NEAREST : 0);
    // passes: {flags, blend}
    struct Pass { uint32_t flags; int blend; } passes[2];
    int np = 0;
    if (!p->semi) passes[np++] = {base, BLEND_NONE};
    else if (!tex) passes[np++] = {base, semi_blend};
    else { passes[np++] = {base | F_PASS_OPAQUE, BLEND_NONE}; passes[np++] = {base | F_PASS_SEMI, semi_blend}; }

    uint32_t tp = p->texpage, clut = p->clut, tw = p->texwindow;
    if (p->type == PRIM_RECT) {
        cur_box[0] = p->v[0].u; cur_box[1] = p->v[0].v;
        cur_box[2] = (uint16_t)(p->v[0].u + p->w - 1); cur_box[3] = (uint16_t)(p->v[0].v + p->h - 1);
    } else {
        cur_box[0] = std::min({p->v[0].u, p->v[1].u, p->v[2].u}); cur_box[1] = std::min({p->v[0].v, p->v[1].v, p->v[2].v});
        cur_box[2] = std::max({p->v[0].u, p->v[1].u, p->v[2].u}); cur_box[3] = std::max({p->v[0].v, p->v[1].v, p->v[2].v});
    }
    // geometry in VRAM coordinates
    struct GV { float x, y, u, v; GpuVertex c; } g[6];
    int ng = 0;
    if (p->type == PRIM_TRI) {
        for (int i = 0; i < 3; i++) {
            GpuVertex c = p->v[i];
            if (!p->gouraud) { c.r = p->v[0].r; c.g = p->v[0].g; c.b = p->v[0].b; }
            float fx = g_subpixel ? p->v[i].fx * (1.0f / 256.0f) : 0.0f, fy = g_subpixel ? p->v[i].fy * (1.0f / 256.0f) : 0.0f;
            g[ng++] = {(float)p->v[i].x + fx, (float)p->v[i].y + fy, (float)p->v[i].u, (float)p->v[i].v, c};
        }
        // 3D slivers (Stage 1's drum and cymbal stands are triangles under a native pixel thick): the PS1 still
        // fills a solid 1-px line for them, but at a high internal resolution they come out a fraction of a pixel
        // wide and break up/flicker as they move. Widen them to one native pixel across their longest edge.
        if (p->gte3d && g_thin_fix) {
            int e = 0; float best = -1;
            for (int i = 0; i < 3; i++) {
                float dx = g[(i + 1) % 3].x - g[i].x, dy = g[(i + 1) % 3].y - g[i].y, l = dx * dx + dy * dy;
                if (l > best) { best = l; e = i; }
            }
            float len = std::sqrt(best);
            if (len >= 3.0f) {
                GV &a = g[e], &b = g[(e + 1) % 3], &o = g[(e + 2) % 3];
                float nx = -(b.y - a.y) / len, ny = (b.x - a.x) / len;
                float t = (o.x - a.x) * nx + (o.y - a.y) * ny; // signed thickness
                if (t < 0) { nx = -nx; ny = -ny; t = -t; }
                if (t < 0.9f) {
                    float grow = (1.0f - t) * 0.5f;
                    a.x -= nx * grow; a.y -= ny * grow; b.x -= nx * grow; b.y -= ny * grow;
                    o.x += nx * grow; o.y += ny * grow;
                }
            }
        }
    } else if (p->type == PRIM_RECT) {
        const GpuVertex &v = p->v[0];
        float x0 = v.x, y0 = v.y, x1 = (float)v.x + p->w, y1 = (float)v.y + p->h;
        float u0 = v.u, v0 = v.v, u1 = (float)v.u + p->w, v1 = (float)v.v + p->h;
        g[ng++] = {x0, y0, u0, v0, v}; g[ng++] = {x1, y0, u1, v0, v}; g[ng++] = {x0, y1, u0, v1, v};
        g[ng++] = {x1, y0, u1, v0, v}; g[ng++] = {x1, y1, u1, v1, v}; g[ng++] = {x0, y1, u0, v1, v};
    } else { // line: a 1-native-pixel-wide quad, endpoints inclusive
        const GpuVertex &a = p->v[0], &b = p->v[1];
        float ax = a.x + 0.5f, ay = a.y + 0.5f, bx = b.x + 0.5f, by = b.y + 0.5f;
        float dx = bx - ax, dy = by - ay;
        float len = std::max(std::abs(dx), std::abs(dy));
        float ex = 0, ey = 0;
        if (len > 0) { ex = dx / len * 0.5f; ey = dy / len * 0.5f; }
        ax -= ex; ay -= ey; bx += ex; by += ey;
        float nx = std::abs(dx) >= std::abs(dy) ? 0.f : 0.5f, ny = std::abs(dx) >= std::abs(dy) ? 0.5f : 0.f;
        GpuVertex cb = p->gouraud ? b : a;
        g[ng++] = {ax - nx, ay - ny, 0, 0, a}; g[ng++] = {ax + nx, ay + ny, 0, 0, a}; g[ng++] = {bx - nx, by - ny, 0, 0, cb};
        g[ng++] = {ax + nx, ay + ny, 0, 0, a}; g[ng++] = {bx + nx, by + ny, 0, 0, cb}; g[ng++] = {bx - nx, by - ny, 0, 0, cb};
    }

    // target 1: VRAM (always: keeps render-to-texture and the 4:3 view exact). When the primitive also goes to
    // a wide twin, the VRAM copy isn't what's shown (screen copies read the twin), so skip the costly smoothing.
    uint32_t vram_extra = wide_for_rect(p->clip_x0, p->clip_y0, p->clip_x1, p->clip_y1) >= 0 ? (uint32_t)F_NEAREST : 0u;
    for (int k = 0; k < np; k++) {
        cur_vt = -1;
        for (int i = 0; i < ng; i++) add_vertex(g[i].c, g[i].u, g[i].v, tp, clut, passes[k].flags | vram_extra, tw, g[i].x, g[i].y);
        push_batch(passes[k].blend, -1, -1, p->clip_x0 * S, p->clip_y0 * S, (p->clip_x1 - p->clip_x0 + 1) * S,
                   (p->clip_y1 - p->clip_y0 + 1) * S, ng);
    }

    // target 2: the wide twin of the frame buffer this primitive draws into
    int wi = wide_for_rect(p->clip_x0, p->clip_y0, p->clip_x1, p->clip_y1);
    if (wi < 0) return;
    Wide &W = wide[wi];
    // margin snapped to whole target pixels: 2D art at a fractional offset (Auto aspect) samples texel edges unevenly
    float ww = wide_native_w(W), cx = W.x + W.w * 0.5f, margin = g_dbg_margin_snap ? std::round((ww - W.w) * 0.5f * S) / S : (ww - W.w) * 0.5f;
    int src = -1;
    if (hires) { // screen copy: map it wide-to-wide when its source is a wide frame buffer too
        int tpx = (p->texpage & 15) * 64, tpy = ((p->texpage >> 4) & 1) * 256;
        src = wide_for_rect(tpx + cur_box[0], tpy + cur_box[1], tpx + std::min<int>(cur_box[2], 255), tpy + std::min<int>(cur_box[3], 255));
        if (src >= 0) {
            int vmax = p->type == PRIM_RECT ? cur_box[1] + p->h - 1 : cur_box[3];
            sync_wide(src, tpy + cur_box[1] - wide[src].y, tpy + std::min(vmax, 255) - wide[src].y);
        }
    }
    // A 2D primitive covering the whole frame is a backdrop or overlay, not UI (Stage 2's sky is two flat
    // full-screen triangles; fades too): it fills the wide frame. Textured ones only in 3D scenes, so 2D
    // menu backgrounds stay pillarboxed instead of stretched.
    // Untextured: any band spanning the full width counts (Stage 2 draws the sky only down to the horizon, so it
    // isn't always full height). Textured: full frame only, and only in 3D scenes.
    bool fullwidth = bx0 <= W.x && bx1 >= W.x + W.w - 1;
    bool fullframe = fullwidth && by0 <= W.y + 8 && by1 >= W.y + W.h - 9;
    // Large geometry reaching (or past) the 4:3 edges in a 3D scene (3D drawn the frame before; not last_wide,
    // which this rule would keep alive by itself in 2D menus) is scenery too: fever mode's burst background is
    // made of 2D rays from the screen center out past the edges (only the center vertex comes from the GTE, so
    // the 3D tag misses them). HUD elements stay inside the screen.
    // Include the top/bottom edges: nearly vertical burst rays can miss both horizontal edges. Leaving
    // those rays pillarboxed while their neighbors stretch pulls their shared edges apart in ultrawide.
    bool offscreen = !tex && (((bx0 <= W.x || bx1 >= W.x + W.w - 1) && bx1 - bx0 >= W.w / 3) ||
                              ((by0 <= W.y || by1 >= W.y + W.h - 1) && by1 - by0 >= W.h / 3));
    // GTE geometry projected around a center other than the screen's (HUD sprites rotated with RotTransPers
    // around their own position: the head flip and the spinning button symbols) is UI: undo the squeeze around
    // its own center and place it like 2D, instead of stretching it away from the screen center.
    bool gte_ui = p->gte3d && p->gte_ofx != 0x7FFF && std::abs(p->gte_ofx - cx) > 2.0f && !g_stretch_ui;
    if (gte_ui)
        for (int i = 0; i < ng; i++) g[i].x = p->gte_ofx + (g[i].x - p->gte_ofx) / g_wide_k;
    bool stretch = (p->gte3d && !gte_ui) || src >= 0 || g_stretch_ui || (!tex && fullwidth) || (tex && fullframe && W.last_wide) ||
                   (offscreen && W.last3d);
    if (p->gte3d && !gte_ui) W.drew3d = true;
    auto X = [&](float x) { return stretch ? (x - cx) / g_wide_k + ww * 0.5f : x - W.x + margin; };
    if (stretch) {
        float mx0 = X((float)std::max(bx0, (int)p->clip_x0)), mx1 = X((float)std::min(bx1, (int)p->clip_x1) + 1);
        if (mx0 < margin - 0.5f || mx1 > margin + W.w + 0.5f) {
            if (!W.wide_drawn && getenv("UJL_WIDE_DEBUG"))
                fprintf(stderr, "wide: margin reached by type%d tex%d g%d src%d xy(%d,%d) bb x%d..%d y%d..%d\n", p->type, (int)tex, p->gte3d, src,
                        p->v[0].x, p->v[0].y, bx0, bx1, by0, by1);
            W.wide_drawn = true;
        }
    }
    float ux0 = X((float)p->clip_x0), ux1 = X((float)p->clip_x1 + 1);
    if (stretch && p->clip_x0 <= W.x && p->clip_x1 >= W.x + W.w - 1) { ux0 = 0; ux1 = ww; } // full width
    int sx = std::max(0, (int)std::floor(ux0 * S)), sx1 = std::min(W.pw, (int)std::ceil(ux1 * S));
    int sy = (p->clip_y0 - W.y) * S, sh = (p->clip_y1 - p->clip_y0 + 1) * S;
    if (sx1 <= sx) return;
    for (int k = 0; k < np; k++) {
        uint32_t fl = passes[k].flags;
        if (src >= 0) fl = (fl & ~(uint32_t)F_HIRES) | F_WIDESRC;
        cur_vt = wi;
        for (int i = 0; i < ng; i++) {
            float u = g[i].u, v = g[i].v;
            if (src >= 0) { // source texel -> source wide-twin pixel coordinates
                const Wide &Sw = wide[src];
                int tpx = (p->texpage & 15) * 64, tpy = ((p->texpage >> 4) & 1) * 256;
                u = ((float)tpx + u - (Sw.x + Sw.w * 0.5f)) / g_wide_k + wide_native_w(Sw) * 0.5f;
                v = (float)tpy + v - Sw.y;
            }
            add_vertex(g[i].c, u, v, tp, clut, fl, tw, X(g[i].x), g[i].y - W.y);
        }
        push_batch(passes[k].blend, wi, src, sx, sy, sx1 - sx, sh, ng);
    }
    wide_mark(W, std::max(by0, (int)p->clip_y0) - W.y, std::min(by1, (int)p->clip_y1) - W.y);
}

// copies `src` (texture, coords in its own texels) into the draw target at native rect (dx,dy,w,h)
void copy_into_draw(GLuint src, int src_x, int src_y, int div, int dx, int dy, int w, int h) {
    GLF(BindFramebuffer)(GL_FRAMEBUFFER, fbo_draw);
    glViewport(0, 0, 1024 * S, 512 * S);
    glDisable(GL_BLEND);
    glEnable(GL_SCISSOR_TEST);
    glScissor(dx * S, dy * S, w * S, h * S);
    GLF(UseProgram)(prog_copy);
    GLF(ActiveTexture)(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, src);
    GLF(Uniform1i)(u_copy_src, 0);
    GLF(Uniform2i)(u_copy_off, src_x, src_y);
    GLF(Uniform1i)(u_copy_div, div);
    GLF(Uniform2i)(u_copy_dst, dx * S, dy * S);
    GLF(BindVertexArray)(vao_empty);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glDisable(GL_SCISSOR_TEST);
}

std::vector<uint32_t> stage_px;
int stage_w, stage_h;

inline uint32_t raw_to_rgba(uint16_t c) {
    uint32_t r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
    r = r << 3 | r >> 2; g = g << 3 | g >> 2; b = b << 3 | b >> 2;
    return r | g << 8 | b << 16 | (c & 0x8000 ? 0xFF000000u : 0);
}

// copy native VRAM-sized content into a wide twin, centered (pillarboxed 2D: FMV frames, loading screens)
void copy_into_wide(int wi, GLuint src, int src_x, int src_y, int div, int dx, int dy, int w, int h) {
    pc_wide_copy++;
    Wide &W = wide[wi];
    float margin = (wide_native_w(W) - W.w) * 0.5f;
    int ox = (int)std::lround((dx - W.x + margin) * S), oy = (dy - W.y) * S;
    GLF(BindFramebuffer)(GL_FRAMEBUFFER, W.fbo_draw);
    glViewport(0, 0, W.pw, W.ph);
    glDisable(GL_BLEND);
    glEnable(GL_SCISSOR_TEST);
    glScissor(ox, oy, w * S, h * S);
    GLF(UseProgram)(prog_copy);
    GLF(ActiveTexture)(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, src);
    GLF(Uniform1i)(u_copy_src, 0);
    GLF(Uniform2i)(u_copy_off, src_x, src_y);
    GLF(Uniform1i)(u_copy_div, div);
    GLF(Uniform2i)(u_copy_dst, ox, oy);
    GLF(BindVertexArray)(vao_empty);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glDisable(GL_SCISSOR_TEST);
    wide_mark(W, dy - W.y, dy - W.y + h - 1);
}

void do_upload(const RecRect *r) {
    pc_upload++;
    flush();
    const uint16_t *px = (const uint16_t *)(r + 1);
    int w = r->w, h = r->h;
    stage_px.resize((size_t)w * h);
    for (int i = 0; i < w * h; i++) stage_px[i] = raw_to_rgba(px[i]);
    glBindTexture(GL_TEXTURE_2D, tex_stage);
    if (w > stage_w || h > stage_h) {
        stage_w = std::max(stage_w, w); stage_h = std::max(stage_h, h);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, stage_w, stage_h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    }
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, stage_px.data());
    // handle horizontal/vertical wrap by splitting into up to 4 pieces
    for (int py = 0; py < h;) {
        int y = (r->y + py) & 511, hh = std::min(h - py, 512 - y);
        for (int pxo = 0; pxo < w;) {
            int x = (r->x + pxo) & 1023, ww = std::min(w - pxo, 1024 - x);
            copy_into_draw(tex_stage, pxo, py, S, x, y, ww, hh);
            { int wi = wide_for_rect(x, y, x + ww - 1, y + hh - 1); if (wi >= 0) copy_into_wide(wi, tex_stage, pxo, py, S, x, y, ww, hh); }
            mark_dirty(x, y, x + ww - 1, y + hh - 1);
            mark_rendered(x, y, x + ww - 1, y + hh - 1, false);
            pxo += ww;
        }
        py += hh;
    }
}

void do_fill(const RecFill *f) {
    pc_fill++;
    flush();
    int x = f->x & 1023, y = f->y & 511;
    int w = std::min((int)f->w, 1024 - x), h = std::min((int)f->h, 512 - y);
    if (w <= 0 || h <= 0) return;
    GLF(BindFramebuffer)(GL_FRAMEBUFFER, fbo_draw);
    glEnable(GL_SCISSOR_TEST);
    glScissor(x * S, y * S, w * S, h * S);
    uint32_t c = f->rgb;
    glClearColor((float)((c >> 3) & 31) / 31.f, (float)((c >> 11) & 31) / 31.f, (float)((c >> 19) & 31) / 31.f, 0.f);
    glClear(GL_COLOR_BUFFER_BIT);
    glDisable(GL_SCISSOR_TEST);
    mark_dirty(x, y, x + w - 1, y + h - 1);
    mark_rendered(x, y, x + w - 1, y + h - 1, true);
    int wi = wide_for_rect(x, y, x + w - 1, y + h - 1);
    if (wi >= 0) { // a fill covering the whole frame buffer clears the whole wide twin (margins too)
        Wide &W = wide[wi];
        bool full = x <= W.x && y <= W.y && x + w >= W.x + W.w && y + h >= W.y + W.h;
        float margin = (wide_native_w(W) - W.w) * 0.5f;
        GLF(BindFramebuffer)(GL_FRAMEBUFFER, W.fbo_draw);
        glEnable(GL_SCISSOR_TEST);
        if (full && W.last_wide) glScissor(0, 0, W.pw, W.ph); // 3D scene: the clear color is its backdrop
        else glScissor((int)std::lround((x - W.x + margin) * S), (y - W.y) * S, w * S, h * S);
        glClear(GL_COLOR_BUFFER_BIT);
        if (full && !W.last_wide) { // 2D screen: plain black pillarbox bars
            int m = (int)std::floor(margin * S);
            glClearColor(0, 0, 0, 0);
            glScissor(0, 0, m, W.ph);
            glClear(GL_COLOR_BUFFER_BIT);
            glScissor(W.pw - m, 0, m, W.ph);
            glClear(GL_COLOR_BUFFER_BIT);
        }
        glDisable(GL_SCISSOR_TEST);
        wide_mark(W, full ? 0 : y - W.y, full ? W.h - 1 : y - W.y + h - 1);
    }
}

// whole-frame copy between two frame buffers that both have wide twins: copy the twins (full width)
void copy_wide_to_wide(int si, int di) {
    Wide &Sw = wide[si], &Dw = wide[di];
    flush();
    if (Sw.dirty) { // force a fresh read copy of the whole source twin
        GLF(BindFramebuffer)(GL_READ_FRAMEBUFFER, Sw.fbo_draw);
        GLF(BindFramebuffer)(GL_DRAW_FRAMEBUFFER, Sw.fbo_read);
        glDisable(GL_SCISSOR_TEST);
        GLF(BlitFramebuffer)(0, 0, Sw.pw, Sw.ph, 0, 0, Sw.pw, Sw.ph, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        Sw.dirty = false;
        std::fill(Sw.drows.begin(), Sw.drows.end(), 0);
    }
    GLF(BindFramebuffer)(GL_FRAMEBUFFER, Dw.fbo_draw);
    glViewport(0, 0, Dw.pw, Dw.ph);
    glDisable(GL_BLEND);
    glEnable(GL_SCISSOR_TEST);
    glScissor(0, 0, std::min(Dw.pw, Sw.pw), std::min(Dw.ph, Sw.ph));
    GLF(UseProgram)(prog_copy);
    GLF(ActiveTexture)(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, Sw.tex_read);
    GLF(Uniform1i)(u_copy_src, 0);
    GLF(Uniform2i)(u_copy_off, 0, 0);
    GLF(Uniform1i)(u_copy_div, 1);
    GLF(Uniform2i)(u_copy_dst, 0, 0);
    GLF(BindVertexArray)(vao_empty);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glDisable(GL_SCISSOR_TEST);
    wide_mark(Dw, 0, Dw.h - 1);
    Dw.wide_drawn = true; // a full-width image now fills the margins
}

void do_copy(const RecCopy *c) {
    pc_copy++;
    sync_read();
    int w = std::min((int)c->w, std::min(1024 - c->sx, 1024 - c->dx));
    int h = std::min((int)c->h, std::min(512 - c->sy, 512 - c->dy));
    if (w <= 0 || h <= 0) return;
    copy_into_draw(tex_read, c->sx * S, c->sy * S, 1, c->dx, c->dy, w, h);
    {
        int wi = wide_for_rect(c->dx, c->dy, c->dx + w - 1, c->dy + h - 1);
        int si = wide_for_rect(c->sx, c->sy, c->sx + w - 1, c->sy + h - 1);
        if (wi >= 0 && si >= 0 && si != wi && c->sx == wide[si].x && c->sy == wide[si].y && c->dx == wide[wi].x &&
            c->dy == wide[wi].y && w >= wide[wi].w && h >= wide[wi].h)
            copy_wide_to_wide(si, wi);   // e.g. the AGAIN? screen re-copies its frozen frame every frame
        else if (wi >= 0)
            copy_into_wide(wi, tex_read, c->sx * S, c->sy * S, 1, c->dx, c->dy, w, h);
    }
    mark_dirty(c->dx, c->dy, c->dx + w - 1, c->dy + h - 1);
    if (region_rendered(c->sx, c->sy, c->sx + w - 1, c->sy + h - 1)) mark_rendered(c->dx, c->dy, c->dx + w - 1, c->dy + h - 1, true);
}

// UJL_GL_CHECK=1: glGetError after every replayed command, naming the step (finds which one a GL error at the end
// of hw_replay came from; a new GL, e.g. ES on iOS, is stricter than desktop GL in places)
void gl_check(const char *where) {
    static int on = -1;
    if (on < 0) on = getenv("UJL_GL_CHECK") != nullptr;
    if (!on) return;
    static int n;
    for (GLenum e; (e = glGetError()) != GL_NO_ERROR;)
        if (n++ < 40) fprintf(stderr, "hw: GL error 0x%x after %s (frame %u)\n", e, where, frame_no);
}

struct GlState {
    GLint fbo, prog, vao, tex, viewport[4], scissor_box[4];
    GLboolean blend, scissor;
};
void save_state(GlState &s) {
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &s.fbo);
    glGetIntegerv(GL_CURRENT_PROGRAM, &s.prog);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &s.vao);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &s.tex);
    glGetIntegerv(GL_VIEWPORT, s.viewport);
    glGetIntegerv(GL_SCISSOR_BOX, s.scissor_box);
    s.blend = glIsEnabled(GL_BLEND);
    s.scissor = glIsEnabled(GL_SCISSOR_TEST);
}
void restore_state(const GlState &s) {
    GLF(BindFramebuffer)(GL_FRAMEBUFFER, (GLuint)s.fbo);
    GLF(UseProgram)((GLuint)s.prog);
    GLF(BindVertexArray)((GLuint)s.vao);
    glBindTexture(GL_TEXTURE_2D, (GLuint)s.tex);
    glViewport(s.viewport[0], s.viewport[1], s.viewport[2], s.viewport[3]);
    glScissor(s.scissor_box[0], s.scissor_box[1], s.scissor_box[2], s.scissor_box[3]);
    if (s.blend) glEnable(GL_BLEND); else glDisable(GL_BLEND);
    if (s.scissor) glEnable(GL_SCISSOR_TEST); else glDisable(GL_SCISSOR_TEST);
}

} // namespace

// OpenGL ES 3.0 (iOS) takes the same shaders as GLSL ES 3.00: explicit default precisions, and no
// `noperspective` (not in ES 3.0). Every vertex has w = 1, so perspective-correct interpolation is the same thing.
static bool g_gles;
static std::string glsl(const char *src) {
    std::string s = src;
    if (!g_gles) return s;
    s.replace(0, strlen("#version 330 core"),
              "#version 300 es\nprecision highp float;\nprecision highp int;\nprecision highp sampler2D;");
    for (size_t p; (p = s.find("noperspective ")) != std::string::npos;) s.erase(p, strlen("noperspective "));
    return s;
}

bool hw_init(int scale, int samples) {
    S = std::max(1, std::min(scale, 8));
    want_ns = std::max(1, samples);
    GLint major = 3;
    glGetIntegerv(0x821B /* GL_MAJOR_VERSION */, &major);
    const char *ver = (const char *)glGetString(GL_VERSION);
    g_gles = ver && !strncmp(ver, "OpenGL ES", 9);
    have_sample_mask = major >= 4 && !g_gles;
    GLint prev_fbo = 0;   // the window's framebuffer (not 0 on iOS, where UIKit's view owns one)
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev_fbo);
    std::string fs = glsl(k_fs);
    if (have_sample_mask) fs.replace(0, strlen("#version 330 core"), "#version 400 core\n#define HAVE_SAMPLE_MASK 1");
    prog_draw = link(glsl(k_vs).c_str(), fs.c_str());
    u_draw_vram = GLF(GetUniformLocation)(prog_draw, "u_vram");
    u_draw_scale = GLF(GetUniformLocation)(prog_draw, "u_scale");
    u_draw_smooth = GLF(GetUniformLocation)(prog_draw, "u_smooth");
    u_draw_edge_clamp = GLF(GetUniformLocation)(prog_draw, "u_edge_clamp");
    u_draw_samples = GLF(GetUniformLocation)(prog_draw, "u_samples");
    u_draw_size = GLF(GetUniformLocation)(prog_draw, "u_size");
    u_draw_wide = GLF(GetUniformLocation)(prog_draw, "u_wide");
    prog_copy = link(glsl(k_copy_vs).c_str(), glsl(k_copy_fs).c_str());
    u_copy_src = GLF(GetUniformLocation)(prog_copy, "u_src");
    u_copy_off = GLF(GetUniformLocation)(prog_copy, "u_off");
    u_copy_div = GLF(GetUniformLocation)(prog_copy, "u_div");
    u_copy_dst = GLF(GetUniformLocation)(prog_copy, "u_dst");

    GLF(GenVertexArrays)(1, &vao);
    GLF(GenVertexArrays)(1, &vao_empty);
    GLF(GenBuffers)(1, &vbo);
    GLF(BindVertexArray)(vao);
    GLF(BindBuffer)(GL_ARRAY_BUFFER, vbo);
    GLF(EnableVertexAttribArray)(0);
    GLF(VertexAttribPointer)(0, 2, GL_FLOAT, GL_FALSE, sizeof(Vtx), (void *)offsetof(Vtx, x));
    GLF(EnableVertexAttribArray)(1);
    GLF(VertexAttribPointer)(1, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(Vtx), (void *)offsetof(Vtx, r));
    GLF(EnableVertexAttribArray)(2);
    GLF(VertexAttribPointer)(2, 2, GL_FLOAT, GL_FALSE, sizeof(Vtx), (void *)offsetof(Vtx, u));
    GLF(EnableVertexAttribArray)(3);
    GLF(VertexAttribIPointer)(3, 4, GL_UNSIGNED_INT, sizeof(Vtx), (void *)offsetof(Vtx, tp));
    GLF(EnableVertexAttribArray)(4);
    GLF(VertexAttribIPointer)(4, 4, GL_UNSIGNED_SHORT, sizeof(Vtx), (void *)offsetof(Vtx, box));
    GLF(BindVertexArray)(0);

    glGenTextures(1, &tex_stage);
    glBindTexture(GL_TEXTURE_2D, tex_stage);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    stage_w = 1024; stage_h = 512;
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, stage_w, stage_h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    alloc_targets();
    GLF(BindFramebuffer)(GL_FRAMEBUFFER, (GLuint)prev_fbo);
    fprintf(stderr, "hw: renderer ready at %dx (%dx%d), %s\n", S, 1024 * S, 512 * S, ver ? ver : "GL ?");
    return true;
}

void hw_set_scale(int scale) {
    scale = std::max(1, std::min(scale, 8));
    if (scale == S) return;
    S = scale;
    GlState st;
    save_state(st);
    alloc_targets();
    restore_state(st);
    clear_verts();
    clear_batches();
}

int hw_scale() { return S; }
int hw_msaa() { return NS; }
bool hw_sprite_edge_aa() { return have_sample_mask; }
void hw_set_msaa(int samples) {
    samples = std::max(1, samples);
    if (samples == want_ns) return;
    want_ns = samples;
    GlState st;
    save_state(st);
    alloc_targets();
    restore_state(st);
    clear_verts();
    clear_batches();
}
void hw_set_smooth(int on) { g_smooth = on; }
void hw_set_ui_sharp(int on) { g_ui_sharp = on; }
void hw_set_subpixel(int on) { g_subpixel = on != 0; }
void hw_set_thin_fix(int on) { g_thin_fix = on != 0; }
int *hw_debug_flag(int i) { // settings > Debug A/B toggles: 0 edge clamp, 1 margin snap, 2 sharp 2D only, 3 seed wide
    static int dummy;
    switch (i) { case 0: return &g_dbg_edge_clamp; case 1: return &g_dbg_margin_snap; case 2: return &g_dbg_sharp_2d_only; case 3: return &g_dbg_seed_wide; }
    return &dummy;
}
unsigned hw_vram_texture() { return tex_disp; }

void hw_set_widescreen(float aspect, bool stretch_ui) {
    float k = aspect > 0 ? std::min(1.0f, (4.0f / 3.0f) / aspect) : 1.0f;
    g_stretch_ui = stretch_ui;
    if (std::fabs(k - g_wide_k) < 1e-4f) return;
    g_wide_k = k;
    GlState st;
    save_state(st);
    for (Wide &w : wide) free_wide(w);
    restore_state(st);
}

bool hw_view(const GpuDisplay &d, HwView *v) {
    for (const Wide &W : wide)
        if (W.tex_disp && g_wide_k < 0.999f && !d.depth24 && W.x == d.disp_x && W.y == d.disp_y && W.h == d.vres) {
            v->tex = W.tex_disp; v->u0 = 0; v->v0 = 0; v->u1 = 1; v->v1 = 1;
            v->aspect = (4.0f / 3.0f) / g_wide_k;
            return true;
        }
    v->tex = tex_disp;
    v->u0 = d.disp_x / 1024.0f; v->v0 = d.disp_y / 512.0f;
    v->u1 = (d.disp_x + d.hres) / 1024.0f; v->v1 = (d.disp_y + d.vres) / 512.0f;
    v->aspect = 4.0f / 3.0f;
    return false;
}

bool hw_replay(const uint8_t *data, size_t len, GpuDisplay *disp) {
    if (!len) return false;
    uint64_t t0 = SDL_GetPerformanceCounter();
    gl_check("(left over from before this replay)");
    GlState st;
    save_state(st);
    gl_check("save_state");
    bool frame = false;
    size_t off = 0;
    while (off + sizeof(RecHdr) <= len) {
        const RecHdr *h = (const RecHdr *)(data + off);
        const void *payload = h + 1;
        switch (h->type) {
        case REC_PRIM: draw_prim((const GpuPrim *)payload); gl_check("prim"); break;
        case REC_UPLOAD: do_upload((const RecRect *)payload); gl_check("upload"); break;
        case REC_FILL: do_fill((const RecFill *)payload); gl_check("fill"); break;
        case REC_COPY: do_copy((const RecCopy *)payload); gl_check("copy"); break;
        case REC_FRAME:
            memcpy(disp, payload, sizeof(GpuDisplay));
            frame = true;
            disp_x0 = disp->disp_x; disp_y0 = disp->disp_y;
            disp_x1 = std::min(1023, disp->disp_x + disp->hres - 1);
            disp_y1 = std::min(511, disp->disp_y + disp->vres - 1);
            frame_no++;
            clear_stale_margins(*disp);
            note_display(*disp);
            if (frame_no % 600 == 0 || g_gpu_rec_drops != st_last_drops) {
                st_last_drops = g_gpu_rec_drops;
                fprintf(stderr, "hw: stats: flush %d sync_read %d wide_batches %d replays %d replay_ms %.1f drops %d alloc_wide %d disp %d,%d t=%llu\n",
                        st_flush, st_sync_read, st_wide_batches, st_replays, st_replay_ms, g_gpu_rec_drops, st_alloc_wide,
                        disp->disp_x, disp->disp_y, (unsigned long long)SDL_GetTicks());
                st_replay_ms = 0;
                st_wide_batches = st_replays = 0;
                st_flush = st_sync_read = st_sync_wide = st_alloc_wide = 0;
            }
            break;
        }
        off += sizeof(RecHdr) + h->size;
    }
    flush();
    gl_check("flush");
    resolve_display();
    gl_check("resolve");
    st_replays++;
    {
        double ms = (double)(SDL_GetPerformanceCounter() - t0) * 1000.0 / (double)SDL_GetPerformanceFrequency();
        st_replay_ms += ms;
        perf_replay_ms += ms;
        perf_replays++;
    }
    for (GLenum e; (e = glGetError()) != GL_NO_ERROR;) { if (st_glerr++ < 20) fprintf(stderr, "hw: GL error 0x%x\n", e); }
    restore_state(st);
    return frame;
}

double hw_take_replay_ms(int *replays) {
    double ms = perf_replay_ms;
    *replays = perf_replays;
    perf_replay_ms = 0;
    perf_replays = 0;
    return ms;
}

void hw_take_counts(char *buf, int len) {
    SDL_snprintf(buf, len, "flush %d sync_read %d sync_wide %d upload %d fill %d copy %d wide_copy %d", pc_flush,
                 pc_sync_read, pc_sync_wide, pc_upload, pc_fill, pc_copy, pc_wide_copy);
    pc_upload = pc_fill = pc_copy = pc_sync_read = pc_sync_wide = pc_flush = pc_wide_copy = 0;
}

void hw_set_log(int on) { g_hw_log = on != 0; }
