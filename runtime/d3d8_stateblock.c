/*
 * d3d8_stateblock.c - D3D8 state blocks and vertex shader declarations.
 *
 * State blocks: a block stores values plus a mask of which states it holds.
 * CreateStateBlock captures all (or the pixel / vertex subset) of the
 * current device state. Between BeginStateBlock and EndStateBlock, Set*
 * calls are recorded into the block instead of changing the device (D3D8
 * semantics); rr_sb_rec_* return 1 when they consumed a call.
 *
 * Vertex shaders: the game uses declarations with no shader function (fixed
 * function). A declaration whose registers line up with an FVF layout is
 * turned into that FVF; handles always have bit 0 set, which no FVF code
 * does, so SetVertexShader can tell them apart.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "d3d8_slots.h"
#include "d3d8_internal.h"

typedef struct {
    uint32_t rs[RR_RS_COUNT];           uint8_t rs_m[RR_RS_COUNT];
    uint32_t tss[RR_MAX_STAGES][RR_TSS_COUNT]; uint8_t tss_m[RR_MAX_STAGES][RR_TSS_COUNT];
    float    xf[RR_NUM_TRANSFORMS][16]; uint8_t xf_m[RR_NUM_TRANSFORMS];
    uint32_t tex[RR_MAX_STAGES];        uint8_t tex_m[RR_MAX_STAGES];
    rr_D3DLIGHT8 light[RR_MAX_LIGHTS]; int light_en[RR_MAX_LIGHTS]; uint8_t light_m[RR_MAX_LIGHTS], light_en_m[RR_MAX_LIGHTS];
    rr_D3DVIEWPORT8 vp; rr_D3DMATERIAL8 mat;
    uint32_t fvf, vb, stride, ib, base_vertex;
    uint8_t vp_m, mat_m, fvf_m, stream_m, ib_m;
} sblock;

#define MAXSB 1024
static sblock *g_sb[MAXSB];
static sblock *g_rec;                 /* recording, or NULL */
static int g_rec_idx;

/* pixel-pipeline render states (D3D8 docs' D3DSBT_PIXELSTATE list, the subset this device keeps) */
static int is_pixel_rs(uint32_t s) {
    switch (s) {
    case D3DRS_ZENABLE: case D3DRS_FILLMODE: case D3DRS_SHADEMODE: case D3DRS_ZWRITEENABLE: case D3DRS_ALPHATESTENABLE:
    case D3DRS_LASTPIXEL: case D3DRS_SRCBLEND: case D3DRS_DESTBLEND: case D3DRS_ZFUNC: case D3DRS_ALPHAREF:
    case D3DRS_ALPHAFUNC: case D3DRS_DITHERENABLE: case D3DRS_ALPHABLENDENABLE: case D3DRS_FOGSTART: case D3DRS_FOGEND:
    case D3DRS_FOGDENSITY: case D3DRS_ZBIAS: case D3DRS_STENCILENABLE: case D3DRS_STENCILFAIL: case D3DRS_STENCILZFAIL:
    case D3DRS_STENCILPASS: case D3DRS_STENCILFUNC: case D3DRS_STENCILREF: case D3DRS_STENCILMASK:
    case D3DRS_STENCILWRITEMASK: case D3DRS_TEXTUREFACTOR: case D3DRS_COLORWRITEENABLE: case D3DRS_BLENDOP:
        return 1;
    }
    return 0;
}
static void capture(sblock *b) {
    for (int i = 0; i < RR_RS_COUNT; i++) if (b->rs_m[i]) b->rs[i] = g_dev.rs[i];
    for (int s = 0; s < RR_MAX_STAGES; s++) {
        for (int t = 0; t < RR_TSS_COUNT; t++) if (b->tss_m[s][t]) b->tss[s][t] = g_dev.tss[s][t];
        if (b->tex_m[s]) b->tex[s] = g_dev.tex[s];
    }
    for (int i = 0; i < RR_NUM_TRANSFORMS; i++) if (b->xf_m[i]) memcpy(b->xf[i], g_dev.xf[i], 64);
    for (int i = 0; i < RR_MAX_LIGHTS; i++) {
        if (b->light_m[i]) b->light[i] = g_dev.light[i];
        if (b->light_en_m[i]) b->light_en[i] = g_dev.light_enabled[i];
    }
    if (b->vp_m) b->vp = g_dev.vp;
    if (b->mat_m) b->mat = g_dev.material;
    if (b->fvf_m) b->fvf = g_dev.fvf;
    if (b->stream_m) { b->vb = g_dev.stream_vb; b->stride = g_dev.stream_stride; }
    if (b->ib_m) { b->ib = g_dev.ib; b->base_vertex = g_dev.base_vertex; }
}
static void apply(const sblock *b) {
    for (int i = 0; i < RR_RS_COUNT; i++) if (b->rs_m[i]) g_dev.rs[i] = b->rs[i];
    for (int s = 0; s < RR_MAX_STAGES; s++) {
        for (int t = 0; t < RR_TSS_COUNT; t++) if (b->tss_m[s][t]) g_dev.tss[s][t] = b->tss[s][t];
        if (b->tex_m[s]) g_dev.tex[s] = b->tex[s];
    }
    for (int i = 0; i < RR_NUM_TRANSFORMS; i++) if (b->xf_m[i]) memcpy(g_dev.xf[i], b->xf[i], 64);
    for (int i = 0; i < RR_MAX_LIGHTS; i++) {
        if (b->light_m[i]) g_dev.light[i] = b->light[i];
        if (b->light_en_m[i]) g_dev.light_enabled[i] = b->light_en[i];
    }
    if (b->vp_m) g_dev.vp = b->vp;
    if (b->mat_m) g_dev.material = b->mat;
    if (b->fvf_m) g_dev.fvf = b->fvf;
    if (b->stream_m) { g_dev.stream_vb = b->vb; g_dev.stream_stride = b->stride; }
    if (b->ib_m) { g_dev.ib = b->ib; g_dev.base_vertex = b->base_vertex; }
    g_dev.state_dirty = ~0u;
}
static int alloc_slot(sblock *b) {
    for (int i = 1; i < MAXSB; i++) if (!g_sb[i]) { g_sb[i] = b; return i; }
    free(b); return 0;
}

