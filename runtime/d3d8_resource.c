#include <time.h>
/*
 * d3d8_resource.c - textures, surfaces, vertex and index buffers.
 *
 * Every lockable resource keeps its bytes in guest memory (the engine reads
 * and writes them through Lock). GL copies are refreshed lazily: textures at
 * bind time (dirty bit per level), buffers at draw time (dirty byte range).
 * D3D pixel layouts are converted to GLES ones on upload.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gl_funcs.h"
#include "d3d8_slots.h"
#include "d3d8_internal.h"

RR_DEFINE_IFACE(Tex, IDirect3DTexture8, IDIRECT3DTEXTURE8)
RR_DEFINE_IFACE(Surf, IDirect3DSurface8, IDIRECT3DSURFACE8)
RR_DEFINE_IFACE(VB, IDirect3DVertexBuffer8, IDIRECT3DVERTEXBUFFER8)
RR_DEFINE_IFACE(IB, IDirect3DIndexBuffer8, IDIRECT3DINDEXBUFFER8)

/* ------------------------------------------------------------- formats */
typedef struct { uint32_t fmt; int bpp; int alpha; } fmt_info;
static const fmt_info k_formats[] = {
    { D3DFMT_A8R8G8B8, 32, 1 }, { D3DFMT_X8R8G8B8, 32, 0 }, { D3DFMT_R5G6B5, 16, 0 },
    { D3DFMT_A1R5G5B5, 16, 1 }, { D3DFMT_X1R5G5B5, 16, 0 }, { D3DFMT_A4R4G4B4, 16, 1 },
    { D3DFMT_L8, 8, 0 }, { D3DFMT_A8, 8, 1 }, { D3DFMT_A8L8, 16, 1 },
};
static const fmt_info *fmt_of(uint32_t f) {
    for (size_t i = 0; i < sizeof k_formats / sizeof *k_formats; i++) if (k_formats[i].fmt == f) return &k_formats[i];
    return NULL;
}
int rr_texture_format_supported(uint32_t usage, uint32_t rtype, uint32_t fmt) {
    if (usage & D3DUSAGE_DEPTHSTENCIL) return fmt == D3DFMT_D16 || fmt == D3DFMT_D24S8 || fmt == D3DFMT_D24X8;
    if (usage & D3DUSAGE_RENDERTARGET) return fmt == D3DFMT_X8R8G8B8 || fmt == D3DFMT_A8R8G8B8 || fmt == D3DFMT_R5G6B5;
    (void)rtype;
    return fmt_of(fmt) != NULL;
}

/* ---------------------------------------------------------- guest memory */
static uint32_t guest_alloc(uint32_t n) { void *p = hle_heap_alloc(n ? n : 4, 1); return p ? hle_va(p) : 0; }
static void guest_free(uint32_t va) { if (va) hle_heap_free(hle_ptr(va)); }

rr_texture *rr_texture_of(uint32_t obj) { if (!obj) return NULL; rr_texture *t = rr_com_host(obj); return t && t->kind == RR_RES_TEXTURE ? t : NULL; }
rr_surface *rr_surface_of(uint32_t obj) { if (!obj) return NULL; rr_surface *s = rr_com_host(obj); return s && s->kind == RR_RES_SURFACE ? s : NULL; }
rr_buffer  *rr_buffer_of(uint32_t obj)  { if (!obj) return NULL; rr_buffer *b = rr_com_host(obj); return b && (b->kind == RR_RES_VB || b->kind == RR_RES_IB) ? b : NULL; }

