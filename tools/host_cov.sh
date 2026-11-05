#!/bin/bash
# Kernel line coverage of the host tests: every test/test_*.c is built with gcov
# instrumentation (no sanitizers) under build/host-cov/<test>/, run, and the per file line
# coverage of kernel/*.c is merged over all programs. Prints a table and, per file, the
# line ranges no test executed. Needs gcc and gcov only.
set -u
cd "$(dirname "$0")/.."
ROOT=$PWD
COV=build/host-cov
rm -rf "$COV"
for t in test/test_*.c; do
    n=$(basename "$t" .c)
    mkdir -p "$COV/$n"
    ( cd "$COV/$n" && gcc -std=c99 -Wall -g -O0 --coverage \
        -I"$ROOT/kernel" -I"$ROOT/port/host" -I"$ROOT/test/$n" -I"$ROOT/test" \
        -o "$n" "$ROOT/$t" "$ROOT"/kernel/*.c "$ROOT"/port/host/*.c \
      && ./"$n" > run.log 2>&1; echo "$n: $(tail -1 run.log)"
      gcov -q ./*.gcda > /dev/null 2>&1 )
done
python3 - "$COV" <<'PY'
import glob, os, re, sys, collections
cov = collections.defaultdict(dict)
esc = re.compile(r'\x1b\[[0-9;]*[mK]')
for gc in glob.glob(os.path.join(sys.argv[1], 'test_*', 'rtos_*.c.gcov')):
    src = os.path.basename(gc)[:-5]
    for line in open(gc, errors='replace'):
        m = re.match(r'\s*([^:]+):\s*(\d+):', esc.sub('', line))
        if not m: continue
        cnt, ln = m.group(1).strip(), int(m.group(2))
        if cnt == '-' or ln == 0: continue
        hit = not cnt.startswith('#') and not cnt.startswith('=')
        cov[src][ln] = cov[src].get(ln, False) or hit
tl = th = 0
print(f"\n{'file':18s} {'lines':>6s} {'hit':>5s} {'cov':>6s}")
for f in sorted(cov):
    l = len(cov[f]); h = sum(cov[f].values()); tl += l; th += h
    print(f"{f:18s} {l:6d} {h:5d} {100*h/l:5.1f}%")
print(f"{'kernel total':18s} {tl:6d} {th:5d} {100*th/tl:5.1f}%\n")
for f in sorted(cov):
    miss = sorted(ln for ln, h in cov[f].items() if not h)
    if not miss: continue
    r = []; s = p = None
    for ln in miss:
        if s is None: s = p = ln
        elif ln == p + 1: p = ln
        else: r.append((s, p)); s = p = ln
    r.append((s, p))
    print(f"{f}: uncovered", ' '.join(f"{a}-{b}" if a != b else str(a) for a, b in r))
PY
