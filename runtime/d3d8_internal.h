/* d3d8_internal.h - state shared by d3d8.c, d3d8_resource.c and d3d8_draw.c. */
#pragma once
#include <stdint.h>
#include <SDL2/SDL.h>
#include "d3d8_enums.h"
#include "d3d8_types.h"
#include "com.h"
#include <math.h>

#define RR_MAX_STAGES   4          /* the game blends at most a few stages; GLES has >= 8 units */
#define RR_MAX_STREAMS  1
#define RR_MAX_LIGHTS   8
#define RR_NUM_TRANSFORMS 512      /* D3DTRANSFORMSTATETYPE indices; WORLD is 256 */
#define RR_TSS_COUNT    32
#define RR_RS_COUNT     256

/* resource kinds */
enum { RR_RES_TEXTURE = 1, RR_RES_SURFACE, RR_RES_VB, RR_RES_IB, RR_RES_CUBE };

typedef struct rr_texture rr_texture;
typedef struct rr_surface {
    int kind;                    /* RR_RES_SURFACE */
    uint32_t obj;                /* guest COM object */
    uint32_t width, height, format, usage, pool;
    uint32_t pitch, size;
    uint32_t mem;                /* guest VA of the lockable copy (0 for depth/back buffers) */
    rr_texture *parent;         /* level of this texture, or NULL */
    int level;
    int is_backbuffer, is_depth;
    int dirty;
    uint32_t fbo, depth_rb;      /* render-to-texture: GL framebuffer for this level */
    int lock_ro;                 /* the current lock is D3DLOCK_READONLY */
} rr_surface;

struct rr_texture {
    int kind;                    /* RR_RES_TEXTURE */
    uint32_t obj;
    uint32_t width, height, levels, format, usage, pool;
    rr_surface **lv;            /* one surface per level */
    uint32_t gl;                 /* GL texture name */
    uint32_t dirty_levels;       /* bit per level needing upload */
    uint32_t gl_wrap_u, gl_wrap_v, gl_min, gl_mag;   /* cached sampler state */
    int has_alpha;
    int rgba;                    /* stored as GL_RGBA8 (a blit target), never BGRA */
};

typedef struct {
    int kind;                    /* RR_RES_VB / RR_RES_IB */
    uint32_t obj;
    uint32_t size, usage, fvf_or_format, pool;
    uint32_t mem;                /* guest VA of the data */
    uint32_t gl;                 /* GL buffer name, 0 when drawn from client memory */
    uint32_t dirty_lo, dirty_hi; /* byte range written since the last upload */
    uint32_t lock_off, lock_size;
    int discard;                 /* a D3DLOCK_DISCARD lock since the last upload */
} rr_buffer;

typedef struct {
    uint32_t obj;                    /* the guest IDirect3DDevice8 */
    SDL_Window *window;
    SDL_GLContext gl;
    int gles_major;
    char gl_version[128], gl_renderer[128];
    int ext_bgra, ext_depth24, ext_packed_ds, ext_uint_index, ext_npot, ext_s3tc;
    int drawable_w, drawable_h, bb_w, bb_h;
    int out_x, out_y, out_w, out_h;  /* where the back buffer lands in the drawable (4:3, centred) */
    rr_D3DPRESENT_PARAMETERS pp;
    uint32_t hwnd, behavior;

    uint32_t rs[RR_RS_COUNT];
    uint32_t tss[RR_MAX_STAGES][RR_TSS_COUNT];
    float    xf[RR_NUM_TRANSFORMS][16];
    rr_D3DVIEWPORT8 vp;
    rr_D3DMATERIAL8 material;
    rr_D3DLIGHT8 light[RR_MAX_LIGHTS];
    int light_enabled[RR_MAX_LIGHTS];
    uint32_t state_dirty;            /* RR_DIRTY_* */

    uint32_t tex[RR_MAX_STAGES];    /* bound guest texture objects */
    uint32_t stream_vb, stream_stride;
    uint32_t ib, base_vertex;
    uint32_t fvf;                    /* current vertex shader handle (FVF code) */
    uint32_t backbuffer_obj, depth_obj;
    uint32_t rt_obj, zs_obj;         /* current render target / depth surface */
    int rt_off, rt_w, rt_h;          /* drawing into a texture (FBO): no letterbox, Y flipped */
    int in_scene;
    uint64_t frame;
} rr_device;

