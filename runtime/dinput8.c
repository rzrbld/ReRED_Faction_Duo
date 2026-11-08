/*
 * dinput8.c - DirectInput 8: keyboard, mouse, and the handheld's controller
 * as a joystick (and a DX7 DirectInputCreateEx that lists no joysticks).
 *
 * The controller is PortMaster's SDL game controller (its SDL_GAMECONTROLLERCONFIG
 * mapping), presented the way Windows presents an Xbox 360 pad to DirectInput
 * games ("Controller (XBOX 360 For Windows)"): X/Y left stick, Rx/Ry right
 * stick, Z = both triggers on one axis (LT up, RT down), buttons A B X Y LB RB
 * Back Start LS RS, one POV hat for the d-pad. The game's own data format
 * (SetDataFormat) decides where each object lands in its state buffer; axis
 * ranges and dead zones follow SetProperty. gptokeyb then runs without a key
 * map (exit hotkey only), so nothing arrives twice.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "com.h"
#include "dinput_slots.h"
#include "input.h"
#include "d3d8_internal.h"
#define APTR_VA(va) ((void *)ADDR(va))

RR_DEFINE_IFACE(DI8, IDirectInput8A, IDIRECTINPUT8A)
RR_DEFINE_IFACE(DID8, IDirectInputDevice8A, IDIRECTINPUTDEVICE8A)
RR_DEFINE_IFACE(DI7, IDirectInput7A, IDIRECTINPUT7A)

#define DI_OK 0u
#define DIERR_NOTACQUIRED 0x8007000Cu
#define DIERR_DEVICENOTREG 0x80040154u
#define DI_BUFFEROVERFLOW 1u

enum { DEV_KEYBOARD = 1, DEV_MOUSE = 2, DEV_JOY = 3 };
/* joystick objects: 0-4 axes X Y Z Rx Ry, 5 POV, 6-15 buttons */
#define JOY_NOBJ 16
typedef struct { int32_t min, max; uint32_t dead, sat; } joy_axis;
typedef struct { int type; int acquired; uint32_t buffer_size; uint32_t obj;
                 uint32_t data_size; int nfmt; struct { uint32_t ofs; int o; } fmt[64];
                 joy_axis ax[5]; } didev;
#include "input.h"

/* {6F1D2B61-D5A0-11CF-BFC7-444553540000} keyboard, ...60... mouse */
static int guid_is(const uint8_t *g, uint32_t first) {
    static const uint8_t tail[12] = { 0xA0, 0xD5, 0xCF, 0x11, 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00 };
    uint32_t d1; memcpy(&d1, g, 4);
    return d1 == first && !memcmp(g + 4, tail, 12);
}

static void di_QueryInterface(void) { if (A32(2)) MEM32(A32(2)) = A32(0); rr_com_addref(A32(0)); RET(DI_OK, 3); }
static void di_AddRef(void) { RET(rr_com_addref(A32(0)), 1); }
static void di_Release(void) { RET(rr_com_release(A32(0)), 1); }

