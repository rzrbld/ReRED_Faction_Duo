/*
 * prof.c - RERED_PROF=1: statistical CPU profiler for on-device measurement.
 *
 * ITIMER_PROF delivers SIGPROF about every millisecond of process CPU time to
 * whichever thread is running; the handler stores that thread's program
 * counter in a lock-free ring. Every 10 s the samples are aggregated and
 * written to <user dir>/prof.txt: per mapped object (from /proc/self/maps),
 * then the hottest addresses inside our own executable as offsets, which
 * tools/prof_report.py resolves against the unstripped binary (lifted
 * functions carry the original x86 address in their names).
 */
#define _GNU_SOURCE
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>
#include <sys/syscall.h>
#include "rered.h"

#define RING (1 << 18)
static uintptr_t g_pc[RING], g_lr[RING];
static uint8_t g_main[RING];
static volatile uint32_t g_head;
static pid_t g_main_tid;

static void on_prof(int sig, siginfo_t *si, void *ctx) {
    (void)sig; (void)si;
    ucontext_t *uc = ctx;
#if defined(__aarch64__)
    uintptr_t pc = (uintptr_t)uc->uc_mcontext.pc, lr = (uintptr_t)uc->uc_mcontext.regs[30];
#elif defined(__x86_64__)
    uintptr_t pc = (uintptr_t)uc->uc_mcontext.gregs[REG_RIP], lr = 0;
#else
    uintptr_t pc = 0, lr = 0;
#endif
    uint32_t i = __atomic_fetch_add(&g_head, 1, __ATOMIC_RELAXED);
    g_pc[i & (RING - 1)] = pc;
    g_lr[i & (RING - 1)] = lr;
    g_main[i & (RING - 1)] = (pid_t)syscall(SYS_gettid) == g_main_tid;
}

typedef struct { uintptr_t lo, hi, off; char name[160]; } map_t;
static int load_maps(map_t *m, int max) {
    FILE *f = fopen("/proc/self/maps", "r"); if (!f) return 0;
    char ln[512]; int n = 0;
    while (n < max && fgets(ln, sizeof ln, f)) {
        unsigned long lo, hi, off; char perm[8], path[256] = "";
        if (sscanf(ln, "%lx-%lx %7s %lx %*s %*s %255[^\n]", &lo, &hi, perm, &off, path) < 4) continue;
        if (perm[2] != 'x') continue;
        m[n].lo = lo; m[n].hi = hi; m[n].off = off;
        const char *b = strrchr(path, '/');
        snprintf(m[n].name, sizeof m[n].name, "%s", path[0] ? (b ? b + 1 : path) : "[anon]");
        n++;
    }
    fclose(f);
    return n;
}

static int find_map(const map_t *m, int n, uintptr_t a) {
    for (int j = 0; j < n; j++) if (a >= m[j].lo && a < m[j].hi) return j;
    return -1;
}
typedef struct { int obj, cobj; uintptr_t off, coff; int main; long c; } prof_key;
static int cmp_key(const void *a, const void *b) {
    const prof_key *x = a, *y = b;
    if (x->obj != y->obj) return x->obj - y->obj;
    if (x->off != y->off) return x->off < y->off ? -1 : 1;
    if (x->cobj != y->cobj) return x->cobj - y->cobj;
    if (x->coff != y->coff) return x->coff < y->coff ? -1 : 1;
    return x->main - y->main;
}
/* one line per distinct (pc, caller, thread): "s <obj> <off> <callerobj> <calleroff> <main> <count>";
 * objects are listed first as "m <index> <name>" */
static void dump(uint32_t from, uint32_t to) {
    uint32_t n = to - from; if (n > RING) { from = to - RING; n = RING; }
    if (!n) return;
    static map_t maps[512]; int nm = load_maps(maps, 512);
    prof_key *k = malloc(sizeof *k * n);
    for (uint32_t i = 0; i < n; i++) {
        uint32_t r = (from + i) & (RING - 1);
        int o = find_map(maps, nm, g_pc[r]), co = find_map(maps, nm, g_lr[r]);
        k[i] = (prof_key){ o, co, o >= 0 ? g_pc[r] - maps[o].lo + maps[o].off : g_pc[r],
                        co >= 0 ? g_lr[r] - maps[co].lo + maps[co].off : g_lr[r], g_main[r], 1 };
    }
    qsort(k, n, sizeof *k, cmp_key);
    char path[1024]; snprintf(path, sizeof path, "%s/prof.txt", rr_user_dir());
    FILE *f = fopen(path, "a"); if (!f) { free(k); return; }
    fprintf(f, "== %u samples\n", n);
    for (int j = 0; j < nm; j++) fprintf(f, "m %d %s\n", j, maps[j].name);
    for (uint32_t i = 0; i < n; ) {
        uint32_t e = i + 1;
        while (e < n && !cmp_key(&k[i], &k[e])) e++;
        fprintf(f, "s %d %lx %d %lx %d %u\n", k[i].obj, (unsigned long)k[i].off, k[i].cobj, (unsigned long)k[i].coff, k[i].main, e - i);
        i = e;
    }
    fclose(f);
    free(k);
}

static void *dumper(void *arg) {
    (void)arg;
    uint32_t last = 0;
    for (;;) {
        sleep(10);
        uint32_t now = g_head;
        dump(last, now);
        last = now;
    }
    return NULL;
}

void rr_prof_start(void) {
    const char *e = getenv("RERED_PROF");
    if (!e || !atoi(e)) return;
    g_main_tid = (pid_t)syscall(SYS_gettid);
    struct sigaction sa; memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_prof; sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGPROF, &sa, NULL);
    /* a CPU-time timer of the game thread itself, signalled to that thread:
     * samples follow the game thread's CPU use exactly (process-wide
     * ITIMER_PROF delivered to arbitrary threads and lost samples) */
    timer_t tm;
    struct sigevent sev; memset(&sev, 0, sizeof sev);
    sev.sigev_notify = SIGEV_THREAD_ID;
    sev.sigev_signo = SIGPROF;
    sev._sigev_un._tid = g_main_tid;
    if (timer_create(CLOCK_THREAD_CPUTIME_ID, &sev, &tm) == 0) {
        struct itimerspec its = { { 0, 1000000 }, { 0, 1000000 } };
        timer_settime(tm, 0, &its, NULL);
    } else {
        struct itimerval it = { { 0, 1000 }, { 0, 1000 } };
        setitimer(ITIMER_PROF, &it, NULL);
    }
    pthread_t t; pthread_create(&t, NULL, dumper, NULL); pthread_detach(t);
    fprintf(stderr, "[prof] sampling the game thread every 1 ms of its CPU time -> %s/prof.txt\n", rr_user_dir());
}
