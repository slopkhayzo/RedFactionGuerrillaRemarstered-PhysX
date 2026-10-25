"""Inspect record/replay harness files.

python tools/rec_tool.py list                       recordings with their manifests
python tools/rec_tool.py info RECORDING             header, frame-time and input statistics of input.rfgrec
python tools/rec_tool.py compare A B                checkpoint-by-checkpoint comparison of two checkpoints.csv
                                                    (a recording folder or a run folder each)
python tools/rec_tool.py tracediff A B              first differing random draw of two rng_trace.csv
                                                    (runs made with Harness.TraceRng=1)
python tools/rec_tool.py stats RUN...               gameplay totals of run folders (frame times, contacts,
                                                    debris-debris contacts, active bodies, governor deletions)
python tools/rec_tool.py trackdiff A B              how far two player tracks (track.bin) are apart: player,
                                                    camera, shots (a recording folder or a run folder each)
python tools/rec_tool.py adopt-track RUN [--force]  makes a vanilla replay's track.bin the reference track of
                                                    its recording (replays then lock camera, player and shots)
python tools/rec_tool.py salvage RECORDING [OUT]    an unfinished recording (the game hung or crashed before the
                                                    match ended): a copy that replays the frames that reached the
                                                    disk (frame count set, a cut-off last frame dropped, no track);
                                                    OUT defaults to RECORDING-salvaged

RECORDING is a folder name under recordings/ or a path. Format: src/rfgphys/harness/rec_file.h.
"""
import json
import os
import shutil
import struct
import sys

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
INPUT_SIZE = 0x444
# Header, FrameClock (framecount, total ms x2, sample count, frametime total, fps, frametimes[16], tickers x2,
# amount_rounded), reserved.
HEADER = struct.Struct('<8sIIIIIIiII44s4i64sIB3s64s' + 'iIIiff16fiif' + '64s')
START_TYPES = {0: 'none', 1: 'save', 2: 'wrecking_crew'}


def resolve(name):
    if os.path.isdir(name):
        return name
    return os.path.join(ROOT, 'recordings', name)


def read_recording(folder):
    data = open(os.path.join(folder, 'input.rfgrec'), 'rb').read()
    h = HEADER.unpack_from(data, 0)
    header = dict(magic=h[0], header_size=h[1], input_size=h[2], seed=h[3], start_type=START_TYPES.get(h[4], h[4]),
                  frame_count=h[5], flags=h[6], clock_anchor_ms=h[7], checkpoint_interval=h[8],
                  exe_timestamp='%08X' % h[9], wc_options=h[10].hex(), wc_character_index=list(h[11:15]),
                  wc_map_file=h[15].split(b'\0')[0].decode(), save_slot=h[16], save_reset_destruction=h[17],
                  save_file=h[19].split(b'\0')[0].decode(), clock_framecount=h[20], clock_fps=round(h[25], 2),
                  clock_ticker=h[42], clock_realtime_ticker=h[43])
    if header['magic'] != b'RFGREC01' or header['header_size'] != HEADER.size:
        raise SystemExit('not an RFGREC01 file')
    frames = []
    pos = HEADER.size
    prev = bytearray(INPUT_SIZE)
    while pos < len(data):
        tag, flags, end_state, n_gov = struct.unpack_from('<BBBB', data, pos)
        if tag != 0xA5:
            print('bad record tag at offset %d' % pos)
            break
        dt, dt_real, rng_index, crt_rand = struct.unpack_from('<ffHI', data, pos + 4)
        pos += 18
        gov = list(struct.unpack_from('<%di' % n_gov, data, pos))
        pos += 4 * n_gov
        (runs,) = struct.unpack_from('<H', data, pos)
        pos += 2
        changed = 0
        for _ in range(runs):
            off, length = struct.unpack_from('<HH', data, pos)
            pos += 4
            prev[off:off + length] = data[pos:pos + length]
            pos += length
            changed += length
        frames.append(dict(flags=flags, end_state=end_state, dt=dt, dt_real=dt_real, rng_index=rng_index,
                           crt_rand=crt_rand, gov=gov, changed=changed, input=bytes(prev)))
    return header, frames


def cmd_list():
    base = os.path.join(ROOT, 'recordings')
    for name in sorted(os.listdir(base)) if os.path.isdir(base) else []:
        m = os.path.join(base, name, 'manifest.json')
        if not os.path.exists(m):
            print('%-60s (no manifest: unfinished)' % name)
            continue
        j = json.load(open(m))
        start = j['start']
        what = start.get('setup', '') if start['type'] == 'wrecking_crew' else 'save slot %s' % start.get('slot')
        print('%-60s %6d frames %7.1f s  %s%s' % (name, j['frames'], j['game_seconds'], what,
                                                  '  [synthetic]' if j.get('synthetic_input') else ''))


