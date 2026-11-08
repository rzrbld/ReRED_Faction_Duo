/*
 * main.c - ReRed Faction II entry point: platform init, guest memory, module
 * mapping (dependency order), IAT binding, DllMains, then rf2.exe's CRT entry.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "win32hle.h"
#include "host.h"
#include "loader.h"
#include "rered.h"

static const struct { const char *file; uint32_t base; } MODULES[] = {
    { "rf2.exe", 0x00400000u },         /* must match config/modules.json */
};

void recomp_unimplemented(uint32_t va, const char *insn) {
    hle_fatal("unimplemented instruction '%s' reached at 0x%08X", insn, va);
}
/* An indirect call or tail jump to an address with no translated function:
 * a function the disassembler missed. Fatal by default, naming the address
 * (add it to config/seeds/<module>.json); RERED_UNRESOLVED_CONTINUE=1 logs
 * and returns instead, for diagnosis. */
void recomp_unresolved(uint32_t va, uint32_t from, const char *kind) {
    static int cont = -1;
    if (cont < 0) cont = getenv("RERED_UNRESOLVED_CONTINUE") != NULL;
    fprintf(stderr, "%s: unresolved VA 0x%08X from 0x%08X\n", kind, va, from);
    if (!cont) hle_fatal("no translated function at 0x%08X (%s from 0x%08X)", va, kind, from);
}
void recomp_lift_failed(uint32_t va) { hle_fatal("function 0x%08X failed to lift", va); }

int main(int argc, char **argv) {
    const char *game = getenv("RERED_GAME_DIR");
    if (!game) game = argc > 1 ? argv[1] : "data";
    int permissive = getenv("RERED_PERMISSIVE") && atoi(getenv("RERED_PERMISSIVE"));
    { void rr_crash_install(void); rr_crash_install(); }
    rr_paths_init(game);
    game = ".";                      /* rr_paths_init made the game dir current */
    hle_mem_init();
    recomp_host_init();
    rr_register_shims();
    { void rr_watch_start(void); rr_watch_start(); }
    { void rr_prof_start(void); rr_prof_start(); }
    for (size_t i = 0; i < sizeof MODULES / sizeof *MODULES; i++)
        if (rr_map(game, MODULES[i].file, MODULES[i].base, i + 1 == sizeof MODULES / sizeof *MODULES)) return 2;
    int bad = 0;
    for (int i = 0; i < rr_module_count(); i++) bad += rr_bind(rr_module_at(i), permissive);
    if (bad && !permissive) { fprintf(stderr, "[rered] %d unresolved imports (RERED_PERMISSIVE=1 to stub)\n", bad); return 3; }
    if (rr_attach_dlls()) { fprintf(stderr, "[rered] a DLL refused to attach\n"); return 4; }
    /* native replacements: after every module is mapped, so the self-check's
     * scratch memory and guest stack cannot take an image's fixed address */
    rr_module *exe = rr_find_module("rf2.exe");
    fprintf(stderr, "[rered] entering rf2.exe at 0x%08X\n", exe->entry);
    uint32_t args[1] = {0};
    return (int)hle_call_guest(exe->entry, 0, args);
}
