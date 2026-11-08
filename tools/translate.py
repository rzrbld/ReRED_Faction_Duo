#!/usr/bin/env python3
"""Multi-module x86-32 -> C translation driver for ReRed Faction II (rf2.exe).

Uses pcrecomp's public disasm32/lift32/pe_analyze libraries. For each module:
optional build-time rebase (PE relocations), function recovery, lift, and
emission of chunked C, plus:
  build/gen/<mod>/map.tsv        VA -> C symbol -> file (debug mapping)
  build/gen/<mod>/ledger.json    lift failures / unsupported instructions
  build/gen/dispatch.c           merged, sorted VA dispatch table (all modules)
  build/gen/modules.json         bases, sizes, entry points, input hashes
Nothing here edits input files; rebased copies go to build/rebased/.
"""
import argparse, hashlib, json, os, re, sys, time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PCRECOMP = Path(os.environ.get("PCRECOMP", Path.home() / "repos/pcrecomp-main"))
for part in ("pe", "disasm", "lift"):
    sys.path.insert(0, str(PCRECOMP / "tools" / part))
import pefile
from pe_analyze import analyze_pe, build_iat_map
from disasm32 import Disassembler
from lift32 import Lifter
sys.path.insert(0, str(Path(__file__).resolve().parent))
import x87cache

# Modules to translate: config/modules.json, a list of
#   {"name": "...", "file": "...", "base": "0x..." or null (keep preferred)}
def load_modules():
    p = ROOT / "config/modules.json"
    mods = json.loads(p.read_text())
    return [(m["name"], m["file"], int(m["base"], 16) if m.get("base") else None) for m in mods]
MODULES = None
CHUNK = 400   # functions per C file

# Instructions lift32 leaves as `; /* UNIMPLEMENTED: x */ /* 0xVA: x */`.
# Real uses get an implementation; everything else becomes a fatal call that
# names its VA (in practice data decoded as code, never reached).
_UNIMPL = re.compile(r"; /\* UNIMPLEMENTED: ([^*]*?) \*/ /\* 0x([0-9A-F]{8}): ([^*]*)\*/")
_GOTO = re.compile(r"goto L_([0-9A-F]{8});")
# x87 operand-order fixes for pcrecomp's lift32 (upstream bugs, fixed here so
# the shared repo stays untouched):
#  fsincos: ST0 = cos, ST1 = sin (lift32 left sin on top);
#  fpatan:  ST1 = atan(ST1 / ST0), pop (lift32 computed atan2(ST0, ST1)).
X87_FIXES = [
    ("{ double _a = _st[0]; _st[0] = cos(_a); fp_push(sin(_a)); }",
     "{ double _a = _st[0]; _st[0] = sin(_a); fp_push(cos(_a)); }"),
    ("{ double _v = fp_pop(); _st[0] = atan2(_v, _st[0]); }",
     "{ double _v = fp_pop(); _st[0] = atan2(_st[0], _v); }"),
]
def fix_x87(c):
    for a, b in X87_FIXES:
        c = c.replace(a, b)
    return c

def fix_dangling_gotos(c):
    """A branch to a label the function never defines (a target outside the
    recovered body: in practice data decoded as code) becomes a fatal call
    naming the target, instead of a compile error or a silent fallthrough."""
    defined = set(re.findall(r"^\s*L_([0-9A-F]{8}):", c, re.M))
    def sub(m):
        t = m.group(1)
        return m.group(0) if t in defined else f'{{ recomp_unimplemented(0x{t}u, "branch outside function"); return; }}'
    return _GOTO.sub(sub, c)