enum { RR_DIRTY_XF = 1, RR_DIRTY_RS = 2, RR_DIRTY_TSS = 4, RR_DIRTY_VP = 8, RR_DIRTY_LIGHT = 16, RR_DIRTY_MAT = 32 };

extern rr_device g_dev;
typedef struct { uint64_t draws, prims, uploads, client_vb, client_ib, progs, binds, unif; } rr_draw_stats;
extern rr_draw_stats g_draw_stats;

/* d3d8.c */
void rr_update_output_rect(void);   /* after a size change */
/* back-buffer pixel rect (top-left origin) -> GL window rect (bottom-left origin) */
static inline void rr_bb_to_gl(int x, int y, int w, int h, int *gx, int *gy, int *gw, int *gh) {
    if (g_dev.rt_off) { *gx = x; *gy = y; *gw = w; *gh = h; return; }   /* texture target: shaders flip Y */
    float sx = (float)g_dev.out_w / (float)g_dev.bb_w, sy = (float)g_dev.out_h / (float)g_dev.bb_h;
    *gx = g_dev.out_x + (int)(x * sx);
    *gy = g_dev.out_y + (int)((g_dev.bb_h - y - h) * sy);
    *gw = (int)(w * sx + 0.5f); *gh = (int)(h * sy + 0.5f);
}
void rr_fill_caps(rr_D3DCAPS8 *c);
uint32_t rr_d3d_object(void);
/* gl_load.c */
int rr_gl_load(void);
/* config.c */
int rr_config_int(const char *key, int def);
/* d3d8_resource.c */
void rr_resources_init(void);
int  rr_texture_format_supported(uint32_t usage, uint32_t rtype, uint32_t fmt);
rr_texture *rr_texture_of(uint32_t obj);
rr_buffer  *rr_buffer_of(uint32_t obj);
rr_surface *rr_surface_of(uint32_t obj);
void rr_texture_bind(rr_texture *t, int unit);
uint32_t rr_buffer_gl(rr_buffer *b, uint32_t target);  /* upload pending bytes, return GL name */
/* d3d8_draw.c */
void rr_draw_init(void);
/* math */
static inline void rr_mat_identity(float *m) {
    for (int i = 0; i < 16; i++) m[i] = (i % 5) == 0 ? 1.f : 0.f;
}

/* Device methods are implemented across the files; each exports a table
 * ({slot, fn} ... {0,0}) and d3d8_device.c concatenates them for CreateDevice. */
#define RR_DEV(slot, fn) { IDIRECT3DDEVICE8_##slot, fn },
const rr_method *rr_device_method_table(void);
extern const rr_method rr_dev_state_methods[], rr_dev_resource_methods[], rr_dev_draw_methods[], rr_dev_sb_methods[];
/* d3d8_stateblock.c: return 1 when a state block is recording and took the call */
int rr_sb_rec_rs(uint32_t s, uint32_t v);
int rr_sb_rec_tss(uint32_t st, uint32_t t, uint32_t v);
int rr_sb_rec_xf(uint32_t s, const float *m);
int rr_sb_rec_tex(uint32_t st, uint32_t t);
int rr_sb_rec_light(uint32_t i, const rr_D3DLIGHT8 *l);
int rr_sb_rec_light_en(uint32_t i, int en);
int rr_sb_rec_vp(const rr_D3DVIEWPORT8 *v);
int rr_sb_rec_mat(const rr_D3DMATERIAL8 *m);
int rr_sb_rec_fvf(uint32_t f);
int rr_sb_rec_stream(uint32_t vb, uint32_t stride);
int rr_sb_rec_ib(uint32_t ib, uint32_t base);
uint32_t rr_vs_fvf(uint32_t handle);     /* FVF code for a SetVertexShader handle */
