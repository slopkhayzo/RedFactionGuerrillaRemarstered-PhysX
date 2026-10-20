"""Map the GOG build of rfg.exe onto another build compiled from the same source (the Steam one).

python tools/match_builds.py [--gog GOG\rfg.exe] [--other STEAM\rfg.exe] [--procs research/out/procs.tsv]
                             [--out tools/steam_map.json]

Every function of the GOG build (from its PDB, via procs.tsv) is looked for in the other build: its bytes, with the
parts that legitimately differ between two builds left out (absolute addresses, from the relocation table, and the
offsets of branches leaving the function), are searched near where the previous function was found. Where the code
matches, its references to data are compared side by side, which pairs each GOG global with the other build's. The
result, written as JSON, is a list of address ranges and their shifts (other RVA = GOG RVA + delta) for code and data,
the functions that were not found, and both builds' identities. tools/gen_symbols.py turns it into the second column
of the generated addresses.
"""
import argparse
import bisect
import json
import os
import struct
import sys

import capstone
import pefile

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
GOG_EXE = r'D:\Program Files\Games\GOG\Red Faction Guerrilla Re-Mars-tered\rfg.exe'
STEAM_EXE = r'D:\Program Files\Games\Steam\steamapps\common\Red Faction Guerrilla Re-MARS-tered\rfg.exe'
WINDOW = 0x4000  # bytes around the expected place searched before the whole code section
MIN_ANCHOR = 6   # the shortest stretch of unmasked bytes used to search for a function


class Build:
    def __init__(self, path):
        data = open(path, 'rb').read()
        self.pe = pefile.PE(data=data, fast_load=True)
        self.pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY['IMAGE_DIRECTORY_ENTRY_BASERELOC'],
                                                    pefile.DIRECTORY_ENTRY['IMAGE_DIRECTORY_ENTRY_DEBUG']])
        self.img = self.pe.get_memory_mapped_image()
        self.base = self.pe.OPTIONAL_HEADER.ImageBase
        self.relocs = sorted(e.rva for b in getattr(self.pe, 'DIRECTORY_ENTRY_BASERELOC', [])
                             for e in b.entries if e.type == pefile.RELOCATION_TYPE['IMAGE_REL_BASED_HIGHLOW'])
        self.reloc_set = set(self.relocs)
        self.sections = [(s.Name.rstrip(b'\0').decode(), s.VirtualAddress, s.VirtualAddress + s.Misc_VirtualSize)
                         for s in self.pe.sections]
        self.identity = {'timestamp': self.pe.FILE_HEADER.TimeDateStamp,
                         'size_of_image': self.pe.OPTIONAL_HEADER.SizeOfImage}
        for d in getattr(self.pe, 'DIRECTORY_ENTRY_DEBUG', []):
            if d.struct.Type == 2:  # IMAGE_DEBUG_TYPE_CODEVIEW
                cv = self.img[d.struct.AddressOfRawData:d.struct.AddressOfRawData + 24]
                if cv[:4] == b'RSDS':
                    self.identity['pdb_guid'] = cv[4:20].hex()
                    self.identity['pdb_age'] = struct.unpack_from('<I', cv, 20)[0]

    def section(self, name):
        for n, lo, hi in self.sections:
            if n == name:
                return lo, hi
        sys.exit(f'no {name} section')

    def section_of(self, rva):
        for n, lo, hi in self.sections:
            if lo <= rva < hi:
                return n
        return None

    def u32(self, rva):
        return struct.unpack_from('<I', self.img, rva)[0]


def load_functions(path, text):
    funcs = {}
    for line in open(path, encoding='utf-8', errors='replace'):
        parts = line.rstrip('\n').split('\t')
        rva, size, name = int(parts[0], 16), int(parts[1]), parts[4]
        if text[0] <= rva < text[1] and size > 0:
            funcs.setdefault(rva, (size, name))
    return sorted((rva, size, name) for rva, (size, name) in funcs.items())


def pattern(g, md, rva, size):
    """The function's unmasked byte runs: (offset, bytes) pairs."""
    code = g.img[rva:rva + size]
    masked = bytearray(size)
    i = bisect.bisect_left(g.relocs, rva - 3)
    while i < len(g.relocs) and g.relocs[i] < rva + size:
        for k in range(4):
            o = g.relocs[i] - rva + k
            if 0 <= o < size:
                masked[o] = 1
        i += 1
    for addr, n, mnem, ops in md.disasm_lite(code, rva):
        if (mnem == 'call' or mnem.startswith('j')) and ops.startswith('0x'):
            target = int(ops, 16)
            if not rva <= target < rva + size:
                disp = 4 if n >= 5 else 1  # rel32 (E8/E9, 0F 8x) or rel8
                for o in range(addr - rva + n - disp, addr - rva + n):
                    masked[o] = 1
    runs, o = [], 0
    while o < size:
        if masked[o]:
            o += 1
            continue
        e = o
        while e < size and not masked[e]:
            e += 1
        runs.append((o, code[o:e]))
        o = e
    return runs


def matches_at(s, runs, at):
    return all(s.img[at + o:at + o + len(b)] == b for o, b in runs)