static void di_CreateDevice(void) {                  /* (this, REFGUID, device**, outer) */
    const uint8_t *g = APTR(1);
    int type = guid_is(g, 0x6F1D2B61u) ? DEV_KEYBOARD : guid_is(g, 0x6F1D2B60u) ? DEV_MOUSE : guid_is(g, 0x6F1D2B70u) ? DEV_JOY : 0;
    if (!type) { fprintf(stderr, "[dinput] CreateDevice: unknown device GUID\n"); RET(DIERR_DEVICENOTREG, 4); }
    didev *d = calloc(1, sizeof *d);
    d->type = type;
    for (int i = 0; i < 5; i++) { d->ax[i].min = 0; d->ax[i].max = 65535; d->ax[i].sat = 10000; }
    d->obj = rr_com_new(&DID8_iface, d);
    MEM32(A32(2)) = d->obj;
    rr_input_init();
    fprintf(stderr, "[dinput] CreateDevice(%s)\n", type == DEV_KEYBOARD ? "keyboard" : type == DEV_MOUSE ? "mouse" : "joystick: Xbox 360 layout");
    RET(DI_OK, 4);
}
/* ---- the controller as an Xbox 360 DirectInput device ---- */
static const uint8_t k_tail_axis[12] = { 0xF3, 0xC9, 0xCF, 0x11, 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00 };
static void guid_put(uint32_t va, uint32_t d1, const uint8_t tail[12]) { MEM32(va) = d1; memcpy(APTR_VA(va + 4), tail, 12); }
static const uint8_t k_tail_dev[12] = { 0xA0, 0xD5, 0xCF, 0x11, 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00 };
static const uint8_t k_tail_pidvid[12] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x50, 0x49, 0x44, 0x56, 0x49, 0x44 };
/* object o: guid first dword, DIDFT type bits, instance, default DIJOYSTATE offset, name */
static const struct { uint32_t guid; uint32_t type; uint32_t inst; uint32_t ofs; const char *name; } k_obj[JOY_NOBJ] = {
    { 0xA36D02E0u, 2, 0, 0, "X Axis" }, { 0xA36D02E1u, 2, 1, 4, "Y Axis" }, { 0xA36D02E2u, 2, 2, 8, "Z Axis" },
    { 0xA36D02F4u, 2, 3, 12, "X Rotation" }, { 0xA36D02F5u, 2, 4, 16, "Y Rotation" },
    { 0xA36D02F2u, 0x10, 0, 32, "Hat Switch" },
    { 0xA36D02F0u, 4, 0, 48, "Button 0" }, { 0xA36D02F0u, 4, 1, 49, "Button 1" }, { 0xA36D02F0u, 4, 2, 50, "Button 2" },
    { 0xA36D02F0u, 4, 3, 51, "Button 3" }, { 0xA36D02F0u, 4, 4, 52, "Button 4" }, { 0xA36D02F0u, 4, 5, 53, "Button 5" },
    { 0xA36D02F0u, 4, 6, 54, "Button 6" }, { 0xA36D02F0u, 4, 7, 55, "Button 7" }, { 0xA36D02F0u, 4, 8, 56, "Button 8" },
    { 0xA36D02F0u, 4, 9, 57, "Button 9" },
};
#define DEVTYPE_X360 0x00010215u   /* DI8DEVTYPE_GAMEPAD | STANDARD subtype | DIDEVTYPE_HID */
static void fill_devinst(uint32_t va, uint32_t size, int joy, int mouse) {   /* DIDEVICEINSTANCEA */
    memset(APTR_VA(va), 0, size);
    MEM32(va) = size;
    guid_put(va + 4, joy ? 0x6F1D2B70u : mouse ? 0x6F1D2B60u : 0x6F1D2B61u, k_tail_dev);
    if (joy) guid_put(va + 20, 0x028E045Eu, k_tail_pidvid); else guid_put(va + 20, mouse ? 0x6F1D2B60u : 0x6F1D2B61u, k_tail_dev);
    MEM32(va + 36) = joy ? DEVTYPE_X360 : mouse ? 0x12u : 0x13u;
    const char *nm = joy ? "Controller (XBOX 360 For Windows)" : mouse ? "Mouse" : "Keyboard";
    if (size >= 300) strcpy((char *)APTR_VA(va + 40), nm);
    if (size >= 560) strcpy((char *)APTR_VA(va + 300), nm);
}
static void di_EnumDevices(void) {                   /* (this, devtype/class, cb, ref, flags) */
    uint32_t t = A32(1) & 0xFF, cb = A32(2), ref = A32(3);
    uint32_t inst = hle_va(hle_alloc(580));
    int want_kb = t == 0 || t == 3 || t == 0x13, want_ms = t == 0 || t == 2 || t == 0x12;
    int want_joy = (t == 0 || t == 4 || t == 0x14 || t == 0x15) && rr_input_has_joy();
    uint32_t args[2] = { inst, ref };
    int go = 1;
    if (go && want_joy) { fill_devinst(inst, 580, 1, 0); go = hle_call_guest(cb, 2, args) != 0; }
    if (go && want_ms) { fill_devinst(inst, 580, 0, 1); go = hle_call_guest(cb, 2, args) != 0; }
    if (go && want_kb) { fill_devinst(inst, 580, 0, 0); go = hle_call_guest(cb, 2, args) != 0; }
    hle_free(APTR_VA(inst));
    static int once; if (!once++) fprintf(stderr, "[dinput] EnumDevices(type %u): controller %s\n", t, want_joy ? "listed" : "not listed");
    RET(DI_OK, 5);
}
static void di_GetDeviceStatus(void) { const uint8_t *g = APTR(1); RET(guid_is(g, 0x6F1D2B61u) || guid_is(g, 0x6F1D2B60u) || (guid_is(g, 0x6F1D2B70u) && rr_input_has_joy()) ? DI_OK : 1u, 2); }
static void di_Initialize(void) { RET(DI_OK, 3); }

