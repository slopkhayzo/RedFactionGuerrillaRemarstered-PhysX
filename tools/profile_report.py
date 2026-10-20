"""Turn a run folder (telemetry.csv, profile.bin, profile_modules.csv) into report.md.

python tools/profile_report.py runs/<run> [--heavy-ms 8] [--top 30]

Heavy frames are gameplay frames slower than --heavy-ms (default: twice the median gameplay frame time).
Function names come from rfg.pdb through research/scripts (procs.tsv in RFGRE_OUT, created when missing).
"""
import argparse
import bisect
import collections
import csv
import os
import struct
import subprocess
import sys

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
sys.path.insert(0, os.path.join(ROOT, 'research', 'scripts'))
import rfgre_paths as P  # noqa: E402

SAMPLE_FMT = '<IIBBbBI8I'
SAMPLE_SIZE = struct.calcsize(SAMPLE_FMT)
HEADER_FMT = '<8sIIIII'
TIMERS = ['sim_frame_ms', 'havok_ms', 'step_ms', 'mt_jobs_ms', 'integrate_ms', 'collide_detect_ms', 'broadphase_ms',
          'collide_post_ms', 'toi_ms', 'dyn_pre_ms', 'shape_cutter_ms', 'debris_ms', 'explosion_ms', 'ai_ms',
          'rfgl_ms', 'handshake_ms', 'handoff_ms', 'deferred_render_ms', 'limiter_ms', 'render_ms']


class Symbols:
    def __init__(self):
        if not os.path.exists(os.path.join(P.OUT, 'procs.tsv')):
            subprocess.check_call([sys.executable, os.path.join(ROOT, 'research', 'scripts', 'pdbdump.py')])
        rows = []
        for line in open(os.path.join(P.OUT, 'procs.tsv')):
            a, sz, g, ft, name, mod = line.rstrip('\n').split('\t')
            rows.append((int(a, 16), int(sz), name))
        rows.sort()
        self.starts = [r[0] for r in rows]
        self.rows = rows

    def name(self, rva):
        i = bisect.bisect_right(self.starts, rva) - 1
        if i >= 0:
            a, sz, name = self.rows[i]
            if rva < a + max(sz, 1):
                return name
        return 'rfg.exe+%X' % rva


def load_frames(run):
    """Rows keyed by frame number. 'frame_ms' is replaced by the frame's own period: from its start to the start of
    the next frame, so the frame time lines up with the timers recorded during that frame."""
    frames = {}
    with open(os.path.join(run, 'telemetry.csv'), newline='') as f:
        for row in csv.DictReader(f):
            frames[int(row['frame'])] = row
    keys = sorted(frames)
    for a, b in zip(keys, keys[1:]):
        frames[a]['frame_ms'] = '%.3f' % (float(frames[b]['t_ms']) - float(frames[a]['t_ms']))
    if keys:
        frames[keys[-1]]['frame_ms'] = '0'
    return frames


def fnum(row, key):
    try:
        return float(row.get(key, 0) or 0)
    except ValueError:
        return 0.0


def pct(values, p):
    if not values:
        return 0.0
    s = sorted(values)
    return s[min(len(s) - 1, int(p * len(s)))]


def split_segments(frames):
    """Consecutive runs of gameplay frames (state 1), e.g. one Wrecking Crew round or one save load each."""
    segments, cur = [], []
    for k in sorted(frames):
        r = frames[k]
        if r['state'] == '1':
            cur.append(r)
        elif cur:
            segments.append(cur)
            cur = []
    if cur:
        segments.append(cur)
    return [s for s in segments if len(s) > 60]


