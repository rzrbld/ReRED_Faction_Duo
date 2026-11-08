/*
 * paths.c - where the game's files live on the host.
 * C:\ is the game directory (the user's installed data), never written.
 * Writable state goes under RERED_USER_DIR (default <game>/../userdata):
 * the registry file, and userdata/game, an overlay of the game directory:
 * every file the game writes there (savegame\*, its logs) lands in the
 * overlay, and reads look in the overlay first, so saves survive updates of
 * data/ and no symlinks are needed (the SD card is vfat).
 */
#define _GNU_SOURCE
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "win32hle.h"
#include "rered.h"

static char g_user[PATH_MAX];

const char *rr_user_dir(void) { return g_user; }

void rr_paths_init(const char *game_dir) {
    char real[PATH_MAX];
    if (!realpath(game_dir, real)) { fprintf(stderr, "[paths] game dir %s missing\n", game_dir); exit(2); }
    const char *u = getenv("RERED_USER_DIR");
    if (u) snprintf(g_user, sizeof g_user, "%s", u);
    else snprintf(g_user, sizeof g_user, "%s/../userdata", real);
    mkdir(g_user, 0755);
    hle_set_drive('C', real);
    static char ov[PATH_MAX];
    snprintf(ov, sizeof ov, "%s/game", g_user);
    mkdir(ov, 0755);
    hle_set_overlay(real, ov);
    static char reg[PATH_MAX];
    snprintf(reg, sizeof reg, "%s/registry.txt", g_user);
    hle_reg_load(reg);
    if (chdir(real) != 0) perror("[paths] chdir");
    fprintf(stderr, "[paths] C:\\ = %s, user dir = %s\n", real, g_user);
}
