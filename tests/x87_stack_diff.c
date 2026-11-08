/* x87_stack_diff.c - differential test: the moving-window x87 stack
 * (recomp_types.h, [rered]) against the original shifting model.
 * Random sequences of push / pop / fxch / fild-qword-shadow / slot writes;
 * after every step the 8 visible slots, the int64 shadows of slot 0..7 and
 * the depth counter must be identical. Includes long monotone runs that force
 * re-centring at both ends of the buffer. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "recomp_types.h"

double   g_st_buf[RECOMP_FP_BUF];
int64_t  g_st_i64_buf[RECOMP_FP_BUF];
int      g_st_base = RECOMP_FP_BUF / 2;
uint32_t g_eax, g_ecx, g_edx, g_esp, g_ebx, g_esi, g_edi, g_ebp;
int g_fp_top; uint16_t g_fpu_cw;

/* reference: the original model, verbatim behaviour */
static double r_st[8]; static int64_t r_i64[8]; static int r_top;
static void r_push(double v) { for (int i = 7; i > 0; i--) { r_st[i] = r_st[i-1]; r_i64[i] = r_i64[i-1]; } r_st[0] = v; r_i64[0] = 0; r_top++; }
static double r_pop(void) { double v = r_st[0]; for (int i = 0; i < 7; i++) { r_st[i] = r_st[i+1]; r_i64[i] = r_i64[i+1]; } r_st[7] = 0; r_i64[7] = 0; r_top--; return v; }

static uint64_t rng = 88172645463325252ull;
static uint64_t xr(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return rng; }

int main(void) {
    long steps = 0;
    for (int seq = 0; seq < 20000; seq++) {
        int depth = 0;                       /* live entries, as the guest keeps them (0..8) */
        int bias = (int)(xr() % 3);          /* 0 random, 1 push-heavy runs, 2 pop-heavy runs */
        for (int k = 0; k < 400; k++, steps++) {
            int op = (int)(xr() % 6);
            if (bias == 1 && depth < 8 && xr() % 2) op = 0;
            if (bias == 2 && depth > 0 && xr() % 2) op = 1;
            double v = (double)(int64_t)(xr() % 2000001) - 1e6;
            switch (op) {
            case 0: if (depth < 8) { fp_push_impl(NULL, &g_fp_top, v); r_push(v); depth++; } break;
            case 1: if (depth > 0) { double a = fp_pop_impl(NULL, &g_fp_top), b = r_pop(); depth--;
                                     if (a != b) { printf("pop value differs at step %ld\n", steps); return 1; } } break;
            case 2: if (depth > 1) { int i = 1 + (int)(xr() % (unsigned)(depth - 1));
                                     double t = g_st[0]; g_st[0] = g_st[i]; g_st[i] = t;
                                     int64_t ti = g_st_i64[0]; g_st_i64[0] = g_st_i64[i]; g_st_i64[i] = ti;
                                     double u = r_st[0]; r_st[0] = r_st[i]; r_st[i] = u;
                                     int64_t ui = r_i64[0]; r_i64[0] = r_i64[i]; r_i64[i] = ui; } break;
            case 3: if (depth < 8) { int64_t x = (int64_t)xr();
                                     fp_push_impl(NULL, &g_fp_top, (double)x); g_st_i64[0] = x;
                                     r_push((double)x); r_i64[0] = x; depth++; } break;
            case 4: if (depth > 0) { int i = (int)(xr() % (unsigned)depth); g_st[i] = v; r_st[i] = v; } break;
            case 5: /* long monotone run: forces re-centring */
                for (int j = 0; j < 300; j++) {
                    if (depth > 0) { fp_pop_impl(NULL, &g_fp_top); r_pop(); depth--; }
                    fp_push_impl(NULL, &g_fp_top, v + j); r_push(v + j); depth++;
                }
                break;
            }
            if (g_fp_top != r_top) { printf("depth counter differs at step %ld\n", steps); return 1; }
            for (int i = 0; i < depth; i++)
                if (g_st[i] != r_st[i] || g_st_i64[i] != r_i64[i]) {
                    printf("slot %d differs at step %ld (op %d): %g/%lld vs %g/%lld\n", i, steps, op,
                           g_st[i], (long long)g_st_i64[i], r_st[i], (long long)r_i64[i]);
                    return 1;
                }
        }
        while (depth > 0) { fp_pop_impl(NULL, &g_fp_top); r_pop(); depth--; }
    }
    printf("x87 window == shifting model: %ld steps, base stayed in [0,%d)\n", steps, RECOMP_FP_BUF);
    return 0;
}
