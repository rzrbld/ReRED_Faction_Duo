/* gl_load.c - resolves RR_GL_FUNCS through SDL once a context exists. */
#include <stdio.h>
#include <string.h>
#include <SDL2/SDL.h>
#define RR_GL_NO_REDIRECT
#include "gl_funcs.h"
#include "d3d8_internal.h"

#define RR_GL_DEF(ret, name, args) PFN_rr_##name rr_##name;
RR_GL_FUNCS(RR_GL_DEF)

int rr_gl_load(void) {
    int missing = 0;
#define RR_GL_GET(ret, name, args) \
    rr_##name = (PFN_rr_##name)SDL_GL_GetProcAddress(#name); \
    if (!rr_##name) { fprintf(stderr, "[gl] missing %s\n", #name); missing++; }
    RR_GL_FUNCS(RR_GL_GET)
#undef RR_GL_GET
    if (missing) return -1;
    const char *v = (const char *)rr_glGetString(GL_VERSION), *r = (const char *)rr_glGetString(GL_RENDERER);
    const char *e = (const char *)rr_glGetString(GL_EXTENSIONS);
    snprintf(g_dev.gl_version, sizeof g_dev.gl_version, "%s", v ? v : "?");
    snprintf(g_dev.gl_renderer, sizeof g_dev.gl_renderer, "%s", r ? r : "?");
    g_dev.ext_bgra = e && strstr(e, "GL_EXT_texture_format_BGRA8888");
    g_dev.ext_depth24 = g_dev.gles_major >= 3 || (e && strstr(e, "GL_OES_depth24"));
    g_dev.ext_packed_ds = g_dev.gles_major >= 3 || (e && strstr(e, "GL_OES_packed_depth_stencil"));
    g_dev.ext_uint_index = g_dev.gles_major >= 3 || (e && strstr(e, "GL_OES_element_index_uint"));
    g_dev.ext_npot = g_dev.gles_major >= 3 || (e && strstr(e, "GL_OES_texture_npot"));
    g_dev.ext_s3tc = e && (strstr(e, "GL_EXT_texture_compression_s3tc") || strstr(e, "GL_EXT_texture_compression_dxt1"));
    fprintf(stderr, "[gl] %s | %s | bgra %d depth24 %d packed_ds %d uint_index %d npot %d s3tc %d\n",
            g_dev.gl_version, g_dev.gl_renderer, g_dev.ext_bgra, g_dev.ext_depth24, g_dev.ext_packed_ds,
            g_dev.ext_uint_index, g_dev.ext_npot, g_dev.ext_s3tc);
    return 0;
}