LOCAL_REGS = os.environ.get("RERED_LOCAL_REGS", "1") == "1"   # must match the Makefile's LOCAL_REGS
def local_regs(c):
    """RECOMP_LOCAL_REGS: guest registers live in C locals for the body (the
    compiler keeps them in host registers instead of reloading globals after
    every guest store). Calls and tail calls already write back / reload
    (RECOMP_CALL/ICALL/ITAIL); every other exit writes back here."""
    head, brace, body = c.partition("{\n")
    body = re.sub(r"\breturn;", "{ RECOMP_REGS_OUT(); return; }", body)
    end = body.rstrip().rfind("}")
    body = body[:end] + "    RECOMP_REGS_OUT();\n" + body[end:]
    return head + brace + "    RECOMP_REGS_LOCALS\n" + body

def _fix_unimpl(m):
    ins, va = m.group(1).strip(), m.group(2)
    if ins == "pushf":     # EFLAGS image: IF + reserved bit 1, DF from the lifter's _df
        return f"PUSH32(esp, 0x202u | (_df < 0 ? 0x400u : 0u)); /* 0x{va}: pushf */"
    if ins == "popf":
        return f"{{ uint32_t _fl = POP32_VAL(esp); _df = (_fl & 0x400u) ? -1 : 1; }} /* 0x{va}: popf */"
    mm = re.fullmatch(r"(fnstenv|fldenv) \[(.*)\]", ins)
    if mm:
        ea = mm.group(2).replace("esp", "esp").replace(" ", "")
        ea = re.sub(r"([+-])0x([0-9a-f]+)", lambda x: f"{x.group(1)}0x{x.group(2)}u", ea)
        if mm.group(1) == "fnstenv":   # 32-bit protected-mode layout, 28 bytes
            return (f"{{ uint32_t _e = {ea}; memset((void*)ADDR(_e), 0, 28); MEM32(_e) = 0xFFFF0000u | _fpu_cw; "
                    f"MEM32(_e + 4) = 0xFFFF0000u | ((uint32_t)(_fp_top & 7) << 11); MEM32(_e + 8) = 0xFFFFFFFFu; }} /* 0x{va}: fnstenv */")
        return f"_fpu_cw = (uint16_t)MEM32({ea}); /* 0x{va}: fldenv (control word only) */"
    return f'recomp_unimplemented(0x{va}u, "{ins}"); /* 0x{va} */'


class ImageDisassembler(Disassembler):
    """Reads through the memory-mapped image, so a read never spills from a
    short raw section into the next section's file bytes."""
    def __init__(self, data, base, sections, image):
        super().__init__(data, base, sections)
        self._image = image
    def read_bytes(self, va, size):
        off = va - self.image_base
        if off < 0 or off >= len(self._image):
            return None
        return self._image[off:off + size]


def sha256(b):
    return hashlib.sha256(b).hexdigest()


_BAD = {"in", "out", "insb", "insd", "insw", "outsb", "outsd", "int1", "int3", "hlt", "salc", "iretd", "cli", "sti",
        "arpl", "bound", "aaa", "aas", "aad", "aam", "daa", "das", "into", "ljmp", "lcall", "retf", "les", "lds", "ud2"}
def plausible_entry(image, base, va, md):
    """First basic block decodes cleanly with no privileged/legacy opcodes."""
    off = va - base
    n = 0
    for ins in md.disasm(image[off:off + 96], va):
        if ins.mnemonic in _BAD or ins.mnemonic.startswith("int") and ins.op_str not in ("0x2e",):
            return False
        n += 1
        if ins.mnemonic in ("ret", "jmp") or ins.mnemonic.startswith("ret") or n >= 6:
            return True
    return False