/* ------------------------------------------------------------- surfaces */
static rr_surface *surface_new(uint32_t w, uint32_t h, uint32_t fmt, uint32_t usage, uint32_t pool, int lockable) {
    rr_surface *s = calloc(1, sizeof *s);
    s->kind = RR_RES_SURFACE;
    s->width = w; s->height = h; s->format = fmt; s->usage = usage; s->pool = pool;
    const fmt_info *fi = fmt_of(fmt);
    s->pitch = w * (uint32_t)(fi ? fi->bpp / 8 : 4);
    s->size = s->pitch * h;
    if (lockable) s->mem = guest_alloc(s->size);
    s->obj = rr_com_new(&Surf_iface, s);
    return s;
}
static void s_GetDesc(void) {                        /* (this, D3DSURFACE_DESC*) */
    rr_surface *s = rr_surface_of(A32(0));
    rr_D3DSURFACE_DESC *d = APTR(1);
    memset(d, 0, sizeof *d);
    d->Format = s->format; d->Type = 1 /* D3DRTYPE_SURFACE */; d->Usage = s->usage; d->Pool = s->pool;
    d->Size = s->size; d->Width = s->width; d->Height = s->height;
    RET(D3D_OK, 2);
}
static void backbuffer_present_locked(rr_surface *s);
static void backbuffer_readback(rr_surface *s, int rx, int ry, int rw, int rh);
static void s_LockRect(void) {                       /* (this, D3DLOCKED_RECT*, RECT*, flags) */
    rr_surface *s = rr_surface_of(A32(0));
    /* The back buffer lives in GL. A lock gets a CPU copy filled with the
     * current frame (night vision reads the screen this way); unlocking a
     * writable lock (Bink movie frames) draws the copy back over the frame. */
    if (s->is_backbuffer && !s->mem && fmt_of(s->format) && fmt_of(s->format)->bpp == 32) s->mem = guest_alloc(s->size);
    if (s->is_backbuffer && s->mem) {
        int rx = 0, ry = 0, rw = (int)s->width, rh = (int)s->height;      /* only the rectangle asked for */
        if (A32(2)) { int32_t *r = APTR(2); rx = r[0]; ry = r[1]; rw = r[2] - r[0]; rh = r[3] - r[1]; }
        if (rx < 0) rx = 0; if (ry < 0) ry = 0;
        if (rx + rw > (int)s->width) rw = (int)s->width - rx; if (ry + rh > (int)s->height) rh = (int)s->height - ry;
        /* a writable lock of the whole buffer is overwritten (Bink movie
         * frames): skip the read-back; read-only or partial locks read it */
        int whole = rx == 0 && ry == 0 && rw == (int)s->width && rh == (int)s->height;
        if (rw > 0 && rh > 0 && ((A32(3) & 0x10u) || !whole)) backbuffer_readback(s, rx, ry, rw, rh);
        s->lock_ro = (A32(3) & 0x10u) != 0;      /* D3DLOCK_READONLY */
    }
    if (!s->mem) {
        static int once; if (!once++) fprintf(stderr, "[d3d8] LockRect on a non-lockable surface (%ux%u%s)\n",
                                               s->width, s->height, s->is_backbuffer ? ", back buffer" : "");
        RET(D3DERR_INVALIDCALL, 4);
    }
    uint32_t off = 0;
    if (A32(2)) { int32_t *r = APTR(2); const fmt_info *fi = fmt_of(s->format); off = (uint32_t)r[1] * s->pitch + (uint32_t)r[0] * (uint32_t)(fi ? fi->bpp / 8 : 4); }
    MEM32(A32(1)) = s->pitch;
    MEM32(A32(1) + 4) = s->mem + off;
    RET(D3D_OK, 4);
}
static void s_UnlockRect(void) {
    rr_surface *s = rr_surface_of(A32(0));
    if (s->is_backbuffer && s->mem) { if (!s->lock_ro) backbuffer_present_locked(s); s->lock_ro = 0; RET(D3D_OK, 1); }
    s->dirty = 1;
    if (s->parent) s->parent->dirty_levels |= 1u << s->level;
    RET(D3D_OK, 1);
}
static void s_AddRef(void) { rr_surface *s = rr_surface_of(A32(0)); RET(rr_com_addref(s->parent ? s->parent->obj : A32(0)), 1); }
static void s_Release(void) {
    rr_surface *s = rr_surface_of(A32(0));
    if (s->parent) { RET(rr_com_release(s->parent->obj), 1); }   /* levels share their texture's count */
    uint32_t n = rr_com_release(A32(0));
    if (!n && !s->is_backbuffer && !s->is_depth) { guest_free(s->mem); hle_com_free(s->obj); free(s); }
    RET(n, 1);
}
static void s_GetContainer(void) {                   /* (this, riid, void**) */
    rr_surface *s = rr_surface_of(A32(0));
    if (!s->parent) RET(E_NOINTERFACE, 3);
    rr_com_addref(s->parent->obj);
    MEM32(A32(2)) = s->parent->obj;
    RET(D3D_OK, 3);
}
static void s_GetDevice(void) { rr_com_addref(g_dev.obj); MEM32(A32(1)) = g_dev.obj; RET(D3D_OK, 2); }

/* ------------------------------------------------------------- textures */
static uint32_t level_dim(uint32_t d, uint32_t l) { d >>= l; return d ? d : 1; }

static void t_CreateTexture(void) {   /* dev (this, w, h, levels, usage, fmt, pool, IDirect3DTexture8**) */
    uint32_t w = A32(1), h = A32(2), lv = A32(3), usage = A32(4), fmt = A32(5), pool = A32(6);
    if (!fmt_of(fmt) && !(usage & D3DUSAGE_RENDERTARGET)) {
        fprintf(stderr, "[d3d8] CreateTexture: unsupported format 0x%X (%ux%u)\n", fmt, w, h);
        RET(D3DERR_INVALIDCALL, 8);
    }
    uint32_t full = 1; for (uint32_t m = w > h ? w : h; m > 1; m >>= 1) full++;
    if (!lv || lv > full) lv = full;
    rr_texture *t = calloc(1, sizeof *t);
    t->kind = RR_RES_TEXTURE;
    t->width = w; t->height = h; t->levels = lv; t->format = fmt; t->usage = usage; t->pool = pool;
    t->has_alpha = fmt_of(fmt) ? fmt_of(fmt)->alpha : 0;
    t->lv = calloc(lv, sizeof *t->lv);
    t->obj = rr_com_new(&Tex_iface, t);
    for (uint32_t i = 0; i < lv; i++) {
        t->lv[i] = surface_new(level_dim(w, i), level_dim(h, i), fmt, usage, pool, 1);
        t->lv[i]->parent = t; t->lv[i]->level = (int)i;
    }
    t->dirty_levels = (1u << lv) - 1;
    MEM32(A32(7)) = t->obj;
    RET(D3D_OK, 8);
}
static void t_GetLevelDesc(void) {                   /* (this, level, desc*) */
    rr_texture *t = rr_texture_of(A32(0));
    if (A32(1) >= t->levels) RET(D3DERR_INVALIDCALL, 3);
    rr_surface *s = t->lv[A32(1)];
    rr_D3DSURFACE_DESC *d = APTR(2);
    memset(d, 0, sizeof *d);
    d->Format = s->format; d->Type = 1; d->Usage = s->usage; d->Pool = s->pool;
    d->Size = s->size; d->Width = s->width; d->Height = s->height;
    RET(D3D_OK, 3);
}
static void t_GetLevelCount(void) { RET(rr_texture_of(A32(0))->levels, 1); }
static void t_GetSurfaceLevel(void) {                /* (this, level, surf**) */
    rr_texture *t = rr_texture_of(A32(0));
    if (A32(1) >= t->levels) RET(D3DERR_INVALIDCALL, 3);
    rr_com_addref(t->obj);
    MEM32(A32(2)) = t->lv[A32(1)]->obj;
    RET(D3D_OK, 3);
}
static void t_LockRect(void) {                       /* (this, level, D3DLOCKED_RECT*, RECT*, flags) */
    rr_texture *t = rr_texture_of(A32(0));
    uint32_t l = A32(1);
    if (l >= t->levels) RET(D3DERR_INVALIDCALL, 5);
    rr_surface *s = t->lv[l];
    uint32_t off = 0;
    if (A32(3)) { int32_t *r = APTR(3); off = (uint32_t)r[1] * s->pitch + (uint32_t)r[0] * (uint32_t)(fmt_of(s->format)->bpp / 8); }
    MEM32(A32(2)) = s->pitch;
    MEM32(A32(2) + 4) = s->mem + off;
    RET(D3D_OK, 5);
}
static void t_UnlockRect(void) {
    rr_texture *t = rr_texture_of(A32(0));
    if (A32(1) < t->levels) t->dirty_levels |= 1u << A32(1);
    RET(D3D_OK, 2);
}
static void t_AddRef(void) { RET(rr_com_addref(A32(0)), 1); }
static void t_Release(void) {
    rr_texture *t = rr_texture_of(A32(0));
    uint32_t n = rr_com_release(A32(0));
    if (!n) {
        for (int s = 0; s < RR_MAX_STAGES; s++) if (g_dev.tex[s] == t->obj) g_dev.tex[s] = 0;
        if (t->gl) { rr_texture_unbind_gl(t->gl); glDeleteTextures(1, &t->gl); }
        for (uint32_t i = 0; i < t->levels; i++) {
            rr_surface *l = t->lv[i];
            if (l->fbo) { if (g_dev.rt_obj == l->obj) { glBindFramebuffer(GL_FRAMEBUFFER, 0); g_dev.rt_off = 0; g_dev.rt_obj = g_dev.backbuffer_obj; }
                          glDeleteFramebuffers(1, &l->fbo); }
            if (l->depth_rb) glDeleteRenderbuffers(1, &l->depth_rb);
            guest_free(l->mem); hle_com_free(l->obj); free(l);
        }
        free(t->lv);
        hle_com_free(t->obj);
        free(t);
    }
    RET(n, 1);
}
static void t_GetType(void) { RET(3 /* D3DRTYPE_TEXTURE */, 1); }
static void t_SetPriority(void) { RET(0, 2); }
static void t_GetPriority(void) { RET(0, 1); }
static void t_PreLoad(void) { RET(0, 1); }
static void t_SetLOD(void) { RET(0, 2); }
static void t_GetLOD(void) { RET(0, 1); }
static void t_AddDirtyRect(void) { rr_texture_of(A32(0))->dirty_levels |= 1; RET(D3D_OK, 2); }

