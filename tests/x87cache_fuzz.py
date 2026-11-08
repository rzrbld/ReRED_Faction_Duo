#!/usr/bin/env python3
"""x87cache_fuzz.py N - differential test of tools/x87cache.py.

Generates N random functions made of the statement forms lift32 emits (loads,
arithmetic on _st[i], pops into memory, fxch, fstp st(i), compares, integer
statements, labels and gotos), runs each untransformed and transformed in a
small C harness with a reference x87 stack, and requires identical final
stacks, memory and compare results (bit for bit)."""
import os, random, subprocess, sys, tempfile
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "tools"))
import x87cache

HARNESS = r'''
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
static double g_buf[64]; static int64_t g_st_i64_buf[64]; static int g_base = 40, g_fp_top;
#define _st (g_buf + g_base)
#define g_st_i64 (g_st_i64_buf + g_base)
static inline void fp_push(double v) { g_base--; g_buf[g_base] = v; g_st_i64_buf[g_base] = 0; g_fp_top++; }
static inline double fp_pop(void) { double v = g_buf[g_base]; g_base++; g_fp_top--; return v; }
#define fp_xch(i) do { double _t = _st[0]; _st[0] = _st[i]; _st[i] = _t; int64_t _ti = g_st_i64[0]; g_st_i64[0] = g_st_i64[i]; g_st_i64[i] = _ti; } while (0)
#define FPU_CMP(a, b) ((a) < (b) ? -1 : (a) > (b) ? 1 : (a) == (b) ? 0 : 2)
static float g_mem[64];
#define ADDR(a) ((uintptr_t)&g_mem[(a) & 63])
static uint32_t eax = 3, ecx = 7, esp = 40;
#define PUSH32(sp, val) do { (sp) -= 4; g_mem[((sp) >> 2) & 63] = (float)(val); } while (0)
#define CMP_B(a, b) ((uint32_t)(a) < (uint32_t)(b))
static int _fpu_cmp;
void FN(void);
int main(void) {
    for (int i = 0; i < 64; i++) g_mem[i] = (float)((i * 37 % 23) - 11) * 0.37f;
    for (int i = 0; i < 64; i++) g_buf[i] = i * 0.5 - 7;
    FN();
    printf("%d %d %d %u\n", g_base, g_fp_top, _fpu_cmp, eax);
    for (int i = g_base; i < 64; i++) printf("%lld ", (long long)g_st_i64_buf[i]);
    printf("\n");
    /* the live stack only: slots below the top are dead (popped) */
    for (int i = g_base; i < 64; i++) { uint64_t u; memcpy(&u, &g_buf[i], 8); printf("%016llx ", (unsigned long long)u); }
    printf("\n");
    for (int i = 0; i < 64; i++) { uint32_t u; memcpy(&u, &g_mem[i], 4); printf("%08x ", u); }
    printf("\n");
    return 0;
}
'''

def gen(rng, nlines):
    lines, depth, labels = [], 0, 0
    for _ in range(nlines):
        r = rng.random()
        a = rng.randrange(16)
        if depth < 7 and r < 0.25:
            lines.append(f"    fp_push(*(float*)ADDR(eax + {a})); /* fld */"); depth += 1
        elif depth < 7 and r < 0.30:
            lines.append(f"    fp_push(_st[{rng.randrange(max(depth,1))}]); /* fld st */"); depth += 1
        elif depth >= 1 and r < 0.45:
            op = rng.choice(["+=", "-=", "*="])
            lines.append(f"    _st[0] {op} (double)*(float*)ADDR(ecx + {a}); /* fop mem */")
        elif depth >= 2 and r < 0.55:
            op = rng.choice(["+=", "-=", "*="])
            lines.append(f"    {{ double _v = fp_pop(); _st[0] {op} _v; }} /* fopp */"); depth -= 1
        elif depth >= 1 and r < 0.65:
            lines.append(f"    {{ float _v = (float)fp_pop(); *(float*)ADDR(eax + {a}) = _v; }} /* fstp */"); depth -= 1
        elif depth >= 2 and r < 0.70:
            lines.append(f"    fp_xch({rng.randrange(1, depth)}); /* fxch */")
        elif depth >= 1 and r < 0.74:
            lines.append(f"    _fpu_cmp = FPU_CMP(_st[0], (double)*(float*)ADDR({a})); /* fcom */")
        elif depth >= 2 and r < 0.77:
            i = rng.randrange(0, depth + 3)     # may reach below this run's values
            lines.append(f"    {{ _st[{i}] = _st[0]; g_st_i64[{i}] = g_st_i64[0]; fp_pop(); }} /* fstp st(i) */"); depth -= 1
        elif depth >= 1 and r < 0.80:
            lines.append("    fp_pop(); /* fstp st0 */"); depth -= 1
        elif r < 0.86:
            lines.append(f"    eax = (eax * 5 + {a}) & 63; /* int */")
        elif depth >= 1 and r < 0.90:
            lines.append(f"    _st[0] = -_st[0]; /* fchs */")
        elif depth >= 1 and r < 0.915:
            lines.append(f"    {{ float _v = (float)_st[{rng.randrange(depth)}]; *(float*)ADDR(eax + {a}) = _v; }} /* fst */")
        elif r < 0.925:
            lines.append(f"    PUSH32(esp, eax); /* push */")
        elif r < 0.93:
            lines.append(f"    eax = CMP_B(eax, {a}) + ecx; /* flags-ish */")
        elif r < 0.94:
            labels += 1
            lines.append(f"L_{labels}:")
        elif r < 0.97:
            lines.append(f"    if (eax & 1) goto L_end; /* jcc */")
        else:
            lines.append(f"    eax = (eax & 0xFFFF0000u) | ((_fpu_cmp == 0) ? 0x4000u : 0x0100u); /* fnstsw */")
    lines.append("L_end:")
    lines.append("    return;")
    return "void FN(void) {\n" + "\n".join(lines) + "\n}\n"

def run(src, d, tag):
    c = os.path.join(d, tag + ".c"); exe = os.path.join(d, tag)
    open(c, "w").write(HARNESS + src)
    subprocess.run(["cc", "-O1", "-w", "-o", exe, c, "-lm"], check=True)
    return subprocess.run([exe], capture_output=True, text=True, check=True).stdout

def main():
    n = int(sys.argv[1]) if len(sys.argv) > 1 else 300
    rng = random.Random(1234)
    with tempfile.TemporaryDirectory() as d:
        for i in range(n):
            src = gen(rng, rng.randrange(10, 80))
            src_fn = src.replace("void FN(void) {", "void recomp_t_0(void) {")
            out = x87cache.transform(src_fn).replace("void recomp_t_0(void) {", "void FN(void) {")
            a, b = run(src, d, "a"), run(out, d, "b")
            if a != b:
                print("MISMATCH in case", i); print(src); print("----"); print(out); return 1
    print(f"x87cache: {n} random functions identical")
    return 0

if __name__ == "__main__":
    sys.exit(main())
