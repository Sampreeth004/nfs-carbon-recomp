#!/usr/bin/env python3
"""Rewrite calls between recompiled game functions so the compiler can inline them.

Every recompiled function sub_X is a weak, noinline alias of __imp__sub_X
(DEFINE_REX_FUNC in generated/default/nfscarbon_pch.h), so a hook defined in
src/ can replace it at link time. The price is that no call to sub_X can be
inlined. For functions nobody hooks, calling __imp__sub_X is the same code and
lets clang inline it (within a file, or across files with ThinLTO).

Hooked functions keep their sub_X calls: any sub_8XXXXXXX named in src/,
renderer/ or a .toml file counts as hooked. nfscarbon_init.cpp (the dispatch
table for indirect calls) is never touched, so indirect calls still see hooks.

Run after every codegen (the generated folder is not in git):
    python tools/direct_calls.py            apply
    python tools/direct_calls.py --undo     restore plain sub_X calls
    python tools/direct_calls.py --status
"""

import argparse
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
GEN = ROOT / "generated" / "default"
HOOK_SOURCES = [ROOT / "src", ROOT / "renderer"]

NAME = re.compile(r"\bsub_(8[0-9A-Fa-f]{7})\b")
# Game functions and the PowerPC register save/restore helpers.
FUNC = r"(sub_8[0-9A-Fa-f]{7}|__(?:save|rest)(?:gprlr|fpr|vmx)_\d+)"
DEFINED = re.compile(r"DEFINE_REX_FUNC\(" + FUNC + r"\)")
CALL = re.compile(r"(?<![\w&])" + FUNC + r"\(ctx, base\)")
DIRECT = re.compile(r"__imp__" + FUNC + r"\(ctx, base\)")


def hooked_addresses():
    hooked = set()
    files = [p for d in HOOK_SOURCES if d.is_dir() for p in d.rglob("*")
             if p.suffix in (".cpp", ".h", ".cc", ".hpp")]
    files += list(ROOT.glob("*.toml"))
    for path in files:
        hooked.update("sub_" + m.upper() for m in NAME.findall(
            path.read_text(encoding="utf-8", errors="ignore")))
    return hooked


def Key(name):
    """sub_ addresses compare case-insensitively; helper names as they are."""
    return "sub_" + name[4:].upper() if name.startswith("sub_") else name


def recomp_files():
    return sorted(GEN.glob("nfscarbon_recomp.*.cpp"))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--undo", action="store_true")
    parser.add_argument("--status", action="store_true")
    args = parser.parse_args()

    files = recomp_files()
    if not files:
        sys.exit(f"no generated sources in {GEN}")

    if args.status:
        direct = sum(len(DIRECT.findall(p.read_text(encoding="utf-8"))) for p in files)
        plain = sum(len(CALL.findall(p.read_text(encoding="utf-8"))) for p in files)
        print(f"direct calls: {direct}, calls through the weak alias: {plain}")
        return

    if args.undo:
        changed = 0
        for path in files:
            text = path.read_text(encoding="utf-8")
            new = DIRECT.sub(r"sub_\1(ctx, base)", text)
            if new != text:
                path.write_text(new, encoding="utf-8", newline="")
                changed += 1
        print(f"restored {changed} files")
        return

    hooked = hooked_addresses()
    defined = set()
    for path in files:
        defined.update(Key(m) for m in DEFINED.findall(path.read_text(encoding="utf-8")))

    rewritten = kept = 0
    for path in files:
        text = path.read_text(encoding="utf-8")

        def replace(m):
            nonlocal rewritten, kept
            name = Key(m.group(1))
            if name in hooked or name not in defined:
                kept += 1
                return m.group(0)
            rewritten += 1
            return f"__imp__{m.group(1)}(ctx, base)"

        new = CALL.sub(replace, text)
        if new != text:
            path.write_text(new, encoding="utf-8", newline="")
    print(f"hooked functions: {len(hooked)}; calls made direct: {rewritten}; kept: {kept}")


if __name__ == "__main__":
    main()