def cmd_info(name):
    folder = resolve(name)
    header, frames = read_recording(folder)
    for k, v in header.items():
        print('%-24s %s' % (k, v))
    if not frames:
        return
    body = frames[1:]
    dts = [f['dt'] for f in body]
    print('records                  %d (transition frame + %d)' % (len(frames), len(body)))
    if dts:
        print('game time                %.3f s, dt min %.4f mean %.4f max %.4f' % (
            sum(dts), min(dts), sum(dts) / len(dts), max(dts)))
    active = sum(1 for f in body if f['changed'])
    print('frames with input change %d' % active)
    print('frames without focus     %d' % sum(1 for f in body if not (f['flags'] & 1)))
    print('markers                  %s' % [i for i, f in enumerate(body) if f['flags'] & 2])
    gov = [g for f in body for g in f['gov']]
    if gov:
        print('governor readings        %d, mean %.0f us, max %d us' % (len(gov), sum(gov) / len(gov), max(gov)))
    states = {}
    for f in body:
        states[f['end_state']] = states.get(f['end_state'], 0) + 1
    print('frames per end state     %s' % states)


def read_checkpoints(path):
    if os.path.isdir(path):
        path = os.path.join(path, 'checkpoints.csv')
    rows = {}
    with open(path) as f:
        header = f.readline().strip().split(',')
        for line in f:
            vals = line.strip().split(',')
            if len(vals) == len(header):
                rows[int(vals[0])] = dict(zip(header, vals))
    return rows


def cmd_compare(a, b):
    """Checkpoint comparison. Gameplay fields decide divergence; rng (which also covers cosmetic call sites such as
    audio variations) is reported separately."""
    ra, rb = read_checkpoints(resolve(a)), read_checkpoints(resolve(b))
    gameplay = ['state', 'player', 'objects', 'havok', 'n_objects', 'n_active', 'movers_destroyed']
    common = sorted(set(ra) & set(rb))
    first = None
    counts = {k: 0 for k in gameplay + ['rng']}
    mismatched = 0
    for fr in common:
        for k in counts:
            if ra[fr][k] != rb[fr][k]:
                counts[k] += 1
        diff = [k for k in gameplay if ra[fr][k] != rb[fr][k]]
        if diff:
            mismatched += 1
            if first is None:
                first = fr
                print('first gameplay difference at frame %d: %s' % (fr, ', '.join(diff)))
                for k in diff + ['player_x', 'player_y', 'player_z']:
                    print('   %-18s %-14s %s' % (k, ra[fr][k], rb[fr][k]))
    print('%d common checkpoints, %d differ in gameplay fields; per field: %s' % (
        len(common), mismatched, ', '.join('%s %d' % kv for kv in counts.items())))


def cmd_tracediff(a, b):
    """First differing random draw between two rng_trace.csv files (Harness.TraceRng=1 runs), gameplay frames only
    (loading frames, traced as frame -1, differ in number from run to run by design)."""
    def load(path):
        if os.path.isdir(path):
            path = os.path.join(path, 'rng_trace.csv')
        with open(path) as f:
            f.readline()
            return [line.strip() for line in f if not line.startswith('-')]
    ta, tb = load(resolve(a)), load(resolve(b))
    n = min(len(ta), len(tb))
    for i in range(n):
        if ta[i] != tb[i]:
            print('draws: %d vs %d; first difference at draw %d:' % (len(ta), len(tb), i))
            for j in range(max(0, i - 6), min(n, i + 6)):
                mark = '>>' if j == i else '  '
                print('%s %-48s %s' % (mark, ta[j], tb[j]))
            return
    print('identical for %d draws (lengths %d and %d)' % (n, len(ta), len(tb)))


def cmd_stats(run):
    """Gameplay totals of a run folder's telemetry.csv (frames in GS_GAMEPLAY)."""
    import csv
    rows = [r for r in csv.DictReader(open(os.path.join(run, 'telemetry.csv'))) if r['state'] == '1']
    if not rows:
        print('%s: no gameplay frames' % os.path.basename(run))
        return
    num = lambda r, k: float(r.get(k) or 0)
    ft = sorted(num(r, 'frame_ms') for r in rows if num(r, 'frame_ms') > 0)
    p = lambda q: ft[min(len(ft) - 1, int(q * len(ft)))] if ft else 0.0
    print('%s: %d frames, frame ms p50 %.2f p95 %.2f p99 %.2f max %.2f; contacts %d, debris-debris contacts %d; '
          'max active bodies %d; movers destroyed %d; governor deleted %d debris, %d movers' % (
              os.path.basename(run), len(rows), p(0.5), p(0.95), p(0.99), ft[-1] if ft else 0,
              sum(num(r, 'contacts_added') for r in rows), sum(num(r, 'debris_contacts') for r in rows),
              max(num(r, 'active_bodies') for r in rows), max(num(r, 'movers_destroyed') for r in rows),
              sum(num(r, 'gov_del_debris') for r in rows), sum(num(r, 'gov_del_movers') for r in rows)))


