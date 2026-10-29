#!/bin/sh
# Compares the mangled symbols wrapper-v2's daemon resolves via dlsym against
# what a given Apple Music build actually exports, per ABI. The result is the
# input for regenerating the guest daemon's abi table.
#
#   tools/abi_scan.sh <abi-dir>...        e.g. build/ampguest/apk/arm64-v8a ...
set -e
HERE=$(cd "$(dirname "$0")/.." && pwd)
ABI_HPP="$HERE/../wrapper-v2/src/daemon/apple/abi.hpp"
if [ ! -f "$ABI_HPP" ]; then
    ABI_HPP="$HOME/wrapper-v2/src/daemon/apple/abi.hpp"
fi
if [ ! -f "$ABI_HPP" ]; then
    echo "cannot locate wrapper-v2's abi.hpp" >&2
    exit 1
fi

# The names the daemon dlsyms, in source order.
MANGLED=$(grep -o 'inline constexpr const char\* [a-zA-Z0-9_]* =' "$ABI_HPP" |
          sed 's/inline constexpr const char\* //; s/ =//') || true
MANGLED_COUNT=$(printf '%s\n' "$MANGLED" | grep -c . || true)

python3 - "$@" "$ABI_HPP" <<'EOF'
import sys, re, subprocess, os

abi_hpp = sys.argv[-1]
abis = sys.argv[1:-1]
if not abis:
    sys.exit("usage: abi_scan.sh <abi-dir>...")

src = open(abi_hpp).read()
# name -> mangled string, in declaration order
entries = re.findall(r'inline constexpr const char\* (\w+) =\s*"([^"]+)";', src)
print("wrapper-v2 abi.hpp: %d dlsym symbols" % len(entries))

# Which library each symbol is expected in, from loader.cpp's dlopen set.
# The scan simply asks every exported .so; the report says where each hit.
for abi in abis:
    print("\n=== %s ===" % os.path.basename(abi.rstrip('/')))
    libs = {}
    for so in sorted(os.listdir(abi)):
        if not so.endswith('.so'):
            continue
        path = os.path.join(abi, so)
        try:
            out = subprocess.run(['readelf', '-W', '--dyn-syms', path],
                                 capture_output=True, text=True, timeout=60).stdout
        except Exception as e:
            print("  readelf failed for %s: %s" % (so, e))
            continue
        for line in out.splitlines():
            p = line.split()
            if len(p) >= 8 and 'FUNC' in line or 'OBJECT' in line:
                # format: idx: value size type bind vis ndx name[@ver]
                name = p[7]
                base = name.split('@')[0]
                libs.setdefault(base, so)
    found, missing = 0, []
    where = {}
    for name, mangled in entries:
        if mangled in libs:
            found += 1
            where[name] = (mangled, libs[mangled])
        else:
            missing.append((name, mangled))
    print("resolved: %d/%d" % (found, len(entries)))
    if missing:
        print("MISSING (%d):" % len(missing))
        for name, mangled in missing[:400]:
            print("  %-45s %s" % (name, mangled[:140]))
    # Per-symbol library map, to inform the new loader's dlopen set.
    counts = {}
    for name, (mangled, so) in where.items():
        counts[so] = counts.get(so, 0) + 1
    print("hits by library:")
    for so, n in sorted(counts.items(), key=lambda kv: -kv[1]):
        print("  %5d  %s" % (n, so))
EOF
