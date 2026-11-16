/* PNG decoder: every colour type and bit depth, Adam7 interlacing, tRNS,
 * iCCP (kept for the JPEG), eXIf. Output is 8-bit RGB; transparency is
 * composited onto white. A cICP chunk with PQ or HLG marks an image that is
 * already HDR and is rejected. */
#include "image.h"

static const uint8_t adam_x[7] = {0, 4, 0, 2, 0, 1, 0}, adam_y[7] = {0, 0, 4, 0, 2, 0, 1};
static const uint8_t adam_dx[7] = {8, 8, 4, 4, 2, 2, 1}, adam_dy[7] = {8, 8, 8, 4, 4, 2, 2};

typedef struct {
    uint32_t w, h; int depth, ctype, interlace, channels;
    uint8_t pal[256][4]; int npal;
    int has_key; uint16_t key[3];
} PngInfo;

static inline int paeth(int a, int b, int c) {
    int p = a + b - c, pa = p > a ? p - a : a - p, pb = p > b ? p - b : b - p, pc = p > c ? p - c : c - p;
    return pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
}
/* Reverses the row filters of one pass in place. */
static int png_unfilter(uint8_t *raw, uint32_t rows, uint32_t rowbytes, uint32_t bpp) {
    uint8_t *prev = NULL;
    for (uint32_t y = 0; y < rows; y++) {
        uint8_t *line = raw + (size_t)y * (rowbytes + 1), f = line[0], *p = line + 1;
        if (f > 4) return ERR_CORRUPT;
        for (uint32_t i = 0; i < rowbytes; i++) {
            int a = i >= bpp ? p[i - bpp] : 0, b = prev ? prev[i] : 0, c = prev && i >= bpp ? prev[i - bpp] : 0;
            int v = p[i];
            switch (f) {
            case 1: v += a; break;
            case 2: v += b; break;
            case 3: v += (a + b) >> 1; break;
            case 4: v += paeth(a, b, c); break;
            }
            p[i] = (uint8_t)v;
        }
        prev = p;
    }
    return ERR_OK;
}
static inline uint32_t png_sample(const uint8_t *row, uint32_t index, int depth) {
    if (depth == 8) return row[index];
    if (depth == 16) return rd16be(row + 2 * index);
    uint32_t bit = index * (uint32_t)depth, byte = row[bit >> 3];
    return byte >> (8 - depth - (bit & 7)) & ((1u << depth) - 1);
}
/* Converts one unfiltered row into RGB (composited onto white). */
static void png_row(const PngInfo *pi, const uint8_t *row, uint32_t pw, uint8_t *dst, uint32_t step, int *alpha) {
    int d = pi->depth, ch = pi->channels;
    uint32_t maxv = (1u << d) - 1;
    for (uint32_t x = 0; x < pw; x++) {
        uint32_t s[4] = {0, 0, 0, 0}, c[3], a = 65535;
        for (int k = 0; k < ch; k++) s[k] = png_sample(row, x * (uint32_t)ch + (uint32_t)k, d);
        switch (pi->ctype) {
        case 3: {
            const uint8_t *e = pi->pal[s[0] < (uint32_t)pi->npal ? s[0] : 0];
            c[0] = e[0] * 257u; c[1] = e[1] * 257u; c[2] = e[2] * 257u; a = e[3] * 257u;
            break;
        }
        case 0: case 4: {
            uint32_t g = s[0] * 65535u / maxv;
            c[0] = c[1] = c[2] = g;
            if (pi->ctype == 4) a = s[1] * 65535u / maxv;
            else if (pi->has_key && s[0] == pi->key[0]) a = 0;
            break;
        }
        default:
            for (int k = 0; k < 3; k++) c[k] = s[k] * 65535u / maxv;
            if (pi->ctype == 6) a = s[3] * 65535u / maxv;
            else if (pi->has_key && s[0] == pi->key[0] && s[1] == pi->key[1] && s[2] == pi->key[2]) a = 0;
        }
        if (a < 65535) *alpha = 1;
        uint8_t *o = dst + (size_t)x * step * 3;
        for (int k = 0; k < 3; k++) {
            uint64_t v = ((uint64_t)c[k] * a + 65535ull * (65535 - a) + 32767) / 65535;   /* onto white */
            o[k] = (uint8_t)((v * 255 + 32767) / 65535);
        }
    }
}

