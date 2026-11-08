/*
 * shims_misc.c - the few Win32 calls the game's modules import that win32hle
 * does not answer (see analysis/imports.txt and build/hle_names.txt).
 * Each is stdcall; RET(eax, argc) pops the arguments.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL2/SDL.h>
#include "win32hle.h"
#include "rered.h"
#include <poll.h>
#include <sys/socket.h>

/* --- kernel32 --- */
static void k_DisableThreadLibraryCalls(void) { RET(1, 1); }

/* --- user32 --- */
static void u_SetClassLongA(void)  { RET(0, 3); }          /* (hwnd, index, value): previous value */
static void u_GetQueueStatus(void) { RET(0, 1); }          /* nothing queued: the engine then pumps anyway */
static void u_IsChild(void)        { RET(0, 2); }
static void u_VkKeyScanA(void) {                           /* (ch) -> low byte VK, high byte shift state */
    uint32_t c = A32(0) & 0xFF;
    uint32_t r;
    if (c >= 'a' && c <= 'z') r = c - 32;
    else if (c >= 'A' && c <= 'Z') r = c | 0x100;
    else if (c >= '0' && c <= '9') r = c;
    else if (c == ' ') r = 0x20;
    else r = 0xFFFF;
    RET(r, 1);
}

/* --- ole32 task allocator: guest heap --- */
static void o_CoTaskMemAlloc(void) { RETP(hle_heap_alloc(A32(0), 0), 1); }
static void o_CoTaskMemFree(void)  { if (A32(0)) hle_heap_free(APTR(0)); RET(0, 1); }
static void o_CoFreeUnusedLibraries(void) { RET(0, 0); }

/* --- binkw32: exports win32hle's Bink layer names differently --- */
static void b_SetVolume12(void) {                         /* (bink, trackid, volume 0..32768) */
    { static int n; if (n++ < 4) fprintf(stderr, "[bink] SetVolume track %u volume %u\n", A32(1), A32(2)); }
    uint32_t va = hle_resolve("_BinkSetVolume@8");
    uint32_t a[2] = { A32(0), A32(2) };
    if (va) hle_call_guest(va, 2, a);
    RET(0, 3);
}
static void b_CopyToBufferRect(void) {
    /* (bink, dest, pitch, destheight, destx, desty, srcx, srcy, srcw, srch, flags).
     * win32hle copies whole frames; the engine asks for the full frame at the
     * origin, so the rect form forwards when it covers the frame. */
    uint32_t va = hle_resolve("_BinkCopyToBuffer@28");
    if (A32(6) || A32(7))
        fprintf(stderr, "[bink] CopyToBufferRect with source offset %u,%u copies the whole frame\n", A32(6), A32(7));
    uint32_t a[7] = { A32(0), A32(1), A32(2), A32(3), A32(4), A32(5), A32(10) };
    uint32_t r = va ? hle_call_guest(va, 7, a) : 0;
    RET(r, 11);
}