static didev *dev_of(uint32_t o) { return rr_com_host(o); }
static void dd_Acquire(void)   { dev_of(A32(0))->acquired = 1; RET(DI_OK, 1); }
static void dd_Unacquire(void) { didev *d = dev_of(A32(0)); d->acquired = 0; if (d->type == DEV_KEYBOARD) rr_input_flush(); RET(DI_OK, 1); }
/* SetDataFormat(this, DIDATAFORMAT*): { dwSize, dwObjSize, dwFlags, dwDataSize,
 * dwNumObjs, rgodf } with DIOBJECTDATAFORMAT { pguid, dwOfs, dwType, dwFlags }.
 * Each entry gets one of our objects: by GUID, type and instance, or the next
 * free one of its kind for "any instance". */
static void dd_SetDataFormat(void) {
    didev *d = dev_of(A32(0));
    uint32_t f = A32(1);
    if (d->type != DEV_JOY || !f) RET(DI_OK, 2);
    d->data_size = MEM32(f + 12);
    uint32_t n = MEM32(f + 16), objs = MEM32(f + 20), osz = MEM32(f + 4) ? MEM32(f + 4) : 16;
    int used[JOY_NOBJ] = {0};
    d->nfmt = 0;
    for (uint32_t i = 0; i < n && d->nfmt < 64; i++) {
        uint32_t e = objs + i * osz, pg = MEM32(e), ofs = MEM32(e + 4), ty = MEM32(e + 8);
        uint32_t g1 = pg ? MEM32(pg) : 0, inst = (ty >> 8) & 0xFFFF;
        int any = inst == 0xFFFF, kind = ty & 0x1F;   /* 1/2/3 axis, 4/8/12 button, 0x10 pov */
        int pick = -1;
        for (int o = 0; o < JOY_NOBJ && pick < 0; o++) {
            if (used[o]) continue;
            int okind = (k_obj[o].type & 3) && (kind & 3) ? 1 : (k_obj[o].type & 0xC) && (kind & 0xC) ? 1 : (k_obj[o].type & 0x10) && (kind & 0x10) ? 1 : 0;
            if (kind && !okind) continue;
            if (g1 && g1 != k_obj[o].guid) continue;
            if (!any && k_obj[o].inst != inst && !g1) continue;
            pick = o;
        }
        fprintf(stderr, "[dinput]   fmt %u: guid %08X ofs %u type %08X -> %s\n", i, g1, ofs, ty, pick >= 0 ? k_obj[pick].name : "(none)");
        if (pick >= 0) { used[pick] = 1; d->fmt[d->nfmt].ofs = ofs; d->fmt[d->nfmt].o = pick; d->nfmt++; }
    }
    fprintf(stderr, "[dinput] joystick data format: %u bytes, %u objects, %d mapped\n", d->data_size, n, d->nfmt);
    RET(DI_OK, 2);
}
static int obj_by_ofs(didev *d, uint32_t ofs) {
    if (d->nfmt) { for (int i = 0; i < d->nfmt; i++) if (d->fmt[i].ofs == ofs) return d->fmt[i].o; return -1; }
    for (int o = 0; o < JOY_NOBJ; o++) if (k_obj[o].ofs == ofs) return o;
    return -1;
}
static uint32_t obj_ofs(didev *d, int o) {
    for (int i = 0; i < d->nfmt; i++) if (d->fmt[i].o == o) return d->fmt[i].ofs;
    return k_obj[o].ofs;
}
static uint32_t obj_typeid(int o) { return k_obj[o].type | (k_obj[o].inst << 8); }
/* objects a DIPROPHEADER { dwSize, dwHeaderSize, dwObj, dwHow } names: bit per object */
static uint32_t prop_objs(didev *d, uint32_t h) {
    uint32_t obj = MEM32(h + 8), how = MEM32(h + 12);
    if (how == 0) return 0x1F;                                   /* DIPH_DEVICE: the axes */
    if (how == 1) { int o = obj_by_ofs(d, obj); return o >= 0 ? 1u << o : 0; }
    if (how == 2) { for (int o = 0; o < JOY_NOBJ; o++) if ((obj & 0xFFFF1F) == (obj_typeid(o) & 0xFFFF1F) || ((obj & 0xFFFF00) == (obj_typeid(o) & 0xFFFF00) && (obj & k_obj[o].type))) return 1u << o; }
    return 0;
}
static int32_t axis_value(const joy_axis *a, float v) {          /* v in -1..1 */
    float dz = a->dead / 10000.f, sat = a->sat ? a->sat / 10000.f : 1.f, m = v < 0 ? -v : v;
    m = m <= dz ? 0.f : m >= sat ? 1.f : (m - dz) / (sat - dz);
    v = v < 0 ? -m : m;
    double mid = ((double)a->min + (double)a->max) / 2.0, half = ((double)a->max - (double)a->min) / 2.0;
    return (int32_t)(mid + v * half + (v >= 0 ? 0.5 : -0.5));
}
static void joy_state(didev *d, uint8_t *p, uint32_t n) {
    rr_joy j; memset(&j, 0, sizeof j);
    int have = rr_input_joy(&j);
    float av[5] = { j.lx / 32767.f, j.ly / 32767.f, (j.lt - j.rt) / 32767.f, j.rx / 32767.f, j.ry / 32767.f };
    for (int o = 0; o < JOY_NOBJ; o++) {
        uint32_t ofs = obj_ofs(d, o);
        if (d->nfmt) { int mapped = 0; for (int i = 0; i < d->nfmt; i++) if (d->fmt[i].o == o) mapped = 1; if (!mapped) continue; }
        if (o < 5) { if (ofs + 4 <= n) { int32_t v = axis_value(&d->ax[o], have ? (av[o] > 1 ? 1 : av[o] < -1 ? -1 : av[o]) : 0.f); memcpy(p + ofs, &v, 4); } }
        else if (o == 5) {
            if (ofs + 4 <= n) {
                uint32_t pov = 0xFFFFFFFFu;
                if (have) {
                    int x = j.right - j.left, y = j.down - j.up;   /* hundredths of degrees, clockwise from up */
                    if (x || y) { static const uint32_t ang[3][3] = { { 31500, 0, 4500 }, { 27000, 0xFFFFFFFFu, 9000 }, { 22500, 18000, 13500 } }; pov = ang[y + 1][x + 1]; }
                }
                memcpy(p + ofs, &pov, 4);
            }
        } else if (ofs < n) p[ofs] = have && (j.buttons >> (o - 6) & 1) ? 0x80 : 0;
    }
}
static void dd_SetCooperativeLevel(void) { RET(DI_OK, 3); }
static void dd_SetEventNotification(void) { RET(DI_OK, 2); }
static void dd_Poll(void) { RET(DI_OK, 1); }
static void dd_SetProperty(void) {                   /* (this, REFGUID prop, DIPROPHEADER*) */
    didev *d = dev_of(A32(0));
    uint32_t prop = A32(1), h = A32(2);
    if (prop == 1 /* DIPROP_BUFFERSIZE */) d->buffer_size = MEM32(h + 16);
    if (d->type == DEV_JOY && (prop == 4 || prop == 5 || prop == 6)) {   /* RANGE, DEADZONE, SATURATION */
        uint32_t m = prop_objs(d, h);
        for (int o = 0; o < 5; o++) if (m & (1u << o)) {
            if (prop == 4) { d->ax[o].min = (int32_t)MEM32(h + 16); d->ax[o].max = (int32_t)MEM32(h + 20); }
            else if (prop == 5) d->ax[o].dead = MEM32(h + 16);
            else d->ax[o].sat = MEM32(h + 16);
        }
    }
    RET(DI_OK, 3);
}
static void dd_GetProperty(void) {                   /* (this, REFGUID prop, DIPROPHEADER*) */
    didev *d = dev_of(A32(0));
    uint32_t prop = A32(1), h = A32(2);
    if (prop == 1) { MEM32(h + 16) = d->buffer_size; RET(DI_OK, 3); }
    if (d->type == DEV_JOY && (prop == 4 || prop == 5 || prop == 6)) {
        uint32_t m = prop_objs(d, h);
        for (int o = 0; o < 5; o++) if (m & (1u << o)) {
            if (prop == 4) { MEM32(h + 16) = (uint32_t)d->ax[o].min; MEM32(h + 20) = (uint32_t)d->ax[o].max; }
            else MEM32(h + 16) = prop == 5 ? d->ax[o].dead : d->ax[o].sat;
            RET(DI_OK, 3);
        }
    }
    RET(0x80070002u /* DIERR_UNSUPPORTED-ish: E_NOTIMPL family */, 3);
}
/* DIDEVICEOBJECTINSTANCEA: dwSize, guidType, dwOfs, dwType, dwFlags, tszName[260], ... */
static void fill_objinst(didev *d, uint32_t va, uint32_t size, int o) {
    memset(APTR_VA(va), 0, size);
    MEM32(va) = size;
    guid_put(va + 4, k_obj[o].guid, k_tail_axis);
    MEM32(va + 20) = obj_ofs(d, o);
    MEM32(va + 24) = obj_typeid(o);
    MEM32(va + 28) = o < 5 ? 0x100u /* DIDOI_ASPECTPOSITION */ : 0;
    if (size >= 32 + 260) strcpy((char *)APTR_VA(va + 32), k_obj[o].name);
}
static void dd_EnumObjects(void) {                   /* (this, cb, ref, flags) */
    didev *d = dev_of(A32(0));
    uint32_t cb = A32(1), ref = A32(2), fl = A32(3);
    if (d->type != DEV_JOY) RET(DI_OK, 4);
    uint32_t va = hle_va(hle_alloc(316));
    for (int o = 0; o < JOY_NOBJ; o++) {
        if (fl && !(fl & k_obj[o].type) && !((fl & 3) && (k_obj[o].type & 3))) continue;
        fill_objinst(d, va, 316, o);
        uint32_t args[2] = { va, ref };
        if (!hle_call_guest(cb, 2, args)) break;
    }
    hle_free(APTR_VA(va));
    RET(DI_OK, 4);
}
static void dd_GetObjectInfo(void) {                 /* (this, DIDEVICEOBJECTINSTANCEA*, obj, how) */
    didev *d = dev_of(A32(0));
    if (d->type != DEV_JOY) RET(0x80070057u, 4);
    uint32_t hdr[4] = { 0, 0, A32(2), A32(3) };
    uint32_t tmp = hle_va(hle_alloc(16)); memcpy(APTR_VA(tmp), hdr, 16);
    uint32_t m = prop_objs(d, tmp); hle_free(APTR_VA(tmp));
    for (int o = 0; o < JOY_NOBJ; o++) if (m & (1u << o)) { fill_objinst(d, A32(1), MEM32(A32(1)) ? MEM32(A32(1)) : 316, o); RET(DI_OK, 4); }
    RET(0x80070002u, 4);
}
static void dd_GetDeviceInfo(void) {                 /* (this, DIDEVICEINSTANCEA*) */
    didev *d = dev_of(A32(0));
    uint32_t sz = MEM32(A32(1)); if (!sz) sz = 580;
    fill_devinst(A32(1), sz, d->type == DEV_JOY, d->type == DEV_MOUSE);
    RET(DI_OK, 2);
}
static void dd_GetCapabilities(void) {               /* (this, DIDEVCAPS*) */
    didev *d = dev_of(A32(0));
    uint32_t c = A32(1);
    MEM32(c + 4) = 1;                                /* DIDC_ATTACHED */
    MEM32(c + 8) = d->type == DEV_KEYBOARD ? 0x13 : d->type == DEV_MOUSE ? 0x12 : DEVTYPE_X360;
    MEM32(c + 12) = d->type == DEV_MOUSE ? 3 : d->type == DEV_JOY ? 5 : 0;      /* axes */
    MEM32(c + 16) = d->type == DEV_MOUSE ? 4 : d->type == DEV_JOY ? 10 : 128;   /* buttons */
    MEM32(c + 20) = d->type == DEV_JOY ? 1 : 0;                                 /* POVs */
    RET(DI_OK, 2);
}
static void dd_GetDeviceState(void) {                /* (this, size, data*) */
    didev *d = dev_of(A32(0));
    uint32_t n = A32(1);
    rr_input_pump();
    if (d->type == DEV_JOY) {
        uint8_t *p = APTR(2);
        memset(p, 0, n);
        joy_state(d, p, n);
    } else if (d->type == DEV_KEYBOARD) {
        uint8_t k[256]; rr_input_keys(k);
        memcpy(APTR(2), k, n < 256 ? n : 256);
    } else {
        int32_t dx, dy, dz; uint8_t b;
        rr_input_mouse(&dx, &dy, &dz, &b);
        uint8_t *p = APTR(2);
        memset(p, 0, n);
        if (n >= 12) { memcpy(p, &dx, 4); memcpy(p + 4, &dy, 4); memcpy(p + 8, &dz, 4); }
        for (uint32_t i = 0; i < 4 && 12 + i < n; i++) p[12 + i] = (b >> i) & 1 ? 0x80 : 0;
    }
    RET(DI_OK, 3);
}
static void dd_GetDeviceData(void) {                 /* (this, cbObjectData, rgdod*, inout count*, flags) */
    didev *d = dev_of(A32(0));
    uint32_t sz = A32(1), out = A32(2), cnt = MEM32(A32(3)), n = 0;
    rr_input_pump();
    if (d->type == DEV_KEYBOARD) {
        rr_key_event ev;
        int peek = (A32(4) & 1) != 0;                /* DIGDD_PEEK: not consumed */
        while (n < cnt || (!out && n < 1024)) {
            if (peek) break;
            if (!rr_input_next_key(&ev)) break;
            if (out) {
                uint32_t r = out + n * sz;
                memset(hle_ptr(r), 0, sz);
                MEM32(r) = ev.dik; MEM32(r + 4) = ev.down ? 0x80 : 0; MEM32(r + 8) = ev.time; MEM32(r + 12) = ev.seq;
            }
            n++;
        }
    }
    MEM32(A32(3)) = n;
    RET(DI_OK, 5);
}
static void dd_Release(void) {
    uint32_t n = rr_com_release(A32(0));
    RET(n, 1);
}

