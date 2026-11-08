/*
 * d3d8.c - Direct3D 8 for rf2.exe on SDL2 + OpenGL ES.
 *
 * This file: IDirect3D8 (adapter, modes, caps, CreateDevice), the device's
 * window/context, and device state storage. Resources live in
 * d3d8_resource.c and drawing in d3d8_draw.c. Every method not implemented
 * logs once (com.c) so bring-up shows exactly what the engine needs.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL2/SDL.h>
#include "com.h"
#include "d3d8_slots.h"
#include "d3d8_types.h"
#include "d3d8_internal.h"
#include "rered.h"

RR_DEFINE_IFACE(D3D, IDirect3D8, IDIRECT3D8)
RR_DEFINE_IFACE(Dev, IDirect3DDevice8, IDIRECT3DDEVICE8)

rr_device g_dev;
static uint32_t g_d3d_obj;

/* ---------------------------------------------------------------- modes */
static const struct { uint32_t w, h; } k_modes[] = { {640, 480}, {800, 600}, {1024, 768} };
#define NMODES (sizeof k_modes / sizeof *k_modes)
static int mode_format_ok(uint32_t f) { return f == D3DFMT_X8R8G8B8 || f == D3DFMT_R5G6B5; }

static void d_QueryInterface(void) { if (A32(2)) MEM32(A32(2)) = A32(0); rr_com_addref(A32(0)); RET(S_OK, 3); }
static void d_AddRef(void)  { RET(rr_com_addref(A32(0)), 1); }
static void d_Release(void) { RET(rr_com_release(A32(0)), 1); }
static void d_GetAdapterCount(void) { RET(1, 1); }
static void d_GetAdapterIdentifier(void) {          /* (this, adapter, flags, ident*) */
    rr_D3DADAPTER_IDENTIFIER8 *id = APTR(3);
    memset(id, 0, sizeof *id);
    snprintf(id->Driver, sizeof id->Driver, "rered_gles");
    snprintf(id->Description, sizeof id->Description, "OpenGL ES (%s)", g_dev.gl_renderer[0] ? g_dev.gl_renderer : "SDL2");
    id->DriverVersion = ((uint64_t)6 << 48) | ((uint64_t)14 << 32) | 1;
    id->VendorId = 0x13B5;                          /* Arm; the engine only prints it */
    id->WHQLLevel = 1;
    RET(S_OK, 4);
}
static void d_GetAdapterModeCount(void) { RET(NMODES * 2, 2); }
static void d_EnumAdapterModes(void) {             /* (this, adapter, mode, D3DDISPLAYMODE*) */
    uint32_t i = A32(2);
    if (i >= NMODES * 2) RET(D3DERR_INVALIDCALL, 4);
    rr_D3DDISPLAYMODE *m = APTR(3);
    m->Width = k_modes[i % NMODES].w; m->Height = k_modes[i % NMODES].h;
    m->RefreshRate = 60; m->Format = i < NMODES ? D3DFMT_X8R8G8B8 : D3DFMT_R5G6B5;
    RET(S_OK, 4);
}
static void d_GetAdapterDisplayMode(void) {        /* (this, adapter, mode*) */
    rr_D3DDISPLAYMODE *m = APTR(2);
    m->Width = 640; m->Height = 480; m->RefreshRate = 60; m->Format = D3DFMT_X8R8G8B8;
    RET(S_OK, 3);
}
static void d_CheckDeviceType(void) {              /* (this, adapter, type, displayfmt, bbfmt, windowed) */
    RET(mode_format_ok(A32(3)) ? S_OK : D3DERR_NOTAVAILABLE, 6);
}
static void d_CheckDeviceFormat(void) {            /* (this, adapter, type, adapterfmt, usage, rtype, checkfmt) */
    RET(rr_texture_format_supported(A32(4), A32(5), A32(6)) ? S_OK : D3DERR_NOTAVAILABLE, 7);
}
static void d_CheckDeviceMultiSampleType(void) {   /* (this, adapter, type, fmt, windowed, mstype) */
    RET(A32(5) == 0 ? S_OK : D3DERR_NOTAVAILABLE, 6);
}
static void d_CheckDepthStencilMatch(void) {
    uint32_t ds = A32(5);
    RET(ds == D3DFMT_D16 || ds == D3DFMT_D24S8 || ds == D3DFMT_D24X8 ? S_OK : D3DERR_NOTAVAILABLE, 6);
}