/* --- Red Faction II additions --- */
static void k_SearchPathA(void) {       /* (path, file, ext, buflen, buf, filepart*) -> length or 0 */
    const char *dir = A32(0) ? (const char *)APTR(0) : NULL, *file = APTR(1), *ext = A32(2) ? (const char *)APTR(2) : "";
    char guest[600], host[1024];
    const char *base = strrchr(file, '\\'); base = base ? base + 1 : file;
    if (dir && dir[0]) snprintf(guest, sizeof guest, "%s\\%s%s", dir, file, strchr(base, '.') ? "" : ext);
    else snprintf(guest, sizeof guest, "%s%s", file, strchr(base, '.') ? "" : ext);
    if (!hle_host_path(guest, host, sizeof host)) { RET(0, 6); return; }
    if (!strchr(guest, ':') && guest[0] != '\\') {   /* make it absolute: relative to C:\ (the game directory) */
        char tmp[600]; snprintf(tmp, sizeof tmp, "C:\\%s", guest); strcpy(guest, tmp);
    }
    uint32_t len = (uint32_t)strlen(guest);
    if (len + 1 > A32(3)) { RET(len + 1, 6); return; }
    memcpy(APTR(4), guest, len + 1);
    if (A32(5)) { char *b = strrchr((char *)APTR(4), '\\'); MEM32(A32(5)) = A32(4) + (uint32_t)(b ? b + 1 - (char *)APTR(4) : 0); }
    RET(len, 6);
}
static void k_GetDiskFreeSpaceExA(void) {   /* (dir, avail*, total*, free*): report 4 GB free of 8 GB */
    const uint64_t fr = 4ull << 30, tot = 8ull << 30;
    if (A32(1)) memcpy(APTR(1), &fr, 8);
    if (A32(2)) memcpy(APTR(2), &tot, 8);
    if (A32(3)) memcpy(APTR(3), &fr, 8);
    RET(1, 4);
}
static void u_SystemParametersInfoA(void) {   /* (action, uiParam, pvParam, winIni) */
    uint32_t act = A32(0), pv = A32(2);
    if (act == 0x30 && pv) {                   /* SPI_GETWORKAREA: the whole screen */
        MEM32(pv) = 0; MEM32(pv + 4) = 0; MEM32(pv + 8) = 640; MEM32(pv + 12) = 480;
    } else if (pv && (act == 0x10 || act == 0x3A || act == 0x34 || act == 0x0E)) {
        MEM32(pv) = 0;                         /* screensaver / sticky-, toggle-, filter-keys state: off */
    }
    RET(1, 4);
}
static void u_EnumWindows(void) { RET(1, 2); }   /* no other top-level windows (single-instance check) */
/* wsock32: the game is offline here, but the network layer polls sockets. */
static void w_WSACancelBlockingCall(void) { RET(0, 0); }
static void w_shutdown(void) { RET(shutdown((int)A32(0) - 0x1000, (int)A32(1)) ? 0xFFFFFFFFu : 0, 2); }
static void w_WSAFDIsSet(void) {              /* (SOCKET, fd_set*): { u_int count; SOCKET fds[64]; } */
    uint32_t set = A32(1), n = set ? MEM32(set) : 0, r = 0;
    for (uint32_t i = 0; i < n && i < 64; i++) if (MEM32(set + 4 + 4 * i) == A32(0)) r = 1;
    RET(r, 2);
}
static void w_select(void) {                  /* (nfds, read, write, except, timeval*) on poll() */
    struct pollfd pf[192]; uint32_t own[192]; int np = 0;
    uint32_t sets[3] = { A32(1), A32(2), A32(3) };
    short ev[3] = { POLLIN, POLLOUT, POLLPRI };
    for (int k = 0; k < 3; k++) {
        uint32_t n = sets[k] ? MEM32(sets[k]) : 0;
        for (uint32_t i = 0; i < n && i < 64 && np < 192; i++) {
            pf[np].fd = (int)MEM32(sets[k] + 4 + 4 * i) - 0x1000; pf[np].events = ev[k]; pf[np].revents = 0;
            own[np++] = (uint32_t)k;
        }
    }
    int ms = A32(4) ? (int)(MEM32(A32(4)) * 1000 + MEM32(A32(4) + 4) / 1000) : -1;
    int rc = poll(pf, (nfds_t)np, ms);
    if (rc < 0) { RET(0xFFFFFFFFu, 5); return; }
    int total = 0;
    for (int k = 0; k < 3; k++) if (sets[k]) MEM32(sets[k]) = 0;
    for (int j = 0; j < np; j++) {
        short hit = pf[j].revents & (ev[own[j]] | (own[j] == 0 ? (POLLHUP | POLLERR) : 0));
        if (!hit) continue;
        uint32_t s = sets[own[j]], c = MEM32(s);
        MEM32(s + 4 + 4 * c) = (uint32_t)pf[j].fd + 0x1000; MEM32(s) = c + 1; total++;
    }
    RET((uint32_t)total, 5);
}

static const win32hle_shim g_misc[] = {
    { "kernel32.dll!SearchPathA", k_SearchPathA }, { "kernel32.dll!GetDiskFreeSpaceExA", k_GetDiskFreeSpaceExA },
    { "user32.dll!SystemParametersInfoA", u_SystemParametersInfoA }, { "user32.dll!EnumWindows", u_EnumWindows },
    { "wsock32.dll#113", w_WSACancelBlockingCall }, { "wsock32.dll#22", w_shutdown },
    { "wsock32.dll#151", w_WSAFDIsSet }, { "wsock32.dll#18", w_select },
    /* the GOG build imports these by name */
    { "wsock32.dll!WSACancelBlockingCall", w_WSACancelBlockingCall }, { "wsock32.dll!shutdown", w_shutdown },
    { "wsock32.dll!__WSAFDIsSet", w_WSAFDIsSet }, { "wsock32.dll!select", w_select },
    { "kernel32.dll!DisableThreadLibraryCalls", k_DisableThreadLibraryCalls },
    { "user32.dll!SetClassLongA",   u_SetClassLongA },
    { "user32.dll!GetQueueStatus",  u_GetQueueStatus },
    { "user32.dll!IsChild",         u_IsChild },
    { "user32.dll!VkKeyScanA",      u_VkKeyScanA },
    { "ole32.dll!CoTaskMemAlloc",   o_CoTaskMemAlloc },
    { "ole32.dll!CoTaskMemFree",    o_CoTaskMemFree },
    { "ole32.dll!CoFreeUnusedLibraries", o_CoFreeUnusedLibraries },
    { "binkw32.dll!_BinkSetVolume@12", b_SetVolume12 },
    { "binkw32.dll!_BinkCopyToBufferRect@44", b_CopyToBufferRect },
    { 0, 0 }
};

void rr_register_misc(void) { win32hle_register(g_misc); }
