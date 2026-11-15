#include "gl_funcs.h"
#include <cstdio>

#define UJL_GL_DEF(ret, name, args) PFN_ujl##name ujl_gl##name;
UJL_GL_FUNCS(UJL_GL_DEF)
#undef UJL_GL_DEF

bool ujl_gl_load(void) {
    bool ok = true;
#define UJL_GL_GET(ret, name, args) \
    ujl_gl##name = (PFN_ujl##name)SDL_GL_GetProcAddress("gl" #name); \
    if (!ujl_gl##name) { fprintf(stderr, "gl: missing gl" #name "\n"); ok = false; }
    UJL_GL_FUNCS(UJL_GL_GET)
#undef UJL_GL_GET
    return ok;
}
