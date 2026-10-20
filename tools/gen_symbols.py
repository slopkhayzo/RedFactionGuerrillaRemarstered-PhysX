"""Generate src/rfgphys/game/rfg_symbols.generated.h from rfg.pdb and the Steam build's map.

python tools/gen_symbols.py [--list tools/symbols.txt] [--map tools/steam_map.json]
                            [--out src/rfgphys/game/rfg_symbols.generated.h]

The addresses come from the GOG build's PDB, read with the analysis modules in research/scripts (their tables,
procs.tsv, data.tsv and publics.tsv, are created with research/scripts/pdbdump.py when missing; set RFGRE_OUT to reuse
an existing output folder). The Steam build was compiled from the same source and ships no PDB: tools/match_builds.py
found each GOG function and global in its code, and its map gives every address here a Steam address. An address it
cannot map stops the generation.
"""
import argparse
import bisect
import json
import os
import re
import struct
import subprocess
import sys
import uuid

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
SCRIPTS = os.path.join(ROOT, 'research', 'scripts')
sys.path.insert(0, SCRIPTS)
import rfgre_paths as P  # noqa: E402

if not os.path.exists(os.path.join(P.OUT, 'procs.tsv')):
    subprocess.check_call([sys.executable, os.path.join(SCRIPTS, 'pdbdump.py')])

import rdis  # noqa: E402
import sig as sigmod  # noqa: E402  (prints nothing when imported without arguments)
import tpi  # noqa: E402
from pdbmsf import PDB  # noqa: E402


def ident(name):
    return re.sub(r'[^0-9A-Za-z_]', '_', name.replace('::', '__'))


def proc_signatures():
    sigs = {}
    for line in open(os.path.join(P.OUT, 'procs.tsv')):
        a, sz, g, ft, name, mod = line.rstrip('\n').split('\t')
        try:
            sigs[int(a, 16)] = sigmod.sig(int(ft, 16))
        except Exception:
            sigs[int(a, 16)] = '?'
    return sigs


def data_types():
    types = {}
    for line in open(os.path.join(P.OUT, 'data.tsv')):
        a, t, k, name, mod = line.rstrip('\n').split('\t')
        types.setdefault((name, int(a, 16)), int(t, 16))
    return types


def publics():
    pubs = {}
    for line in open(os.path.join(P.OUT, 'publics.tsv'), encoding='utf-8', errors='replace'):
        a, flags, name = line.rstrip('\n').split('\t')
        pubs.setdefault(name, set()).add(int(a, 16))
    return pubs


def build_identity():
    pe_ts = struct.unpack_from('<I', rdis.img, rdis.pe.DOS_HEADER.e_lfanew + 8)[0]
    size_of_image = rdis.pe.OPTIONAL_HEADER.SizeOfImage
    info = PDB(P.PDB).msf.read(1)
    ver, sig, age = struct.unpack_from('<III', info, 0)
    guid = uuid.UUID(bytes_le=bytes(info[12:28]))
    return pe_ts, size_of_image, guid, age