void rr_fill_caps(rr_D3DCAPS8 *c) {
    memset(c, 0, sizeof *c);
    c->DeviceType = 1;                              /* D3DDEVTYPE_HAL */
    c->Caps2 = 0x20000000;                          /* D3DCAPS2_DYNAMICTEXTURES? kept minimal */
    c->PresentationIntervals = 0x80000001;          /* IMMEDIATE | ONE */
    c->DevCaps = 0x10 | 0x40 | 0x80 | 0x200 | 0x10000 | 0x80000;  /* exec/tl vram, drawprim2, hwtransformandlight, hwrasterization */
    c->PrimitiveMiscCaps = 0x2 | 0x10 | 0x20 | 0x40 | 0x100;       /* masking, cull none/cw/ccw, colorwriteenable */
    c->RasterCaps = 0x1 | 0x10 | 0x80 | 0x100 | 0x200 | 0x400 | 0x4000 | 0x100000; /* dither,zbias?,fogvertex,fogtable,mipmaplodbias,zbufferlessHSR? */
    c->ZCmpCaps = c->AlphaCmpCaps = 0xFF;
    c->SrcBlendCaps = c->DestBlendCaps = 0x1FFF;
    c->ShadeCaps = 0x8 | 0x200 | 0x4000 | 0x80000;  /* color gouraud, specular, alpha gouraud, fog gouraud */
    c->TextureCaps = 0x1 | 0x4 | 0x400 | 0x4000;    /* perspective, alpha, cubemap? mipmap */
    c->TextureFilterCaps = c->CubeTextureFilterCaps = 0x0300 | 0x030000 | 0x03000000;
    c->TextureAddressCaps = 0x1F;
    c->MaxTextureWidth = c->MaxTextureHeight = 2048;
    c->MaxTextureRepeat = 8192;
    c->MaxTextureAspectRatio = 2048;
    c->MaxAnisotropy = 1;
    c->MaxVertexW = 1e10f;
    c->GuardBandLeft = -8192.f; c->GuardBandTop = -8192.f; c->GuardBandRight = 8192.f; c->GuardBandBottom = 8192.f;
    c->StencilCaps = 0xFF;
    c->FVFCaps = 8 | 0x80000;                       /* 8 texcoord sets, PSIZE */
    c->TextureOpCaps = 0x03FFFFFF;
    c->MaxTextureBlendStages = 8;
    c->MaxSimultaneousTextures = RR_MAX_STAGES;
    c->VertexProcessingCaps = 0x1 | 0x2 | 0x8 | 0x10 | 0x20 | 0x40; /* texgen, materialsource7, directionallights, positionallights, localviewer, tweening */
    c->MaxActiveLights = 8;
    c->MaxUserClipPlanes = 0;
    c->MaxVertexBlendMatrices = 4;
    c->MaxVertexBlendMatrixIndex = 0;
    c->MaxPointSize = 64.f;
    c->MaxPrimitiveCount = 0xFFFFF;
    c->MaxVertexIndex = 0xFFFFFF;
    c->MaxStreams = RR_MAX_STREAMS;
    c->MaxStreamStride = 256;
    c->VertexShaderVersion = 0;                     /* fixed function only: no shader paths */
    c->PixelShaderVersion = 0;
    c->MaxPixelShaderValue = 1.f;
}
static void d_GetDeviceCaps(void) { rr_fill_caps(APTR(3)); RET(S_OK, 4); }   /* (this, adapter, type, caps*) */
static void d_GetAdapterMonitor(void) { RET(1, 2); }