TRACK_HEADER = struct.Struct('<8sIII5I')
TRACK_POSE = struct.Struct('<II3f9f3f9f')   # frame, flags (1 camera, 2 player), camera pos/orient, player pos/orient
TRACK_SHOT = struct.Struct('<II3f3f')        # frame, seq, start, target


def read_track(path):
    """track.bin (src/rfgphys/harness/track.h): {frame: pose tuple}, [shot tuples]."""
    data = open(path, 'rb').read()
    magic, version, pose_size, shot_size = TRACK_HEADER.unpack_from(data, 0)[:4]
    if magic != b'RFGTRK01' or pose_size != TRACK_POSE.size or shot_size != TRACK_SHOT.size:
        sys.exit('%s: not a track this tool can read' % path)
    poses, shots, off = {}, [], TRACK_HEADER.size
    while off < len(data):
        tag = data[off]
        off += 1
        if tag == ord('P') and off + TRACK_POSE.size <= len(data):
            p = TRACK_POSE.unpack_from(data, off)
            poses[p[0]] = p
            off += TRACK_POSE.size
        elif tag == ord('S') and off + TRACK_SHOT.size <= len(data):
            shots.append(TRACK_SHOT.unpack_from(data, off))
            off += TRACK_SHOT.size
        else:
            break
    return poses, shots


def track_path(name):
    path = resolve(name)
    return os.path.join(path, 'track.bin') if os.path.isdir(path) else path


def cmd_trackdiff(a, b):
    """How far two tracks (recordings or run folders) are apart: player and camera positions, and the shots."""
    import math
    pa, sa = read_track(track_path(a))
    pb, sb = read_track(track_path(b))
    dist = lambda u, v: math.sqrt(sum((x - y) ** 2 for x, y in zip(u, v)))
    common = sorted(set(pa) & set(pb))
    worst_player = worst_camera = (0.0, -1)
    first_player = None
    for f in common:
        x, y = pa[f], pb[f]
        if x[1] & 2 and y[1] & 2:
            d = dist(x[14:17], y[14:17])
            worst_player = max(worst_player, (d, f))
            if first_player is None and d > 0.01:
                first_player = f
        if x[1] & 1 and y[1] & 1:
            worst_camera = max(worst_camera, (dist(x[2:5], y[2:5]), f))
    print('%d common frames; player: max %.3f m apart (frame %d), first > 1 cm at frame %s; camera: max %.3f m apart '
          '(frame %d)' % (len(common), worst_player[0], worst_player[1], first_player, worst_camera[0],
                          worst_camera[1]))
    ka = {(s[0], s[1]): s for s in sa}
    kb = {(s[0], s[1]): s for s in sb}
    both = sorted(set(ka) & set(kb))
    same = sum(1 for k in both if ka[k][2:] == kb[k][2:])
    print('shots: %d and %d; %d fired in the same frame by both, %d of them with identical start and target' % (
        len(sa), len(sb), len(both), same))


def cmd_adopt_track(run, force=False):
    """Makes a vanilla replay's track.bin the reference track of its recording."""
    result = json.load(open(os.path.join(run, 'replay.json')))
    if not result.get('completed'):
        sys.exit('%s: the replay did not complete' % run)
    log = open(os.path.join(run, 'rfgphys.log'), encoding='utf-8', errors='replace').read(6000)
    if 'exp_debris_collide=0' not in log:
        sys.exit('%s: the replay ran an experiment; only a vanilla replay can be the reference' % run)
    src = os.path.join(run, 'track.bin')
    dst = os.path.join(result['recording'], 'track.bin')
    if os.path.exists(dst) and not force:
        sys.exit('%s already has a track (use --force to replace it)' % result['recording'])
    poses, shots = read_track(src)
    import shutil
    shutil.copyfile(src, dst)
    print('track of %s adopted by %s: %d frames, %d shots' % (os.path.basename(run), result['recording'], len(poses),
                                                             len(shots)))


def complete_frames(data):
    """Records of input.rfgrec that are whole: (count including the transition frame, end offset of the last)."""
    pos, count = HEADER.size, 0
    try:
        while pos < len(data):
            tag, flags, end_state, n_gov = struct.unpack_from('<BBBB', data, pos)
            if tag != 0xA5:
                break
            p = pos + 18 + 4 * n_gov
            (runs,) = struct.unpack_from('<H', data, p)
            p += 2
            for _ in range(runs):
                off, length = struct.unpack_from('<HH', data, p)
                p += 4 + length
            if p > len(data):
                break
            pos = p
            count += 1
    except struct.error:
        pass
    return count, pos


