#!/usr/bin/env python3
"""dis.py PE VA [count] [base] -- disassemble at a guest VA (optionally rebased)."""
import sys, pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_32
pe = pefile.PE(sys.argv[1])
if len(sys.argv) > 4: pe.relocate_image(int(sys.argv[4], 16))
img = pe.get_memory_mapped_image(); base = pe.OPTIONAL_HEADER.ImageBase
va = int(sys.argv[2], 16); n = int(sys.argv[3]) if len(sys.argv) > 3 else 30
md = Cs(CS_ARCH_X86, CS_MODE_32)
for i, ins in enumerate(md.disasm(img[va - base: va - base + n * 15], va)):
    if i >= n: break
    print(f"{ins.address:08X}  {ins.bytes.hex():20s} {ins.mnemonic} {ins.op_str}")