/* --------------------------------------------------------- CreateDevice */
static int window_and_context(const rr_D3DPRESENT_PARAMETERS *pp) {
    if (g_dev.window) return 0;
    int w = pp->BackBufferWidth ? (int)pp->BackBufferWidth : 640;
    int h = pp->BackBufferHeight ? (int)pp->BackBufferHeight : 480;
    if (!SDL_WasInit(SDL_INIT_VIDEO) && SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "[d3d8] SDL video: %s\n", SDL_GetError()); return -1;
    }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    /* Ask for ES 3.0 first (the H700 Mali blob reports 3.2); fall back to 2.0. */
    static const int vers[][2] = { {3, 0}, {2, 0} };
    for (int i = 0; i < 2 && !g_dev.gl; i++) {
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, vers[i][0]);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, vers[i][1]);
        if (!g_dev.window) {
            /* Fullscreen at the display's own resolution (any screen); the
             * 640x480 back buffer is scaled into a centred 4:3 rectangle.
             * RERED_WINDOWED=1 gives a resizable window for desktop tests. */
            uint32_t fl = SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE;
            if (!getenv("RERED_WINDOWED")) fl |= SDL_WINDOW_FULLSCREEN_DESKTOP;
            g_dev.window = SDL_CreateWindow("Red Faction II", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, w, h, fl);
            if (!g_dev.window) { fprintf(stderr, "[d3d8] window: %s\n", SDL_GetError()); return -1; }
        }
        g_dev.gl = SDL_GL_CreateContext(g_dev.window);
        if (g_dev.gl) g_dev.gles_major = vers[i][0];
    }
    if (!g_dev.gl) { fprintf(stderr, "[d3d8] GLES context: %s\n", SDL_GetError()); return -1; }
    SDL_GL_SetSwapInterval(rr_config_int("vsync", 0));
    if (rr_gl_load() != 0) return -1;
    { void rr_crash_install(void); rr_crash_install(); }   /* SDL video installs its own handlers: take them back */
    g_dev.bb_w = w; g_dev.bb_h = h;
    rr_update_output_rect();
    fprintf(stderr, "[d3d8] %dx%d backbuffer, drawable %dx%d, GLES %s / %s\n", w, h,
            g_dev.drawable_w, g_dev.drawable_h, g_dev.gl_version, g_dev.gl_renderer);
    return 0;
}

void rr_update_output_rect(void) {
    if (!g_dev.window) return;
    SDL_GL_GetDrawableSize(g_dev.window, &g_dev.drawable_w, &g_dev.drawable_h);
    int dw = g_dev.drawable_w, dh = g_dev.drawable_h, bw = g_dev.bb_w ? g_dev.bb_w : 640, bh = g_dev.bb_h ? g_dev.bb_h : 480;
    if ((long)dw * bh > (long)dh * bw) { g_dev.out_h = dh; g_dev.out_w = (int)((long)dh * bw / bh); }
    else { g_dev.out_w = dw; g_dev.out_h = (int)((long)dw * bh / bw); }
    g_dev.out_x = (dw - g_dev.out_w) / 2; g_dev.out_y = (dh - g_dev.out_h) / 2;
    g_dev.state_dirty |= RR_DIRTY_VP;
    fprintf(stderr, "[d3d8] output %dx%d at %d,%d in %dx%d\n", g_dev.out_w, g_dev.out_h, g_dev.out_x, g_dev.out_y, dw, dh);
}

