/*
 * watch.c - RERED_WATCH=N: a host thread that every N seconds prints the
 * imports called most often in the last interval, the import currently
 * running and the frame counter. Finds where a stuck guest is spinning
 * without slowing it down (unlike HLE_TRACE).
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "d3d8_internal.h"

extern uint32_t hle_shim_calls[8192];
int hle_shim_count(void);
const char *hle_shim_name_at(int i);
extern const char *g_cur_import;

static void *watch(void *arg) {
    int every = (int)(intptr_t)arg;
    static uint32_t prev[8192];
    for (;;) {
        sleep((unsigned)every);
        int n = hle_shim_count(); if (n > 8192) n = 8192;
        int top[8] = { -1, -1, -1, -1, -1, -1, -1, -1 }; uint32_t topv[8] = {0};
        for (int i = 0; i < n; i++) {
            uint32_t d = hle_shim_calls[i] - prev[i]; prev[i] = hle_shim_calls[i];
            for (int k = 0; k < 8; k++) if (d > topv[k]) {
                memmove(&top[k + 1], &top[k], sizeof(int) * (size_t)(7 - k)); memmove(&topv[k + 1], &topv[k], 4u * (size_t)(7 - k));
                top[k] = i; topv[k] = d; break;
            }
        }
        fprintf(stderr, "[watch] frame %llu, in %s |", (unsigned long long)g_dev.frame, g_cur_import ? g_cur_import : "-");
        for (int k = 0; k < 8 && top[k] >= 0 && topv[k]; k++) fprintf(stderr, " %s:%u", hle_shim_name_at(top[k]), topv[k]);
        fprintf(stderr, "\n");
    }
    return NULL;
}
void rr_watch_start(void) {
    int every = rr_config_int("watch", 0);
    if (every <= 0) return;
    pthread_t t; pthread_create(&t, NULL, watch, (void *)(intptr_t)every); pthread_detach(t);
}
