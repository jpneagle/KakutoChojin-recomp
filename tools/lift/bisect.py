"""Finds lifted functions that break the title, by bisection over KT_LIFT.

Runs kt_loader with the lifted functions below an address enabled and
checks that it reaches a frame count without crashing or trapping. Each
culprit found is excluded and the search repeats.

usage: bisect.py <lifted dir> [--frames N] [--timeout S] [--exclude a,b,...] [--max K]
"""
import argparse
import os
import re
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
BIN = ROOT / 'build' / 'bin'


def lifted_functions(lifted_dir):
    text = open(os.path.join(lifted_dir, 'lifted_table.c'), encoding='utf-8').read()
    return sorted(int(m.group(1), 16) for m in re.finditer(r'\{0x([0-9a-f]{8}), f_', text))


AUTOPRESS = ''


def run(spec, frames, timeout, extra_env=None):
    env = dict(os.environ, KT_LIFT=spec, KT_NO_FRAME_LIMIT='1', KT_SILENT='1')
    env.pop('KT_DUMP_FRAMES', None)
    if AUTOPRESS:
        env['KT_AUTOPRESS'] = AUTOPRESS
    if extra_env:
        env.update(extra_env)
    try:
        subprocess.run([str(BIN / 'kt_loader.exe'), 'game', 'analysis/manifest.txt', 'hdd'], cwd=ROOT, env=env,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=timeout)
    except subprocess.TimeoutExpired:
        subprocess.run(['taskkill', '/f', '/im', 'kt_loader.exe'], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    log = open(BIN / 'kt_host.log', encoding='utf-8', errors='replace').read()
    bad = re.search(r'UNHANDLED|lift: trap|FATAL|Fatal', log)
    reached = max((int(m.group(1)) for m in re.finditer(r'Swap: frame (\d+)', log)), default=0)
    return not bad and reached >= frames, reached, (bad.group(0) if bad else '')


def spec_for(funcs, upto, exclude):
    parts = [f'0-{funcs[upto]:x}'] if upto < len(funcs) else ['all']
    parts += [f'-{e:x}' for e in exclude]
    return ','.join(parts)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('lifted')
    ap.add_argument('--frames', type=int, default=1200)
    ap.add_argument('--timeout', type=int, default=25)
    ap.add_argument('--exclude', default='')
    ap.add_argument('--max', type=int, default=20)
    ap.add_argument('--autopress', default='', help='KT_AUTOPRESS script for the runs')
    args = ap.parse_args()
    global AUTOPRESS
    AUTOPRESS = args.autopress
    funcs = lifted_functions(args.lifted)
    exclude = [int(x, 16) for x in args.exclude.split(',') if x]
    for _ in range(args.max):
        ok, reached, why = run(spec_for(funcs, len(funcs), exclude), args.frames, args.timeout)
        print(f'all enabled (excluding {len(exclude)}): {"ok" if ok else "FAIL"} frame {reached} {why}', flush=True)
        if ok:
            break
        if exclude and why == '' and reached < args.frames:
            print('  (too slow or hung: consider a longer --timeout)', flush=True)
        lo, hi = 0, len(funcs)  # prefix [0, lo) passes, [0, hi) fails
        while hi - lo > 1:
            mid = (lo + hi) // 2
            ok, reached, why = run(spec_for(funcs, mid, exclude), args.frames, args.timeout)
            print(f'  first {mid} ({funcs[mid]:08x}): {"ok" if ok else "FAIL"} frame {reached} {why}', flush=True)
            if ok:
                lo = mid
            else:
                hi = mid
        culprit = funcs[hi - 1]
        print(f'culprit: {culprit:08x}', flush=True)
        exclude.append(culprit)
    print('exclude=' + ','.join(f'{e:x}' for e in exclude))


if __name__ == '__main__':
    main()
