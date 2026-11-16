/* The whole core as one translation unit (whole-program optimisation without
 * LTO, which the small WASM linker does not consume). */
#include "mem.c"
#include "mathx.c"
#include "huff.c"
#include "jpeg.c"
#include "inflate.c"
#include "png.c"
#include "jpegdec.c"
#include "hdr.c"
#include "engine.c"