/* D3D pixels -> GLES upload (scratch buffer reused) */
static uint8_t *g_conv; static size_t g_conv_cap;
static void *conv_buf(size_t n) { if (n > g_conv_cap) { free(g_conv); g_conv = malloc(n); g_conv_cap = n; } return g_conv; }

uint64_t rr_t_upload_ns, rr_t_upload_bytes, rr_t_read_ns;
static uint64_t now_ns(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec; }
static void upload_level_impl(rr_texture *t, uint32_t l);
static void upload_level(rr_texture *t, uint32_t l) {
    uint64_t t0 = now_ns();
    upload_level_impl(t, l);
    rr_t_upload_ns += now_ns() - t0; rr_t_upload_bytes += t->lv[l]->size;
}
static void upload_level_impl(rr_texture *t, uint32_t l) {
    rr_surface *s = t->lv[l];
    const uint8_t *src = hle_ptr(s->mem);
    uint32_t w = s->width, h = s->height, n = w * h;
    GLenum ifmt, fmt, type; const void *px = src;
    switch (t->format) {
    case D3DFMT_A8R8G8B8: case D3DFMT_X8R8G8B8: {
        int x = t->format == D3DFMT_X8R8G8B8;
        if (g_dev.ext_bgra && !x && !t->rgba) { ifmt = fmt = GL_BGRA_EXT; type = GL_UNSIGNED_BYTE; break; }
        uint8_t *d = conv_buf((size_t)n * 4);
        for (uint32_t i = 0; i < n; i++) { d[i*4] = src[i*4+2]; d[i*4+1] = src[i*4+1]; d[i*4+2] = src[i*4]; d[i*4+3] = x ? 255 : src[i*4+3]; }
        ifmt = fmt = GL_RGBA; type = GL_UNSIGNED_BYTE; px = d; break;
    }
    case D3DFMT_R5G6B5: ifmt = fmt = GL_RGB; type = GL_UNSIGNED_SHORT_5_6_5; break;
    case D3DFMT_A1R5G5B5: case D3DFMT_X1R5G5B5: {
        const uint16_t *p = (const uint16_t *)src; uint16_t *d = conv_buf((size_t)n * 2);
        uint16_t a = t->format == D3DFMT_X1R5G5B5;
        for (uint32_t i = 0; i < n; i++) d[i] = (uint16_t)((p[i] << 1) | (a ? 1 : (p[i] >> 15)));
        ifmt = fmt = GL_RGBA; type = GL_UNSIGNED_SHORT_5_5_5_1; px = d; break;
    }
    case D3DFMT_A4R4G4B4: {
        const uint16_t *p = (const uint16_t *)src; uint16_t *d = conv_buf((size_t)n * 2);
        for (uint32_t i = 0; i < n; i++) d[i] = (uint16_t)((p[i] << 4) | (p[i] >> 12));
        ifmt = fmt = GL_RGBA; type = GL_UNSIGNED_SHORT_4_4_4_4; px = d; break;
    }
    case D3DFMT_L8: ifmt = fmt = GL_LUMINANCE; type = GL_UNSIGNED_BYTE; break;
    case D3DFMT_A8: ifmt = fmt = GL_ALPHA; type = GL_UNSIGNED_BYTE; break;
    case D3DFMT_A8L8: ifmt = fmt = GL_LUMINANCE_ALPHA; type = GL_UNSIGNED_BYTE; break;
    default: return;
    }
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, (GLint)l, (GLint)ifmt, (GLsizei)w, (GLsizei)h, 0, fmt, type, px);
    /* A D3D texture may stop its mip chain early (RF2: 256x256 with 4 levels).
     * GLES treats a mipmapped texture without levels down to 1x1 as incomplete
     * and samples it as black, unless MAX_LEVEL ends the chain (GLES 3.0). */
    if (l == 0) glTexParameteri(GL_TEXTURE_2D, 0x813D /* GL_TEXTURE_MAX_LEVEL */, (GLint)t->levels - 1);
}

