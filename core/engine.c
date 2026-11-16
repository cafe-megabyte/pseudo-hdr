/* Session state: the open image, the accordion curve, the screen preview and
 * the export. One image at a time. */
#include "image.h"

#define PREVIEW_QUALITY 90
#define PNG_QUALITY 95
#define STOPS_MIN 0.1
#define STOPS_MAX 6.0
#define THRESHOLD_MAX 0.95

typedef struct {
    Image im;
    uint8_t *src; uint32_t src_len;        /* original JPEG file, kept for export */
    Curve curve;
    uint8_t *prgb; uint32_t pw, ph;        /* preview pixels, oriented */
    uint32_t pmaxw, pmaxh;
    uint8_t *pgain;                        /* preview gain map codes */
    Buf psdr, phdr, out;
    int error_detail;
} Session;
static Session S;

static void session_reset(void) {
    image_free(&S.im);
    xfree(S.src); xfree(S.prgb); xfree(S.pgain);
    buf_free(&S.psdr); buf_free(&S.phdr); buf_free(&S.out);
    Curve keep = S.curve;
    xmemset(&S, 0, sizeof S);
    S.curve = keep;
}
static void engine_init(void) {
    static int ready;
    if (ready) return;
    curve_build(&S.curve, 2.0, 0.6);
    ready = 1;
}
/* 1: the export writes a new primary JPEG; 0: the original JPEG data is kept. */
static int engine_reencodes(void) { return S.im.kind == KIND_PNG || (S.im.flags & FLAG_GRAY); }
/* Only an RGB profile can describe an RGB JPEG (greyscale sources are
 * written in colour; their grey profile is then dropped). */
static int icc_rgb(const uint8_t *icc, uint32_t n) { return icc && n >= 132 && !xmemcmp(icc + 16, "RGB ", 4); }
static uint32_t oriented_w(void) { return S.im.orientation >= 5 ? S.im.h : S.im.w; }
static uint32_t oriented_h(void) { return S.im.orientation >= 5 ? S.im.w : S.im.h; }

/* Takes ownership of `d` on success. */
int engine_open(uint8_t *d, uint32_t n) {
    engine_init();
    session_reset();
    int r;
    if (n >= 8 && d[0] == 137 && d[1] == 'P' && d[2] == 'N' && d[3] == 'G') r = png_decode(d, n, &S.im);
    else if (n >= 3 && d[0] == 0xFF && d[1] == 0xD8 && d[2] == 0xFF) r = jpeg_decode(d, n, &S.im);
    else r = ERR_FORMAT;
    if (r) { S.error_detail = S.im.detail; image_free(&S.im); return r; }
    if (S.im.kind == KIND_JPEG) {
        /* the export reuses the original data; check its structure now */
        Seg *seg = xalloc(sizeof(Seg) * SEG_MAX);
        int nseg, eoi; uint32_t a, b;
        r = seg ? jpeg_layout(d, n, seg, &nseg, &a, &b, &eoi) : ERR_MEMORY;
        xfree(seg);
        if (r) { if (r == ERR_UNSUPPORTED) S.error_detail = UNSUP_LAYOUT; image_free(&S.im); return r; }
        S.src = d; S.src_len = n;
    } else xfree(d);
    return ERR_OK;
}
int engine_set_params(double stops, double threshold) {
    engine_init();
    if (!(stops == stops) || !(threshold == threshold)) return ERR_FORMAT;
    stops = CLAMP(stops, STOPS_MIN, STOPS_MAX);
    threshold = CLAMP(threshold, 0.0, THRESHOLD_MAX);
    curve_build(&S.curve, stops, threshold);
    return ERR_OK;
}

