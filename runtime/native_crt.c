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
