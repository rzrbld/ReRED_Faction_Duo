/* com.c - see com.h. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "com.h"

uint32_t rr_iface_vtable(rr_iface *it, const rr_method *impl) {
    if (it->vtable) return it->vtable;
    win32hle_shim *m = calloc((size_t)it->nslots + 1, sizeof *m);
    for (int i = 0; i < it->nslots; i++) {
        char *nm = malloc(strlen(it->iface) + strlen(it->names[i]) + 3);
        sprintf(nm, "%s::%s", it->iface, it->names[i]);
        m[i].name = nm;
        m[i].fn = it->stubs[i];
    }
    for (; impl && impl->fn; impl++) {
        if (impl->slot < 0 || impl->slot >= it->nslots) { fprintf(stderr, "[com] %s: bad slot %d\n", it->iface, impl->slot); abort(); }
        m[impl->slot].fn = impl->fn;
    }
    it->vtable = hle_com_vtable(m);     /* registers the shims and writes the VA array */
    return it->vtable;
}

uint32_t rr_com_new(rr_iface *it, void *host) {
    if (!it->vtable) rr_iface_vtable(it, NULL);
    uint32_t o = hle_com_new(it->vtable, 16);
    uint64_t h = (uint64_t)(uintptr_t)host;
    memcpy(hle_ptr(o + 8), &h, 8);      /* host pointers stay out of guest-visible fields */
    return o;
}

void *rr_com_host(uint32_t obj) {
    uint64_t h;
    memcpy(&h, hle_ptr(obj + 8), 8);
    return (void *)(uintptr_t)h;
}
uint32_t rr_com_addref(uint32_t obj)  { return ++MEM32(obj + 4); }
uint32_t rr_com_release(uint32_t obj) { uint32_t n = MEM32(obj + 4); if (n) MEM32(obj + 4) = --n; return n; }

void rr_com_unimpl(rr_iface *it, int slot) {
    static unsigned char seen[64][128];
    uintptr_t k = ((uintptr_t)it >> 4) & 63;
    if (slot < 128 && !seen[k][slot]) {
        seen[k][slot] = 1;
        fprintf(stderr, "[com] %s::%s unimplemented (returning S_OK)\n", it->iface, it->names[slot]);
    }
    RET(S_OK, it->argc[slot]);
}
