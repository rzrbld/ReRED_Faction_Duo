/*
 * d3d8_device.c - IDirect3DDevice8 state methods: IUnknown, caps/mode
 * queries, render/texture-stage state, transforms, viewport, material,
 * lights, scenes, Clear and Present. Drawing state is only recorded here;
 * d3d8_draw.c turns it into GL state at draw time.
 */
#include <stdio.h>
#include <string.h>
#include "gl_funcs.h"
#include "d3d8_slots.h"
#include "d3d8_internal.h"
#include "rered.h"

static void v_QueryInterface(void) { if (A32(2)) MEM32(A32(2)) = A32(0); rr_com_addref(A32(0)); RET(S_OK, 3); }
static void v_AddRef(void)  { RET(rr_com_addref(A32(0)), 1); }
static void v_Release(void) { RET(rr_com_release(A32(0)), 1); }   /* the device lives for the process */
static void v_TestCooperativeLevel(void) { RET(D3D_OK, 1); }
static void v_GetAvailableTextureMem(void) { RET(256u << 20, 1); }
static void v_ResourceManagerDiscardBytes(void) { RET(D3D_OK, 2); }
static void v_GetDirect3D(void) { uint32_t d = rr_d3d_object(); rr_com_addref(d); MEM32(A32(1)) = d; RET(D3D_OK, 2); }
static void v_GetDeviceCaps(void) { rr_fill_caps(APTR(1)); RET(D3D_OK, 2); }
static void v_GetDisplayMode(void) {
    rr_D3DDISPLAYMODE *m = APTR(1);
    m->Width = g_dev.pp.BackBufferWidth; m->Height = g_dev.pp.BackBufferHeight;
    m->RefreshRate = 60; m->Format = D3DFMT_X8R8G8B8;
    RET(D3D_OK, 2);
}
static void v_GetCreationParameters(void) {
    rr_D3DDEVICE_CREATION_PARAMETERS *p = APTR(1);
    p->AdapterOrdinal = 0; p->DeviceType = 1; p->hFocusWindow = g_dev.hwnd; p->BehaviorFlags = g_dev.behavior;
    RET(D3D_OK, 2);
}
static void v_ShowCursor(void) { RET(0, 2); }
static void v_Reset(void) {                         /* (this, pp*) */
    rr_D3DPRESENT_PARAMETERS *pp = APTR(1);
    fprintf(stderr, "[d3d8] Reset %ux%u\n", pp->BackBufferWidth, pp->BackBufferHeight);
    if (!pp->BackBufferWidth)  pp->BackBufferWidth = g_dev.pp.BackBufferWidth;
    if (!pp->BackBufferHeight) pp->BackBufferHeight = g_dev.pp.BackBufferHeight;
    g_dev.pp = *pp;
    g_dev.vp.X = g_dev.vp.Y = 0; g_dev.vp.Width = pp->BackBufferWidth; g_dev.vp.Height = pp->BackBufferHeight;
    g_dev.state_dirty = ~0u;
    RET(D3D_OK, 2);
}

/* --- Present: the default framebuffer is the back buffer --- */
void rr_present_hook(void);   /* frame pacing / overlay / profiler (frame.c) */
static void v_Present(void) {                       /* (this, src, dst, hwnd, dirty) */
    rr_present_hook();
    /* bars around the 4:3 picture stay black */
    if (g_dev.out_w != g_dev.drawable_w || g_dev.out_h != g_dev.drawable_h) {
        glEnable(GL_SCISSOR_TEST); glColorMask(1, 1, 1, 1); glClearColor(0, 0, 0, 1);
        if (g_dev.out_x) { glScissor(0, 0, g_dev.out_x, g_dev.drawable_h); glClear(GL_COLOR_BUFFER_BIT);
                           glScissor(g_dev.out_x + g_dev.out_w, 0, g_dev.drawable_w - g_dev.out_x - g_dev.out_w, g_dev.drawable_h); glClear(GL_COLOR_BUFFER_BIT); }
        if (g_dev.out_y) { glScissor(0, 0, g_dev.drawable_w, g_dev.out_y); glClear(GL_COLOR_BUFFER_BIT);
                           glScissor(0, g_dev.out_y + g_dev.out_h, g_dev.drawable_w, g_dev.drawable_h - g_dev.out_y - g_dev.out_h); glClear(GL_COLOR_BUFFER_BIT); }
        glDisable(GL_SCISSOR_TEST); rr_apply_write_masks();
        g_dev.state_dirty |= RR_DIRTY_VP;
    }
    {   uint64_t t0 = SDL_GetPerformanceCounter();
        SDL_GL_SwapWindow(g_dev.window);
        extern uint64_t rr_swap_ticks; rr_swap_ticks += SDL_GetPerformanceCounter() - t0; }
    g_dev.frame++;
    RET(D3D_OK, 5);
}