def cmd_salvage(name, out=None):
    folder = resolve(name)
    if os.path.exists(os.path.join(folder, 'manifest.json')):
        sys.exit('%s has a manifest: it finished, nothing to salvage' % folder)
    data = open(os.path.join(folder, 'input.rfgrec'), 'rb').read()
    count, end = complete_frames(data)
    if count < 2:
        sys.exit('%s: no frame after the transition frame reached the disk' % folder)
    frames = count - 1  # the header counts the frames after the transition frame
    out = out or folder.rstrip('\\/') + '-salvaged'
    if os.path.exists(out):
        sys.exit('%s exists already' % out)
    os.makedirs(out)
    fixed = bytearray(data[:end])
    struct.pack_into('<I', fixed, 24, frames)  # Header.frame_count: after magic, header_size, input_size, seed, start_type
    open(os.path.join(out, 'input.rfgrec'), 'wb').write(fixed)
    header, records = read_recording(out)
    assert header['frame_count'] == frames and len(records) == count
    game_s = sum(r['dt'] for r in records[1:])
    wall_s = sum(r['dt_real'] for r in records[1:])
    kept = 0
    src = os.path.join(folder, 'checkpoints.csv')
    lines = open(src, encoding='utf-8').read().splitlines() if os.path.exists(src) else []
    if lines:  # an empty file: the game never flushed it
        keep = lines[:1] + [ln for ln in lines[1:] if ln.split(',', 1)[0].isdigit() and int(ln.split(',', 1)[0]) <= frames]
        kept = len(keep) - 1
        open(os.path.join(out, 'checkpoints.csv'), 'w', encoding='utf-8', newline='\n').write('\n'.join(keep) + '\n')
    # Newer builds write the track at the same pace as the input; replays read it up to its first incomplete record.
    track = os.path.join(folder, 'track.bin')
    has_track = os.path.exists(track) and os.path.getsize(track) > 16
    if has_track:
        shutil.copyfile(track, os.path.join(out, 'track.bin'))
    h = HEADER.unpack_from(fixed, 0)
    start = {'type': header['start_type'], 'map_file': header['wc_map_file'], 'setup': 'salvaged; see %s' % name,
             'options_hex': h[10].hex().upper(), 'character_index': header['wc_character_index']}
    if header['start_type'] == 'save':
        start = {'type': 'save', 'slot': header['save_slot'], 'file': header['save_file']}
    manifest = {
        'format': 'rfgrec1', 'created': '', 'title': '', 'notes': 'salvaged from an unfinished recording: the game '
        'hung or crashed; the frames that reached the disk replay, ' + (
            'with the part of the player track that reached it' if has_track else 'without the player track'),
        'game_exe_timestamp': header['exe_timestamp'], 'plugin_version': '', 'start': start, 'seed': header['seed'],
        'virtual_clock': bool(header['flags'] & 1), 'checkpoint_interval': header['checkpoint_interval'],
        'frames': frames, 'game_seconds': round(game_s, 3), 'wall_seconds': round(wall_s, 3),
        'synthetic_input': False, 'markers': [], 'salvaged_from': os.path.basename(folder.rstrip('\\/')),
        'end_reason': 'salvaged: cut off after %d frames' % frames}
    json.dump(manifest, open(os.path.join(out, 'manifest.json'), 'w', encoding='utf-8'), indent=2)
    print('%s: %d frames (%.1f s of game time), %d checkpoints, %s; %d bytes cut off the end' % (
        out, frames, game_s, kept, 'track kept' if has_track else 'no track', len(data) - end))


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    cmd = sys.argv[1]
    if cmd == 'stats' and len(sys.argv) >= 3:
        for run in sys.argv[2:]:
            cmd_stats(run)
        return
    if cmd == 'adopt-track' and len(sys.argv) in (3, 4):
        cmd_adopt_track(sys.argv[2], force=len(sys.argv) == 4 and sys.argv[3] == '--force')
        return
    if cmd == 'trackdiff' and len(sys.argv) == 4:
        cmd_trackdiff(sys.argv[2], sys.argv[3])
        return
    if cmd == 'salvage' and len(sys.argv) in (3, 4):
        cmd_salvage(sys.argv[2], sys.argv[3] if len(sys.argv) == 4 else None)
        return
    if cmd == 'list':
        cmd_list()
    elif cmd == 'info' and len(sys.argv) == 3:
        cmd_info(sys.argv[2])
    elif cmd == 'compare' and len(sys.argv) == 4:
        cmd_compare(sys.argv[2], sys.argv[3])
    elif cmd == 'tracediff' and len(sys.argv) == 4:
        cmd_tracediff(sys.argv[2], sys.argv[3])
    else:
        sys.exit(__doc__)


if __name__ == '__main__':
    main()
