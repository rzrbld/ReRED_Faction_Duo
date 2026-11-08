#!/usr/bin/env python3
"""jumptable_seeds.py PE MAP.tsv -> JSON list of jump-table targets with no
translated function. Finds MSVC switch dispatches `jmp dword ptr [reg*4 + table]`
(FF 24 xx, SIB with scale 4 and no base) in executable sections, reads each
table while its entries point at code, and reports the entries that are not
function starts in the translation map."""
import json, struct, sys
import pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_32
pe = pefile.PE(sys.argv[1]); img = pe.get_memory_mapped_image(); base = pe.OPTIONAL_HEADER.ImageBase
funcs = {int(l.split('\t')[0], 16) for l in open(sys.argv[2]).read().split('\n')[1:] if l}
code = [(base + s.VirtualAddress, base + s.VirtualAddress + max(s.Misc_VirtualSize, s.SizeOfRawData))
        for s in pe.sections if s.Characteristics & 0x20000000]
def in_code(v): return any(lo <= v < hi for lo, hi in code)
md = Cs(CS_ARCH_X86, CS_MODE_32)
def is_insn_start(v):
    off = v - base
    return any(True for _ in md.disasm(img[off:off + 15], v, count=1))
out, tables = set(), 0
for lo, hi in code:
    blob = img[lo - base:hi - base]
    i = 0
    while True:
        i = blob.find(b'\xff\x24', i)
        if i < 0 or i + 7 > len(blob): break
        sib = blob[i + 2]
        if (sib >> 6) == 2 and (sib & 7) == 5:            # scale 4, no base, disp32
            tbl = struct.unpack_from('<I', blob, i + 3)[0]
            if base <= tbl < base + len(img) - 4:
                tables += 1
                for k in range(1024):
                    v = struct.unpack_from('<I', img, tbl - base + 4 * k)[0]
                    if not in_code(v) or not is_insn_start(v): break
                    if v not in funcs: out.add(v)
        i += 2
print(f"{tables} jump tables, {len(out)} targets without a function", file=sys.stderr)
print(json.dumps([f"0x{v:08X}" for v in sorted(out)]))
