# FairPlay activation on 6.5.3 — the probe trail

Status: the guest loader and daemon work end to end up to Apple's device
binding; the binding itself is the remaining gate. This file records
everything recovered so far so the next session (or a fresh researcher) does
not have to re-derive it.

## Where it stands

```
boot: loader.ok=1                      # 17 Apple libs, static bionic, qemu-i386
runtime: FooHill session ready (0x…)   # SVFootHillSessionCtrl::instance() live
runtime: ADILoadLibraryWithPath  -> -45075
runtime: ADISetProvisioningPath  -> -45075
runtime: ADISetAndroidID         -> -45075
runtime: FairPlaySetAndroidID    -> -42404
runtime: FairPlayLoadLibraryWithPath -> -42404
runtime: KDSetAndroidID          -> -42792
runtime: ADIIsMachineProvisioned -> -45075
SVError: Error generating a movieId. KDGenMovieId() -42792
```

No file syscalls happen behind the ADI calls (strace): the refusal is an
internal state check, not a missing file.

## What is proven to work

- The 6.5.3 online key flow (`FootHillDecryptionKey` in the dex):
  `getExistingContextKey(adam, uri, offline) -> key string`,
  `getSPC(adam, uri, format, fpsCertB64) -> SPC`,
  `saveContextKey(adam, uri, ckc, offline) -> key string`. The 3.6.0
  `SVFootHillPExchange` ctor is gone (built internally now).
- getSPC internals (libandroidappmusic 0x2d22c0): exchange-map find →
  `CryptoDataFromBase64EncodedString(arg4)` → `strtol(arg3)` → movie id
  generation via the obfuscated `Qhl17oSYJtCJIxM(cert_bytes, len, out, 1)`
  → internal `SVFootHillPExchangeC1` → `spc()`.
- Real FPS certificate (works, public):
  `GET https://init.itunes.apple.com/WebObjects/MZInit.woa/wa/fpsCertificate`
  → 1234-byte X.509, base64 (NO_WRAP) for getSPC arg 4.
- SPC POST endpoint (dex strings):
  `https://play.itunes.apple.com/WebObjects/MZPlay.woa/wa/fpsRequest`.
- Device binding entry points (libstoreapi.so, recovered from the JNI shims
  `Java_…_Companion_*`): `kq56gsgHG6`=ADILoadLibraryWithPath(path),
  `nf92ngaK92`=ADISetProvisioningPath(path), `Sph98paBcz`=ADISetAndroidID(b,16),
  `bsawCXd`=FairPlaySetAndroidID(b,16), `N8jdR29h`=FairPlayLoadLibraryWithPath,
  `hgBR8sd3n7ox0`=KDSetAndroidID(b,16), `aslgmuibau`=ADIIsMachineProvisioned(l).
- Java init order (`P7/a` in classes2.dex): ADISetProvisioningPath →
  ADILoadLibraryWithPath → ADISetProvisioningPath → ADISetAndroidID →
  FairPlaySetAndroidID → FairPlayLoadLibraryWithPath → KDSetAndroidID.
  android_id = Settings.Secure.ANDROID_ID padded to 16 with '0' (`c6.d.a()`).
- The fake JVM works for the JNI libraries' own `JNI_OnLoad`: libCoreADI and
  libCoreLSKD return `JNI_VERSION_1_6` (65542). libandroidappmusic and
  libstoreapi return -1 (they want real Java classes — JavaCPP
  `JNI_OnLoad_jnijavacpp` walks the app's class objects).
- libstoreapi.so + libFPDIFor3P.so are required in the guest tree even though
  no DT_NEEDED closure pulls them (the device-binding entries live there).

## What the gate most likely is

The `-45075` family plus the dex strings (`MachineDataProvisioningStatus`,
`AnisetteMDFinishProvisioning`, `AMSCore::IdMS::BagKey::MidStartProvisioning`,
`startProvisioning`/`finishProvisioning`) point at Apple's machine
provisioning (Anisette/MID): a server dance that provisions the device
identity into the ADI layer (`ADIOTPRequest` → server →
`ADIProvisioningStart/End`), after which the ADI functions and
`KDGenMovieId` become usable. The dance runs through the app's authenticated
store session, which the standalone daemon does not have by design (auth
stays on the player/wrapper side).

## Ideas for the next attempt (untested)

1. Drive the provisioning dance from the daemon with tokens supplied by the
   host: `ADIOTPRequest` → the MZPlay provisioning endpoint →
   `ADIProvisioningStart/End`. The endpoints are in the dex strings
   (`getMusicSDKAuthorizationsSrv`, MID bag keys).
2. Give JavaCPP's `JNI_OnLoad` what it wants: a minimal set of real
   `jclass` objects for the `com.apple.android.music.*` callback classes
   (FindClass stubs that hand back per-name fake classes with working
   `RegisterNatives`), so libandroidappmusic/libstoreapi finish their
   `JNI_OnLoad` and register their native tables.
3. Check whether the ADI state can be warmed without JNI: `libCoreADI`'s
   `fp_dh_*` exports are what `libCoreLSKD` imports directly (66 of them) —
   a direct call map (which `fp_dh_*` is which ADI entry) would bypass
   libstoreapi's wrappers entirely.
4. The `-45075` constant is computed, not immediate: the error sites sit
   behind the control-flow-flattening dispatcher (`madsvsfvjk` et al. in
   libstoreapi). If needed, deobfuscate by instrumenting qemu (`-d in_asm`
   filtered around the dispatcher's `jmpl *%eax`).

## Tooling built for this hunt (reusable)

- `guest_report_fault()` + the SIGSEGV handler in `main.cpp`: every guest
  fault prints `lib+0xoffset` for the stack words — used to find
  `strlen(NULL)`, the `PushLocalFrame` slot (JNI table 0x4c) and the
  movie-id call chain.
- `/tmp/ampdrive.py` (session scratch, not committed): spawns qemu with the
  static daemon, sends Health + GetSPC frames, child stderr to a file.
  `AMP_QEMU_PREFIX="-strace"` adds qemu tracing.
- The PoC rig `tools/wv2dtest.c` speaks the same protocol; `AMP_FP_CERT`
  feeds the real certificate, `AMP_ANDROID_ID` the device id.
