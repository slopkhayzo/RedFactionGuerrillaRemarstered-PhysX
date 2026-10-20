"""Print a run's hang report (rfgphys.log lines starting with "hang:") with rfg.exe addresses resolved to functions.

python tools/hang_report.py runs/<run folder>   (or a path to rfgphys.log)

The plugin writes the report when a multithreaded Havok step deadlocks (experiments/havok_mt.cpp): lock owners, then
for each involved thread its instruction pointer and the return addresses found on its stack, as module+offset. A
stack scan can include stale return addresses; read each thread's list from the top (innermost call) down.
"""
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from profile_report import Symbols  # noqa: E402

TOKEN = re.compile(r'\brfg\+0x([0-9A-Fa-f]+)')


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    path = sys.argv[1]
    if os.path.isdir(path):
        path = os.path.join(path, 'rfgphys.log')
    lines = [line.rstrip('\n') for line in open(path, encoding='utf-8', errors='replace') if '] hang:' in line]
    if not lines:
        print('no hang report in', path)
        return 1
    symbols = Symbols()

    def resolve(m):
        return '%s (%s)' % (symbols.name(int(m.group(1), 16)), m.group(0))

    for line in lines:
        text = line.split('] hang:', 1)[1]
        if text.startswith('     '):  # a thread's return addresses: one per line, innermost first
            for token in text.split():
                print('          ' + TOKEN.sub(resolve, token))
        else:
            print(TOKEN.sub(resolve, text))
    return 0


if __name__ == '__main__':
    sys.exit(main())
