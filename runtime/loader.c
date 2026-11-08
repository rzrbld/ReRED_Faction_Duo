/*
 * loader.c - maps the game's lifted modules into guest memory at the bases they
 * were translated for, applies relocations for the build-time-rebased DLLs,
 * binds every IAT, and runs the DLL entry points in dependency order.
 *
 * Import resolution, by importing DLL name:
 *   - a lifted module (rf2.exe; any further DLLs listed in config/modules.json): its export table in
 *     guest memory -> the lifted function's VA (found by the dispatch table);
 *   - anything else: the shim registry (win32hle + this port's shims). With
 *     RERED_PERMISSIVE=1 an unknown import binds to a stub that names itself
 *     when called; otherwise the boot fails and lists them.
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "win32hle.h"
#include "pe_format.h"
#include "pe_loader.h"
#include "loader.h"

static rr_module g_mods[RR_MAX_MODULES];
static int g_nmods;

static uint8_t *slurp(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc((size_t)n);
    if (b && fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); b = NULL; }
    fclose(f);
    *len = (size_t)n;
    return b;
}

rr_module *rr_find_module(const char *dll) {
    const char *slash = strrchr(dll, '\\');
    if (slash) dll = slash + 1;
    for (int i = 0; i < g_nmods; i++)
        if (!strcasecmp(g_mods[i].file, dll)) return &g_mods[i];
    return NULL;
}
int rr_module_count(void) { return g_nmods; }
rr_module *rr_module_at(int i) { return i >= 0 && i < g_nmods ? &g_mods[i] : NULL; }

int rr_map(const char *game_dir, const char *file, uint32_t base, int main_image) {
    char path[2048];
    snprintf(path, sizeof path, "%s/%s", game_dir, file);
    size_t flen;
    uint8_t *raw = slurp(path, &flen);
    if (!raw) { fprintf(stderr, "[load] cannot read %s\n", path); return -1; }
    pe_dos_header *dos = (pe_dos_header *)raw;
    pe_nt_headers32 *nt = (pe_nt_headers32 *)(raw + dos->e_lfanew);
    if (dos->e_magic != 0x5A4D || nt->Signature != 0x4550 || nt->FileHeader.Machine != 0x14C) {
        fprintf(stderr, "[load] %s is not a PE32 i386 image\n", path); free(raw); return -1;
    }
    pe_opt_header32 *opt = &nt->OptionalHeader;
    if (!base) base = opt->ImageBase;
    uint32_t span = (opt->SizeOfImage + 0xFFFFu) & ~0xFFFFu;
    uint32_t got = hle_vm_reserve(base, span);
    if (got != base) {
        fprintf(stderr, "[load] %s: cannot reserve 0x%08X+0x%X (got 0x%08X)\n", file, base, span, got);
        free(raw); return -1;
    }
    memcpy(hle_ptr(base), raw, opt->SizeOfHeaders);
    pe_section_header *sec = (pe_section_header *)((uint8_t *)opt + nt->FileHeader.SizeOfOptionalHeader);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++) {
        uint32_t n = sec[i].SizeOfRawData < sec[i].VirtualSize ? sec[i].SizeOfRawData : sec[i].VirtualSize;
        if (!sec[i].VirtualSize) n = sec[i].SizeOfRawData;
        if (n && (size_t)sec[i].PointerToRawData + n <= flen)
            memcpy(hle_ptr(base + sec[i].VirtualAddress), raw + sec[i].PointerToRawData, n);
    }
    if (base != opt->ImageBase) {
        pe_data_dir r = opt->DataDirectory[PE_DIR_BASERELOC];
        if (!r.Size) { fprintf(stderr, "[load] %s has no relocations\n", file); free(raw); return -1; }
        recomp_pe_relocate(base, r.VirtualAddress, r.Size, (int32_t)(base - opt->ImageBase));
    }
    rr_module *m = &g_mods[g_nmods++];
    memset(m, 0, sizeof *m);
    snprintf(m->file, sizeof m->file, "%s", file);
    m->base = base;
    m->span = span;
    m->entry = opt->AddressOfEntryPoint ? base + opt->AddressOfEntryPoint : 0;
    m->is_dll = (nt->FileHeader.Characteristics & 0x2000) != 0;
    m->import_rva = opt->DataDirectory[PE_DIR_IMPORT].VirtualAddress;
    m->export_rva = opt->DataDirectory[0 /* export dir */].VirtualAddress;
    m->export_size = opt->DataDirectory[0 /* export dir */].Size;
    char guest[1024];
    hle_guest_path(path, guest, sizeof guest);
    hle_module_add(guest, base, opt->DataDirectory[PE_DIR_RESOURCE].VirtualAddress,
                   opt->DataDirectory[PE_DIR_RESOURCE].Size, main_image);
    free(raw);
    fprintf(stderr, "[load] %-12s at 0x%08X span 0x%07X entry 0x%08X\n", file, base, span, m->entry);
    return 0;
}