static void v_BeginScene(void) { g_dev.in_scene = 1; RET(D3D_OK, 1); }
static void v_EndScene(void)   { g_dev.in_scene = 0; RET(D3D_OK, 1); }

void rr_apply_viewport(void);
void rr_apply_write_masks(void);
static void v_Clear(void) {                         /* (this, count, rects, flags, color, z, stencil) */
    uint32_t flags = A32(3), color = A32(4), stencil = A32(6);
    float z; uint32_t zb = A32(5); memcpy(&z, &zb, 4);
    GLbitfield m = 0;
    rr_apply_viewport();
    glEnable(GL_SCISSOR_TEST);   /* D3D clears only the viewport */
    if (flags & D3DCLEAR_TARGET) {
        glClearColor(((color >> 16) & 255) / 255.f, ((color >> 8) & 255) / 255.f, (color & 255) / 255.f, (color >> 24) / 255.f);
        glColorMask(1, 1, 1, 1); m |= GL_COLOR_BUFFER_BIT;
    }
    if (flags & D3DCLEAR_ZBUFFER) { glClearDepthf(z); glDepthMask(1); m |= GL_DEPTH_BUFFER_BIT; }
    if (flags & D3DCLEAR_STENCIL) { glClearStencil((GLint)stencil); glStencilMask(0xFF); m |= GL_STENCIL_BUFFER_BIT; }
    uint32_t n = A32(1);
    if (n && A32(2)) {               /* D3DRECT list: x1,y1,x2,y2 in back-buffer pixels */
        for (uint32_t i = 0; i < n; i++) {
            int32_t *r = (int32_t *)hle_ptr(A32(2) + i * 16);
            int x, y, w, h;
            rr_bb_to_gl(r[0], r[1], r[2] - r[0], r[3] - r[1], &x, &y, &w, &h);
            glScissor(x, y, w, h);
            glClear(m);
        }
    } else glClear(m);
    glDisable(GL_SCISSOR_TEST);
    rr_apply_write_masks();
    RET(D3D_OK, 7);
}

/* --- transforms --- */
uint32_t rr_proj_caller;                          /* debug (RERED_DLOG): who set the projection */
static void v_SetTransform(void) {                  /* (this, state, D3DMATRIX*) */
    uint32_t s = A32(1);
    if (s == 3) rr_proj_caller = MEM32(g_esp);
    if (rr_sb_rec_xf(s, APTR(2))) RET(D3D_OK, 3);
    if (s < RR_NUM_TRANSFORMS) { memcpy(g_dev.xf[s], APTR(2), 64); g_dev.state_dirty |= RR_DIRTY_XF; }
    RET(D3D_OK, 3);
}
static void v_GetTransform(void) {
    uint32_t s = A32(1);
    if (s < RR_NUM_TRANSFORMS) memcpy(APTR(2), g_dev.xf[s], 64);
    RET(D3D_OK, 3);
}
static void mat_mul(float *o, const float *a, const float *b) {   /* row-vector D3D order: o = a * b */
    float r[16];
    for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++)
        r[i * 4 + j] = a[i * 4] * b[j] + a[i * 4 + 1] * b[4 + j] + a[i * 4 + 2] * b[8 + j] + a[i * 4 + 3] * b[12 + j];
    memcpy(o, r, 64);
}
static void v_MultiplyTransform(void) {
    uint32_t s = A32(1);
    if (s < RR_NUM_TRANSFORMS) { mat_mul(g_dev.xf[s], APTR(2), g_dev.xf[s]); g_dev.state_dirty |= RR_DIRTY_XF; }
    RET(D3D_OK, 3);
}

