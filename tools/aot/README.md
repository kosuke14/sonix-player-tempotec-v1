# The AOT pipeline for the Apple ADI libraries (phase 4)

The path the device's FairPlay layer takes: the ARM Apple libraries, lifted
to LLVM IR, compiled for mipsel, linked into the player. The pipeline is
proven end to end on a plain ARM object; the obfuscated ADI (one 850 KB
flattened function) is where the work remains.

## The toolchain

- `llvm-mctoll`, built from microsoft/llvm-mctoll against LLVM 8dfdcc7b
  (2022-11) with clang-17 and `-include cstdint` (GCC 15's headers break
  the 2022 tree). Built at `~/aot/build/bin/llvm-mctoll`; the cmake flags
  and the two build fixes are recorded in HANDOVER-APPLEMUSIC.md.
- `llc-17 -march=mipsel -mattr=+mips32r2 -filetype=obj` turns the raised
  IR into MIPS objects.
- `mctoll-arm-hardening.patch` -- the release-build null dereferences the
  obfuscated code trips (six sites, each a debug assert that NDEBUG
  deleted). Apply to the mctoll tree before building.

## Where it stands

- A plain ARM `.so` raises cleanly and the IR compiles to correct mipsel.
- `libCoreADI.so` raises through the revising and the DAG, and stops in
  `InstSelector::selectCode` -- a `getNode` fed an unraised register. The
  selection layer has hundreds of such call sites; the hardening so far
  covered the earlier passes.
- `retdec` (v5.0, runs under `qemu-x86_64 -L /tmp/x64root`) is the second
  route: slower, but its decompiler handles flattened control flow.
- The anti-tamper caveat: the obfuscated function guards its own
  initialisation with timing checks; a translation that runs at MIPS speed
  may still fail them. This is the research-grade part of phase 4.
