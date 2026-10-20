#!/usr/bin/env python3
"""Compile the real Vita renderer against a host allocation/fence contract mock."""
import argparse
import os
from pathlib import Path
import shlex
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sanitize', action='store_true')
    args = parser.parse_args()
    dest = ROOT / 'build' / ('vita-gpu-tests-sanitize' if args.sanitize else 'vita-gpu-tests')
    dest.mkdir(parents=True, exist_ok=True)
    flags = ['-std=c++20', '-fno-fast-math', '-ffp-contract=off', '-DM2_VITA_RENDER_OPT=1',
             '-I', str(ROOT / 'tests/vita_gpu_shim'), '-I', str(ROOT / 'src')]
    flags += (['-O1', '-g', '-fsanitize=address,undefined', '-fno-omit-frame-pointer']
              if args.sanitize else ['-O2'])
    executable = dest / 'test_vita_gpu_lifetime'
    subprocess.run(shlex.split(os.environ.get('CXX', 'c++')) + flags +
                   [str(ROOT / 'tests/test_vita_gpu_lifetime.cpp'),
                    str(ROOT / 'src/runtime/video.cpp'), str(ROOT / 'src/runtime/raster.cpp'),
                    '-o', str(executable)], check=True)
    subprocess.run([str(executable)], check=True)


if __name__ == '__main__':
    main()