/* GL binding cache: which texture each unit holds and the active unit, so a
 * draw that keeps its textures issues no GL calls for them. */
static uint32_t g_bound[8];
static int g_active = -1;
static void active_unit(int unit) { if (g_active != unit) { glActiveTexture(GL_TEXTURE0 + unit); g_active = unit; } }

#ifndef GL_READ_FRAMEBUFFER
#define GL_READ_FRAMEBUFFER 0x8CA8
#define GL_DRAW_FRAMEBUFFER_BINDING 0x8CA6
#endif
#ifndef GL_DRAW_FRAMEBUFFER
#define GL_DRAW_FRAMEBUFFER 0x8CA9
#endif
/* Unlock of a locked back buffer: upload the CPU copy (X8R8G8B8 / A8R8G8B8)
 * and blit it, flipped to GL's bottom-up rows, onto the back buffer's place in
 * the drawable. glBlitFramebuffer ignores the shader/blend/depth state, so the
 * only state touched is the scissor (restored) and this unit's binding. */
/* The current frame into the back buffer's CPU copy: X8R8G8B8 rows, top
 * first, at back-buffer size (scaled down with a blit when the drawable is
 * larger). */
static void backbuffer_readback(rr_surface *s, int rx, int ry, int rw, int rh) {
    static GLuint fbo, rb; static uint32_t rbw, rbh;
    uint32_t w = s->width, h = s->height;
    GLint draw_fb = 0, read_fb = 0;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw_fb); glGetIntegerv(0x8CAA /* GL_READ_FRAMEBUFFER_BINDING */, &read_fb);
    GLboolean sc = glIsEnabled(GL_SCISSOR_TEST);
    if (sc) glDisable(GL_SCISSOR_TEST);
    int x = g_dev.out_x, y = g_dev.out_y;
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    if ((uint32_t)g_dev.out_w != w || (uint32_t)g_dev.out_h != h) {
        if (!fbo || rbw != w || rbh != h) {
            if (!fbo) { glGenFramebuffers(1, &fbo); glGenRenderbuffers(1, &rb); }
            glBindRenderbuffer(GL_RENDERBUFFER, rb);
            glRenderbufferStorage(GL_RENDERBUFFER, 0x8058 /* GL_RGBA8 */, (GLsizei)w, (GLsizei)h);
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo);
            glFramebufferRenderbuffer(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rb);
            rbw = w; rbh = h;
        }
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo);
        glBlitFramebuffer(g_dev.out_x, g_dev.out_y, g_dev.out_x + g_dev.out_w, g_dev.out_y + g_dev.out_h, 0, 0, (GLint)w, (GLint)h, GL_COLOR_BUFFER_BIT, GL_LINEAR);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
        x = y = 0;
    }
    /* the rectangle (top-left origin) in GL's bottom-up rows */
    uint8_t *px = conv_buf((size_t)rw * (size_t)rh * 4);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    uint64_t t0 = now_ns();
    glReadPixels(x + rx, y + (int)h - ry - rh, (GLsizei)rw, (GLsizei)rh, GL_RGBA, GL_UNSIGNED_BYTE, px);
    rr_t_read_ns += now_ns() - t0;
    uint8_t *dst = hle_ptr(s->mem);
    for (int r = 0; r < rh; r++) {
        const uint8_t *in = px + (size_t)(rh - 1 - r) * (size_t)rw * 4;
        uint8_t *out = dst + (size_t)(ry + r) * s->pitch + (size_t)rx * 4;
        for (int i = 0; i < rw; i++) { out[4*i] = in[4*i+2]; out[4*i+1] = in[4*i+1]; out[4*i+2] = in[4*i]; out[4*i+3] = 255; }
    }
    glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)read_fb);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)draw_fb);
    if (sc) glEnable(GL_SCISSOR_TEST);
}
static void backbuffer_present_locked(rr_surface *s) {
    static GLuint tex, fbo;
    const uint8_t *src = hle_ptr(s->mem);
    uint32_t w = s->width, h = s->height, n = w * h;
    uint8_t *d = conv_buf((size_t)n * 4);
    for (uint32_t i = 0; i < n; i++) { d[i*4] = src[i*4+2]; d[i*4+1] = src[i*4+1]; d[i*4+2] = src[i*4]; d[i*4+3] = 255; }
    if (!tex) { glGenTextures(1, &tex); glGenFramebuffers(1, &fbo); }
    int unit = g_active < 0 ? 0 : g_active;
    active_unit(unit);
    glBindTexture(GL_TEXTURE_2D, tex);
    g_bound[unit] = 0xFFFFFFFFu;                     /* the cache no longer knows this unit */
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, (GLsizei)w, (GLsizei)h, 0, GL_RGBA, GL_UNSIGNED_BYTE, d);
    GLint draw_fb = 0; glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw_fb);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    GLboolean sc = glIsEnabled(GL_SCISSOR_TEST);
    if (sc) glDisable(GL_SCISSOR_TEST);
    glBlitFramebuffer(0, 0, (GLint)w, (GLint)h, g_dev.out_x, g_dev.out_y + g_dev.out_h, g_dev.out_x + g_dev.out_w, g_dev.out_y,
                      GL_COLOR_BUFFER_BIT, GL_LINEAR);
    if (sc) glEnable(GL_SCISSOR_TEST);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)draw_fb);
}
void rr_texture_unbind_gl(uint32_t gl) { for (int i = 0; i < 8; i++) if (g_bound[i] == gl) g_bound[i] = 0; }
void rr_texture_bind(rr_texture *t, int unit) {
    uint32_t want = t ? t->gl : 0;
    if (t && !t->gl) {
        active_unit(unit);
        glGenTextures(1, &t->gl);
        glBindTexture(GL_TEXTURE_2D, t->gl);
        g_bound[unit] = t->gl;
        t->gl_wrap_u = t->gl_wrap_v = t->gl_min = t->gl_mag = 0;
    } else if (g_bound[unit] != want || (t && t->dirty_levels)) {
        active_unit(unit);
        g_draw_stats.binds++;
        glBindTexture(GL_TEXTURE_2D, want);
        g_bound[unit] = want;
    } else {
        active_unit(unit);              /* sampler parameters that follow apply to this unit */
        return;
    }
    if (!t) return;
    if (t->dirty_levels) {
        if (!(t->usage & D3DUSAGE_RENDERTARGET) || !t->lv[0]->fbo)
            for (uint32_t l = 0; l < t->levels; l++) if (t->dirty_levels & (1u << l)) upload_level(t, l);
        t->dirty_levels = 0;
    }
}