def segment_table(segments):
    out = ['## Gameplay segments', '',
           '| # | start s | length s | mean FPS | p50 ms | p99 ms | 1% low FPS | max ms | max active bodies | '
           'max havok ms | mean havok ms (frames > 6 ms) |',
           '| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |']
    for i, seg in enumerate(segments, 1):
        ft = [fnum(r, 'frame_ms') for r in seg if fnum(r, 'frame_ms') > 0]
        if not ft:
            continue
        slow = [r for r in seg if fnum(r, 'frame_ms') > 6]
        low = sorted(ft)[-max(1, len(ft) // 100):]
        out.append('| %d | %.0f | %.0f | %.0f | %.2f | %.2f | %.0f | %.1f | %d | %.1f | %.2f |' % (
            i, fnum(seg[0], 't_ms') / 1000, (fnum(seg[-1], 't_ms') - fnum(seg[0], 't_ms')) / 1000,
            1000 * len(ft) / sum(ft), pct(ft, .5), pct(ft, .99), 1000 / (sum(low) / len(low)), max(ft),
            max(int(fnum(r, 'active_bodies')) for r in seg), max(fnum(r, 'havok_ms') for r in seg),
            sum(fnum(r, 'havok_ms') for r in slow) / len(slow) if slow else 0.0))
    return out + ['']


def load_samples(run):
    path = os.path.join(run, 'profile.bin')
    if not os.path.exists(path):
        return None, []
    data = open(path, 'rb').read()
    hsize = struct.calcsize(HEADER_FMT)
    magic, ssize, exe_base, text_lo, text_hi, interval = struct.unpack_from(HEADER_FMT, data, 0)
    assert magic == b'RFGPROF1' and ssize == SAMPLE_SIZE, (magic, ssize)
    samples = [struct.unpack_from(SAMPLE_FMT, data, off) for off in range(hsize, len(data) - SAMPLE_SIZE + 1, SAMPLE_SIZE)]
    return {'exe_base': exe_base, 'interval_us': interval}, samples


def load_modules(run):
    mods = []
    path = os.path.join(run, 'profile_modules.csv')
    if os.path.exists(path):
        with open(path, newline='') as f:
            for row in csv.DictReader(f):
                mods.append((int(row['base'], 16), int(row['size']), row['name']))
    return mods


OBJECT_TYPES = {-1: 'undefined', 0: 'human', 1: 'item', 2: 'mover', 3: 'vehicle', 4: 'effect', 5: 'debris',
                6: 'turret'}


def census_table(run, file_name, title, top=25):
    """Object census (creations.csv / deletions.csv, whole session): gameplay counts by calling function and type.
    Calls through a forwarding function (world::create_object<T>, the by-handle destroy_object) count for its caller."""
    path = os.path.join(run, file_name)
    if not os.path.exists(path):
        return []
    syms = Symbols()
    by_fn = collections.Counter()
    with open(path, newline='') as f:
        for row in csv.DictReader(f):
            rva = int(row['caller_rva'], 16)
            outer = int(row.get('outer_rva') or '0', 16)
            if rva == 0xFFFFFFFF:
                name = 'lost (census table full)'
            elif outer:
                name = '%s (via %s)' % (syms.name(outer), syms.name(rva))
            else:
                name = syms.name(rva)
            kind = OBJECT_TYPES.get(int(row['obj_type']), 'type %s' % row['obj_type'])
            by_fn[(name, kind)] += int(row['gameplay_count'])
    rows = [(k, v) for k, v in by_fn.most_common(top) if v]
    if not rows:
        return []
    out = ['## %s' % title, '', '| count | object type | caller |', '| --- | --- | --- |']
    out += ['| %d | %s | %s |' % (v, kind, name) for (name, kind), v in rows]
    return out + ['']


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('run')
    ap.add_argument('--heavy-ms', type=float, default=0.0)
    ap.add_argument('--top', type=int, default=30)
    ap.add_argument('--all-states', action='store_true', help='use all frames, not only gameplay (menus, loading)')
    ap.add_argument('--segment', type=int, default=0,
                    help='only this gameplay segment (1-based; -1 = last), e.g. one Wrecking Crew round')
    ap.add_argument('--skip-s', type=float, default=0.0, help='ignore the first seconds of each segment')
    ap.add_argument('--out', default='report.md', help='report file name inside the run folder')
    args = ap.parse_args()
    run = args.run
    frames = load_frames(run)
    segments = split_segments(frames)
    gp = [r for r in frames.values() if (args.all_states or r['state'] == '1') and fnum(r, 'frame_ms') > 0]
    out = ['# Run report: %s' % os.path.basename(os.path.normpath(run)), '']
    if segments:
        out += segment_table(segments)
    if args.segment and not segments:
        gp = []
    elif args.segment:
        seg = segments[args.segment - 1] if args.segment > 0 else segments[-1]
        t0 = fnum(seg[0], 't_ms') + args.skip_s * 1000
        gp = [r for r in seg if fnum(r, 't_ms') >= t0 and fnum(r, 'frame_ms') > 0]
        out += ['Details below: segment %d only (%.1f s).' % (segments.index(seg) + 1,
                                                              (fnum(seg[-1], 't_ms') - fnum(seg[0], 't_ms')) / 1000), '']
    if not gp:
        out.append('No gameplay frames in this run.')
    else:
        ft = [fnum(r, 'frame_ms') for r in gp]
        median = pct(ft, 0.5)
        heavy_ms = args.heavy_ms or 2 * median
        heavy = [r for r in gp if fnum(r, 'frame_ms') > heavy_ms]
        out += ['## Frame times (gameplay)', '',
                '| frames | mean | p50 | p95 | p99 | max | mean FPS | heavy frames (> %.1f ms) |' % heavy_ms,
                '| --- | --- | --- | --- | --- | --- | --- | --- |',
                '| %d | %.2f | %.2f | %.2f | %.2f | %.2f | %.0f | %d (%.1f%%) |' % (
                    len(gp), sum(ft) / len(ft), median, pct(ft, .95), pct(ft, .99), max(ft), 1000 * len(ft) / sum(ft),
                    len(heavy), 100.0 * len(heavy) / len(gp)), '']
        out += ['## Where the sim-thread time goes (mean ms per frame)', '',
                '| timer | all gameplay frames | heavy frames |', '| --- | --- | --- |']
        for t in ['frame_ms', 'do_frame_ms'] + TIMERS:
            a = sum(fnum(r, t) for r in gp) / len(gp)
            h = sum(fnum(r, t) for r in heavy) / len(heavy) if heavy else 0.0
            out.append('| %s | %.3f | %.3f |' % (t, a, h))
        work = lambda r: fnum(r, 'do_frame_ms') - fnum(r, 'limiter_ms') - fnum(r, 'handshake_ms')
        out.append('| sim work (do_frame - limiter - handshake) | %.3f | %.3f |' % (
            sum(work(r) for r in gp) / len(gp), sum(work(r) for r in heavy) / len(heavy) if heavy else 0.0))
        gap = lambda r: fnum(r, 'frame_ms') - fnum(r, 'do_frame_ms')
        out.append('| between frames (outside rfg_do_frame) | %.3f | %.3f |' % (
            sum(gap(r) for r in gp) / len(gp), sum(gap(r) for r in heavy) / len(heavy) if heavy else 0.0))
        for c in ['contacts_added', 'debris_contacts', 'active_bodies', 'active_islands', 'inactive_bodies', 'rfg_load',
                  'recent_history',
                  'draw_calls', 'gov_del_debris', 'gov_del_movers', 'act_movers', 'act_debris', 'act_humans',
                  'act_vehicles', 'act_items', 'act_other', 'lay_rfg_normal', 'lay_rfg_lite', 'lay_rfg_ultra_lite',
                  'lay_rfg_world_only', 'lay_effect_rb', 'lay_other']:
            if c not in gp[0]:
                continue
            vals = [fnum(r, c) for r in gp]
            hv = [fnum(r, c) for r in heavy] or [0]
            out.append('| %s (mean / max; heavy mean) | %.1f / %.0f | %.1f |' % (c, sum(vals) / len(vals), max(vals),
                                                                               sum(hv) / len(hv)))
        out += ['', '## Slowest frames', '', '| frame | frame_ms | ' + ' | '.join(TIMERS[:9]) + ' | bodies | contacts |',
                '|' + ' --- |' * 13]
        for r in sorted(gp, key=lambda r: -fnum(r, 'frame_ms'))[:20]:
            out.append('| %s | %.2f | %s | %s | %s |' % (r['frame'], fnum(r, 'frame_ms'),
                                                        ' | '.join('%.2f' % fnum(r, t) for t in TIMERS[:9]),
                                                        r.get('active_bodies', ''), r.get('contacts_added', '')))
        heavy_frames = {int(r['frame']) for r in heavy}
        selected_frames = {int(r['frame']) for r in gp}

        hdr, samples = load_samples(run)
        if args.segment:
            samples = [s for s in samples if s[1] in selected_frames]
        if samples:
            syms = Symbols()
            mods = load_modules(run)
            base = hdr['exe_base']

            def where(addr):
                rva = addr - base
                if 0x1000 <= rva < 0x1000 + 0xE3DFCB:
                    return syms.name(rva)
                for mb, ms, mn in mods:
                    if mb <= addr < mb + ms:
                        return '[%s]' % mn
                return '[unknown]'

            for thread, tname in ((0, 'sim'), (1, 'render'), (2, 'scene')):
                for label, flt in (('all gameplay samples', None), ('samples in heavy frames', heavy_frames)):
                    ss = [s for s in samples if s[2] == thread and (flt is None or s[1] in flt)]
                    if not ss:
                        continue
                    self_c = collections.Counter()
                    incl_c = collections.Counter()
                    for s in ss:
                        eip, nret, rets = s[6], s[3], s[7:7 + s[3]]
                        here = where(eip)
                        self_c[here] += 1
                        seen = {here}
                        for ra in rets:
                            seen.add(where(ra))
                        for n in seen:
                            incl_c[n] += 1
                    out += ['', '## Profile: %s thread, %s (%d samples)' % (tname, label, len(ss)), '',
                            'Self is exact (the sampled instruction). Inclusive comes from a scan of the stack for '
                            'return addresses and also counts stale ones: treat it as an upper bound.', '',
                            '| self % | function | | inclusive % | function |', '| --- | --- | --- | --- | --- |']
                    top_self = self_c.most_common(args.top)
                    top_incl = incl_c.most_common(args.top)
                    for i in range(max(len(top_self), len(top_incl))):
                        a = top_self[i] if i < len(top_self) else ('', 0)
                        b = top_incl[i] if i < len(top_incl) else ('', 0)
                        out.append('| %s | %s | | %s | %s |' % ('%.1f' % (100.0 * a[1] / len(ss)) if a[0] else '',
                                                               a[0], '%.1f' % (100.0 * b[1] / len(ss)) if b[0] else '',
                                                               b[0]))
    out += census_table(run, 'creations.csv', 'Objects created during gameplay, by calling function')
    out += census_table(run, 'fades.csv', 'Pieces marked to fade out during gameplay, by calling function')
    out += census_table(run, 'deletions.csv', 'Objects destroyed during gameplay, by calling function')
    report = os.path.join(run, args.out)
    with open(report, 'w', encoding='utf-8') as f:
        f.write('\n'.join(out) + '\n')
    print('\n'.join(out[:30]))
    print('...\nwrote', report)


if __name__ == '__main__':
    main()
