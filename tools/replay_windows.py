"""Compare replay runs over windows of replay frames.

python tools/replay_windows.py runs/<run> [runs/<run> ...] [--windows 36000-40000,40000-42000,...]

Replays of one recording are compared by its frames, not by time: the log's "harness: replay frame N/M" lines
(every 2,000 frames) place each window in the run's telemetry. Per window: gameplay frames, frame time (mean, p50,
p95, p99 in ms), the render thread's frame (render_ms) and, with Telemetry.RenderCensus=1, its parts
(render_timeline.csv: before_kick = visibility and batch building, rt_parallel = its own passes while the scene thread
draws, the scene thread, draw calls on each), and the pieces in PhysX (live, frozen). Heavy replays diverge from run
to run, so compare a few runs of each setting.
"""
import argparse
import csv
import os
import re
import statistics as st


def frame_marks(run):
    marks = []
    with open(os.path.join(run, 'rfgphys.log'), encoding='utf-8', errors='replace') as f:
        for line in f:
            m = re.match(r'\[\s*([\d.]+)\].*harness: replay frame (\d+)/(\d+)', line)
            if m:
                marks.append((int(m.group(2)), float(m.group(1)) * 1000.0))
    return marks


def time_at(marks, frame):
    """Log time (ms) of a replay frame, between the marks or past the last one at its pace."""
    for (f0, t0), (f1, t1) in zip(marks, marks[1:]):
        if f0 <= frame <= f1:
            return t0 + (t1 - t0) * (frame - f0) / max(1, f1 - f0)
    if len(marks) >= 2 and frame > marks[-1][0]:
        (f0, t0), (f1, t1) = marks[-2], marks[-1]
        return t1 + (t1 - t0) * (frame - f1) / max(1, f1 - f0)
    return None


def pct(values, p):
    values = sorted(values)
    return values[min(len(values) - 1, int(len(values) * p))] if values else float('nan')


def mean(rows, key):
    return st.mean(float(r[key] or 0) for r in rows) if rows else float('nan')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('runs', nargs='+')
    ap.add_argument('--windows', default='4000-10000,10000-20000,20000-30000,30000-36000,36000-40000,40000-42000,'
                                         '42000-44000,44000-45775')
    args = ap.parse_args()
    windows = [tuple(int(x) for x in w.split('-')) for w in args.windows.split(',')]
    for run in args.runs:
        marks = frame_marks(run)
        tel = [r for r in csv.DictReader(open(os.path.join(run, 'telemetry.csv'))) if r['state'] == '1']
        rt_path = os.path.join(run, 'render_timeline.csv')
        rt = [r for r in csv.DictReader(open(rt_path)) if r['state'] == '1'] if os.path.exists(rt_path) else []
        print(f'== {os.path.basename(os.path.normpath(run))} (replay frames logged up to '
              f'{marks[-1][0] if marks else "none"})')
        print('   window          frames  mean   p50   p95   p99  render  b_kick  rt_par  scene  draws_r draws_s   '
              'live frozen')
        for lo, hi in windows:
            t0, t1 = time_at(marks, lo), time_at(marks, hi)
            if t0 is None or t1 is None:
                print(f'   {lo}-{hi}: not reached')
                continue
            t0 += 300  # past whatever happened at the window's first frame
            sel = [r for r in tel if t0 <= float(r['t_ms']) < t1]
            if not sel:
                print(f'   {lo}-{hi}: no frames')
                continue
            ft = [float(r['frame_ms']) for r in sel]
            rsel = [r for r in rt if t0 <= float(r['t_ms']) < t1]
            print(f'   {lo:>5}-{hi:<5} {len(sel):8d} {st.mean(ft):5.2f} {pct(ft, .5):5.2f} {pct(ft, .95):5.2f} '
                  f'{pct(ft, .99):5.2f} {mean(sel, "render_ms"):6.2f} {mean(rsel, "before_kick_ms"):7.2f} '
                  f'{mean(rsel, "rt_parallel_ms"):7.2f} {mean(rsel, "scene_thread_ms"):6.2f} '
                  f'{mean(rsel, "draws_render"):8.0f} {mean(rsel, "draws_scene"):7.0f} '
                  f'{mean(sel, "px_live"):6.0f} {mean(sel, "px_frozen"):6.0f}')


if __name__ == '__main__':
    main()
