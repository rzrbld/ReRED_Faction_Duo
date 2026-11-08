#!/usr/bin/env python3
"""prof_report.py PROF.txt UNSTRIPPED_ELF [--last N] [--top K] [--libs DIR]

Aggregates the last N dumps written by runtime/prof.c (format:
"m <idx> <object>", "s <obj> <off> <callerobj> <calleroff> <main> <count>").
Resolves file offsets to symbols: the executable through the unstripped ELF
(lifted functions print as module!X86ADDR), shared libraries through copies
of the device's own libraries in --libs (dynamic symbols). Prints per-object
shares split by thread, the top functions, and the callers of the hottest
library functions."""
import bisect, collections, os, re, struct, subprocess, sys

args = sys.argv[1:]
prof, elf = args[0], args[1]
opt = lambda k, d: args[args.index(k) + 1] if k in args else d
last, top, libdir = int(opt('--last', 3)), int(opt('--top', 40)), opt('--libs', 'build/devlibs')


def loads(path):
    d = open(path, 'rb').read(1 << 16)
    phoff, = struct.unpack_from('<Q', d, 0x20); phes, phn = struct.unpack_from('<HH', d, 0x36)
    out = []
    for i in range(phn):
        t, fl, off, va, pa, fsz, msz, al = struct.unpack_from('<IIQQQQQQ', d, phoff + i * phes)
        if t == 1: out.append((off, off + fsz, va))
    return out


def symtab(path, dynamic):
    cache = path + ('.dsyms' if dynamic else '.syms')
    if not os.path.exists(cache) or os.path.getmtime(cache) < os.path.getmtime(path):
        d = os.path.abspath(os.path.dirname(path))
        cmd = ['docker', 'run', '--rm', '-v', d + ':/b', 'rered-cross:bookworm', 'aarch64-linux-gnu-nm', '-n', '--defined-only']
        if dynamic: cmd.append('-D')
        open(cache, 'w').write(subprocess.run(cmd + ['/b/' + os.path.basename(path)], capture_output=True, text=True).stdout)
    syms = []
    for l in open(cache):
        p = l.split()
        if len(p) == 3 and p[1] in 'tTwWiI': syms.append((int(p[0], 16), p[2].split('@')[0]))
    syms.sort()
    return [a for a, _ in syms], syms, loads(path)


tables = {}
def resolve(obj, off):
    if obj.startswith(('rered', 'rered')):
        path, dyn = elf, False
    else:
        path, dyn = os.path.join(libdir, obj), True
        if not os.path.exists(path):
            m = re.match(r'(.*\.so)', obj)
            alt = os.path.join(libdir, m.group(1)) if m else None
            if not alt or not os.path.exists(alt): return obj + '+?'
            path = alt
    if path not in tables: tables[path] = symtab(path, dyn)
    addrs, syms, ld = tables[path]
    va = next((off - a + v for a, b, v in ld if a <= off < b), off)
    i = bisect.bisect_right(addrs, va) - 1
    if i < 0: return obj + '+?'
    name = syms[i][1]
    m = re.match(r'recomp_(\w+?)_([0-9A-F]{8})$', name)
    return f'{m.group(1)}!{m.group(2)}' if m else (name if obj.startswith(('rered', 'rered')) else f'{obj.split(".so")[0]}:{name}')


blocks = open(prof).read().split('== ')[1:][-last:]
by_obj = collections.Counter(); by_obj_main = collections.Counter()
fn = collections.Counter(); fn_main = collections.Counter(); callers = collections.defaultdict(collections.Counter)
total = 0
for b in blocks:
    names = {}
    for line in b.split('\n')[1:]:
        p = line.split()
        if not p: continue
        if p[0] == 'm': names[int(p[1])] = p[2] if len(p) > 2 else '[anon]'
        elif p[0] == 's':
            o, off, co, coff, main, c = int(p[1]), int(p[2], 16), int(p[3]), int(p[4], 16), int(p[5]), int(p[6])
            on = names.get(o, '[unknown]'); total += c
            by_obj[on] += c
            if main: by_obj_main[on] += c
            f = resolve(on, off) if o >= 0 else '[unknown]'
            fn[f] += c
            if main: fn_main[f] += c
            if not on.startswith(('rered', 'rered')):
                cn = names.get(co, '[unknown]')
                callers[f][resolve(cn, coff) if co >= 0 else '[unknown]'] += c
pct = lambda v: 100.0 * v / max(total, 1)
print(f'{total} samples in {len(blocks)} dump(s); game thread {pct(sum(by_obj_main.values())):.1f}%')
print('-- by object (all threads / game thread)')
for k, v in by_obj.most_common(10): print(f'{pct(v):6.2f}% {pct(by_obj_main[k]):6.2f}%  {k}')
print(f'-- top functions (all / game thread)')
for k, v in fn.most_common(top): print(f'{pct(v):6.2f}% {pct(fn_main[k]):6.2f}%  {k}')
print('-- callers of the hottest library functions')
for k, v in [kv for kv in fn.most_common(200) if ':' in kv[0]][:10]:
    print(f'{pct(v):6.2f}%  {k}')
    for ck, cv in callers[k].most_common(5): print(f'           {pct(cv):6.2f}%  <- {ck}')
