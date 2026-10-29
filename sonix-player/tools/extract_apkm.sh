#!/bin/sh
# Extracts the native libraries of an Apple Music .apkm bundle into
# build/ampguest/apk/<abi>/. The user supplies their own APK; nothing
# Apple-shipped is committed to the repository.
#
#   tools/extract_apkm.sh <path/to/app.apkm>
set -e
APKM="$1"
if [ ! -f "$APKM" ]; then
    echo "usage: $0 <path/to/app.apkm>" >&2
    exit 1
fi

HERE=$(cd "$(dirname "$0")/.." && pwd)
OUT="$HERE/build/ampguest/apk"
mkdir -p "$OUT"

python3 - "$APKM" "$OUT" <<'EOF'
import sys, zipfile, io, os

apkm, out = sys.argv[1], sys.argv[2]
z = zipfile.ZipFile(apkm)
names = z.namelist()
splits = [n for n in names if n.startswith('split_config.') and n.endswith('.apk')]
abi_of = {
    'arm64_v8a': 'arm64-v8a',
    'armeabi_v7a': 'armeabi-v7a',
    'x86_64': 'x86_64',
    'x86': 'x86',
}
for split in splits:
    tag = split[len('split_config.'): -len('.apk')]
    abi = abi_of.get(tag)
    if not abi:
        continue
    inner = zipfile.ZipFile(io.BytesIO(z.read(split)))
    prefix = 'lib/%s/' % abi
    for n in inner.namelist():
        if n.startswith(prefix) and n.endswith('.so'):
            d = os.path.join(out, abi)
            os.makedirs(d, exist_ok=True)
            with open(os.path.join(d, os.path.basename(n)), 'wb') as f:
                f.write(inner.read(n))
    print('%s: %d .so files' % (abi, len([n for n in inner.namelist()
                                          if n.startswith(prefix) and n.endswith('.so')])))
EOF
echo "extracted to $OUT"