/* --- recording hooks used by the setters --- */
int rr_sb_rec_rs(uint32_t s, uint32_t v) { if (!g_rec || s >= RR_RS_COUNT) return 0; g_rec->rs[s] = v; g_rec->rs_m[s] = 1; return 1; }
int rr_sb_rec_tss(uint32_t st, uint32_t t, uint32_t v) { if (!g_rec || st >= RR_MAX_STAGES || t >= RR_TSS_COUNT) return 0; g_rec->tss[st][t] = v; g_rec->tss_m[st][t] = 1; return 1; }
int rr_sb_rec_xf(uint32_t s, const float *m) { if (!g_rec || s >= RR_NUM_TRANSFORMS) return 0; memcpy(g_rec->xf[s], m, 64); g_rec->xf_m[s] = 1; return 1; }
int rr_sb_rec_tex(uint32_t st, uint32_t t) { if (!g_rec || st >= RR_MAX_STAGES) return 0; g_rec->tex[st] = t; g_rec->tex_m[st] = 1; return 1; }
int rr_sb_rec_light(uint32_t i, const rr_D3DLIGHT8 *l) { if (!g_rec || i >= RR_MAX_LIGHTS) return 0; g_rec->light[i] = *l; g_rec->light_m[i] = 1; return 1; }
int rr_sb_rec_light_en(uint32_t i, int en) { if (!g_rec || i >= RR_MAX_LIGHTS) return 0; g_rec->light_en[i] = en; g_rec->light_en_m[i] = 1; return 1; }
int rr_sb_rec_vp(const rr_D3DVIEWPORT8 *v) { if (!g_rec) return 0; g_rec->vp = *v; g_rec->vp_m = 1; return 1; }
int rr_sb_rec_mat(const rr_D3DMATERIAL8 *m) { if (!g_rec) return 0; g_rec->mat = *m; g_rec->mat_m = 1; return 1; }
int rr_sb_rec_fvf(uint32_t f) { if (!g_rec) return 0; g_rec->fvf = f; g_rec->fvf_m = 1; return 1; }
int rr_sb_rec_stream(uint32_t vb, uint32_t stride) { if (!g_rec) return 0; g_rec->vb = vb; g_rec->stride = stride; g_rec->stream_m = 1; return 1; }
int rr_sb_rec_ib(uint32_t ib, uint32_t base) { if (!g_rec) return 0; g_rec->ib = ib; g_rec->base_vertex = base; g_rec->ib_m = 1; return 1; }

