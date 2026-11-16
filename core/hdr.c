/* HDR JPEG with a gain map.
 *
 * The SDR image stays as it is. A greyscale gain map tells HDR displays how
 * much to brighten each pixel: in linear light, HDR = SDR * 2^(stops * m),
 * with m = code / 255 of the map. The code follows the "accordion" curve of
 * the pixel's brightness v (sRGB-encoded luminance):
 *
 *   m(v) = smoothstep(lo, hi, v),  lo = t - d, hi = t + d,  d = min(t, 1 - t) / 2
 *
 * where t is the threshold. Below lo nothing changes; above hi pixels get the
 * full gain. With t = 0 the whole image is stretched linearly (every pixel
 * times 2^stops, black stays black); a high threshold makes only the
 * highlights glow. HDR brightness grows monotonically with SDR brightness
 * for every setting, so the order of tones is always kept.
 *
 * Container: the primary JPEG (its compressed data untouched) gets an Ultra
 * HDR XMP directory, the ISO 21496-1 marker and an MPF index; the gain map
 * follows after its EOI with ISO 21496-1 metadata and Ultra HDR XMP. Offsets
 * are zero and the map uses the base colour space. */
#include "image.h"

#define GAINMAP_QUALITY 90
#define STOPS_DEN 1000000u

typedef struct {
    double stops, threshold;
    uint8_t code[4097];          /* linear luminance (x4096) -> gain map code */
    uint8_t ui[256];             /* encoded brightness (x255) -> code, for the curve view */
} Curve;

static float srgb_lin[256];
static void srgb_init(void) {
    static int ready;
    if (ready) return;
    for (int i = 0; i < 256; i++) {
        double c = i / 255.0;
        srgb_lin[i] = (float)(c <= 0.04045 ? c / 12.92 : m_pow((c + 0.055) / 1.055, 2.4));
    }
    ready = 1;
}
static double srgb_encode(double y) { return y <= 0.0031308 ? 12.92 * y : 1.055 * m_pow(y, 1.0 / 2.4) - 0.055; }
static double curve_m(double t, double v) {
    double d = 0.5 * MIN(t, 1.0 - t), lo = t - d, hi = t + d;
    if (v <= lo) return 0.0;
    if (v >= hi) return 1.0;
    double x = (v - lo) / (hi - lo);
    return x * x * (3.0 - 2.0 * x);
}
static void curve_build(Curve *c, double stops, double threshold) {
    srgb_init();
    c->stops = stops; c->threshold = threshold;
    for (int i = 0; i <= 4096; i++) c->code[i] = (uint8_t)(255.0 * curve_m(threshold, srgb_encode(i / 4096.0)) + 0.5);
    for (int i = 0; i < 256; i++) c->ui[i] = (uint8_t)(255.0 * curve_m(threshold, i / 255.0) + 0.5);
}
/* Gain map codes of n RGB pixels. */
static void gain_codes(const uint8_t *rgb, size_t n, const Curve *c, uint8_t *out) {
    for (size_t i = 0; i < n; i++, rgb += 3) {
        float y = 0.2126f * srgb_lin[rgb[0]] + 0.7152f * srgb_lin[rgb[1]] + 0.0722f * srgb_lin[rgb[2]];
        int k = (int)(y * 4096.0f + 0.5f);
        out[i] = c->code[CLAMP(k, 0, 4096)];
    }
}

/* ---- metadata ---- */
static const char xmp_id[] = "http://ns.adobe.com/xap/1.0/";              /* + NUL */
static const char xmp_ext_id[] = "http://ns.adobe.com/xmp/extension/";    /* + NUL */
static const char iso_id[] = "urn:iso:std:iso:ts:21496:-1";               /* + NUL */
static const char xpacket_begin[] = "<?xpacket begin=\"\xEF\xBB\xBF\" id=\"W5M0MpCehiHzreSzNTczkc9d\"?>\n";
static const char xpacket_end[] = "<?xpacket end=\"w\"?>";
static uint32_t put_text(char *d, const char *s) { uint32_t n = 0; while (s[n]) { if (d) d[n] = s[n]; n++; } return n; }
static uint32_t put_dec(char *d, uint32_t v) {
    char t[10]; uint32_t n = 0;
    do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    for (uint32_t i = 0; i < n; i++) if (d) d[i] = t[n - 1 - i];
    return n;
}
/* stops as rational numerator (denominator STOPS_DEN) and as decimal text */
static uint32_t stops_num(double stops) { return (uint32_t)(stops * STOPS_DEN + 0.5); }
static uint32_t put_stops(char *d, uint32_t num) {
    uint32_t n = put_dec(d, num / STOPS_DEN);
    char frac[7]; uint32_t f = num % STOPS_DEN;
    for (int i = 5; i >= 0; i--) { frac[i] = (char)('0' + f % 10); f /= 10; }
    int last = 5; while (last > 0 && frac[last] == '0') last--;
    if (d) d[n] = '.';
    n++;
    for (int i = 0; i <= last; i++) { if (d) d[n] = frac[i]; n++; }
    return n;
}
/* Primary image XMP (Ultra HDR container directory), padded to `size`
 * bytes; returns the unpadded size (d = NULL: just measure). */
