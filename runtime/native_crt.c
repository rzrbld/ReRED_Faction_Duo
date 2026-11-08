/*
 * native_crt.c - native replacements for MSVC runtime routines in rf2.exe
 * (wired by tools/native_hooks.py from config/native.json).
 *
 * The CRT's memcpy (0x5A8B40) and memmove (0x5B1E80) in the GOG rf2.exe are the same overlap-safe
 * routine: unrolled dword copies entered through jump tables into the middle
 * of their own bodies, which static recovery cannot follow (and which would be
 * slow translated). cdecl: [ret][dst][src][n] -> eax = dst; the caller pops
 * the arguments. ecx/edx are scratch. RERED_NATIVE=0 runs the translations.
 */
#include <stdlib.h>
#include <string.h>
#include "win32hle.h"

int rr_native_on = 1;
__attribute__((constructor)) static void native_env(void) {
    const char *e = getenv("RERED_NATIVE");
    if (e && !atoi(e)) rr_native_on = 0;
}

void rr_native_memmove(void) {
    uint32_t dst = MEM32(g_esp + 4), src = MEM32(g_esp + 8), n = MEM32(g_esp + 12);
    if (n) memmove((void *)ADDR(dst), (const void *)ADDR(src), n);
    g_eax = dst; g_ecx = 0; g_edx = 0;
    g_esp += 4;                                  /* ret */
}

/* ---- skinning helpers (GOG rf2.exe) ----
 * 0x553280, thiscall (ecx = v[3]; [esp+4] = out[3], [esp+8] = M[9]), ret 8:
 *   out[i] = (M[i+6]*v2 + M[i+3]*v1) + M[i]*v0, summed in this order as the
 *   x87 code does (in double, as the translation models the x87 stack),
 *   stored as float; eax = out. */
void rr_native_vec_rot(void) {
    uint32_t v = g_ecx, out = MEM32(g_esp + 4), m = MEM32(g_esp + 8);
    float vf[3], mf[9];
    memcpy(vf, (const void *)ADDR(v), 12); memcpy(mf, (const void *)ADDR(m), 36);
    float r[3];
    for (int i = 0; i < 3; i++) {
        double s = (double)mf[i + 6] * vf[2] + (double)mf[i + 3] * vf[1];
        s += (double)mf[i] * vf[0];
        r[i] = (float)s;
    }
    memcpy((void *)ADDR(out), r, 12);
    g_eax = out;
    g_esp += 4 + 8;                                       /* ret 8 */
}
/* 0x552B40, thiscall (ecx = src[9]; [esp+4] = dst), ret 4: copy 36 bytes;
 * eax = dst, edx = src[7], ecx = src[8] as the original leaves them. */
void rr_native_mat_copy(void) {
    uint32_t src = g_ecx, dst = MEM32(g_esp + 4);
    memmove((void *)ADDR(dst), (const void *)ADDR(src), 36);
    g_eax = dst; g_edx = MEM32(src + 0x1C); g_ecx = MEM32(src + 0x20);
    g_esp += 4 + 4;                                       /* ret 4 */
}