static void state_defaults(void);
static void d_CreateDevice(void) {   /* (this, adapter, type, hwnd, behavior, pp*, dev**) */
    rr_D3DPRESENT_PARAMETERS *pp = APTR(5);
    fprintf(stderr, "[d3d8] CreateDevice %ux%u fmt %u count %u windowed %u autodepth %u/%u flags 0x%X behavior 0x%X\n",
            pp->BackBufferWidth, pp->BackBufferHeight, pp->BackBufferFormat, pp->BackBufferCount,
            pp->Windowed, pp->EnableAutoDepthStencil, pp->AutoDepthStencilFormat, pp->Flags, A32(4));
    if (window_and_context(pp) != 0) RET(D3DERR_NOTAVAILABLE, 7);
    if (!pp->BackBufferWidth)  pp->BackBufferWidth = (uint32_t)g_dev.bb_w;
    if (!pp->BackBufferHeight) pp->BackBufferHeight = (uint32_t)g_dev.bb_h;
    if (pp->BackBufferFormat == 0) pp->BackBufferFormat = D3DFMT_X8R8G8B8;
    g_dev.pp = *pp;
    g_dev.bb_w = (int)pp->BackBufferWidth; g_dev.bb_h = (int)pp->BackBufferHeight;
    rr_update_output_rect();
    g_dev.hwnd = A32(3) ? A32(3) : pp->hDeviceWindow;
    g_dev.behavior = A32(4);
    state_defaults();
    rr_resources_init();
    rr_draw_init();
    if (!g_dev.obj) {
        rr_iface_vtable(&Dev_iface, rr_device_method_table());
        g_dev.obj = rr_com_new(&Dev_iface, &g_dev);
    } else rr_com_addref(g_dev.obj);
    MEM32(A32(6)) = g_dev.obj;
    /* A fullscreen device takes the display and activates its focus window;
     * Windows then sends WM_ACTIVATEAPP / WM_ACTIVATE. Red Faction II's main
     * loop waits on the "active" flag its window procedure sets from them
     * (it installs that procedure after the window was shown). */
    if (!pp->Windowed && g_dev.hwnd) {
        hle_send(g_dev.hwnd, 0x001C /* WM_ACTIVATEAPP */, 1, 0);
        hle_send(g_dev.hwnd, 0x0006 /* WM_ACTIVATE */, 1 /* WA_ACTIVE */, 0);
    }
    RET(S_OK, 7);
}

/* -------------------------------------------------------- device state */
static void state_defaults(void) {
    memset(g_dev.rs, 0, sizeof g_dev.rs);
    uint32_t *rs = g_dev.rs;
    rs[D3DRS_ZENABLE] = g_dev.pp.EnableAutoDepthStencil ? 1 : 0;
    rs[D3DRS_FILLMODE] = 3; rs[D3DRS_SHADEMODE] = 2;
    rs[D3DRS_ZWRITEENABLE] = 1; rs[D3DRS_ALPHATESTENABLE] = 0; rs[D3DRS_LASTPIXEL] = 1;
    rs[D3DRS_SRCBLEND] = 2; rs[D3DRS_DESTBLEND] = 1; rs[D3DRS_CULLMODE] = 3;
    rs[D3DRS_ZFUNC] = 4; rs[D3DRS_ALPHAFUNC] = 8; rs[D3DRS_DITHERENABLE] = 0;
    rs[D3DRS_ALPHABLENDENABLE] = 0; rs[D3DRS_FOGENABLE] = 0; rs[D3DRS_SPECULARENABLE] = 0;
    rs[D3DRS_FOGCOLOR] = 0; rs[D3DRS_FOGTABLEMODE] = 0; rs[D3DRS_FOGVERTEXMODE] = 0;
    float one = 1.f; memcpy(&rs[D3DRS_FOGEND], &one, 4); memcpy(&rs[D3DRS_FOGDENSITY], &one, 4);
    rs[D3DRS_STENCILENABLE] = 0; rs[D3DRS_STENCILFUNC] = 8; rs[D3DRS_STENCILMASK] = rs[D3DRS_STENCILWRITEMASK] = 0xFFFFFFFF;
    rs[D3DRS_STENCILFAIL] = rs[D3DRS_STENCILZFAIL] = rs[D3DRS_STENCILPASS] = 1;
    rs[D3DRS_TEXTUREFACTOR] = 0xFFFFFFFF; rs[D3DRS_LIGHTING] = 1; rs[D3DRS_AMBIENT] = 0;
    rs[D3DRS_COLORVERTEX] = 1; rs[D3DRS_LOCALVIEWER] = 1; rs[D3DRS_CLIPPING] = 1;
    rs[D3DRS_DIFFUSEMATERIALSOURCE] = 1; rs[D3DRS_SPECULARMATERIALSOURCE] = 2;
    rs[D3DRS_AMBIENTMATERIALSOURCE] = 0; rs[D3DRS_EMISSIVEMATERIALSOURCE] = 0;
    rs[D3DRS_COLORWRITEENABLE] = 0xF; rs[D3DRS_BLENDOP] = 1;
    for (int s = 0; s < RR_MAX_STAGES; s++) {
        uint32_t *t = g_dev.tss[s];
        memset(t, 0, sizeof g_dev.tss[s]);
        t[D3DTSS_COLOROP] = s == 0 ? D3DTOP_MODULATE : D3DTOP_DISABLE;
        t[D3DTSS_COLORARG1] = D3DTA_TEXTURE; t[D3DTSS_COLORARG2] = D3DTA_CURRENT;
        t[D3DTSS_ALPHAOP] = s == 0 ? D3DTOP_SELECTARG1 : D3DTOP_DISABLE;
        t[D3DTSS_ALPHAARG1] = D3DTA_TEXTURE; t[D3DTSS_ALPHAARG2] = D3DTA_CURRENT;
        t[D3DTSS_TEXCOORDINDEX] = (uint32_t)s;
        t[D3DTSS_ADDRESSU] = t[D3DTSS_ADDRESSV] = t[D3DTSS_ADDRESSW] = 1;
        t[D3DTSS_MAGFILTER] = t[D3DTSS_MINFILTER] = 1; t[D3DTSS_MIPFILTER] = 0;
        t[D3DTSS_MAXANISOTROPY] = 1;
    }
    for (int i = 0; i < RR_NUM_TRANSFORMS; i++) rr_mat_identity(g_dev.xf[i]);
    g_dev.vp.X = g_dev.vp.Y = 0; g_dev.vp.Width = g_dev.pp.BackBufferWidth; g_dev.vp.Height = g_dev.pp.BackBufferHeight;
    g_dev.vp.MinZ = 0.f; g_dev.vp.MaxZ = 1.f;
    g_dev.state_dirty = ~0u;
}