static int png_decode(const uint8_t *d, uint32_t n, Image *im) {
    static const uint8_t sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    if (n < 8 + 25 || xmemcmp(d, sig, 8)) return ERR_FORMAT;
    PngInfo pi; xmemset(&pi, 0, sizeof pi);
    Buf idat = {0};
    int seen_ihdr = 0, seen_end = 0, r = ERR_OK;
    uint32_t pos = 8;
    while (pos + 12 <= n && !seen_end) {
        uint32_t len = rd32be(d + pos);
        const uint8_t *type = d + pos + 4, *data = d + pos + 8;
        if (len > n - pos - 12) { r = seen_ihdr && idat.len ? ERR_OK : ERR_CORRUPT; break; }   /* truncated tail */
        pos += 12 + len;
        if (!seen_ihdr) {
            if (xmemcmp(type, "IHDR", 4) || len != 13) { r = ERR_FORMAT; break; }
            pi.w = rd32be(data); pi.h = rd32be(data + 4); pi.depth = data[8]; pi.ctype = data[9]; pi.interlace = data[12];
            static const int chans[7] = {1, 0, 3, 1, 2, 0, 4};
            int ok = pi.ctype <= 6 && chans[pi.ctype] && data[10] == 0 && data[11] == 0 && pi.interlace <= 1;
            if (ok) switch (pi.ctype) {
                case 0: ok = pi.depth == 1 || pi.depth == 2 || pi.depth == 4 || pi.depth == 8 || pi.depth == 16; break;
                case 3: ok = pi.depth == 1 || pi.depth == 2 || pi.depth == 4 || pi.depth == 8; break;
                default: ok = pi.depth == 8 || pi.depth == 16;
            }
            if (!ok || !pi.w || !pi.h) { r = ERR_FORMAT; break; }
            if (pi.w > 65535 || pi.h > 65535 || (uint64_t)pi.w * pi.h > MAX_PIXELS) { r = ERR_TOO_LARGE; break; }
            pi.channels = chans[pi.ctype];
            seen_ihdr = 1;
            continue;
        }
        if (!xmemcmp(type, "IDAT", 4)) buf_put(&idat, data, len);
        else if (!xmemcmp(type, "IEND", 4)) seen_end = 1;
        else if (!xmemcmp(type, "PLTE", 4)) {
            if (len % 3 || len > 768) { r = ERR_CORRUPT; break; }
            pi.npal = (int)(len / 3);
            for (int i = 0; i < pi.npal; i++) { pi.pal[i][0] = data[3 * i]; pi.pal[i][1] = data[3 * i + 1]; pi.pal[i][2] = data[3 * i + 2]; pi.pal[i][3] = 255; }
        } else if (!xmemcmp(type, "tRNS", 4)) {
            if (pi.ctype == 3) for (uint32_t i = 0; i < len && i < 256; i++) pi.pal[i][3] = data[i];
            else if (pi.ctype == 0 && len >= 2) { pi.has_key = 1; pi.key[0] = (uint16_t)rd16be(data); }
            else if (pi.ctype == 2 && len >= 6) { pi.has_key = 1; for (int k = 0; k < 3; k++) pi.key[k] = (uint16_t)rd16be(data + 2 * k); }
        } else if (!xmemcmp(type, "cICP", 4)) {
            if (len >= 4 && (data[1] == 16 || data[1] == 18)) { im->detail = UNSUP_HDR_PNG; r = ERR_UNSUPPORTED; break; }
        } else if (!xmemcmp(type, "iCCP", 4) && !im->icc) {
            uint32_t k = 0;
            while (k < len && k < 80 && data[k]) k++;
            if (k + 2 < len && data[k + 1] == 0) {
                size_t cap = 4u << 20, got = 0;
                uint8_t *icc = xalloc(cap);
                if (icc && !zlib_inflate(data + k + 2, len - k - 2, icc, cap, &got) && got >= 132 && rd32be(icc) == got) {
                    im->icc = xrealloc(icc, got); im->icc_len = (uint32_t)got;
                } else xfree(icc);       /* unusable profile: treated as sRGB */
            }
        } else if (!xmemcmp(type, "eXIf", 4) && !im->exif && len >= 8) {
            im->exif = xalloc(len);
            if (im->exif) { xmemcpy(im->exif, data, len); im->exif_len = len; }
        }
    }
    if (!r && (!seen_ihdr || !idat.len)) r = ERR_CORRUPT;
    if (!r && idat.failed) r = ERR_MEMORY;
    if (!r && pi.ctype == 3 && !pi.npal) r = ERR_CORRUPT;
    /* Size of the filtered data, all passes. */
    uint32_t bits = (uint32_t)(pi.depth * pi.channels);
    uint64_t total = 0;
    for (int p = 0; !r && p < (pi.interlace ? 7 : 1); p++) {
        uint32_t sx = pi.interlace ? adam_x[p] : 0, sy = pi.interlace ? adam_y[p] : 0;
        uint32_t dx = pi.interlace ? adam_dx[p] : 1, dy = pi.interlace ? adam_dy[p] : 1;
        if (pi.w <= sx || pi.h <= sy) continue;
        uint64_t pw = (pi.w - sx + dx - 1) / dx, ph = (pi.h - sy + dy - 1) / dy;
        total += ph * (1 + (pw * bits + 7) / 8);
    }
    if (!r && total > 0xF0000000u) r = ERR_TOO_LARGE;
    uint8_t *raw = NULL;
    if (!r) {
        raw = xalloc((size_t)total);
        im->rgb = xalloc((size_t)pi.w * pi.h * 3);
        if (!raw || !im->rgb) r = ERR_MEMORY;
    }
    size_t got = 0;
    if (!r) r = zlib_inflate(idat.data, idat.len, raw, (size_t)total, &got);
    if (!r && got != total) r = ERR_CORRUPT;
    buf_free(&idat);
    int alpha = 0;
    uint8_t *pass = raw;
    for (int p = 0; !r && p < (pi.interlace ? 7 : 1); p++) {
        uint32_t sx = pi.interlace ? adam_x[p] : 0, sy = pi.interlace ? adam_y[p] : 0;
        uint32_t dx = pi.interlace ? adam_dx[p] : 1, dy = pi.interlace ? adam_dy[p] : 1;
        if (pi.w <= sx || pi.h <= sy) continue;
        uint32_t pw = (pi.w - sx + dx - 1) / dx, ph = (pi.h - sy + dy - 1) / dy;
        uint32_t rowbytes = (uint32_t)(((uint64_t)pw * bits + 7) / 8);
        r = png_unfilter(pass, ph, rowbytes, MAX(1u, bits / 8));
        for (uint32_t y = 0; !r && y < ph; y++)
            png_row(&pi, pass + (size_t)y * (rowbytes + 1) + 1, pw,
                    im->rgb + ((size_t)(sy + y * dy) * pi.w + sx) * 3, dx, &alpha);
        pass += (size_t)ph * (rowbytes + 1);
    }
    xfree(raw);
    if (r) { xfree(im->rgb); im->rgb = NULL; return r; }
    im->kind = KIND_PNG; im->w = pi.w; im->h = pi.h;
    im->orientation = exif_orientation(im->exif, im->exif_len);
    im->flags |= (alpha ? FLAG_ALPHA : 0) | (im->icc ? FLAG_ICC : 0) | (pi.ctype == 0 || pi.ctype == 4 ? FLAG_GRAY : 0);
    return ERR_OK;
}