class BuildMap:
    """GOG RVA -> the other build's, from tools/match_builds.py's ranges."""

    def __init__(self, path):
        m = json.load(open(path))
        self.gog, self.other = m['gog'], m['other']
        self.text = sorted(m['text'])
        self.data = sorted(m['data'])
        self.unmatched = sorted(m['unmatched'])
        self._t = [t[0] for t in self.text]
        self._d = [d[0] for d in self.data]
        self._u = [u[0] for u in self.unmatched]

    def code(self, a):
        k = bisect.bisect_right(self._u, a) - 1
        if k >= 0 and self.unmatched[k][0] <= a <= self.unmatched[k][1]:
            return None, 'differs in that build (%s)' % self.unmatched[k][2]
        k = bisect.bisect_right(self._t, a) - 1
        if k >= 0 and self.text[k][0] <= a <= self.text[k][1]:
            return a + self.text[k][2], ''
        return None, 'outside the matched code'

    def data_(self, a):
        k = bisect.bisect_right(self._d, a) - 1
        if k >= 0 and self.data[k][0] <= a <= self.data[k][1]:
            return a + self.data[k][2], ''
        prev = self.data[k] if k >= 0 else None
        nxt = self.data[k + 1] if k + 1 < len(self.data) else None
        if prev and nxt and prev[2] == nxt[2]:
            return a + prev[2], ''
        return None, 'between data ranges with different shifts (%s / %s)' % (prev, nxt)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--list', default=os.path.join(ROOT, 'tools', 'symbols.txt'))
    ap.add_argument('--map', default=os.path.join(ROOT, 'tools', 'steam_map.json'))
    ap.add_argument('--out', default=os.path.join(ROOT, 'src', 'rfgphys', 'game', 'rfg_symbols.generated.h'))
    args = ap.parse_args()

    sigs = proc_signatures()
    dtypes = data_types()
    pubs = publics()
    pe_ts, soi, guid, age = build_identity()
    if str(guid) != P.EXPECTED_PDB_GUID or age != P.EXPECTED_PDB_AGE:
        sys.exit('PDB %s age %d is not the documented build' % (guid, age))
    steam = BuildMap(args.map)
    if (steam.gog['pdb_guid'] != guid.bytes_le.hex() or steam.gog['pdb_age'] != age or
            steam.gog['timestamp'] != pe_ts or steam.gog['size_of_image'] != soi):
        sys.exit('%s was made from another GOG build: run tools/match_builds.py again' % args.map)

    # (C++ name, GOG RVA, Steam RVA, comment); functions also have a size.
    fns, vars_, enums, errors = [], [], [], []
    for raw in open(args.list):
        line = raw.split('#', 1)[0].strip()
        if not line:
            continue
        kind, rest = line.split(None, 1)
        rest, _, alias = rest.partition(' as ')
        # fn NAME @RVA picks one of several functions of that name; public names can contain '@' themselves.
        m = re.match(r'^(.*?)\s+@([0-9A-Fa-f]{8})$', rest.strip()) if kind == 'fn' else None
        name, at = (m.group(1).strip(), m.group(2)) if m else (rest.strip(), '')
        alias = alias.strip()
        if kind == 'fn':
            cands = sorted(a for a in set(rdis.byname.get(name, [])) if a in rdis.procs)
            if at:
                want = int(at, 16)
                if want not in cands:
                    sys.exit('%s has no function at %s (candidates %s)' % (name, at, [hex(c) for c in cands]))
                cands = [want]
            if len(cands) != 1:
                sys.exit('%s: expected one function, found %s' % (name, [hex(c) for c in cands]))
            a = cands[0]
            s, why = steam.code(a)
            if s is None:
                errors.append('fn %s (%08X): %s' % (name, a, why))
            size = rdis.procsizes[a]
            fns.append((alias or ident(name), a, s, '%s, %d bytes: %s' % (name, size, sigs.get(a, '?')), size))
        elif kind in ('var', 'pub'):
            if kind == 'var':
                cands = sorted({a for (n, a) in dtypes if n == name})
                what = lambda a: tpi.tname(dtypes[(name, a)])  # noqa: E731
            else:
                cands = sorted(pubs.get(name, ()))
                what = lambda a: name  # noqa: E731
            if len(cands) != 1:
                sys.exit('%s: expected one %s, found %s' % (name, 'global' if kind == 'var' else 'public symbol',
                                                            [hex(c) for c in cands]))
            a = cands[0]
            s, why = steam.data_(a)
            if s is None:
                errors.append('%s %s (%08X): %s' % (kind, name, a, why))
            vars_.append((alias or ident(name), a, s, what(a)))
        elif kind == 'enum':
            r = tpi.layout(name)
            if not r:
                sys.exit('enum %s not found' % name)
            enums.append((name, [(v, n) for k, v, n, t in r[2] if k == 'enum']))
        else:
            sys.exit('unknown entry: ' + raw)
    if errors:
        sys.exit('no Steam address for:\n  ' + '\n  '.join(errors))

    def guid_bytes(hexstr):
        return ', '.join('0x%02X' % b for b in bytes.fromhex(hexstr))

    o = steam.other
    out = []
    out.append('// Generated by tools/gen_symbols.py from rfg.pdb (GOG build) and tools/steam_map.json (Steam build).')
    out.append('// Do not edit; edit tools/symbols.txt instead.')
    out.append('#pragma once')
    out.append('#include <cstdint>')
    out.append('')
    out.append('namespace rfg {')
    out.append('')
    out.append('// The builds of rfg.exe these addresses cover, told apart by the PDB GUID and age in their debug directory.')
    out.append('// The GOG build ships its PDB; the Steam build, compiled from the same source, was mapped onto it by')
    out.append('// tools/match_builds.py.')
    out.append('struct BuildId {')
    out.append('    const char* name;')
    out.append('    uint32_t exe_timestamp, exe_size_of_image;')
    out.append('    uint8_t pdb_guid[16];')
    out.append('    uint32_t pdb_age;')
    out.append('};')
    out.append('inline constexpr int kBuildCount = 2;')
    out.append('inline constexpr BuildId kBuilds[kBuildCount] = {')
    out.append('    {"GOG", 0x%08X, 0x%08X, {%s}, %d},' % (pe_ts, soi, guid_bytes(guid.bytes_le.hex()), age))
    out.append('    {"Steam", 0x%08X, 0x%08X, {%s}, %d},' % (o['timestamp'], o['size_of_image'],
                                                           guid_bytes(o['pdb_guid']), o['pdb_age']))
    out.append('};')
    out.append('')
    out.append('// RVAs in the running build: the GOG build\'s until select_build() sets another build\'s (rfg.h).')
    out.append('namespace rva {')
    for cname, a, s, comment, size in fns:
        out.append('inline uint32_t %s = 0x%08X;  // Steam 0x%08X; %s' % (cname, a, s, comment))
    out.append('')
    for cname, a, s, t in vars_:
        out.append('inline uint32_t %s = 0x%08X;  // Steam 0x%08X; %s' % (cname, a, s, t))
    out.append('}  // namespace rva')
    out.append('')
    out.append('// Every address above in each build, in the order of kBuilds.')
    out.append('struct RvaEntry {')
    out.append('    uint32_t* rva;')
    out.append('    uint32_t in_build[kBuildCount];')
    out.append('};')
    out.append('inline constexpr RvaEntry kRvaTable[] = {')
    for cname, a, s, *_ in fns + vars_:
        out.append('    {&rva::%s, {0x%08X, 0x%08X}},' % (cname, a, s))
    out.append('};')
    out.append('')
    out.append('// Code size of each function above, the same in every build (call-site range checks: return address - rva <')
    out.append('// size).')
    out.append('namespace size {')
    for cname, a, s, comment, size in fns:
        out.append('inline constexpr uint32_t %s = %d;' % (cname, size))
    out.append('}  // namespace size')
    out.append('')
    out.append('// The Steam build\'s code ranges (GOG RVA + delta = Steam RVA), for diagnostics: stack and profiler addresses')
    out.append('// in the Steam build are reported as the GOG build\'s, which its PDB names.')
    out.append('struct CodeRange {')
    out.append('    uint32_t gog_first, gog_last;')
    out.append('    int32_t delta;')
    out.append('};')
    out.append('inline constexpr CodeRange kSteamCode[] = {')
    for lo, hi, d in steam.text:
        out.append('    {0x%08X, 0x%08X, %d},' % (lo, hi, d))
    out.append('};')
    for name, items in enums:
        out.append('')
        out.append('struct %s_name { int value; const char* name; };' % ident(name))
        out.append('inline constexpr %s_name %s_names[] = {' % (ident(name), ident(name)))
        for v, n in items:
            out.append('    {%d, "%s"},' % (v, n))
        out.append('};')
    out.append('')
    out.append('}  // namespace rfg')
    os.makedirs(os.path.dirname(args.out), exist_ok=True)
    with open(args.out, 'w', newline='\n') as f:
        f.write('\n'.join(out) + '\n')
    print('wrote %s: %d functions, %d globals, %d enums, Steam addresses for all of them; %d Steam code ranges'
          % (args.out, len(fns), len(vars_), len(enums), len(steam.text)))


if __name__ == '__main__':
    main()