/* ------------------------------------------------------------- module */
static void s_Direct3DCreate8(void) {               /* (SDKVersion) */
    if (!g_d3d_obj) {
        static const rr_method impl[] = {
            { IDIRECT3D8_QueryInterface, d_QueryInterface }, { IDIRECT3D8_AddRef, d_AddRef },
            { IDIRECT3D8_Release, d_Release }, { IDIRECT3D8_GetAdapterCount, d_GetAdapterCount },
            { IDIRECT3D8_GetAdapterIdentifier, d_GetAdapterIdentifier },
            { IDIRECT3D8_GetAdapterModeCount, d_GetAdapterModeCount },
            { IDIRECT3D8_EnumAdapterModes, d_EnumAdapterModes },
            { IDIRECT3D8_GetAdapterDisplayMode, d_GetAdapterDisplayMode },
            { IDIRECT3D8_CheckDeviceType, d_CheckDeviceType },
            { IDIRECT3D8_CheckDeviceFormat, d_CheckDeviceFormat },
            { IDIRECT3D8_CheckDeviceMultiSampleType, d_CheckDeviceMultiSampleType },
            { IDIRECT3D8_CheckDepthStencilMatch, d_CheckDepthStencilMatch },
            { IDIRECT3D8_GetDeviceCaps, d_GetDeviceCaps },
            { IDIRECT3D8_GetAdapterMonitor, d_GetAdapterMonitor },
            { IDIRECT3D8_CreateDevice, d_CreateDevice },
            { 0, 0 } };
        rr_iface_vtable(&D3D_iface, impl);
        g_d3d_obj = rr_com_new(&D3D_iface, NULL);
    } else rr_com_addref(g_d3d_obj);
    fprintf(stderr, "[d3d8] Direct3DCreate8(%u) -> 0x%08X\n", A32(0), g_d3d_obj);
    RET(g_d3d_obj, 1);
}
uint32_t rr_d3d_object(void) { return g_d3d_obj; }

static const win32hle_shim g_d3d8_shims[] = {
    { "d3d8.dll!Direct3DCreate8", s_Direct3DCreate8 },
    { "Direct3DCreate8", s_Direct3DCreate8 },     /* GetProcAddress on a LoadLibrary'd d3d8.dll */
    { 0, 0 } };
void rr_register_d3d8(void) { win32hle_register(g_d3d8_shims); }