/* -------------------------------------------------------------- buffers */
static void b_new(int kind, rr_iface *it) {   /* dev (this, length, usage, fvf/format, pool, out**) */
    rr_buffer *b = calloc(1, sizeof *b);
    b->kind = kind; b->size = A32(1); b->usage = A32(2); b->fvf_or_format = A32(3); b->pool = A32(4);
    b->mem = guest_alloc(b->size);
    b->dirty_lo = 0; b->dirty_hi = b->size;
    b->obj = rr_com_new(it, b);
    MEM32(A32(5)) = b->obj;
    RET(b->mem ? D3D_OK : E_OUTOFMEMORY, 6);
}
static void d_CreateVertexBuffer(void) { b_new(RR_RES_VB, &VB_iface); }
static void d_CreateIndexBuffer(void)  { b_new(RR_RES_IB, &IB_iface); }
static void b_Lock(void) {                           /* (this, offset, size, BYTE**, flags) */
    rr_buffer *b = rr_buffer_of(A32(0));
    uint32_t off = A32(1), n = A32(2) ? A32(2) : b->size - A32(1);
    if (off + n > b->size) n = b->size - off;
    b->lock_off = off; b->lock_size = n;
    if (A32(4) & 0x2000u /* D3DLOCK_DISCARD */) b->discard = 1;
    MEM32(A32(3)) = b->mem + off;
    RET(D3D_OK, 5);
}
static void b_Unlock(void) {
    rr_buffer *b = rr_buffer_of(A32(0));
    uint32_t lo = b->lock_off, hi = b->lock_off + b->lock_size;
    if (b->dirty_hi <= b->dirty_lo) { b->dirty_lo = lo; b->dirty_hi = hi; }
    else { if (lo < b->dirty_lo) b->dirty_lo = lo; if (hi > b->dirty_hi) b->dirty_hi = hi; }
    RET(D3D_OK, 1);
}
static void vb_GetDesc(void) {                       /* (this, D3DVERTEXBUFFER_DESC*) */
    rr_buffer *b = rr_buffer_of(A32(0));
    rr_D3DVERTEXBUFFER_DESC *d = APTR(1);
    d->Format = 100 /* D3DFMT_VERTEXDATA */; d->Type = 4; d->Usage = b->usage; d->Pool = b->pool; d->Size = b->size; d->FVF = b->fvf_or_format;
    RET(D3D_OK, 2);
}
static void ib_GetDesc(void) {
    rr_buffer *b = rr_buffer_of(A32(0));
    rr_D3DINDEXBUFFER_DESC *d = APTR(1);
    d->Format = b->fvf_or_format; d->Type = 5; d->Usage = b->usage; d->Pool = b->pool; d->Size = b->size;
    RET(D3D_OK, 2);
}
static void b_Release(void) {
    rr_buffer *b = rr_buffer_of(A32(0));
    uint32_t n = rr_com_release(A32(0));
    if (!n) {
        if (g_dev.stream_vb == b->obj) g_dev.stream_vb = 0;
        if (g_dev.ib == b->obj) g_dev.ib = 0;
        if (b->gl) glDeleteBuffers(1, &b->gl);
        guest_free(b->mem); hle_com_free(b->obj); free(b);
    }
    RET(n, 1);
}
static void b_GetType(void) { RET(rr_buffer_of(A32(0))->kind == RR_RES_VB ? 4 : 5, 1); }

/* Static buffers live in GL buffers; dynamic ones are drawn from client
 * memory by the caller (returns 0) so a frequently refilled buffer never
 * makes the driver wait on a draw still reading it. */
/* DYNAMIC buffers: by default drawn from guest memory (client arrays).
 * RERED_DYNVBO=1 gives them GL buffers too: each lock's dirty range is
 * uploaded once at the next draw, a DISCARD lock orphans the storage first so
 * the driver never waits for the GPU. Measured on the RG40XX H it is slower
 * than client arrays (main menu 42 vs 60 FPS, 9.9 vs 4.5 ms CPU): Mali's
 * client-array path is cheaper than buffer updates here. Off by default. */
static int dynvbo_on(void) { static int on = -1; if (on < 0) { const char *e = getenv("RERED_DYNVBO"); on = e && *e == '1'; } return on; }
uint32_t rr_buffer_gl(rr_buffer *b, uint32_t target) {
    if (b->usage & D3DUSAGE_DYNAMIC) {
        if (!dynvbo_on()) return 0;
        if (!b->gl) { glGenBuffers(1, &b->gl); glBindBuffer(target, b->gl); glBufferData(target, b->size, hle_ptr(b->mem), GL_STREAM_DRAW); b->dirty_hi = b->dirty_lo = 0; b->discard = 0; return b->gl; }
        glBindBuffer(target, b->gl);
        if (b->dirty_hi > b->dirty_lo) {
            if (b->discard) glBufferData(target, b->size, NULL, GL_STREAM_DRAW);   /* orphan */
            glBufferSubData(target, b->dirty_lo, b->dirty_hi - b->dirty_lo, (uint8_t *)hle_ptr(b->mem) + b->dirty_lo);
            b->dirty_hi = b->dirty_lo = 0; b->discard = 0;
        }
        return b->gl;
    }
    if (!b->gl) { glGenBuffers(1, &b->gl); glBindBuffer(target, b->gl); glBufferData(target, b->size, hle_ptr(b->mem), GL_STATIC_DRAW); b->dirty_hi = b->dirty_lo = 0; return b->gl; }
    glBindBuffer(target, b->gl);
    if (b->dirty_hi > b->dirty_lo) {
        glBufferSubData(target, b->dirty_lo, b->dirty_hi - b->dirty_lo, (uint8_t *)hle_ptr(b->mem) + b->dirty_lo);
        b->dirty_hi = b->dirty_lo = 0;
    }
    return b->gl;
}