/* Box-filtered, oriented preview fitting maxw x maxh (never enlarged). */
static int preview_pixels(uint32_t maxw, uint32_t maxh) {
    uint32_t w = S.im.w, h = S.im.h, ow = oriented_w(), oh = oriented_h();
    double scale = MIN(1.0, MIN((double)maxw / ow, (double)maxh / oh));
    uint32_t pw = MAX(1u, (uint32_t)(ow * scale + 0.5)), ph = MAX(1u, (uint32_t)(oh * scale + 0.5));
    pw = MIN(pw, ow); ph = MIN(ph, oh);
    int rot = S.im.orientation >= 5;
    uint32_t sw = rot ? ph : pw, sh = rot ? pw : ph;      /* stored-orientation preview size */
    uint8_t *small = xalloc((size_t)sw * sh * 3), *dst = xalloc((size_t)pw * ph * 3);
    uint32_t *acc = xalloc(sizeof(uint32_t) * sw * 3);
    uint32_t *x0 = xalloc(sizeof(uint32_t) * (sw + 1));
    if (!small || !dst || !acc || !x0) { xfree(small); xfree(dst); xfree(acc); xfree(x0); return ERR_MEMORY; }
    for (uint32_t x = 0; x <= sw; x++) x0[x] = (uint32_t)((uint64_t)x * w / sw);
    for (uint32_t y = 0; y < sh; y++) {
        uint32_t ya = (uint32_t)((uint64_t)y * h / sh), yb = MAX(ya + 1, (uint32_t)((uint64_t)(y + 1) * h / sh));
        xmemset(acc, 0, sizeof(uint32_t) * sw * 3);
        for (uint32_t sy = ya; sy < yb; sy++) {
            const uint8_t *row = S.im.rgb + (size_t)sy * w * 3;
            for (uint32_t x = 0; x < sw; x++) {
                uint32_t xa = x0[x], xb = MAX(xa + 1, x0[x + 1]), r = 0, g = 0, b = 0;
                for (uint32_t sx = xa; sx < xb; sx++) { r += row[sx * 3]; g += row[sx * 3 + 1]; b += row[sx * 3 + 2]; }
                acc[x * 3] += r; acc[x * 3 + 1] += g; acc[x * 3 + 2] += b;
            }
        }
        for (uint32_t x = 0; x < sw; x++) {
            uint32_t cnt = (yb - ya) * (MAX(x0[x] + 1, x0[x + 1]) - x0[x]);
            for (int c = 0; c < 3; c++) small[((size_t)y * sw + x) * 3 + c] = (uint8_t)((acc[x * 3 + c] + cnt / 2) / cnt);
        }
    }
    /* orientation: destination (ox, oy) <- stored (x, y) */
    int o = S.im.orientation;
    for (uint32_t oy = 0; oy < ph; oy++)
        for (uint32_t ox = 0; ox < pw; ox++) {
            uint32_t x, y;
            switch (o) {
            case 2: x = sw - 1 - ox; y = oy; break;
            case 3: x = sw - 1 - ox; y = sh - 1 - oy; break;
            case 4: x = ox; y = sh - 1 - oy; break;
            case 5: x = oy; y = ox; break;
            case 6: x = oy; y = sh - 1 - ox; break;
            case 7: x = sw - 1 - oy; y = sh - 1 - ox; break;
            case 8: x = sw - 1 - oy; y = ox; break;
            default: x = ox; y = oy;
            }
            const uint8_t *s = small + ((size_t)y * sw + x) * 3;
            uint8_t *t = dst + ((size_t)oy * pw + ox) * 3;
            t[0] = s[0]; t[1] = s[1]; t[2] = s[2];
        }
    xfree(small); xfree(acc); xfree(x0);
    xfree(S.prgb); xfree(S.pgain);
    S.prgb = dst; S.pw = pw; S.ph = ph;
    S.pgain = xalloc((size_t)pw * ph);
    return S.pgain ? ERR_OK : ERR_MEMORY;
}
/* Preview: the SDR JPEG (re-encoded only when the size changes) and the
 * HDR JPEG with the current curve. */
