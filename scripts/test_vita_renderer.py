#!/usr/bin/env python3
"""ROM-free scalar-reference/optimized renderer comparisons; --bench is host only."""
import argparse, os, re, shlex, subprocess
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--sanitize',action='store_true')
    ap.add_argument('--bench',action='store_true')
    ap.add_argument('--reference-root',type=Path,default=ROOT)
    args=ap.parse_args()
    dest=ROOT/'build/renderer-tests'
    if args.sanitize: dest=ROOT/'build/renderer-tests-sanitize'
    dest.mkdir(parents=True,exist_ok=True);ref=dest/'reference';ref.mkdir(exist_ok=True)
    base=args.reference_root/'src/runtime'
    for name in ('geo.h','raster.h','video.h','video_profile.h','frame_profile.h','raster.cpp','video.cpp'):
        path=base/name
        if not path.exists():continue
        text=path.read_text().replace('namespace rt {','namespace reference {')
        # The harness itself enables optimization. Its reference headers must
        # nevertheless have the same layout as the non-optimized objects.
        text=text.replace('M2_VITA_RENDER_OPT','M2_VITA_REFERENCE_NEVER_ENABLE')
        text=re.sub(r'"runtime/(geo|raster|video|video_profile|frame_profile)\.h"',r'"reference/\1.h"',text)
        (ref/name).write_text(text)
    compiler=shlex.split(os.environ.get('CXX','c++'))
    flags=['-std=c++20','-fno-fast-math','-ffp-contract=off','-Wall','-Wextra','-Werror','-I',str(ROOT/'src'),'-I',str(dest)]
    flags+=['-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer'] if args.sanitize else ['-O3']
    objects=[]
    for name,opt in [('raster',False),('video',False),('raster',True),('video',True)]:
        source=ROOT/'src/runtime'/f'{name}.cpp' if opt else ref/f'{name}.cpp'
        obj=dest/(name+('-opt' if opt else '-ref')+'.o')
        subprocess.run(compiler+flags+(['-DM2_VITA_RENDER_OPT=1'] if opt else [])+['-c',str(source),'-o',str(obj)],check=True)
        objects.append(str(obj))
    exe=dest/'test_vita_renderer'
    subprocess.run(compiler+flags+['-DM2_VITA_RENDER_OPT=1',str(ROOT/'tests/test_vita_renderer.cpp')]+objects+['-o',str(exe)],check=True)
    subprocess.run([str(exe)]+(['--bench'] if args.bench else []),check=True)
if __name__=='__main__':main()
