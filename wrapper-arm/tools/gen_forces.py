import subprocess, re, os, glob, sys

# Collect every UNDEF the Apple x86 libs will ask for at runtime, then find
# which NDK static archives define them. -u pulls the defining member out of
# the archive so the static daemon can satisfy the Apple libs' relocations.
#
#   Usage: python3 gen_forces.py [apple_libs_dir] > force_symbols.txt
#
# Symbols the NDK archives cannot provide (CF*, ICU*, xml*, dispatch, blocks,
# C++ runtime) are intentionally left out: they are resolved at guest-load
# time across the manually loaded Apple libs, not linked into the daemon.
undef = set()
base = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
    '..', 'sonix-player', 'build', 'ampguest', 'apk', 'x86')
sysroot = sys.argv[2] if len(sys.argv) > 2 else None
libs = ['libandroidappmusic.so','libmediaplatform.so','libCoreFP.so','libCoreLSKD.so',
        'libCoreADI.so','libCoreFoundation.so','libdispatch.so','libBlocksRuntime.so',
        'libdaapkit.so','libmedialibrarycore.so','libicui18n_sv_apple.so',
        'libicuuc_sv_apple.so','libicudata_sv_apple.so','libxml2.so','libc++_shared.so']
for so in libs:
    r = subprocess.run(['readelf','-W','--dyn-syms', os.path.join(base, so)], capture_output=True, text=True)
    for line in r.stdout.splitlines():
        p = line.split()
        if len(p) >= 8 and 'UND' in line and not line.strip().startswith('Num'):
            undef.add(p[7].split('@')[0])

# Scan every static archive the static link uses (libc/libm/libz/libdl).
if sysroot is None:
    ndk = os.environ.get('ANDROID_NDK_HOME', '/tmp/android-ndk-r27c')
    sysroot = os.path.join(ndk, 'toolchains/llvm/prebuilt/linux-x86_64/sysroot/usr/lib/i686-linux-android')
archives = []
for a in ['libc.a','libm.a','libz.a','libdl.a']:
    p = os.path.join(sysroot, a)
    if os.path.exists(p):
        archives.append(p)
in_arch = set()
for arch in archives:
    r = subprocess.run(['nm', arch], capture_output=True, text=True)
    for line in r.stdout.splitlines():
        m = re.match(r'[0-9a-fA-F]+ [TDBRWAGI] (\S+)', line)
        if m:
            in_arch.add(m.group(1))

need_force = sorted(s for s in undef if s in in_arch and not s.startswith('_Z'))
missing = sorted(s for s in undef if s not in in_arch and not s.startswith('_Z')
                 and not s.startswith('__cxa_') and not s.startswith('_Unwind')
                 and not s.startswith('__gxx'))
print(f'archives scanned: {[os.path.basename(a) for a in archives]}', file=__import__("sys").stderr)
print(f'forced: {len(need_force)}, remaining undefined (stub/manual): {len(missing)}', file=__import__("sys").stderr)
for s in missing:
    print(f'  MISSING: {s}', file=__import__("sys").stderr)
print(len(need_force))
print(' '.join(f'-Wl,-u,{s}' for s in need_force))
