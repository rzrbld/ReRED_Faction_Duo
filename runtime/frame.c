/*
 * frame.c - per-Present work: frame timing statistics (RERED_STATS=1
 * prints a line every 5 s with fps, frame-time percentiles and draw counts).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL2/SDL.h>
#include "d3d8_internal.h"
#include "input.h"

#define WIN 512
uint64_t rr_swap_ticks;
#include <sys/syscall.h>
#include <unistd.h>
static double g_ft[WIN];
static int g_nft;
static uint64_t g_last, g_window_start, g_window_frames, g_window_draws;

static int cmpd(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return x < y ? -1 : x > y; }

#include "gl_funcs.h"
void rr_apply_write_masks(void);
/* RERED_SHOT=N: every N seconds write the back buffer to userdata/shot_NNN.ppm */
const char *rr_user_dir(void);
static void screenshot(void) {
    static int every = -1, n; static uint64_t last;
    if (every < 0) every = rr_config_int("shot", 0);
    if (!every) return;
    uint64_t now = SDL_GetTicks64();
    if (now - last < (uint64_t)every * 1000) return;
    last = now;
    int w = g_dev.out_w, h = g_dev.out_h;
    uint8_t *px = malloc((size_t)w * h * 4);
    glReadPixels(g_dev.out_x, g_dev.out_y, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px);
    char path[1024]; snprintf(path, sizeof path, "%s/shot_%03d.ppm", rr_user_dir(), n++);
    FILE *f = fopen(path, "wb");
    if (f) {
        fprintf(f, "P6\n%d %d\n255\n", w, h);
        for (int y = h - 1; y >= 0; y--) for (int x = 0; x < w; x++) fwrite(px + ((size_t)y * w + x) * 4, 1, 3, f);
        fclose(f);
    }
    free(px);
}
/* RERED_FPS=1: frames per second in the top-left corner. A 3x5 pixel font
 * drawn as scissored clears (no textures, no shaders); GL state restored. */
static const uint16_t k_glyph[] = {      /* 5 rows top->bottom, one octal digit (3 bits, 4 = left) each */
    075557, 026227, 071747, 071717, 055711, 074717, 074757, 071111, 075757, 075717,
};
static uint16_t glyph_of(char c) {
    if (c >= '0' && c <= '9') return k_glyph[c - '0'];
    switch (c) { case 'F': return 074744; case 'P': return 075744; case 'S': return 074717; }
    return 0;
}
static void glyph_bits(uint16_t g, int row, int out[3]) {
    static const int shift[5] = { 12, 9, 6, 3, 0 };
    int r = (g >> shift[row]) & 7;
    out[0] = r & 4; out[1] = r & 2; out[2] = r & 1;
}
static void fps_overlay(double fps) {
    char txt[16]; snprintf(txt, sizeof txt, "%d FPS", (int)(fps + 0.5));
    const int sc = g_dev.out_h >= 960 ? 4 : 2, cw = 4 * sc, x0 = g_dev.out_x + 4, h = g_dev.out_y + g_dev.out_h;
    int len = (int)strlen(txt);
    GLboolean sc_on = glIsEnabled(GL_SCISSOR_TEST);
    GLfloat cc[4]; glGetFloatv(GL_COLOR_CLEAR_VALUE, cc);
    GLint box[4]; glGetIntegerv(GL_SCISSOR_BOX, box);
    glEnable(GL_SCISSOR_TEST);
    glColorMask(1, 1, 1, 1);
    glClearColor(0, 0, 0, 1);
    glScissor(x0 - 2, h - 4 - 5 * sc - 2, len * cw + 2, 5 * sc + 4);
    glClear(GL_COLOR_BUFFER_BIT);
    glClearColor(1.f, 0.85f, 0.2f, 1);
    for (int i = 0; i < len; i++) {
        uint16_t g = glyph_of(txt[i]);
        for (int row = 0; row < 5; row++) {
            int bits[3]; glyph_bits(g, row, bits);
            for (int col = 0; col < 3; col++) if (bits[col]) {
                glScissor(x0 + i * cw + col * sc, h - 4 - (row + 1) * sc, sc, sc);
                glClear(GL_COLOR_BUFFER_BIT);
            }
        }
    }
    glClearColor(cc[0], cc[1], cc[2], cc[3]);
    glScissor(box[0], box[1], box[2], box[3]);
    if (!sc_on) glDisable(GL_SCISSOR_TEST);
    rr_apply_write_masks();
}
/* PGO training runs (RERED_GCOV_DUMP=SECONDS): write the profile counters
 * once that long after start, then keep running. Weak: absent in normal builds. */