static uint32_t primary_xmp(char *d, uint32_t size, uint32_t gainmap_len) {
    uint32_t n = put_text(d, xpacket_begin);
    n += put_text(d ? d + n : NULL,
        "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\" x:xmptk=\"Pseudo HDR\">\n"
        "<rdf:RDF xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\">\n"
        "<rdf:Description rdf:about=\"\" xmlns:Container=\"http://ns.google.com/photos/1.0/container/\"\n"
        " xmlns:Item=\"http://ns.google.com/photos/1.0/container/item/\"\n"
        " xmlns:hdrgm=\"http://ns.adobe.com/hdr-gain-map/1.0/\" hdrgm:Version=\"1.0\">\n"
        "<Container:Directory><rdf:Seq>\n"
        "<rdf:li rdf:parseType=\"Resource\"><Container:Item Item:Semantic=\"Primary\" Item:Mime=\"image/jpeg\"/></rdf:li>\n"
        "<rdf:li rdf:parseType=\"Resource\"><Container:Item Item:Semantic=\"GainMap\" Item:Mime=\"image/jpeg\" Item:Length=\"");
    n += put_dec(d ? d + n : NULL, gainmap_len);
    n += put_text(d ? d + n : NULL, "\"/></rdf:li>\n</rdf:Seq></Container:Directory>\n</rdf:Description>\n</rdf:RDF>\n</x:xmpmeta>\n");
    uint32_t used = n + (uint32_t)sizeof xpacket_end - 1;
    if (d && size >= used) {
        for (uint32_t i = n; i < size - (sizeof xpacket_end - 1); i++) d[i] = ' ';
        put_text(d + size - (sizeof xpacket_end - 1), xpacket_end);
    }
    return used;
}
/* Gain map XMP (Adobe gain map / Ultra HDR); d = NULL measures. */
static uint32_t gainmap_xmp(char *d, uint32_t num) {
    uint32_t n = put_text(d,
        "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\" x:xmptk=\"Pseudo HDR\">\n"
        "<rdf:RDF xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\">\n"
        "<rdf:Description rdf:about=\"\" xmlns:hdrgm=\"http://ns.adobe.com/hdr-gain-map/1.0/\"\n"
        " hdrgm:Version=\"1.0\" hdrgm:GainMapMin=\"0\" hdrgm:GainMapMax=\"");
    n += put_stops(d ? d + n : NULL, num);
    n += put_text(d ? d + n : NULL, "\" hdrgm:Gamma=\"1\"\n"
        " hdrgm:OffsetSDR=\"0\" hdrgm:OffsetHDR=\"0\" hdrgm:HDRCapacityMin=\"0\" hdrgm:HDRCapacityMax=\"");
    n += put_stops(d ? d + n : NULL, num);
    n += put_text(d ? d + n : NULL, "\"\n hdrgm:BaseRenditionIsHDR=\"False\"/>\n</rdf:RDF>\n</x:xmpmeta>\n");
    return n;
}
static uint8_t *put32be(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; return p + 4; }
static uint8_t *put16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; return p + 2; }
/* ISO 21496-1 GainMapMetadata: one channel, base colour space. */
static uint32_t iso_metadata(uint8_t *p0, uint32_t num) {
    const uint32_t v[] = {0, 1, num, STOPS_DEN,                     /* base / alternate headroom */
                          0, 1, num, STOPS_DEN, 1, 1, 0, 1, 0, 1};  /* min, max, gamma, offsets */
    uint8_t *p = put16(put16(p0, 0), 0);   /* minimum_version, writer_version */
    *p++ = 0x40;                           /* single channel, use_base_colour_space */
    for (uint32_t i = 0; i < sizeof v / sizeof v[0]; i++) p = put32be(p, v[i]);
    return (uint32_t)(p - p0);
}
/* MPF (CIPA DC-007) index of the two images; offsets relative to the TIFF
 * header that follows "MPF\0". */