/* --- device methods --- */
static void v_BeginStateBlock(void) {
    if (g_rec) RET(D3DERR_INVALIDCALL, 1);
    g_rec = calloc(1, sizeof *g_rec);
    g_rec_idx = alloc_slot(g_rec);
    if (!g_rec_idx) { g_rec = NULL; RET(E_OUTOFMEMORY, 1); }
    RET(D3D_OK, 1);
}
static void v_EndStateBlock(void) {                  /* (this, DWORD *token) */
    if (!g_rec) RET(D3DERR_INVALIDCALL, 2);
    MEM32(A32(1)) = (uint32_t)g_rec_idx;
    g_rec = NULL;
    RET(D3D_OK, 2);
}
static void v_CreateStateBlock(void) {               /* (this, D3DSTATEBLOCKTYPE, DWORD *token) */
    uint32_t type = A32(1);                          /* 1 ALL, 2 PIXELSTATE, 3 VERTEXSTATE */
    sblock *b = calloc(1, sizeof *b);
    for (int i = 0; i < RR_RS_COUNT; i++) b->rs_m[i] = type == 1 || (type == 2) == is_pixel_rs((uint32_t)i);
    int pix = type != 3, vtx = type != 2;
    for (int s = 0; s < RR_MAX_STAGES; s++) {
        for (int t = 0; t < RR_TSS_COUNT; t++) b->tss_m[s][t] = (uint8_t)(t == D3DTSS_TEXCOORDINDEX || t == D3DTSS_TEXTURETRANSFORMFLAGS ? vtx || pix : pix);
        b->tex_m[s] = (uint8_t)(type == 1);
    }
    if (type == 1 || vtx) {
        for (int i = 0; i < RR_MAX_LIGHTS; i++) b->light_m[i] = b->light_en_m[i] = 1;
        b->fvf_m = 1;
    }
    if (type == 1) {
        for (int i = 0; i < RR_NUM_TRANSFORMS; i++) b->xf_m[i] = 1;
        b->vp_m = b->mat_m = b->stream_m = b->ib_m = 1;
    }
    capture(b);
    int h = alloc_slot(b);
    if (!h) RET(E_OUTOFMEMORY, 3);
    MEM32(A32(2)) = (uint32_t)h;
    RET(D3D_OK, 3);
}
static sblock *sb_of(uint32_t h) { return h > 0 && h < MAXSB ? g_sb[h] : NULL; }
static void v_ApplyStateBlock(void)   { sblock *b = sb_of(A32(1)); if (!b || g_rec) RET(D3DERR_INVALIDCALL, 2); apply(b); RET(D3D_OK, 2); }
static void v_CaptureStateBlock(void) { sblock *b = sb_of(A32(1)); if (!b || g_rec) RET(D3DERR_INVALIDCALL, 2); capture(b); RET(D3D_OK, 2); }
static void v_DeleteStateBlock(void)  { uint32_t h = A32(1); if (!sb_of(h)) RET(D3DERR_INVALIDCALL, 2); free(g_sb[h]); g_sb[h] = NULL; RET(D3D_OK, 2); }

/* ------------------------------------------------- vertex shader decls */
#define MAXVS 256
typedef struct { uint32_t fvf; int used; } vshader;
static vshader g_vs[MAXVS];