/* --- viewport / material / lights --- */
static void v_SetViewport(void) { if (rr_sb_rec_vp(APTR(1))) RET(D3D_OK, 2); memcpy(&g_dev.vp, APTR(1), sizeof g_dev.vp); g_dev.state_dirty |= RR_DIRTY_VP; RET(D3D_OK, 2); }
static void v_GetViewport(void) { memcpy(APTR(1), &g_dev.vp, sizeof g_dev.vp); RET(D3D_OK, 2); }
static void v_SetMaterial(void) { if (rr_sb_rec_mat(APTR(1))) RET(D3D_OK, 2); memcpy(&g_dev.material, APTR(1), sizeof g_dev.material); g_dev.state_dirty |= RR_DIRTY_MAT; RET(D3D_OK, 2); }
static void v_GetMaterial(void) { memcpy(APTR(1), &g_dev.material, sizeof g_dev.material); RET(D3D_OK, 2); }
static void v_SetLight(void) {
    uint32_t i = A32(1);
    if (rr_sb_rec_light(i, APTR(2))) RET(D3D_OK, 3);
    if (i < RR_MAX_LIGHTS) { memcpy(&g_dev.light[i], APTR(2), sizeof g_dev.light[i]); g_dev.state_dirty |= RR_DIRTY_LIGHT; }
    RET(D3D_OK, 3);
}
static void v_GetLight(void) { uint32_t i = A32(1); if (i < RR_MAX_LIGHTS) memcpy(APTR(2), &g_dev.light[i], sizeof g_dev.light[i]); RET(D3D_OK, 3); }
static void v_LightEnable(void) {
    uint32_t i = A32(1);
    if (rr_sb_rec_light_en(i, A32(2) != 0)) RET(D3D_OK, 3);
    if (i < RR_MAX_LIGHTS) { g_dev.light_enabled[i] = A32(2) != 0; g_dev.state_dirty |= RR_DIRTY_LIGHT; }
    else { static int once; if (!once++) fprintf(stderr, "[d3d8] LightEnable(%u) beyond %d lights ignored\n", i, RR_MAX_LIGHTS); }
    RET(D3D_OK, 3);
}
static void v_GetLightEnable(void) { uint32_t i = A32(1); MEM32(A32(2)) = i < RR_MAX_LIGHTS ? (uint32_t)g_dev.light_enabled[i] : 0; RET(D3D_OK, 3); }