int engine_preview(uint32_t maxw, uint32_t maxh) {
    if (!S.im.rgb) return ERR_STATE;
    maxw = CLAMP(maxw, 16u, 8192u); maxh = CLAMP(maxh, 16u, 8192u);
    int r = ERR_OK;
    if (!S.prgb || maxw != S.pmaxw || maxh != S.pmaxh) {
        r = preview_pixels(maxw, maxh);
        if (r) return r;
        S.pmaxw = maxw; S.pmaxh = maxh;
        buf_free(&S.psdr);
        Buf app = {0};
        if (icc_rgb(S.im.icc, S.im.icc_len)) jpeg_icc(&app, S.im.icc, S.im.icc_len);
        r = app.failed ? ERR_MEMORY : jpeg_encode(&S.psdr, S.prgb, S.pw, S.ph, PREVIEW_QUALITY, JPEG_420, &app);
        buf_free(&app);
        if (r) { buf_free(&S.psdr); xfree(S.prgb); S.prgb = NULL; return r; }
    }
    gain_codes(S.prgb, (size_t)S.pw * S.ph, &S.curve, S.pgain);
    buf_free(&S.phdr);
    r = hdr_write(&S.phdr, S.psdr.data, S.psdr.len, S.pgain, S.pw, S.ph, S.curve.stops);
    if (r) buf_free(&S.phdr);
    return r;
}

/* The Exif APP1 segment of the original JPEG, appended to `app`. */
static void copy_jpeg_exif(Buf *app) {
    Seg *seg = xalloc(sizeof(Seg) * SEG_MAX);
    int nseg = 0, eoi; uint32_t a, b;
    if (seg && !jpeg_layout(S.src, S.src_len, seg, &nseg, &a, &b, &eoi))
        for (int i = 0; i < nseg; i++) {
            const uint8_t *p = S.src + seg[i].at;
            if (p[1] == 0xE1 && seg[i].len > 10 && !xmemcmp(p + 4, "Exif\0\0", 6)) { buf_put(app, p, seg[i].len); break; }
        }
    xfree(seg);
}
/* Minimal TIFF check before a PNG's eXIf goes into the JPEG. */
static int exif_valid(const uint8_t *t, uint32_t n) {
    return n >= 8 && n <= 65527 && ((t[0] == 'M' && t[1] == 'M' && t[2] == 0 && t[3] == 42) || (t[0] == 'I' && t[1] == 'I' && t[2] == 42 && t[3] == 0));
}
int engine_export(void) {
    if (!S.im.rgb) return ERR_STATE;
    size_t n = (size_t)S.im.w * S.im.h;
    uint8_t *gain = xalloc(n);
    if (!gain) return ERR_MEMORY;
    gain_codes(S.im.rgb, n, &S.curve, gain);
    buf_free(&S.out);
    int r;
    if (!engine_reencodes()) r = hdr_write(&S.out, S.src, S.src_len, gain, S.im.w, S.im.h, S.curve.stops);
    else {
        /* PNG or greyscale JPEG (Apple ignores gain maps of greyscale JPEGs):
         * new colour JPEG (4:4:4) with the source's EXIF and RGB ICC profile */
        Buf app = {0}, primary = {0};
        if (S.im.kind == KIND_JPEG) copy_jpeg_exif(&app);
        else if (S.im.exif && exif_valid(S.im.exif, S.im.exif_len)) jpeg_app(&app, 1, "Exif\0", 6, S.im.exif, S.im.exif_len);
        if (icc_rgb(S.im.icc, S.im.icc_len)) jpeg_icc(&app, S.im.icc, S.im.icc_len);
        r = app.failed ? ERR_MEMORY : jpeg_encode(&primary, S.im.rgb, S.im.w, S.im.h, PNG_QUALITY, JPEG_444, &app);
        buf_free(&app);
        if (!r) r = hdr_write(&S.out, primary.data, primary.len, gain, S.im.w, S.im.h, S.curve.stops);
        buf_free(&primary);
    }
    xfree(gain);
    if (r) buf_free(&S.out);
    return r;
}
