/* A decoded SDR source image: 8-bit RGB in stored orientation, plus the
 * metadata needed to write the HDR JPEG. */
#ifndef PHDR_IMAGE_H
#define PHDR_IMAGE_H
#include "base.h"

enum { KIND_NONE = 0, KIND_JPEG = 1, KIND_PNG = 2 };
enum {
    FLAG_ALPHA = 1,        /* PNG with transparency, composited onto white */
    FLAG_GAINMAP = 2,      /* JPEG that already carried a gain map (replaced) */
    FLAG_PROGRESSIVE = 4,  /* progressive JPEG */
    FLAG_ICC = 8,          /* embedded ICC profile */
    FLAG_GRAY = 16         /* greyscale source */
};
typedef struct {
    int kind;
    uint32_t w, h;          /* stored (unrotated) size */
    uint8_t *rgb;           /* w * h * 3 */
    uint8_t *icc; uint32_t icc_len;    /* owned copy, or NULL */
    uint8_t *exif; uint32_t exif_len;  /* PNG eXIf (TIFF data), owned, or NULL */
    int orientation;        /* EXIF orientation 1..8 */
    int flags;
    int detail;             /* UNSUP_* for ERR_UNSUPPORTED */
} Image;

static void image_free(Image *im) {
    xfree(im->rgb); xfree(im->icc); xfree(im->exif);
    xmemset(im, 0, sizeof(*im));
}
/* EXIF orientation from TIFF data ("MM"/"II" header); 1 if absent. */
static int exif_orientation(const uint8_t *t, uint32_t n) {
    if (!t || n < 8) return 1;
    int le = t[0] == 'I' && t[1] == 'I';
    if (!le && !(t[0] == 'M' && t[1] == 'M')) return 1;
#define U16(p) (le ? (uint32_t)(p)[0] | (uint32_t)(p)[1] << 8 : rd16be(p))
#define U32(p) (le ? (uint32_t)(p)[0] | (uint32_t)(p)[1] << 8 | (uint32_t)(p)[2] << 16 | (uint32_t)(p)[3] << 24 : rd32be(p))
    if (U16(t + 2) != 42) return 1;
    uint32_t ifd = U32(t + 4);
    if (ifd > n - 2 || ifd < 8) return 1;
    uint32_t count = U16(t + ifd);
    for (uint32_t i = 0; i < count; i++) {
        uint32_t e = ifd + 2 + i * 12;
        if (e + 12 > n) break;
        if (U16(t + e) == 0x0112 && U16(t + e + 2) == 3) {
            uint32_t v = U16(t + e + 8);
            return v >= 1 && v <= 8 ? (int)v : 1;
        }
    }
#undef U16
#undef U32
    return 1;
}
#endif
