#pragma once
/* Port-specific services shared by the shims. */
void rr_paths_init(const char *game_dir);   /* drive mapping, writable dirs */
void rr_register_shims(void);               /* d3d8, dinput, vorbisfile, ... */
const char *rr_user_dir(void);              /* writable: saves, registry, config */