/* --- render / texture stage state --- */
static void v_SetRenderState(void) {
    uint32_t s = A32(1);
    if (rr_sb_rec_rs(s, A32(2))) RET(D3D_OK, 3);
    if (s < RR_RS_COUNT && g_dev.rs[s] != A32(2)) { g_dev.rs[s] = A32(2); g_dev.state_dirty |= RR_DIRTY_RS; }
    RET(D3D_OK, 3);
}
static void v_GetRenderState(void) { uint32_t s = A32(1); MEM32(A32(2)) = s < RR_RS_COUNT ? g_dev.rs[s] : 0; RET(D3D_OK, 3); }
static void v_SetTextureStageState(void) {
    uint32_t st = A32(1), t = A32(2);
    if (rr_sb_rec_tss(st, t, A32(3))) RET(D3D_OK, 4);
    if (st < RR_MAX_STAGES && t < RR_TSS_COUNT && g_dev.tss[st][t] != A32(3)) { g_dev.tss[st][t] = A32(3); g_dev.state_dirty |= RR_DIRTY_TSS; }
    RET(D3D_OK, 4);
}
static void v_GetTextureStageState(void) {
    uint32_t st = A32(1), t = A32(2);
    MEM32(A32(3)) = st < RR_MAX_STAGES && t < RR_TSS_COUNT ? g_dev.tss[st][t] : 0;
    RET(D3D_OK, 4);
}
static void v_ValidateDevice(void) { if (A32(1)) MEM32(A32(1)) = 1; RET(D3D_OK, 2); }
static void v_GetInfo(void) { RET(1u /* S_FALSE */, 4); }
static void v_SetGammaRamp(void) { RET(0, 3); }
static void v_GetGammaRamp(void) { if (A32(1)) { uint16_t *g = APTR(1); for (int c = 0; c < 3; c++) for (int i = 0; i < 256; i++) g[c * 256 + i] = (uint16_t)(i * 257); } RET(0, 2); }
static void v_SetClipPlane(void) { RET(D3D_OK, 3); }
static void v_SetClipStatus(void) { RET(D3D_OK, 2); }


const rr_method rr_dev_state_methods[] = {
    RR_DEV(QueryInterface, v_QueryInterface) RR_DEV(AddRef, v_AddRef) RR_DEV(Release, v_Release)
    RR_DEV(TestCooperativeLevel, v_TestCooperativeLevel) RR_DEV(GetAvailableTextureMem, v_GetAvailableTextureMem)
    RR_DEV(ResourceManagerDiscardBytes, v_ResourceManagerDiscardBytes) RR_DEV(GetDirect3D, v_GetDirect3D)
    RR_DEV(GetDeviceCaps, v_GetDeviceCaps) RR_DEV(GetDisplayMode, v_GetDisplayMode)
    RR_DEV(GetCreationParameters, v_GetCreationParameters) RR_DEV(ShowCursor, v_ShowCursor)
    RR_DEV(Reset, v_Reset) RR_DEV(Present, v_Present) RR_DEV(BeginScene, v_BeginScene) RR_DEV(EndScene, v_EndScene)
    RR_DEV(Clear, v_Clear) RR_DEV(SetTransform, v_SetTransform) RR_DEV(GetTransform, v_GetTransform)
    RR_DEV(MultiplyTransform, v_MultiplyTransform) RR_DEV(SetViewport, v_SetViewport) RR_DEV(GetViewport, v_GetViewport)
    RR_DEV(SetMaterial, v_SetMaterial) RR_DEV(GetMaterial, v_GetMaterial) RR_DEV(SetLight, v_SetLight)
    RR_DEV(GetLight, v_GetLight) RR_DEV(LightEnable, v_LightEnable) RR_DEV(GetLightEnable, v_GetLightEnable)
    RR_DEV(SetRenderState, v_SetRenderState) RR_DEV(GetRenderState, v_GetRenderState)
    RR_DEV(SetTextureStageState, v_SetTextureStageState) RR_DEV(GetTextureStageState, v_GetTextureStageState)
    RR_DEV(ValidateDevice, v_ValidateDevice) RR_DEV(GetInfo, v_GetInfo) RR_DEV(SetGammaRamp, v_SetGammaRamp)
    RR_DEV(GetGammaRamp, v_GetGammaRamp) RR_DEV(SetClipPlane, v_SetClipPlane) RR_DEV(SetClipStatus, v_SetClipStatus)
    { 0, 0 }
};

const rr_method *rr_device_method_table(void) {
    static rr_method all[IDIRECT3DDEVICE8_SLOTS + 1];
    static int built;
    if (!built) {
        int n = 0;
        const rr_method *parts[] = { rr_dev_state_methods, rr_dev_resource_methods, rr_dev_draw_methods, rr_dev_sb_methods };
        for (int p = 0; p < 4; p++)
            for (const rr_method *m = parts[p]; m->fn; m++) all[n++] = *m;
        all[n].fn = 0;
        built = 1;
    }
    return all;
}
