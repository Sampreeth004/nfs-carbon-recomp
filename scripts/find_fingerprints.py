#!/usr/bin/env python3
"""Re-derive Carbon render-mode addresses for OUR XEX build by instruction fingerprint.

The NFS-CARBON-360-DECOMP repo targets a different XEX build, so its addresses do not
match ours. We search guest_image.bin for the instruction shapes (not the addresses),
map each hit to an enclosing function start from generated/default/nfscarbon_init.cpp,
and print candidate globals (lis+lwz/stw/addi pairs) found inside each matched function.

Usage: python scripts/find_fingerprints.py [--image guest_image.bin] [--base 0x82000000]
"""
import argparse, bisect, re, struct, sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
FULL = 0xFFFFFFFF
BL = 0xFC000003      # opcode + AA/LK bits of a branch; displacement wildcarded
SHIFT_WINDOW = 0x2000  # how far from decomp+shift a loose match may sit

# name -> (decomp address, [(word offset in bytes, word, mask)])
# Words come from the decomp's fingerprint list (instruction shape only).
FINGERPRINTS = {
    "renderer_ctor": (0x824FFD30, [(0x00, 0x7D8802A6, FULL), (0x08, 0x38E301E4, FULL),
                                   (0x0C, 0x39430064, FULL), (0x10, 0x38C300A4, FULL)]),
    "mode_select": (0x8250B350, [(0x00, 0x7D8802A6, FULL), (0x10, 0x48000001, BL)]),
    "output_size": (0x825003E8, [(0x00, 0x54AB063E, FULL), (0x28, 0x2F040000, FULL)]),
    "ring_wait": (0x826DEFD0, [(0x00, 0x7D8802A6, FULL), (0x08, 0x9421FF80, FULL),
                               (0x64, 0x817D2A10, FULL), (0x70, 0x814B0000, FULL)]),
}

def load_func_starts(init_cpp: Path):
    starts = sorted({int(m, 16) for m in re.findall(r"\{\s*0x([0-9A-Fa-f]{8}),\s*sub_", init_cpp.read_text())})
    return starts

def search(words, base, pattern):
    n = len(words)
    first_off, first_w, first_m = pattern[0]
    hits = []
    for i in range(n):
        if (words[i] & first_m) != first_w:
            continue
        ok = True
        for off, w, m in pattern[1:]:
            j = i + off // 4
            if j >= n or (words[j] & m) != w:
                ok = False
                break
        if ok:
            hits.append(base + i * 4 - first_off)
    return hits

def candidate_globals(words, base, start, span=0x200):
    """lis rX,hi followed within 4 insns by lwz/stw/addi rY,rX,lo -> hi:lo (0x82xxxxxx range)."""
    out = {}
    i0 = (start - base) // 4
    for i in range(i0, min(i0 + span // 4, len(words) - 5)):
        w = words[i]
        if (w >> 26) != 15 or ((w >> 16) & 31) != 0 and False:
            continue
        if (w >> 26) != 15 or ((w >> 16) & 0x1F) != 0:  # addis with rA=0 == lis
            continue
        rd, hi = (w >> 21) & 31, w & 0xFFFF
        if hi < 0x8200 or hi > 0x82FF:
            continue
        for k in range(1, 5):
            x = words[i + k]
            op, ra = x >> 26, (x >> 16) & 31
            if op in (14, 32, 36, 34, 38, 40, 44, 48, 52) and ra == rd:
                lo = x & 0xFFFF
                lo = lo - 0x10000 if lo & 0x8000 else lo
                addr = ((hi << 16) + lo) & 0xFFFFFFFF
                kind = {14: "addi", 32: "lwz", 36: "stw", 34: "lbz", 38: "stb", 40: "lhz", 44: "sth"}.get(op, f"op{op}")
                out.setdefault(addr, set()).add(kind)
    return out

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--image", default=str(ROOT / "guest_image.bin"))
    ap.add_argument("--base", type=lambda s: int(s, 0), default=0x82000000)
    ap.add_argument("--init", default=str(ROOT / "generated/default/nfscarbon_init.cpp"))
    a = ap.parse_args()

    data = Path(a.image).read_bytes()
    data = data[: len(data) // 4 * 4]
    words = struct.unpack(">%dI" % (len(data) // 4), data)
    starts = load_func_starts(Path(a.init))
    print(f"image: {len(data):#x} bytes at {a.base:#010x}; {len(starts)} function starts\n")

    # Pass 1: unique hits give the build shift (ours - decomp). Loose patterns are then
    # resolved to the hit nearest decomp+shift within SHIFT_WINDOW.
    results = {n: search(words, a.base, pat) for n, (_, pat) in FINGERPRINTS.items()}
    shifts = [h[0] - FINGERPRINTS[n][0] for n, h in results.items() if len(h) == 1]
    shift = max(set(shifts), key=shifts.count) if shifts else 0
    print(f"anchor shift (ours - decomp) from unique hits: {shift:+#x} (votes {shifts.count(shift)}/{len(shifts)})\n")

    print(f"{'name':14} {'decomp':10} {'ours':10} {'is_start':8} {'conf':6} notes")
    for name, (decomp, pat) in FINGERPRINTS.items():
        hits = results[name]
        total = len(hits)
        note = ""
        if total > 1:
            near = [h for h in hits if abs(h - (decomp + shift)) <= SHIFT_WINDOW]
            note = f"{total} raw hits; "
            hits = sorted(near, key=lambda h: abs(h - (decomp + shift)))[:1]
        if not hits:
            print(f"{name:14} {decomp:#010x} {'-':10} {'-':8} {'none':6} {note}no match")
            continue
        h = hits[0]
        idx = bisect.bisect_right(starts, h) - 1
        enc = starts[idx] if idx >= 0 else None
        is_start = h in starts
        if total == 1:
            conf = "high" if is_start else "med"
        else:
            conf = "med" if is_start and h - decomp == shift else "low"
            note += f"nearest to shift ({h - decomp:+#x}); "
        if not is_start:
            note += f"not a start; enclosing {enc:#010x}" if enc else "no enclosing func"
        print(f"{name:14} {decomp:#010x} {h:#010x} {str(is_start):8} {conf:6} {note}")
        if name in ("renderer_ctor", "mode_select", "output_size") and is_start:
            for addr, kinds in sorted(candidate_globals(words, a.base, h).items()):
                if 0x82C00000 <= addr < 0x83000000:
                    print(f"{'':14}   global {addr:#010x} via {','.join(sorted(kinds))}")

if __name__ == "__main__":
    sys.exit(main())