#define MPF_SIZE 82
static void mpf_index(uint8_t *p, uint32_t primary_len, uint32_t gainmap_len, uint32_t gainmap_offset) {
    xmemset(p, 0, MPF_SIZE);
    p[0] = 'M'; p[1] = 'M'; put16(p + 2, 42); put32be(p + 4, 8);
    uint8_t *e = put16(p + 8, 3);
    e = put16(e, 0xB000); e = put16(e, 7); e = put32be(e, 4); e[0] = '0'; e[1] = '1'; e[2] = '0'; e[3] = '0'; e += 4;
    e = put16(e, 0xB001); e = put16(e, 4); e = put32be(e, 1); e = put32be(e, 2);
    e = put16(e, 0xB002); e = put16(e, 7); e = put32be(e, 32); e = put32be(e, 50);
    e = put32be(e, 0);                                   /* no next IFD */
    e = put32be(e, 0x030000); e = put32be(e, primary_len); e = put32be(e, 0); e += 4;   /* baseline primary */
    e = put32be(e, 0); e = put32be(e, gainmap_len); e = put32be(e, gainmap_offset);
}

/* ---- JPEG structure ---- */
typedef struct { uint32_t at, len; int group; } Seg;   /* group: 0 leading APP, 1 other APP/COM, 2 tables/frame */
enum { SEG_MAX = 512 };
/* Splits a JPEG into header segments (classified, metadata of earlier HDR
 * versions dropped) and the scan data from the first SOS up to EOI. */
static int jpeg_layout(const uint8_t *d, uint32_t n, Seg *seg, int *nseg, uint32_t *scan_at, uint32_t *scan_end, int *has_eoi) {
    if (n < 4 || d[0] != 0xFF || d[1] != 0xD8) return ERR_FORMAT;
    uint32_t pos = 2; int k = 0;
    for (;;) {
        while (pos < n && d[pos] != 0xFF) pos++;
        while (pos + 1 < n && d[pos + 1] == 0xFF) pos++;
        if (pos + 4 > n) return ERR_CORRUPT;
        int m = d[pos + 1];
        if (m == 0xDA) break;
        if (m == 0xD9 || (m >= 0xD0 && m <= 0xD7) || m == 0x01 || m == 0x00) { pos += 2; continue; }
        uint32_t len = rd16be(d + pos + 2);
        if (len < 2 || pos + 2 + len > n) return ERR_CORRUPT;
        const uint8_t *s = d + pos + 4; uint32_t sl = len - 2;
        int drop = 0, group = m >= 0xE0 && m <= 0xEF ? 1 : m == 0xFE ? 1 : 2;
        if (m == 0xE1 && sl >= sizeof xmp_id && !xmemcmp(s, xmp_id, sizeof xmp_id)) drop = 1;
        if (m == 0xE1 && sl >= sizeof xmp_ext_id && !xmemcmp(s, xmp_ext_id, sizeof xmp_ext_id)) drop = 1;
        if (m == 0xE2 && sl >= 4 && !xmemcmp(s, "MPF", 4)) drop = 1;
        if (m == 0xE2 && sl >= sizeof iso_id && !xmemcmp(s, iso_id, sizeof iso_id)) drop = 1;
        if (m == 0xE0 && sl >= 5 && !xmemcmp(s, "JFIF", 5)) group = 0;
        if (m == 0xE1 && sl >= 6 && !xmemcmp(s, "Exif\0\0", 6)) group = 0;
        if (!drop) {
            if (k == SEG_MAX) return ERR_UNSUPPORTED;
            seg[k].at = pos; seg[k].len = 2 + len; seg[k].group = group; k++;
        }
        pos += 2 + len;
    }
    *scan_at = pos; *has_eoi = 0;
    /* scans, tables between them, up to EOI; anything after EOI is dropped */
    while (pos + 1 < n) {
        int m = d[pos + 1];
        if (d[pos] != 0xFF) { pos++; continue; }
        if (m == 0xD9) { pos += 2; *has_eoi = 1; break; }
        if (m == 0xFF) { pos++; continue; }
        if (pos + 4 > n) { pos = n; break; }
        uint32_t len = rd16be(d + pos + 2);
        if (len < 2 || pos + 2 + len > n) { pos = n; break; }
        pos += 2 + len;
        if (m == 0xDA) {   /* entropy-coded data up to the next marker other than RSTn */
            while (pos + 1 < n && !(d[pos] == 0xFF && d[pos + 1] != 0 && d[pos + 1] != 0xFF && (d[pos + 1] < 0xD0 || d[pos + 1] > 0xD7))) pos++;
            if (pos + 1 >= n) pos = n;
        }
    }
    *scan_end = MIN(pos, n);
    *nseg = k;
    return ERR_OK;
}