/* Export lookup inside guest memory (by name or by ordinal). */
uint32_t rr_export(const rr_module *m, const char *name, uint32_t ordinal) {
    if (!m->export_rva) return 0;
    const uint32_t *d = (const uint32_t *)hle_ptr(m->base + m->export_rva);
    uint32_t ord_base = d[4], nfunc = d[5], nname = d[6];
    const uint32_t *funcs = (const uint32_t *)hle_ptr(m->base + d[7]);
    const uint32_t *names = (const uint32_t *)hle_ptr(m->base + d[8]);
    const uint16_t *ords = (const uint16_t *)hle_ptr(m->base + d[9]);
    uint32_t idx = UINT32_MAX;
    if (name) {
        for (uint32_t i = 0; i < nname; i++)
            if (!strcmp((const char *)hle_ptr(m->base + names[i]), name)) { idx = ords[i]; break; }
    } else if (ordinal >= ord_base) {
        idx = ordinal - ord_base;
    }
    if (idx >= nfunc || !funcs[idx]) return 0;
    uint32_t rva = funcs[idx];
    if (rva >= m->export_rva && rva < m->export_rva + m->export_size) return 0;   /* forwarder */
    return m->base + rva;
}

/* A shim registered as "dll!name" wins over a bare "name", so two DLLs with
 * the same export name (vorbisfile/vorbis, dinput/dinput8) never collide. */
static uint32_t resolve_shim(const char *dll, const char *name, int permissive) {
    char key[256], low[64];
    size_t k = 0;
    for (; dll[k] && k < sizeof low - 1; k++) low[k] = (char)tolower((unsigned char)dll[k]);
    low[k] = 0;
    snprintf(key, sizeof key, "%s!%s", low, name);
    uint32_t va = hle_resolve(key);
    if (!va) va = hle_resolve(name);
    if (!va && permissive) va = hle_resolve_or_stub(strdup(key));
    return va;
}

int rr_bind(rr_module *m, int permissive) {
    if (!m->import_rva) return 0;
    int bad = 0, n = 0;
    for (pe_import_descriptor *d = (pe_import_descriptor *)hle_ptr(m->base + m->import_rva); d->Name; d++) {
        const char *dll = (const char *)hle_ptr(m->base + d->Name);
        rr_module *dep = rr_find_module(dll);
        uint32_t *ilt = (uint32_t *)hle_ptr(m->base + (d->OriginalFirstThunk ? d->OriginalFirstThunk : d->FirstThunk));
        uint32_t *iat = (uint32_t *)hle_ptr(m->base + d->FirstThunk);
        for (; *ilt; ilt++, iat++, n++) {
            uint32_t va = 0;
            char ordname[32];
            const char *name;
            if (*ilt & PE_ORDINAL_FLAG) {
                snprintf(ordname, sizeof ordname, "#%u", *ilt & 0xFFFFu);
                name = ordname;
                va = dep ? rr_export(dep, NULL, *ilt & 0xFFFFu) : 0;
            } else {
                name = (const char *)hle_ptr(m->base + *ilt + 2);
                va = dep ? rr_export(dep, name, 0) : 0;
            }
            if (!va && !dep) {
                if (name == ordname) {           /* "dll#N", the registry's ordinal alias */
                    char key[96]; size_t k = 0;
                    for (; dll[k] && k < 80; k++) key[k] = (char)tolower((unsigned char)dll[k]);
                    snprintf(key + k, sizeof key - k, "#%u", *ilt & 0xFFFFu);
                    va = hle_resolve(key);
                    if (!va && permissive) va = hle_resolve_or_stub(strdup(key));
                } else {
                    va = resolve_shim(dll, name, permissive);
                }
            }
            if (!va) { fprintf(stderr, "[load] %s: unresolved %s!%s\n", m->file, dll, name); bad++; }
            *iat = va;
        }
    }
    fprintf(stderr, "[load] %s: %d imports bound, %d unresolved\n", m->file, n - bad, bad);
    return bad;
}

int rr_attach_dlls(void) {
    /* Dependencies first: the vector is mapped in dependency order. */
    for (int i = 0; i < g_nmods; i++) {
        rr_module *m = &g_mods[i];
        if (!m->is_dll || !m->entry) continue;
        uint32_t a[3] = { m->base, 1 /* DLL_PROCESS_ATTACH */, 0 };
        uint32_t ok = hle_call_guest(m->entry, 3, a);
        fprintf(stderr, "[load] %s DllMain(ATTACH) = %u\n", m->file, ok);
        if (!ok) return -1;
    }
    return 0;
}