def find(s, runs, size, expected, text):
    """The other build's RVA of a function with these runs, closest to expected; None if absent."""
    if matches_at(s, runs, expected):
        return expected
    anchor_o, anchor = max(runs, key=lambda r: len(r[1]))
    if len(anchor) < MIN_ANCHOR:
        return None
    for lo, hi in ((max(text[0], expected - WINDOW), min(text[1], expected + size + WINDOW)), text):
        best = None
        pos = s.img.find(anchor, lo + anchor_o, hi)
        while pos != -1:
            at = pos - anchor_o
            if matches_at(s, runs, at) and (best is None or abs(at - expected) < abs(best - expected)):
                best = at
            pos = s.img.find(anchor, pos + 1, hi)
        if best is not None:
            return best
    return None


def segments(pairs):
    """[start, end, delta] ranges of GOG addresses with one delta, from sorted (gog, delta) pairs."""
    out = []
    for a, d in pairs:
        if out and out[-1][2] == d:
            out[-1][1] = max(out[-1][1], a)
        else:
            out.append([a, a, d])
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--gog', default=GOG_EXE)
    ap.add_argument('--other', default=STEAM_EXE)
    ap.add_argument('--procs', default=os.path.join(ROOT, 'research', 'out', 'procs.tsv'))
    ap.add_argument('--out', default=os.path.join(ROOT, 'tools', 'steam_map.json'))
    args = ap.parse_args()

    g, s = Build(args.gog), Build(args.other)
    if g.base != s.base:
        sys.exit('the builds have different preferred image bases')
    gtext, stext = g.section('.text'), s.section('.text')
    funcs = load_functions(args.procs, gtext)
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    print(f'{len(funcs)} GOG functions; code {gtext[1] - gtext[0]:#x} / {stext[1] - stext[0]:#x} bytes')

    # Code ranges: consecutive matched functions with one delta; a function not found ends a range.
    matched, unmatched, text_segments, delta = [], [], [], 0
    for rva, size, name in funcs:
        runs = pattern(g, md, rva, size)
        at = find(s, runs, size, rva + delta, stext) if runs else None
        if at is None:
            unmatched.append((rva, size, name))
            text_segments.append(None)
            continue
        delta = at - rva
        matched.append((rva, size, delta, name))
        last = text_segments[-1] if text_segments else None
        if last is not None and last[2] == delta:
            last[1] = rva + size - 1
        else:
            text_segments.append([rva, rva + size - 1, delta])
    text_segments = [t for t in text_segments if t is not None]
    print(f'matched {len(matched)}, not found {len(unmatched)}')

    # Data: every relocated address inside matched code, read side by side. Code targets check the function map.
    fdelta = {rva: d for rva, size, d, name in matched}
    starts = [m[0] for m in matched]
    data_pairs, code_checks, code_bad = {}, 0, 0
    for rva, size, d, name in matched:
        i = bisect.bisect_left(g.relocs, rva)
        while i < len(g.relocs) and g.relocs[i] < rva + size:
            p = g.relocs[i]
            i += 1
            if p + 4 > rva + size or (p + d) not in s.reloc_set:
                continue
            gt, st = g.u32(p) - g.base, s.u32(p + d) - s.base
            sec = g.section_of(gt)
            if sec == '.text':
                k = bisect.bisect_right(starts, gt) - 1
                if k >= 0 and starts[k] <= gt < starts[k] + matched[k][1]:
                    code_checks += 1
                    code_bad += (st - gt) != matched[k][2]
            elif sec:
                data_pairs.setdefault(gt, set()).add(st - gt)
    conflicts = {a: sorted(ds) for a, ds in data_pairs.items() if len(ds) > 1}
    pairs = sorted((a, next(iter(ds))) for a, ds in data_pairs.items() if len(ds) == 1)
    data_segments = segments(pairs)
    print(f'code references checked {code_checks}, inconsistent {code_bad}; data addresses paired {len(pairs)}, '
          f'conflicting {len(conflicts)}')
    print(f'code ranges {len(text_segments)}, data ranges {len(data_segments)}')
    for a, b, d in data_segments[:40]:
        print(f'  data {a:#010x}-{b:#010x} ({g.section_of(a)}) delta {d:+#x}')

    biggest = sorted(unmatched, key=lambda f: -f[1])
    print('functions not found (largest first):')
    for rva, size, name in biggest[:40]:
        print(f'  {rva:#010x} {size:6} {name}')
    result = {
        'gog': g.identity, 'other': s.identity,
        'text': text_segments, 'data': data_segments,
        'unmatched': [[rva, rva + size - 1, name] for rva, size, name in unmatched],
        'stats': {'functions': len(funcs), 'matched': len(matched), 'code_refs_checked': code_checks,
                  'code_refs_inconsistent': code_bad, 'data_pairs': len(pairs), 'data_conflicts': len(conflicts)},
    }
    with open(args.out, 'w', newline='\n') as f:
        json.dump(result, f, indent=0)
    print(f'wrote {args.out}')


if __name__ == '__main__':
    main()
