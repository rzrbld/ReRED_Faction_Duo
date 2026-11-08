#pragma once
#include <stdint.h>
#define RR_MAX_MODULES 8
typedef struct {
    char file[64];
    uint32_t base, span, entry, import_rva, export_rva, export_size;
    int is_dll;
} rr_module;
int rr_map(const char *game_dir, const char *file, uint32_t base, int main_image);
int rr_bind(rr_module *m, int permissive);
int rr_attach_dlls(void);
uint32_t rr_export(const rr_module *m, const char *name, uint32_t ordinal);
rr_module *rr_find_module(const char *dll);
int rr_module_count(void);
rr_module *rr_module_at(int i);