def data_pointer_seeds(pe, image, base, code_lo, code_hi):
    """Code addresses stored in data: vtables, callback and initializer tables.
    With relocations the stored pointers are exactly the fixed-up dwords; a
    fixed-base image without them is scanned for aligned dwords that point
    at plausible code."""
    from capstone import Cs, CS_ARCH_X86, CS_MODE_32
    md = Cs(CS_ARCH_X86, CS_MODE_32)
    out = set()
    def in_code(rva):
        return any(sec.Characteristics & 0x20000000 and sec.VirtualAddress <= rva < sec.VirtualAddress + max(sec.Misc_VirtualSize, sec.SizeOfRawData)
                   for sec in pe.sections)
    if hasattr(pe, "DIRECTORY_ENTRY_BASERELOC"):
        # slots inside code are immediates and switch tables (the lifter's business)
        slots = [r.rva for blk in pe.DIRECTORY_ENTRY_BASERELOC for r in blk.entries if r.type == 3 and not in_code(r.rva)]
    else:
        slots = []
        for sec in pe.sections:
            if sec.Characteristics & 0x20000000:   # executable: switch tables there are found by the lifter
                continue
            lo = sec.VirtualAddress
            slots += range(lo, lo + max(sec.Misc_VirtualSize, sec.SizeOfRawData) - 3, 4)
    import struct
    for rva in slots:
        if rva + 4 > len(image):
            continue
        v = struct.unpack_from("<I", image, rva)[0]
        if code_lo <= v < code_hi and plausible_entry(image, base, v, md):
            out.add(v)
    return out

def jump_table_targets(pe, image, base):
    """Targets of MSVC switch dispatches `jmp dword ptr [reg*4 + table]`
    (FF 24 with a SIB of scale 4 and no base register) in code sections; each
    table is read while its entries are instruction starts in code."""
    import struct
    from capstone import Cs, CS_ARCH_X86, CS_MODE_32
    md = Cs(CS_ARCH_X86, CS_MODE_32)
    code = [(base + s.VirtualAddress, base + s.VirtualAddress + max(s.Misc_VirtualSize, s.SizeOfRawData))
            for s in pe.sections if s.Characteristics & 0x20000000]
    def in_code(v):
        return any(a <= v < b for a, b in code)
    def insn_at(v):
        o = v - base
        return any(True for _ in md.disasm(image[o:o + 15], v, count=1))
    out = set()
    for a, b in code:
        blob = image[a - base:b - base]
        i = blob.find(b'\xff\x24')
        while 0 <= i <= len(blob) - 7:
            sib = blob[i + 2]
            if (sib >> 6) == 2 and (sib & 7) == 5:
                tbl = struct.unpack_from('<I', blob, i + 3)[0]
                if base <= tbl < base + len(image) - 4:
                    for k in range(1024):
                        o = tbl - base + 4 * k
                        if o + 4 > len(image):
                            break
                        v = struct.unpack_from('<I', image, o)[0]
                        if not in_code(v) or not insn_at(v):
                            break
                        out.add(v)
            i = blob.find(b'\xff\x24', i + 2)
    return sorted(out)


def code_immediates(funcs, image, base, lo, hi):
    """Immediate operands of recovered instructions that point at plausible
    function starts inside the code range."""
    from capstone import Cs, CS_ARCH_X86, CS_MODE_32
    md = Cs(CS_ARCH_X86, CS_MODE_32)
    out = set()
    for f in funcs.values():
        for blk in f.blocks.values():
            for ins in blk.instructions:
                if ins.mnemonic.startswith("j") or ins.mnemonic == "call":
                    continue                       # branch targets are handled by recovery itself
                for op in ins.operands or ():
                    if op.type == 2:               # X86_OP_IMM
                        v = op.imm & 0xFFFFFFFF
                        if lo <= v < hi and plausible_entry(image, base, v, md):
                            out.add(v)
    return out


def load_seeds(mod):
    p = ROOT / "config/seeds" / f"{mod}.json"
    return [int(x, 16) if isinstance(x, str) else x for x in json.loads(p.read_text())] if p.exists() else []


