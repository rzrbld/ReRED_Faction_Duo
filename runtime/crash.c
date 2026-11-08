/*
 * crash.c - fatal-signal report (SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT).
 * Prints, to stderr (log.txt): the fault address (and guest VA when inside
 * the guest's 4 GB window), the host PC as an executable offset (resolve with
 * tools/addr.sh), the lifted x86 function and import that were running, the
 * guest registers and the recent indirect-call trail. Then re-raises.
 */
#define _GNU_SOURCE
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ucontext.h>
#include <unistd.h>
#include <execinfo.h>
#include <fcntl.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include "recomp_types.h"
static pid_t g_main_tid;

/* read guest memory without faulting (process_vm_readv returns EFAULT instead) */
static int guest_read(uint32_t va, void *out, size_t n) {
    struct iovec l = { out, n }, r = { (void *)((uintptr_t)g_mem_base + va), n };
    return process_vm_readv(getpid(), &l, 1, &r, 1, 0) == (ssize_t)n ? 0 : -1;
}
static void dump_guest(const char *name, uint32_t va, uint32_t bytes) {
    uint32_t w[128];
    if (bytes > sizeof w) bytes = sizeof w;
    if (va < 0x10000 || guest_read(va, w, bytes)) return;
    fprintf(stderr, "  [%s=%08X]", name, va);
    for (uint32_t i = 0; i < bytes / 4; i++) fprintf(stderr, "%s%08X", i % 8 ? " " : "\n    ", w[i]);
    fprintf(stderr, "\n");
}

/* the /proc/self/maps line that contains a (async-signal-safe enough: open/read) */
static void where(const char *what, uintptr_t a) {
    char buf[8192]; int fd = open("/proc/self/maps", O_RDONLY);
    if (fd < 0) return;
    ssize_t n, used = 0;
    while ((n = read(fd, buf + used, sizeof buf - 1 - (size_t)used)) > 0) {
        used += n; buf[used] = 0;
        char *line = buf, *nl;
        while ((nl = strchr(line, '\n'))) {
            *nl = 0;
            unsigned long lo, hi, off;
            if (sscanf(line, "%lx-%lx %*s %lx", &lo, &hi, &off) == 3 && a >= lo && a < hi) {
                const char *path = strchr(line, '/');
                fprintf(stderr, "  %s %p = %s +0x%lx\n", what, (void *)a, path ? path : "[anon]", (unsigned long)(a - lo + off));
                close(fd); return;
            }
            line = nl + 1;
        }
        used = (ssize_t)strlen(line); memmove(buf, line, (size_t)used + 1);
    }
    close(fd);
    fprintf(stderr, "  %s %p not mapped\n", what, (void *)a);
}

extern const char *g_cur_import;
extern char __executable_start;
#ifndef ICALL_TRACE_SIZE
#define ICALL_TRACE_SIZE 64
#endif
extern uint32_t g_icall_trace[], g_icall_from[];
extern uint32_t g_icall_trace_idx;

static void on_fatal(int sig, siginfo_t *si, void *ctx) {
    ucontext_t *uc = ctx;
    uintptr_t pc = 0, lr = 0;
#if defined(__aarch64__)
    pc = (uintptr_t)uc->uc_mcontext.pc; lr = (uintptr_t)uc->uc_mcontext.regs[30];
#elif defined(__x86_64__)
    pc = (uintptr_t)uc->uc_mcontext.gregs[REG_RIP];
#endif
    uintptr_t exe = (uintptr_t)&__executable_start, addr = (uintptr_t)si->si_addr;
    fprintf(stderr, "\n=== CRASH: signal %d (%s), fault address %p", sig, strsignal(sig), (void *)addr);
    if (si->si_code <= 0) fprintf(stderr, " (sent by a process: not a memory fault)");
    if (addr >= (uintptr_t)g_mem_base && addr - (uintptr_t)g_mem_base < (1ull << 32))
        fprintf(stderr, " = guest 0x%08lX", (unsigned long)(addr - (uintptr_t)g_mem_base));
    pid_t tid = (pid_t)syscall(SYS_gettid);
    fprintf(stderr, "\n  thread %d%s\n", (int)tid, tid == g_main_tid ? " (main/game thread)" : " (other thread)");
    where("pc", pc); where("lr", lr);
    (void)exe;
    fprintf(stderr, "  lifted function 0x%08X, last import %s\n", g_cur_func, g_cur_import ? g_cur_import : "-");
    fprintf(stderr, "  eax %08X ebx %08X ecx %08X edx %08X esi %08X edi %08X ebp %08X esp %08X\n",
            g_eax, g_ebx, g_ecx, g_edx, g_esi, g_edi, g_ebp, g_esp);
    dump_guest("eax", g_eax, 0x40); dump_guest("esi", g_esi, 0x1A0); dump_guest("edi", g_edi, 0x40); dump_guest("ecx", g_ecx, 0x40);
    dump_guest("ebx", g_ebx, 0x40); dump_guest("esp", g_esp, 0x40);
#ifdef RECOMP_ICALL_TRACE
    fprintf(stderr, "  recent indirect calls (target <- from):");
    for (int i = 8; i >= 1; i--) {
        uint32_t k = (g_icall_trace_idx - (uint32_t)i) & (ICALL_TRACE_SIZE - 1);
        fprintf(stderr, " %08X<-%08X", g_icall_trace[k], g_icall_from[k]);
    }
#else
    fprintf(stderr, "  (indirect-call trail not recorded: build with ICALL_TRACE=1)");
#endif
    fprintf(stderr, "\n  native backtrace:\n");
    void *bt[48]; int nb = backtrace(bt, 48);
    for (int i = 0; i < nb; i++) where("  at", (uintptr_t)bt[i]);
    fflush(stderr);
    signal(sig, SIG_DFL);
    raise(sig);
}

void rr_crash_install(void) {
    g_main_tid = (pid_t)syscall(SYS_gettid);
    { void *warm[2]; backtrace(warm, 2); }   /* load the unwinder now, not inside the handler */
    static char altstack[64 * 1024];
    stack_t ss = { .ss_sp = altstack, .ss_size = sizeof altstack, .ss_flags = 0 };
    sigaltstack(&ss, NULL);
    struct sigaction sa; memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_fatal; sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESETHAND;
    sigemptyset(&sa.sa_mask);
    int sigs[] = { SIGSEGV, SIGBUS, SIGILL, SIGFPE };
    for (size_t i = 0; i < sizeof sigs / sizeof *sigs; i++) sigaction(sigs[i], &sa, NULL);
}