/* ----------------------------------------------- device-level methods */
static void d_SetTexture(void) {                     /* (this, stage, IDirect3DBaseTexture8*) */
    uint32_t s = A32(1);
    if (rr_sb_rec_tex(s, A32(2))) RET(D3D_OK, 3);
    if (s < RR_MAX_STAGES) g_dev.tex[s] = A32(2);
    RET(D3D_OK, 3);
}
static void d_GetTexture(void) {
    uint32_t s = A32(1), t = s < RR_MAX_STAGES ? g_dev.tex[s] : 0;
    if (t) rr_com_addref(t);
    MEM32(A32(2)) = t;
    RET(D3D_OK, 3);
}
static void d_SetStreamSource(void) {                /* (this, stream, vb, stride) */
    if (A32(1) == 0 && rr_sb_rec_stream(A32(2), A32(3))) RET(D3D_OK, 4);
    if (A32(1) == 0) { g_dev.stream_vb = A32(2); g_dev.stream_stride = A32(3); }
    RET(D3D_OK, 4);
}
static void d_SetIndices(void) { if (rr_sb_rec_ib(A32(1), A32(2))) RET(D3D_OK, 3); g_dev.ib = A32(1); g_dev.base_vertex = A32(2); RET(D3D_OK, 3); }
static void d_GetBackBuffer(void) {                  /* (this, index, type, surf**) */
    rr_surface *s = rr_surface_of(g_dev.backbuffer_obj);
    rr_com_addref(s->obj);
    MEM32(A32(3)) = s->obj;
    RET(D3D_OK, 4);
}
static void d_GetDepthStencilSurface(void) {
    if (!g_dev.depth_obj) RET(D3DERR_NOTFOUND, 2);
    rr_com_addref(g_dev.depth_obj); MEM32(A32(1)) = g_dev.depth_obj; RET(D3D_OK, 2);
}
/* ---- render to texture ----
 * A texture level becomes a GL framebuffer the first time it is a target
 * (colour: the texture; depth/stencil: a renderbuffer of its size when the
 * game passes a depth surface). While one is bound, rr_bb_to_gl maps without
 * the letterbox and the shaders flip Y (u_yflip), so the texture's first row
 * is the picture's top, as D3D samples it; winding flips with it (culling). */
#ifndef GL_DEPTH_STENCIL_ATTACHMENT
#define GL_DEPTH_STENCIL_ATTACHMENT 0x821A
#endif
#ifndef GL_DEPTH24_STENCIL8
#define GL_DEPTH24_STENCIL8 0x88F0
#endif
static int surface_fbo(rr_surface *s, int want_depth) {
    rr_texture *t = s->parent;
    if (!t) return 0;
    if (!s->fbo) {
        int unit = g_active < 0 ? 0 : g_active;
        if (!t->gl) { glGenTextures(1, &t->gl); }
        active_unit(unit);
        glBindTexture(GL_TEXTURE_2D, t->gl);
        g_bound[unit] = t->gl;
        glTexImage2D(GL_TEXTURE_2D, s->level, GL_RGBA, (GLsizei)s->width, (GLsizei)s->height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        glTexParameteri(GL_TEXTURE_2D, 0x813D /* MAX_LEVEL */, (GLint)t->levels - 1);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        t->gl_min = t->gl_mag = 0;                  /* sampler state re-applied on next use */
        t->dirty_levels = 0;
        glGenFramebuffers(1, &s->fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, s->fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t->gl, s->level);
    } else glBindFramebuffer(GL_FRAMEBUFFER, s->fbo);
    if (want_depth && !s->depth_rb) {
        glGenRenderbuffers(1, &s->depth_rb);
        glBindRenderbuffer(GL_RENDERBUFFER, s->depth_rb);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, (GLsizei)s->width, (GLsizei)s->height);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, s->depth_rb);
    }
    GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (st != GL_FRAMEBUFFER_COMPLETE) {
        static int once; if (!once++) fprintf(stderr, "[d3d8] render target %ux%u fmt %u: framebuffer incomplete (0x%X)\n", s->width, s->height, s->format, st);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        return 0;
    }
    return 1;
}
static void d_SetRenderTarget(void) {                /* (this, surface or NULL, depth surface or NULL) */
    uint32_t so = A32(1), zo = A32(2);
    rr_surface *s = so ? rr_surface_of(so) : rr_surface_of(g_dev.rt_obj);
    if (!s) RET(D3DERR_INVALIDCALL, 3);
    if (s->is_backbuffer) {
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        g_dev.rt_off = 0;
    } else {
        static int once; if (!once++) fprintf(stderr, "[d3d8] SetRenderTarget: rendering to a %ux%u texture\n", s->width, s->height);
        if (!surface_fbo(s, zo != 0)) RET(D3DERR_INVALIDCALL, 3);
        g_dev.rt_off = 1; g_dev.rt_w = (int)s->width; g_dev.rt_h = (int)s->height;
        if (s->parent) rr_texture_unbind_gl(s->parent->gl);   /* not sampled while it is the target */
    }
    g_dev.rt_obj = s->obj;
    g_dev.zs_obj = zo;
    /* D3D: setting a render target resets the viewport to the whole target */
    g_dev.vp.X = g_dev.vp.Y = 0; g_dev.vp.Width = s->width; g_dev.vp.Height = s->height; g_dev.vp.MinZ = 0; g_dev.vp.MaxZ = 1;
    g_dev.state_dirty |= RR_DIRTY_VP | RR_DIRTY_RS;
    RET(D3D_OK, 3);
}
static void d_GetRenderTarget(void) { rr_com_addref(g_dev.rt_obj); MEM32(A32(1)) = g_dev.rt_obj; RET(D3D_OK, 2); }
static void d_CreateImageSurface(void) {             /* (this, w, h, fmt, surf**) */
    rr_surface *s = surface_new(A32(1), A32(2), A32(3), 0, 2 /* SYSTEMMEM */, 1);
    MEM32(A32(4)) = s->obj;
    RET(D3D_OK, 5);
}
/* CopyRects from the back buffer into a 32-bit texture level: entirely on
 * the GPU (glBlitFramebuffer into the texture), no read-back and no upload.
 * Night vision copies the screen this way every frame; the CPU path cost
 * ~50 ms of GPU syncs per frame. The level's CPU copy is then stale, so it is
 * not re-uploaded from it. */