static void s_DirectInput8Create(void) {             /* (hinst, version, riid, out**, outer) */
    static uint32_t obj;
    if (!obj) {
        static const rr_method di[] = {
            { IDIRECTINPUT8A_QueryInterface, di_QueryInterface }, { IDIRECTINPUT8A_AddRef, di_AddRef },
            { IDIRECTINPUT8A_Release, di_Release }, { IDIRECTINPUT8A_CreateDevice, di_CreateDevice },
            { IDIRECTINPUT8A_EnumDevices, di_EnumDevices }, { IDIRECTINPUT8A_GetDeviceStatus, di_GetDeviceStatus },
            { IDIRECTINPUT8A_Initialize, di_Initialize }, { 0, 0 } };
        static const rr_method dd[] = {
            { IDIRECTINPUTDEVICE8A_QueryInterface, di_QueryInterface }, { IDIRECTINPUTDEVICE8A_AddRef, di_AddRef },
            { IDIRECTINPUTDEVICE8A_Release, dd_Release }, { IDIRECTINPUTDEVICE8A_GetCapabilities, dd_GetCapabilities },
            { IDIRECTINPUTDEVICE8A_SetProperty, dd_SetProperty }, { IDIRECTINPUTDEVICE8A_Acquire, dd_Acquire },
            { IDIRECTINPUTDEVICE8A_Unacquire, dd_Unacquire }, { IDIRECTINPUTDEVICE8A_GetDeviceState, dd_GetDeviceState },
            { IDIRECTINPUTDEVICE8A_GetDeviceData, dd_GetDeviceData }, { IDIRECTINPUTDEVICE8A_SetDataFormat, dd_SetDataFormat },
            { IDIRECTINPUTDEVICE8A_SetEventNotification, dd_SetEventNotification },
            { IDIRECTINPUTDEVICE8A_SetCooperativeLevel, dd_SetCooperativeLevel }, { IDIRECTINPUTDEVICE8A_Poll, dd_Poll },
            { IDIRECTINPUTDEVICE8A_GetProperty, dd_GetProperty }, { IDIRECTINPUTDEVICE8A_EnumObjects, dd_EnumObjects },
            { IDIRECTINPUTDEVICE8A_GetObjectInfo, dd_GetObjectInfo }, { IDIRECTINPUTDEVICE8A_GetDeviceInfo, dd_GetDeviceInfo },
            { 0, 0 } };
        rr_iface_vtable(&DI8_iface, di);
        rr_iface_vtable(&DID8_iface, dd);
        obj = rr_com_new(&DI8_iface, NULL);
    } else rr_com_addref(obj);
    MEM32(A32(3)) = obj;
    RET(DI_OK, 5);
}

/* DirectInputCreateEx (DX7): an interface whose device enumeration finds nothing. */
static void s_DirectInputCreateEx(void) {            /* (hinst, version, riid, out**, outer) */
    static uint32_t obj;
    if (!obj) {
        static const rr_method di[] = {
            { IDIRECTINPUT7A_QueryInterface, di_QueryInterface }, { IDIRECTINPUT7A_AddRef, di_AddRef },
            { IDIRECTINPUT7A_Release, di_Release }, { IDIRECTINPUT7A_EnumDevices, di_EnumDevices },
            { IDIRECTINPUT7A_Initialize, di_Initialize }, { 0, 0 } };
        rr_iface_vtable(&DI7_iface, di);
        obj = rr_com_new(&DI7_iface, NULL);
    } else rr_com_addref(obj);
    MEM32(A32(3)) = obj;
    RET(DI_OK, 5);
}

static const win32hle_shim g_dinput[] = {
    { "dinput8.dll!DirectInput8Create", s_DirectInput8Create },
    { "dinput.dll!DirectInputCreateEx", s_DirectInputCreateEx },
    { 0, 0 }
};
void rr_register_dinput(void) { win32hle_register(g_dinput); }
