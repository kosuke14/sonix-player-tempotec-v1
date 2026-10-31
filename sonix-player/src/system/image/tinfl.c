// miniz's streaming inflate, compiled into this one translation unit.
//
// miniz_export.h stubs MINIZ_EXPORT out to nothing, so every file that
// includes tinfl_impl.inc defines the decompressor itself. png_scaled used to
// be the only one; the guest-tree unpacker (ampguest.c) inflates the tar
// download through the same routines, reached over the plain declarations in
// miniz_tinfl.h rather than a second copy of the implementation.

#include "miniz/tinfl_impl.inc"