/* Writes `jpeg` (any JPEG; its compressed image data is copied unchanged)
 * as an HDR JPEG with the gain map `gray` (w x h codes, same orientation
 * and size as the JPEG's stored image). */
static int hdr_write(Buf *o, const uint8_t *jpeg, uint32_t len, const uint8_t *gray, uint32_t w, uint32_t h, double stops) {
    Seg *seg = xalloc(sizeof(Seg) * SEG_MAX);
    if (!seg) return ERR_MEMORY;
    int nseg = 0, has_eoi = 0;
    uint32_t scan_at = 0, scan_end = 0;
    int r = jpeg_layout(jpeg, len, seg, &nseg, &scan_at, &scan_end, &has_eoi);
    uint32_t num = stops_num(stops);
    char *xmp = NULL; Buf app = {0};
    uint32_t start = o->len;
    if (!r) {
        buf_byte(o, 0xFF); buf_byte(o, 0xD8);
        for (int i = 0; i < nseg; i++) if (seg[i].group == 0) buf_put(o, jpeg + seg[i].at, seg[i].len);
        uint32_t xmp_size = primary_xmp(NULL, 0, 0xFFFFFFFFu) + 16, xmp_at = o->len + 4 + sizeof xmp_id;
        xmp = xalloc(xmp_size);
        if (!xmp) r = ERR_MEMORY;
        else { primary_xmp(xmp, xmp_size, 0); jpeg_app(o, 1, xmp_id, sizeof xmp_id, xmp, xmp_size); }
        for (int i = 0; i < nseg; i++) if (seg[i].group == 1) buf_put(o, jpeg + seg[i].at, seg[i].len);
        static const uint8_t iso_version[4] = {0, 0, 0, 0};
        jpeg_app(o, 2, iso_id, sizeof iso_id, iso_version, 4);
        uint8_t mpf[MPF_SIZE] = {0};
        uint32_t mpf_at = o->len + 4 + 4;
        jpeg_app(o, 2, "MPF", 4, mpf, MPF_SIZE);
        for (int i = 0; i < nseg; i++) if (seg[i].group == 2) buf_put(o, jpeg + seg[i].at, seg[i].len);
        buf_put(o, jpeg + scan_at, scan_end - scan_at);
        if (!has_eoi) { buf_byte(o, 0xFF); buf_byte(o, 0xD9); }
        uint32_t primary_len = o->len - start;
        /* the gain map image */
        char gx[1024];
        jpeg_app(&app, 1, xmp_id, sizeof xmp_id, gx, gainmap_xmp(gx, num));
        uint8_t iso[64];
        jpeg_app(&app, 2, iso_id, sizeof iso_id, iso, iso_metadata(iso, num));
        if (!r && app.failed) r = ERR_MEMORY;
        if (!r) r = jpeg_encode(o, gray, w, h, GAINMAP_QUALITY, JPEG_GRAY, &app);
        if (!r && o->failed) r = ERR_MEMORY;
        if (!r) {
            uint32_t gainmap_len = o->len - start - primary_len;
            primary_xmp(xmp, xmp_size, gainmap_len);
            xmemcpy(o->data + xmp_at, xmp, xmp_size);
            mpf_index(o->data + mpf_at, primary_len, gainmap_len, start + primary_len - mpf_at);
        }
    }
    xfree(seg); xfree(xmp); buf_free(&app);
    return r;
}
