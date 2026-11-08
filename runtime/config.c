/*
 * config.c - port settings. Read once from RERED_<KEY> environment
 * variables (the launcher exports them from userdata/rered.cfg).
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include "d3d8_internal.h"

int rr_config_int(const char *key, int def) {
    char env[96]; size_t i = 0;
    i += (size_t)snprintf(env, sizeof env, "RERED_");
    for (const char *k = key; *k && i < sizeof env - 1; k++) env[i++] = (char)toupper((unsigned char)*k);
    env[i] = 0;
    const char *v = getenv(env);
    return v && *v ? atoi(v) : def;
}
