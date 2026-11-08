#!/usr/bin/env python3
"""native_hooks.py [GEN_DIR] - route listed translated functions through
native replacements (config/native.json: module -> {VA: native symbol}).

The translated body is renamed recomp_<mod>_<VA>_orig and a wrapper with the
original name calls the native function, or the original when rr_native_on
is 0 (RERED_NATIVE=0). Direct calls, indirect calls and the dispatch table
all keep using the original name. Idempotent."""
import json, re, sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

def hook_text(c, mod, va, native):
    name = f"recomp_{mod}_{va}"
    if f"{name}_orig(void)" in c:
        return c, False
    head = f"void {name}(void) {{"
    if head not in c:
        return c, False
    c = c.replace(head, f"void {name}_orig(void) {{", 1)
    c += (f"\n/* native replacement (tools/native_hooks.py, config/native.json) */\n"
          f"extern int rr_native_on;\nvoid {native}(void);\n"
          f"void {name}(void) {{ if (rr_native_on) {{ {native}(); return; }} {name}_orig(); }}\n")
    return c, True

def main():
    gen = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "build/gen"
    cfg = json.loads((ROOT / "config/native.json").read_text())
    for mod, hooks in cfg.items():
        for va, native in hooks.items():
            va = va.upper()
            for f in sorted((gen / mod).glob("f_*.c")):
                c = f.read_text()
                if f"void recomp_{mod}_{va}(void) {{" in c or f"recomp_{mod}_{va}_orig(void)" in c:
                    c2, changed = hook_text(c, mod, va, native)
                    if changed:
                        f.write_text(c2)
                    print(f"{mod}!{va} -> {native} in {f.name}{'' if changed else ' (already)'}")
                    break
            else:
                print(f"{mod}!{va}: not found", file=sys.stderr)

if __name__ == "__main__":
    main()