static uint32_t decl_to_fvf(const uint32_t *tok, int *ok) {
    /* D3DVSD tokens: [31:29] type: 1 STREAM, 2 STREAMDATA (reg: [3:0]... data type [19:16]), 7 END */
    uint32_t fvf = 0; int stream = -1, ntex = 0, last = -1;
    *ok = 1;
    static const int order[] = { 0, 1, 3, 4, 5, 6, 7, 8, 9, 10 };   /* FVF register order */
    (void)order;
    for (int i = 0; i < 64; i++) {
        uint32_t t = tok[i];
        if (t == 0xFFFFFFFFu) break;
        uint32_t kind = t >> 29;
        if (kind == 1) { stream = (int)(t & 0xF); if (stream != 0) *ok = 0; continue; }
        if (kind != 2 || (t & 0x10000000u)) { *ok = 0; continue; }      /* skips, constants, tessellation */
        uint32_t reg = t & 0x1F, type = (t >> 16) & 0xF;
        if ((int)reg < last) *ok = 0;   /* FVF fixes the order */
        last = (int)reg;
        switch (reg) {
        case 0: if (type == 2) fvf |= D3DFVF_XYZ; else if (type == 3) fvf |= D3DFVF_XYZRHW; else *ok = 0; break;
        case 1: if (type >= 0 && type <= 3) fvf = (fvf & ~D3DFVF_POSITION_MASK) | (D3DFVF_XYZB1 + type * 2); else *ok = 0; break;
        case 3: if (type == 2) fvf |= D3DFVF_NORMAL; else *ok = 0; break;
        case 4: if (type == 0) fvf |= D3DFVF_PSIZE; else *ok = 0; break;
        case 5: if (type == 4) fvf |= D3DFVF_DIFFUSE; else *ok = 0; break;
        case 6: if (type == 4) fvf |= D3DFVF_SPECULAR; else *ok = 0; break;
        default:
            if (reg >= 7 && reg <= 14 && (int)(reg - 7) == ntex && type <= 3) {
                static const uint32_t size_bits[4] = { 3, 0, 1, 2 };     /* FLOAT1..4 -> D3DFVF_TEXCOORDSIZEn */
                fvf |= size_bits[type] << (16 + ntex * 2);
                ntex++;
            } else *ok = 0;
        }
    }
    fvf |= (uint32_t)ntex << D3DFVF_TEXCOUNT_SHIFT;
    return fvf;
}
static void v_CreateVertexShader(void) {             /* (this, decl*, function*, handle*, usage) */
    const uint32_t *decl = A32(1) ? (const uint32_t *)APTR(1) : NULL;
    if (A32(2)) {
        static int once; if (!once++) fprintf(stderr, "[d3d8] CreateVertexShader with a shader function: programmable shaders not supported\n");
        RET(D3DERR_INVALIDCALL, 5);
    }
    int ok = 0; uint32_t fvf = decl ? decl_to_fvf(decl, &ok) : 0;
    if (!ok) {
        fprintf(stderr, "[d3d8] CreateVertexShader: declaration not expressible as an FVF:");
        for (int i = 0; decl && i < 32 && decl[i] != 0xFFFFFFFFu; i++) fprintf(stderr, " %08X", decl[i]);
        fprintf(stderr, "\n");
        RET(D3DERR_INVALIDCALL, 5);
    }
    int i = 0; while (i < MAXVS && g_vs[i].used) i++;
    if (i == MAXVS) RET(E_OUTOFMEMORY, 5);
    g_vs[i].used = 1; g_vs[i].fvf = fvf;
    MEM32(A32(3)) = ((uint32_t)i << 1) | 1u;
    RET(D3D_OK, 5);
}
static void v_DeleteVertexShader(void) { uint32_t h = A32(1); if ((h & 1) && (h >> 1) < MAXVS) g_vs[h >> 1].used = 0; RET(D3D_OK, 2); }
uint32_t rr_vs_fvf(uint32_t handle) {
    if (!(handle & 1)) return handle;
    uint32_t i = handle >> 1;
    return i < MAXVS && g_vs[i].used ? g_vs[i].fvf : 0;
}

const rr_method rr_dev_sb_methods[] = {
    RR_DEV(BeginStateBlock, v_BeginStateBlock) RR_DEV(EndStateBlock, v_EndStateBlock)
    RR_DEV(CreateStateBlock, v_CreateStateBlock) RR_DEV(ApplyStateBlock, v_ApplyStateBlock)
    RR_DEV(CaptureStateBlock, v_CaptureStateBlock) RR_DEV(DeleteStateBlock, v_DeleteStateBlock)
    RR_DEV(CreateVertexShader, v_CreateVertexShader) RR_DEV(DeleteVertexShader, v_DeleteVertexShader)
    { 0, 0 }
};
