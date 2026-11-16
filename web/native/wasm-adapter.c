/* WebAssembly interface of the Pseudo HDR engine. Every function marked
 * EXPORT is exported by the build (see build.sh). Pointers are linear-memory
 * offsets. */
#include "../../core/phdr.c"

#define EXPORT

EXPORT uint32_t abi_version(void) { return 1; }
EXPORT uint32_t mem_alloc(uint32_t size) { return (uint32_t)(uintptr_t)xalloc(size); }
EXPORT void mem_release(uint32_t ptr) { xfree((void *)(uintptr_t)ptr); }

/* Takes ownership of the buffer on success; frees it on failure. */
EXPORT int image_open(uint32_t ptr, uint32_t len) {
    int r = engine_open((uint8_t *)(uintptr_t)ptr, len);
    if (r) xfree((void *)(uintptr_t)ptr);
    return r;
}
EXPORT int image_error_detail(void) { return S.error_detail; }
EXPORT void image_close(void) { session_reset(); }
EXPORT int image_kind(void) { return S.im.kind; }
/* Size as displayed (EXIF orientation applied). */
EXPORT uint32_t image_width(void) { return S.im.rgb ? oriented_w() : 0; }
EXPORT uint32_t image_height(void) { return S.im.rgb ? oriented_h() : 0; }
EXPORT int image_flags(void) { return S.im.flags; }
/* 1 if the export re-encodes the image (PNG, greyscale JPEG), 0 if the
 * original JPEG data is kept bit for bit. */
EXPORT int image_reencodes(void) { return S.im.rgb ? engine_reencodes() : 0; }

/* Strength in stops (peak = 2^stops times SDR white) and threshold 0..0.95. */
EXPORT int params_set(double stops, double threshold) { return engine_set_params(stops, threshold); }
EXPORT double params_stops(void) { engine_init(); return S.curve.stops; }
EXPORT double params_threshold(void) { engine_init(); return S.curve.threshold; }
/* 256 gain map codes over the SDR brightness 0..255 (for the curve view). */
EXPORT uint32_t params_curve(void) { engine_init(); return (uint32_t)(uintptr_t)S.curve.ui; }

EXPORT int preview_render(uint32_t maxw, uint32_t maxh) { return engine_preview(maxw, maxh); }
EXPORT uint32_t preview_width(void) { return S.pw; }
EXPORT uint32_t preview_height(void) { return S.ph; }
EXPORT uint32_t preview_hdr_address(void) { return (uint32_t)(uintptr_t)S.phdr.data; }
EXPORT uint32_t preview_hdr_size(void) { return S.phdr.len; }
EXPORT uint32_t preview_sdr_address(void) { return (uint32_t)(uintptr_t)S.psdr.data; }
EXPORT uint32_t preview_sdr_size(void) { return S.psdr.len; }

EXPORT int export_jpeg(void) { return engine_export(); }
EXPORT uint32_t export_address(void) { return (uint32_t)(uintptr_t)S.out.data; }
EXPORT uint32_t export_size(void) { return S.out.len; }
EXPORT void export_release(void) { buf_free(&S.out); }