static int copy_screen_to_texture(rr_surface *s, rr_surface *d, uint32_t n, uint32_t rects, uint32_t points) {
    rr_texture *t = d->parent;
    if (!t || !fmt_of(d->format) || fmt_of(d->format)->bpp != 32 || d->level != 0) return 0;
    int unit = g_active < 0 ? 0 : g_active;
    if (!t->rgba) { t->rgba = 1; t->dirty_levels |= 1u; if (t->gl) { rr_texture_unbind_gl(t->gl); glDeleteTextures(1, &t->gl); t->gl = 0; } }
    rr_texture_bind(t, unit);                                  /* storage (RGBA8) with the current CPU contents */
    static GLuint fbo; if (!fbo) glGenFramebuffers(1, &fbo);
    GLint draw_fb = 0, read_fb = 0;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw_fb); glGetIntegerv(0x8CAA /* GL_READ_FRAMEBUFFER_BINDING */, &read_fb);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t->gl, 0);
    int ok = glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    if (ok) {
        glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
        GLboolean sc = glIsEnabled(GL_SCISSOR_TEST);
        if (sc) glDisable(GL_SCISSOR_TEST);
        int saved_off = g_dev.rt_off; g_dev.rt_off = 0;          /* source rects are back-buffer pixels */
        for (uint32_t i = 0; i < (n ? n : 1); i++) {
            int32_t r[4] = { 0, 0, (int32_t)s->width, (int32_t)s->height }, p[2] = { 0, 0 };
            if (rects) memcpy(r, hle_ptr(rects + i * 16), 16);
            if (points) memcpy(p, hle_ptr(points + i * 8), 8);
            int gx, gy, gw, gh;
            rr_bb_to_gl(r[0], r[1], r[2] - r[0], r[3] - r[1], &gx, &gy, &gw, &gh);
            /* GL rows are bottom-up; the texture's row 0 is the picture's top */
            glBlitFramebuffer(gx, gy, gx + gw, gy + gh, p[0], p[1] + (r[3] - r[1]), p[0] + (r[2] - r[0]), p[1],
                              GL_COLOR_BUFFER_BIT, GL_LINEAR);
        }
        g_dev.rt_off = saved_off;
        if (sc) glEnable(GL_SCISSOR_TEST);
        t->dirty_levels &= ~1u;                                 /* the GPU copy is newer than the CPU one */
    }
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)read_fb);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)draw_fb);
    { static int once; if (!once++) fprintf(stderr, "[d3d8] CopyRects screen -> %ux%u texture on the GPU%s\n", d->width, d->height, ok ? "" : " FAILED (incomplete framebuffer)"); }
    return ok;
}
static void d_CopyRects(void) {                      /* (this, src, srcrects, n, dst, dstpoints) */
    rr_surface *s = rr_surface_of(A32(1)), *d = rr_surface_of(A32(4));
    if (s && d && s->is_backbuffer && copy_screen_to_texture(s, d, A32(3), A32(2), A32(5))) RET(D3D_OK, 6);
    if (s && s->is_backbuffer && fmt_of(s->format) && fmt_of(s->format)->bpp == 32) {   /* copy from the screen */
        if (!s->mem) s->mem = guest_alloc(s->size);
        backbuffer_readback(s, 0, 0, (int)s->width, (int)s->height);
    }
    if (!s || !d || !s->mem || !d->mem || s->format != d->format) {
        static int once; if (!once++) fprintf(stderr, "[d3d8] CopyRects between unsupported surfaces\n");
        RET(D3DERR_INVALIDCALL, 6);
    }
    uint32_t bpp = (uint32_t)fmt_of(s->format)->bpp / 8, n = A32(3) ? A32(3) : 1;
    for (uint32_t i = 0; i < n; i++) {
        int32_t r[4] = { 0, 0, (int32_t)s->width, (int32_t)s->height }, p[2] = { 0, 0 };
        if (A32(2)) memcpy(r, hle_ptr(A32(2) + i * 16), 16);
        if (A32(5)) memcpy(p, hle_ptr(A32(5) + i * 8), 8);
        for (int32_t y = r[1]; y < r[3]; y++)
            memcpy((uint8_t *)hle_ptr(d->mem) + (uint32_t)(p[1] + y - r[1]) * d->pitch + (uint32_t)p[0] * bpp,
                   (uint8_t *)hle_ptr(s->mem) + (uint32_t)y * s->pitch + (uint32_t)r[0] * bpp, (size_t)(r[2] - r[0]) * bpp);
    }
    if (d->parent) d->parent->dirty_levels |= 1u << d->level;
    RET(D3D_OK, 6);
}
static void d_UpdateTexture(void) {                  /* (this, srcTex, dstTex) */
    rr_texture *s = rr_texture_of(A32(1)), *d = rr_texture_of(A32(2));
    if (!s || !d) RET(D3DERR_INVALIDCALL, 3);
    for (uint32_t l = 0; l < s->levels && l < d->levels; l++)
        if (s->lv[l]->size == d->lv[l]->size) memcpy(hle_ptr(d->lv[l]->mem), hle_ptr(s->lv[l]->mem), s->lv[l]->size);
    d->dirty_levels = (1u << d->levels) - 1;
    RET(D3D_OK, 3);
}

