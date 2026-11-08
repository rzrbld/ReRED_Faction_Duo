#!/usr/bin/env python3
"""x87cache.py - keep the x87 stack in C locals across straight-line runs.

lift32 emits every x87 instruction against the global moving-window stack
(fp_push / fp_pop / _st[i] with g_st_base, g_st_i64 and g_fp_top updates).
With -fno-strict-aliasing the compiler must reload those globals after every
guest-memory store, so a run like RF2's skinning loop (load, multiply, add,
store) spends most of its time on stack bookkeeping.

This pass rewrites each function's C, one statement line at a time:

  * fp_push(E)                       -> _xK = E           (value cached)
  * _st[i] ... (no stack change)     -> the cached name, or _st[i - depth]
  * { T _v = (T)fp_pop(); REST }     -> { T _v = (T)_xK; REST' }
  * fp_pop();                        -> (dropped: the cached value dies)
  * fp_xch(i), both cached           -> rename only
  * lines with no x87 or control effect (integer/memory statements, fnstsw
    which reads only _fpu_cmp) pass through unchanged.

Anything else - labels, braces, control flow, calls other than pure math,
g_st_i64 shadows, fild qword, an fxch/fstp st(i) touching the real stack -
first "flushes": the cached values are pushed (bottom first) with fp_push, so
the real stack is exactly what the original code would have built (fp_push
also leaves each slot's i64 shadow 0, as the original pushes did).

Values stay C doubles, as on the real stack, and every operation keeps its
original order and operands, so results are bit-identical.
"""
import re
import sys

NREG = 8
ST = re.compile(r"_st\[(\d+)\]")
CALL = re.compile(r"\b([A-Za-z_]\w*)\s*\(")
PURE = {"MEM8", "MEM16", "MEM32", "MEM64", "ADDR", "sizeof", "sqrt", "fabs", "floor", "ceil", "trunc", "nearbyint",
        "sin", "cos", "tan", "atan", "atan2", "pow", "log", "exp", "ldexp", "fmod", "remainder", "FPU_CMP",
        "fp_round_cw", "fp_to_int"}
CTRL = re.compile(r"\b(goto|return|if|else|switch|case|while|for|do|break|continue)\b|^\s*\w+:\s*$|[{}]")

def split(line):
    """code, comment (the lifter puts one /* ... */ comment at the end)."""
    i = line.find("/*")
    return (line, "") if i < 0 else (line[:i], line[i:])

class Fn:
    def __init__(self):
        self.v = []            # cached stack, bottom .. top: register numbers
        self.used = set()
        self.out = []

    def free_reg(self):
        for r in range(NREG):
            if r not in self.v:
                self.used.add(r)
                return r
        return None

    def name(self, i):
        """logical _st[i] -> C expression"""
        k = len(self.v)
        return f"_x{self.v[k - 1 - i]}" if i < k else f"_st[{i - k}]"

    def remap(self, code):
        return ST.sub(lambda m: self.name(int(m.group(1))), code)

    def flush(self, ind="    "):
        if self.v:
            self.out.append(ind + " ".join(f"fp_push(_x{r});" for r in self.v) + " /* x87cache: spill */")
            self.v = []

    def pure_calls(self, code):
        return all(m.group(1) in PURE for m in CALL.finditer(code))

    def line(self, line):
        code, comment = split(line)
        s = code.strip()
        ind = code[:len(code) - len(code.lstrip())]
        if not s:
            self.out.append(line); return
        x87 = ("_st[" in s) or ("fp_" in s) or ("g_st" in s)
        if not x87:
            if CTRL.search(s) or not self.pure_calls(s) or "RECOMP" in s:
                self.flush(ind)
            self.out.append(line); return
        # ---- x87 statements
        m = re.fullmatch(r"\{ _st\[(\d+)\] = _st\[0\]; g_st_i64\[\1\] = g_st_i64\[0\]; fp_pop\(\); \}", s)
        if m and self.v:                    # fstp st(i) with a cached top: its shadow is 0
            i = int(m.group(1)); k = len(self.v)
            top = self.v[-1]
            if i == 0:
                self.v.pop()
            elif i < k:
                self.v[k - 1 - i] = top; self.v.pop()
            else:
                self.v.pop()
                j = i - k
                self.out.append(f"{ind}_st[{j}] = _x{top}; g_st_i64[{j}] = 0; {comment}".rstrip())
                return
            self.out.append(f"{ind}{comment}".rstrip()); return
        if "g_st" in s or "fp_push_i64" in s:
            self.flush(ind); self.out.append(line); return
        m = re.fullmatch(r"fp_push\((.*)\);", s)
        if m and "fp_" not in m.group(1) and self.pure_calls(m.group(1)):
            r = self.free_reg()
            if r is None:
                self.flush(ind); r = self.free_reg()
            expr = self.remap(m.group(1))
            self.v.append(r)
            self.out.append(f"{ind}_x{r} = {expr}; {comment}".rstrip()); return
        if s == "fp_pop();":
            if self.v:
                self.v.pop()
                self.out.append(f"{ind}{comment}".rstrip() if comment else ""); return
            self.out.append(line); return
        if s == "fp_pop(); fp_pop();":
            if len(self.v) >= 2:
                self.v.pop(); self.v.pop()
                self.out.append(f"{ind}{comment}".rstrip()); return
            self.flush(ind); self.out.append(line); return
        m = re.fullmatch(r"fp_xch\((\d+)\);", s)
        if m:
            i = int(m.group(1)); k = len(self.v)
            if i < k:
                self.v[k - 1], self.v[k - 1 - i] = self.v[k - 1 - i], self.v[k - 1]
                self.out.append(f"{ind}{comment}".rstrip()); return
            self.flush(ind); self.out.append(line); return
        if s.count("fp_pop()") == 1 and "fp_push" not in s and s.startswith("{") and s.endswith("}"):
            pre, post = s.split("fp_pop()", 1)
            if "_st[" not in pre and "fp_" not in post and self.pure_calls(pre + post) and not re.search(r"\b(goto|return)\b", s):
                if self.v:
                    top = f"_x{self.v.pop()}"
                    self.out.append(f"{ind}{pre}{top}{self.remap(post)} {comment}".rstrip()); return
                self.out.append(line); return
            self.flush(ind); self.out.append(line); return
        if "fp_" not in s and not CTRL.search(s) and self.pure_calls(s):
            self.out.append(f"{ind}{self.remap(s)} {comment}".rstrip()); return
        self.flush(ind); self.out.append(line)

FN_START = re.compile(r"^void (recomp_\w+)\(void\) \{\s*$")

def transform(c):
    out, fn, body_start = [], None, None
    for line in c.split("\n"):
        if fn is None:
            out.append(line)
            if FN_START.match(line):
                fn = Fn(); body_start = len(out)
            continue
        if line.startswith("}"):            # end of function (top-level brace)
            fn.flush()
            body = fn.out
            if fn.used:
                regs = ", ".join(f"_x{r}" for r in sorted(fn.used))
                body = [f"    double {regs}; /* x87cache */"] + body
            out.extend(body); out.append(line)
            fn = None
            continue
        fn.line(line)
    return "\n".join(out)

if __name__ == "__main__":
    for p in sys.argv[1:]:
        src = open(p).read()
        open(p, "w").write(transform(src))