#ifdef RERED_PGO_GEN
extern void __gcov_dump(void);        /* a strong reference pulls it out of libgcov */
#else
extern void __gcov_dump(void) __attribute__((weak));
#endif
static void gcov_hook(void) {
    static int state = 0; static uint64_t t0;
    if (state == 2 || !__gcov_dump) return;
    uint64_t now = SDL_GetPerformanceCounter(), f = SDL_GetPerformanceFrequency();
    const char *e = getenv("RERED_GCOV_DUMP");
    if (!state) { if (!e) { state = 2; return; } t0 = now; state = 1; }
    if ((double)(now - t0) / (double)f >= atof(e)) { __gcov_dump(); fprintf(stderr, "[pgo] profile written\n"); state = 2; }
}
void rr_present_hook(void) {
    gcov_hook();
    {   /* RERED_DLOG=N: log the draws of one frame every N seconds */
        extern int rr_dlog_on; static int every = -1; static uint64_t last;
        if (every < 0) every = rr_config_int("dlog", 0);
        rr_dlog_on = 0;
        if (every && SDL_GetTicks64() - last >= (uint64_t)every * 1000) {
            last = SDL_GetTicks64(); rr_dlog_on = 1; fprintf(stderr, "[dlog] ---- frame\n");
        }
    }
    static int crash_ok;           /* after SDL video/audio and the GL driver have installed theirs */
    if (!crash_ok) { void rr_crash_install(void); rr_crash_install(); crash_ok = 1; }
    static int fps_on = -1; static double fps_shown; static uint64_t fps_t0, fps_n;
    if (fps_on < 0) fps_on = rr_config_int("fps", 0);
    if (fps_on) {
        uint64_t now = SDL_GetPerformanceCounter(), f = SDL_GetPerformanceFrequency();
        if (!fps_t0) fps_t0 = now;
        fps_n++;
        if (now - fps_t0 >= f / 2) { fps_shown = (double)fps_n * (double)f / (double)(now - fps_t0); fps_t0 = now; fps_n = 0; }
        fps_overlay(fps_shown);
    }
    screenshot();
    /* RERED_LIMIT=30|60: cap presents per second (sleep, then a short spin) */
    static int limit = -1; static uint64_t next;
    if (limit < 0) limit = rr_config_int("limit", 0);
    if (limit > 0) {
        uint64_t f = SDL_GetPerformanceFrequency(), step = f / (uint64_t)limit, now = SDL_GetPerformanceCounter();
        if (!next || now > next + step) next = now;
        else {
            while ((int64_t)(next - now) > (int64_t)(f / 500)) { SDL_Delay((Uint32)((next - now) * 1000 / f) - 1); now = SDL_GetPerformanceCounter(); }
            while (now < next) now = SDL_GetPerformanceCounter();
        }
        next += step;
    }
    rr_input_pump();
    static int stats = -1;
    if (stats < 0) stats = rr_config_int("stats", 0);
    uint64_t now = SDL_GetPerformanceCounter(), f = SDL_GetPerformanceFrequency();
    if (g_last && g_nft < WIN) g_ft[g_nft++] = (double)(now - g_last) * 1000.0 / (double)f;
    g_last = now;
    if (!g_window_start) g_window_start = now;
    g_window_frames++;
    if (stats && (now - g_window_start) > 5 * f && g_nft > 1) {
        double s = (double)(now - g_window_start) / (double)f, sorted[WIN];
        memcpy(sorted, g_ft, sizeof(double) * (size_t)g_nft);
        qsort(sorted, (size_t)g_nft, sizeof(double), cmpd);
        long rss_kb = 0, hwm_kb = 0;            /* resident memory, current and peak */
        FILE *st = fopen("/proc/self/status", "r");
        if (st) { char ln[128]; while (fgets(ln, sizeof ln, st)) { sscanf(ln, "VmRSS: %ld", &rss_kb); sscanf(ln, "VmHWM: %ld", &hwm_kb); } fclose(st); }
        /* where the wall time of a frame goes: game-thread CPU, waiting for
         * the machine lock, and inside the buffer swap */
        static long main_tid; static uint64_t last_cpu_ticks, last_wait, last_swap;
        if (!main_tid) main_tid = (long)syscall(SYS_gettid);
        uint64_t cpu_ticks = 0;
        { char sp[64]; snprintf(sp, sizeof sp, "/proc/self/task/%ld/stat", main_tid);
          FILE *tf = fopen(sp, "r");
          if (tf) { char buf[512]; size_t n = fread(buf, 1, sizeof buf - 1, tf); buf[n] = 0; fclose(tf);
                    char *q = strrchr(buf, ')'); unsigned long ut = 0, stt = 0;
                    if (q && sscanf(q + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %lu %lu", &ut, &stt) == 2) cpu_ticks = ut + stt; } }
        extern volatile uint64_t hle_mach_wait_ns;
        double hz = (double)sysconf(_SC_CLK_TCK), frames = (double)g_window_frames;
        fprintf(stderr, "[stats] per frame: game-thread cpu %.1f ms, lock wait %.1f ms, swap %.1f ms | ",
                (double)(cpu_ticks - last_cpu_ticks) / hz * 1000.0 / frames,
                (double)(hle_mach_wait_ns - last_wait) / 1e6 / frames,
                (double)(rr_swap_ticks - last_swap) * 1000.0 / (double)f / frames);
        last_cpu_ticks = cpu_ticks; last_wait = hle_mach_wait_ns; last_swap = rr_swap_ticks;
        extern volatile uint64_t hle_mach_takes, hle_yields;
        static uint64_t last_takes, last_yields;
        fprintf(stderr, "[stats] rss %ld MB peak %ld MB | lock takes/s %.0f yields/s %.0f | ", rss_kb >> 10, hwm_kb >> 10,
                (double)(hle_mach_takes - last_takes) / s, (double)(hle_yields - last_yields) / s);
        last_takes = hle_mach_takes; last_yields = hle_yields;
        fprintf(stderr, "%.1f fps | frame ms p50 %.1f p95 %.1f p99 %.1f max %.1f | draws/frame %.0f\n",
                (double)g_window_frames / s, sorted[g_nft / 2], sorted[g_nft * 95 / 100], sorted[g_nft * 99 / 100], sorted[g_nft - 1],
                (double)(g_draw_stats.draws - g_window_draws) / (double)g_window_frames);
        { static rr_draw_stats prev; double fr = (double)(g_window_frames ? g_window_frames : 1);
          fprintf(stderr, "[stats] per frame: client vb %.0f ib %.0f, programs %.0f, binds %.0f, uniforms %.0f\n",
                  (g_draw_stats.client_vb - prev.client_vb) / fr, (g_draw_stats.client_ib - prev.client_ib) / fr, (g_draw_stats.progs - prev.progs) / fr,
                  (g_draw_stats.binds - prev.binds) / fr, (g_draw_stats.unif - prev.unif) / fr);
          prev = g_draw_stats;
          extern uint64_t rr_t_upload_ns, rr_t_upload_bytes, rr_t_read_ns; static uint64_t pu, pb, pr;
          fprintf(stderr, "[stats] per frame: texture upload %.2f ms (%.0f KB), read-back %.2f ms\n",
                  (rr_t_upload_ns - pu) / 1e6 / fr, (rr_t_upload_bytes - pb) / 1024.0 / fr, (rr_t_read_ns - pr) / 1e6 / fr);
          pu = rr_t_upload_ns; pb = rr_t_upload_bytes; pr = rr_t_read_ns; }
        g_window_start = now; g_window_frames = 0; g_nft = 0; g_window_draws = g_draw_stats.draws;
    }
}