void rr_resources_init(void) {
    static const rr_method tex[] = {
        { IDIRECT3DTEXTURE8_AddRef, t_AddRef }, { IDIRECT3DTEXTURE8_Release, t_Release },
        { IDIRECT3DTEXTURE8_GetDevice, s_GetDevice }, { IDIRECT3DTEXTURE8_SetPriority, t_SetPriority },
        { IDIRECT3DTEXTURE8_GetPriority, t_GetPriority }, { IDIRECT3DTEXTURE8_PreLoad, t_PreLoad },
        { IDIRECT3DTEXTURE8_GetType, t_GetType }, { IDIRECT3DTEXTURE8_SetLOD, t_SetLOD },
        { IDIRECT3DTEXTURE8_GetLOD, t_GetLOD }, { IDIRECT3DTEXTURE8_GetLevelCount, t_GetLevelCount },
        { IDIRECT3DTEXTURE8_GetLevelDesc, t_GetLevelDesc }, { IDIRECT3DTEXTURE8_GetSurfaceLevel, t_GetSurfaceLevel },
        { IDIRECT3DTEXTURE8_LockRect, t_LockRect }, { IDIRECT3DTEXTURE8_UnlockRect, t_UnlockRect },
        { IDIRECT3DTEXTURE8_AddDirtyRect, t_AddDirtyRect }, { 0, 0 } };
    static const rr_method surf[] = {
        { IDIRECT3DSURFACE8_AddRef, s_AddRef }, { IDIRECT3DSURFACE8_Release, s_Release },
        { IDIRECT3DSURFACE8_GetDevice, s_GetDevice }, { IDIRECT3DSURFACE8_GetContainer, s_GetContainer },
        { IDIRECT3DSURFACE8_GetDesc, s_GetDesc }, { IDIRECT3DSURFACE8_LockRect, s_LockRect },
        { IDIRECT3DSURFACE8_UnlockRect, s_UnlockRect }, { 0, 0 } };
    static const rr_method vb[] = {
        { IDIRECT3DVERTEXBUFFER8_AddRef, t_AddRef }, { IDIRECT3DVERTEXBUFFER8_Release, b_Release },
        { IDIRECT3DVERTEXBUFFER8_GetDevice, s_GetDevice }, { IDIRECT3DVERTEXBUFFER8_GetType, b_GetType },
        { IDIRECT3DVERTEXBUFFER8_Lock, b_Lock }, { IDIRECT3DVERTEXBUFFER8_Unlock, b_Unlock },
        { IDIRECT3DVERTEXBUFFER8_GetDesc, vb_GetDesc }, { 0, 0 } };
    static const rr_method ib[] = {
        { IDIRECT3DINDEXBUFFER8_AddRef, t_AddRef }, { IDIRECT3DINDEXBUFFER8_Release, b_Release },
        { IDIRECT3DINDEXBUFFER8_GetDevice, s_GetDevice }, { IDIRECT3DINDEXBUFFER8_GetType, b_GetType },
        { IDIRECT3DINDEXBUFFER8_Lock, b_Lock }, { IDIRECT3DINDEXBUFFER8_Unlock, b_Unlock },
        { IDIRECT3DINDEXBUFFER8_GetDesc, ib_GetDesc }, { 0, 0 } };
    rr_iface_vtable(&Tex_iface, tex);
    rr_iface_vtable(&Surf_iface, surf);
    rr_iface_vtable(&VB_iface, vb);
    rr_iface_vtable(&IB_iface, ib);
    if (!g_dev.backbuffer_obj) {
        rr_surface *bb = surface_new(g_dev.pp.BackBufferWidth, g_dev.pp.BackBufferHeight, g_dev.pp.BackBufferFormat, D3DUSAGE_RENDERTARGET, 0, 0);
        bb->is_backbuffer = 1;
        g_dev.backbuffer_obj = g_dev.rt_obj = bb->obj;
        if (g_dev.pp.EnableAutoDepthStencil) {
            rr_surface *z = surface_new(g_dev.pp.BackBufferWidth, g_dev.pp.BackBufferHeight, g_dev.pp.AutoDepthStencilFormat, D3DUSAGE_DEPTHSTENCIL, 0, 0);
            z->is_depth = 1;
            g_dev.depth_obj = g_dev.zs_obj = z->obj;
        }
    }
}

const rr_method rr_dev_resource_methods[] = {
    RR_DEV(CreateTexture, t_CreateTexture) RR_DEV(CreateVertexBuffer, d_CreateVertexBuffer)
    RR_DEV(CreateIndexBuffer, d_CreateIndexBuffer) RR_DEV(SetTexture, d_SetTexture) RR_DEV(GetTexture, d_GetTexture)
    RR_DEV(SetStreamSource, d_SetStreamSource) RR_DEV(SetIndices, d_SetIndices) RR_DEV(GetBackBuffer, d_GetBackBuffer)
    RR_DEV(GetDepthStencilSurface, d_GetDepthStencilSurface) RR_DEV(GetRenderTarget, d_GetRenderTarget) RR_DEV(SetRenderTarget, d_SetRenderTarget)
    RR_DEV(CreateImageSurface, d_CreateImageSurface) RR_DEV(CopyRects, d_CopyRects) RR_DEV(UpdateTexture, d_UpdateTexture)
    { 0, 0 }
};

/* For BinkDX8SurfaceType (miles.c): a D3D8 surface's format, 0 if not one. */
uint32_t rr_d3d8_surface_format(uint32_t obj) { rr_surface *s = rr_surface_of(obj); return s ? s->format : 0; }