def translate(game_dir, mod, fname, base, out_root):
    src = Path(game_dir) / fname
    raw = src.read_bytes()
    pe = pefile.PE(data=raw)
    assert pe.FILE_HEADER.Machine == 0x14C, f"{fname}: not i386"
    path = src
    if base is not None and base != pe.OPTIONAL_HEADER.ImageBase:
        pe.relocate_image(base)
        rb = ROOT / "build/rebased" / fname
        rb.parent.mkdir(parents=True, exist_ok=True)
        rb.write_bytes(pe.write())
        path = rb
        pe = pefile.PE(str(rb))
    info = analyze_pe(str(path))
    image = pe.get_memory_mapped_image()
    for s in info.sections:                     # the protector-free build keeps .text as code
        if s.name == ".text":
            s.characteristics |= 0x20000020
    code = [s for s in info.sections if s.is_code]
    lo = info.image_base + min(s.virtual_address for s in code)
    hi = info.image_base + max(s.virtual_address + max(s.raw_size, s.virtual_size) for s in code)
    dis = ImageDisassembler(path.read_bytes(), info.image_base, info.sections, image)
    iat = build_iat_map(info)
    entry = info.image_base + info.entry_point_rva
    exports = {info.image_base + e.rva: e.name for e in info.exports if e.rva}
    dseeds = data_pointer_seeds(pe, image, info.image_base, lo, hi)
    seeds = [entry, *exports, *load_seeds(mod), *sorted(dseeds)]
    t0 = time.time()
    funcs = dis.find_functions(lo, hi, iat, seeds=seeds)
    if entry not in funcs:
        raise SystemExit(f"{fname}: entry point not recovered")
    # Extra entry points: switch-table targets that are not function starts.
    # The lifter turns a table jump into a local switch over the targets inside
    # its function and tail-jumps (RECOMP_ITAIL) to the rest; when recovery
    # ended a function early, those land on addresses nothing was translated
    # at. A second recovery pass seeded with the targets yields functions
    # starting at them; only those new entries are added, so the main set's
    # bodies (and their loops) are unchanged. Registers and the stack are
    # global, so entering at a case label behaves as the jump would.
    jt = [t for t in jump_table_targets(pe, image, info.image_base) if t not in funcs]
    # Code addresses passed as immediates (`push offset callback`,
    # `mov [x], offset handler`): callbacks reached only through a pointer the
    # code builds itself (callbacks the game stores in its own structures).
    imm = sorted(code_immediates(funcs, image, info.image_base, lo, hi) - set(funcs) - set(jt))
    if imm:
        print(f"[{mod}] {len(imm)} code-immediate targets without a function", flush=True)
    jt = jt + imm
    if jt:
        dis2 = ImageDisassembler(path.read_bytes(), info.image_base, info.sections, image)
        funcs2 = dis2.find_functions(lo, hi, iat, seeds=seeds + jt)
        extra = {a: f for a, f in funcs2.items() if a not in funcs}
        missing = [t for t in jt if t not in funcs2]
        funcs.update(extra)
        print(f"[{mod}] {len(jt)} switch targets outside functions: {len(extra)} extra entries"
              f"{', NOT recovered: ' + ' '.join(f'{t:08X}' for t in missing[:8]) if missing else ''}", flush=True)
    names = {a: f"{mod}_{a:08X}" for a in funcs}
    for a, f in funcs.items():
        f.name = "recomp_" + names[a]
    lifter = Lifter(iat_map=iat, func_names=names, lifted=set(funcs),
                    precise_sbb=True, precise_carry=True)
    out = out_root / mod
    out.mkdir(parents=True, exist_ok=True)
    for old in out.glob("f_*.c"):
        old.unlink()
    (out / "functions.h").write_text(
        "#pragma once\n#include \"recomp_types.h\"\n"
        "void recomp_unimplemented(uint32_t va, const char *insn);\n"
        "void recomp_lift_failed(uint32_t va);\n" +
        "".join(f"void recomp_{names[a]}(void);\n" for a in sorted(funcs)))
    ledger = {"lift_errors": [], "unsupported": {}}
    maprows, chunk, n = [], [], 0
    order = sorted(funcs)

    def flush():
        nonlocal chunk, n
        if not chunk:
            return
        fn = out / f"f_{n:03d}.c"
        fn.write_text("#define RECOMP_GENERATED_CODE\n#include \"recomp_types.h\"\n"
                      "#include \"functions.h\"\n#include <math.h>\n#include <string.h>\n\n"
                      + "\n".join(chunk))
        chunk, n = [], n + 1

    for a in order:
        f = funcs[a]
        try:
            c = lifter.lift_function(f)
        except Exception as e:   # never silently drop: emit a fatal stub and record it
            ledger["lift_errors"].append({"va": f"{a:08X}", "error": repr(e)[:300]})
            c = (f"void recomp_{names[a]}(void) {{ recomp_lift_failed(0x{a:08X}u); }}\n")
        for m in _UNIMPL.finditer(c):
            ledger["unsupported"].setdefault(m.group(1).strip(), []).append(m.group(2))
        c = _UNIMPL.sub(_fix_unimpl, c)
        c = fix_dangling_gotos(fix_x87(c))
        if os.environ.get("RERED_X87CACHE", "1") != "0":
            c = x87cache.transform(c)       # x87 stack in C locals (tools/x87cache.py)
        if LOCAL_REGS:
            c = local_regs(c)
        maprows.append(f"{a:08X}\trecomp_{names[a]}\tf_{n:03d}.c\t{exports.get(a, '')}")
        chunk.append(c)
        if len(chunk) >= CHUNK:
            flush()
    flush()
    (out / "map.tsv").write_text("va\tsymbol\tfile\texport\n" + "\n".join(maprows) + "\n")
    (out / "ledger.json").write_text(json.dumps(ledger, indent=1))
    print(f"[{mod}] {len(dseeds)} data-pointer seeds; {len(funcs)} functions, {n} files, {len(ledger['lift_errors'])} lift errors, "
          f"unsupported kinds {sorted(ledger['unsupported'])} in {time.time()-t0:.0f}s", flush=True)
    return {"module": mod, "file": fname, "sha256": sha256(raw), "base": info.image_base,
            "entry": entry, "size_of_image": pe.OPTIONAL_HEADER.SizeOfImage,
            "functions": sorted(funcs), "names": names}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("game_dir")
    ap.add_argument("--out", default=str(ROOT / "build/gen"))
    ap.add_argument("--only", nargs="*")
    a = ap.parse_args()
    out = Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    mods = []
    for m, f, b in load_modules():
        if a.only and m not in a.only:
            cached = out / m / "module.json"
            if cached.exists():
                r = json.loads(cached.read_text())
                r["names"] = {int(k): v for k, v in r["names"].items()}
                mods.append(r)
            continue
        r = translate(a.game_dir, m, f, b, out)
        (out / m / "module.json").write_text(json.dumps(r))
        mods.append(r)
    ents = sorted((va, f"recomp_{r['names'][va]}", r["module"])
                  for r in mods for va in r["functions"])
    with open(out / "dispatch.c", "w") as d:
        d.write("#include \"recomp_types.h\"\n")
        for _, sym, _m in ents:
            d.write(f"void {sym}(void);\n")
        d.write("const recomp_dispatch_entry_t recomp_dispatch_table[] = {\n")
        for va, sym, _m in ents:
            d.write(f"  {{0x{va:08X}u, {sym}}},\n")
        d.write(f"}};\nconst uint32_t recomp_dispatch_count = {len(ents)};\n")
    (out / "modules.json").write_text(json.dumps(
        [{k: r[k] for k in ("module", "file", "sha256", "base", "entry", "size_of_image")} for r in mods], indent=1))


if __name__ == "__main__":
    main()
    # route configured functions through native replacements (config/native.json)
    import subprocess
    subprocess.run([sys.executable, "-P", str(ROOT / "tools/native_hooks.py"), str(Path(sys.argv[sys.argv.index("--out") + 1]) if "--out" in sys.argv else ROOT / "build/gen")], check=True)
