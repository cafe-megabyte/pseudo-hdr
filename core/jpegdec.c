/* JPEG decoder: baseline, extended (8-bit Huffman) and progressive; any
 * sampling factors, restart intervals, greyscale, YCbCr and Adobe RGB
 * transform 0. Not supported (with a detail code): CMYK/YCCK, arithmetic
 * coding, 12-bit, lossless and hierarchical JPEG. Chroma is upsampled
 * bilinearly at the sample centres ("fancy" upsampling). The decoded image is
 * only used to compute the gain map and the preview; the exported JPEG keeps
 * the original compressed data. */
#include "image.h"

typedef struct {
    uint16_t fast[512];          /* len << 8 | value for codes up to 9 bits, 0 = longer */
    uint8_t vals[256];
    int32_t maxcode[18], valoff[17];
    int defined;
} JHuff;
typedef struct {
    const uint8_t *p, *end;
    uint32_t acc; int bits;
    int marker;                  /* a marker was reached: zeros follow */
} JBits;
typedef struct {
    int id, h, v, tq, td, ta;
    uint32_t cw, ch;             /* component size in samples */
    uint32_t bw, bh;             /* blocks, padded to whole MCUs */
    int16_t *coef;               /* bw * bh * 64, natural order, quantised */
    int pred;
} JComp;
typedef struct {
    uint16_t q[4][64];
    JHuff dc[4], ac[4];
    JComp comp[4]; int ncomp;
    uint32_t w, h; int hmax, vmax; uint32_t mcux, mcuy;
    int frame, progressive, restart, eobrun, scans;
    int adobe, transform;
    JBits b;
} JDec;

static const uint8_t jzz[64] = {0,1,8,16,9,2,3,10,17,24,32,25,18,11,4,5,12,19,26,33,40,48,41,34,27,20,13,6,7,14,21,28,
    35,42,49,56,57,50,43,36,29,22,15,23,30,37,44,51,58,59,52,45,38,31,39,46,53,60,61,54,47,55,62,63};

static int jhuff_build(JHuff *h, const uint8_t counts[16], const uint8_t *vals, int nvals) {
    xmemset(h, 0, sizeof(*h));
    xmemcpy(h->vals, vals, (size_t)nvals);
    int32_t code = 0; int k = 0;
    for (int len = 1; len <= 16; len++) {
        h->valoff[len] = k - code;
        for (int i = 0; i < counts[len - 1]; i++, k++, code++)
            if (len <= 9) {
                uint32_t first = (uint32_t)code << (9 - len), n = 1u << (9 - len);
                for (uint32_t f = 0; f < n; f++) h->fast[first + f] = (uint16_t)(len << 8 | vals[k]);
            }
        h->maxcode[len] = counts[len - 1] ? code - 1 : -1;
        if (code > (1 << len)) return 0;
        code <<= 1;
    }
    h->maxcode[17] = 0x7FFFFFFF;
    h->defined = 1;
    return 1;
}
static void jb_fill(JBits *b) {
    while (b->bits <= 24) {
        uint32_t c = 0;
        if (!b->marker && b->p < b->end) {
            c = *b->p;
            if (c == 0xFF) {
                uint8_t next = b->p + 1 < b->end ? b->p[1] : 0xD9;
                if (next == 0x00) b->p += 2;
                else if (next == 0xFF) { b->p++; continue; }   /* fill byte */
                else { b->marker = 1; c = 0; }
            } else b->p++;
        }
        b->acc |= c << (24 - b->bits); b->bits += 8;
    }
}
static inline uint32_t jb_get(JBits *b, int n) {
    if (!n) return 0;
    if (b->bits < n) jb_fill(b);
    uint32_t v = b->acc >> (32 - n);
    b->acc <<= n; b->bits -= n;
    return v;
}
static inline int jb_extend(uint32_t v, int s) { return v < (1u << (s - 1)) ? (int)v - (1 << s) + 1 : (int)v; }
static inline int jhuff_decode(JBits *b, const JHuff *h) {
    if (b->bits < 16) jb_fill(b);
    uint16_t f = h->fast[b->acc >> 23];
    if (f) { int len = f >> 8; b->acc <<= len; b->bits -= len; return f & 255; }
    for (int len = 10; len <= 16; len++) {
        int32_t code = (int32_t)(b->acc >> (32 - len));
        if (code <= h->maxcode[len]) { b->acc <<= len; b->bits -= len; return h->vals[code + h->valoff[len]]; }
    }
    return -1;
}

/* ---- block decoders ---- */
static int block_seq(JDec *j, JComp *c, int16_t *blk) {
    JBits *b = &j->b;
    int t = jhuff_decode(b, &j->dc[c->td]);
    if (t < 0 || t > 11) return ERR_CORRUPT;
    c->pred += t ? jb_extend(jb_get(b, t), t) : 0;
    blk[0] = (int16_t)c->pred;
    for (int k = 1; k < 64;) {
        int rs = jhuff_decode(b, &j->ac[c->ta]);
        if (rs < 0) return ERR_CORRUPT;
        int r = rs >> 4, s = rs & 15;
        if (!s) { if (r != 15) break; k += 16; continue; }
        k += r;
        if (k > 63) return ERR_CORRUPT;
        blk[jzz[k++]] = (int16_t)jb_extend(jb_get(b, s), s);
    }
    return ERR_OK;
}
static int block_dc(JDec *j, JComp *c, int16_t *blk, int ah, int al) {
    JBits *b = &j->b;
    if (ah) { if (jb_get(b, 1)) blk[0] = (int16_t)(blk[0] | (1 << al)); return ERR_OK; }
    int t = jhuff_decode(b, &j->dc[c->td]);
    if (t < 0 || t > 11) return ERR_CORRUPT;
    c->pred += t ? jb_extend(jb_get(b, t), t) : 0;
    blk[0] = (int16_t)(c->pred * (1 << al));
    return ERR_OK;
}
static int block_ac_first(JDec *j, JComp *c, int16_t *blk, int ss, int se, int al) {
    JBits *b = &j->b;
    if (j->eobrun > 0) { j->eobrun--; return ERR_OK; }
    for (int k = ss; k <= se; k++) {
        int rs = jhuff_decode(b, &j->ac[c->ta]);
        if (rs < 0) return ERR_CORRUPT;
        int r = rs >> 4, s = rs & 15;
        if (s) {
            k += r;
            if (k > 63) return ERR_CORRUPT;
            blk[jzz[k]] = (int16_t)(jb_extend(jb_get(b, s), s) * (1 << al));
        } else if (r == 15) k += 15;
        else {
            j->eobrun = (1 << r) - 1;
            if (r) j->eobrun += (int)jb_get(b, r);
            break;
        }
    }
    return ERR_OK;
}
static int block_ac_refine(JDec *j, JComp *c, int16_t *blk, int ss, int se, int al) {
    JBits *b = &j->b;
    int p1 = 1 << al, m1 = -1 * (1 << al), k = ss;
    if (j->eobrun == 0) {
        for (; k <= se; k++) {
            int rs = jhuff_decode(b, &j->ac[c->ta]);
            if (rs < 0) return ERR_CORRUPT;
            int r = rs >> 4, s = rs & 15;
            if (s) s = jb_get(b, 1) ? p1 : m1;
            else if (r != 15) {
                j->eobrun = 1 << r;
                if (r) j->eobrun += (int)jb_get(b, r);
                break;
            }
            do {
                int16_t *coef = blk + jzz[k];
                if (*coef) {
                    if (jb_get(b, 1) && !(*coef & p1)) *coef = (int16_t)(*coef + (*coef >= 0 ? p1 : m1));
                } else if (--r < 0) break;
                k++;
            } while (k <= se);
            if (s && k <= se) blk[jzz[k]] = (int16_t)s;
        }
    }
    if (j->eobrun > 0) {
        for (; k <= se; k++) {
            int16_t *coef = blk + jzz[k];
            if (*coef && jb_get(b, 1) && !(*coef & p1)) *coef = (int16_t)(*coef + (*coef >= 0 ? p1 : m1));
        }
        j->eobrun--;
    }
    return ERR_OK;
}

/* Moves past a restart marker (or to the next marker if data is damaged). */
static void jdec_restart(JDec *j) {
    JBits *b = &j->b;
    b->acc = 0; b->bits = 0;
    if (!b->marker) {
        while (b->p + 1 < b->end && !(b->p[0] == 0xFF && b->p[1] != 0 && b->p[1] != 0xFF)) b->p++;
        b->marker = 1;
    }
    if (b->p + 1 < b->end && b->p[1] >= 0xD0 && b->p[1] <= 0xD7) { b->p += 2; b->marker = 0; }
    for (int i = 0; i < j->ncomp; i++) j->comp[i].pred = 0;
    j->eobrun = 0;
}
/* Decodes one scan starting at d[*pos] (after the SOS header); *pos then
 * points at the next marker. */
static int jdec_scan(JDec *j, const uint8_t *d, uint32_t n, uint32_t *pos, JComp **sc, int ns, int ss, int se, int ah, int al) {
    JBits *b = &j->b;
    b->p = d + *pos; b->end = d + n; b->acc = 0; b->bits = 0; b->marker = 0;
    for (int i = 0; i < j->ncomp; i++) j->comp[i].pred = 0;
    j->eobrun = 0;
    int r = ERR_OK;
    uint32_t count = 0;
#define BLOCK(c, by, bx) do { \
        int16_t *blk = (c)->coef + ((size_t)(by) * (c)->bw + (bx)) * 64; \
        if (!j->progressive) r = block_seq(j, c, blk); \
        else if (ss == 0) r = block_dc(j, c, blk, ah, al); \
        else if (ah == 0) r = block_ac_first(j, c, blk, ss, se, al); \
        else r = block_ac_refine(j, c, blk, ss, se, al); \
    } while (0)
    if (ns == 1) {
        JComp *c = sc[0];
        uint32_t bw = (c->cw + 7) / 8, bh = (c->ch + 7) / 8, total = bw * bh;
        for (uint32_t by = 0; by < bh && !r; by++)
            for (uint32_t bx = 0; bx < bw && !r; bx++) {
                BLOCK(c, by, bx);
                if (j->restart && ++count % (uint32_t)j->restart == 0 && count < total) jdec_restart(j);
            }
    } else {
        uint32_t total = j->mcux * j->mcuy;
        for (uint32_t my = 0; my < j->mcuy && !r; my++)
            for (uint32_t mx = 0; mx < j->mcux && !r; mx++) {
                for (int i = 0; i < ns && !r; i++) {
                    JComp *c = sc[i];
                    for (int v = 0; v < c->v && !r; v++)
                        for (int h = 0; h < c->h && !r; h++) BLOCK(c, my * (uint32_t)c->v + (uint32_t)v, mx * (uint32_t)c->h + (uint32_t)h);
                }
                if (j->restart && ++count % (uint32_t)j->restart == 0 && count < total) jdec_restart(j);
            }
    }
#undef BLOCK
    /* Damaged data stops this scan; earlier scans and blocks are kept. */
    const uint8_t *p = b->p;
    while (p + 1 < b->end && !(p[0] == 0xFF && p[1] != 0 && p[1] != 0xFF && (p[1] < 0xD0 || p[1] > 0xD7))) p++;
    *pos = (uint32_t)(p - d);
    return r;
}

/* ---- inverse DCT, upsampling and colour conversion ---- */
static float idct_c[8][8];
static void idct_init(void) {
    static int ready;
    if (ready) return;
    static const double cosk[9] = {1.0, 0.98078528040323043, 0.92387953251128674, 0.83146961230254524, 0.70710678118654752,
        0.55557023301960218, 0.38268343236508977, 0.19509032201612826, 0.0};
    for (int u = 0; u < 8; u++) for (int x = 0; x < 8; x++) {
        int k = ((2 * x + 1) * u) % 32;
        double v = k <= 8 ? cosk[k] : k <= 16 ? -cosk[16 - k] : k <= 24 ? -cosk[k - 16] : cosk[32 - k];
        idct_c[u][x] = (float)(v * (u ? 0.5 : 0.35355339059327376));
    }
    ready = 1;
}
static void idct_block(const int16_t *coef, const uint16_t *q, uint8_t *out, uint32_t stride) {
    float f[64], t[64];
    for (int i = 0; i < 64; i++) f[i] = (float)(coef[i] * (int)q[i]);
    for (int v = 0; v < 8; v++) {
        const float *row = f + v * 8;
        float *tr = t + v * 8;
        int ac = 0;
        for (int u = 1; u < 8; u++) if (row[u] != 0) { ac = 1; break; }
        if (!ac) { float dc = idct_c[0][0] * row[0]; for (int x = 0; x < 8; x++) tr[x] = dc; continue; }
        for (int x = 0; x < 8; x++) {
            float s = 0; for (int u = 0; u < 8; u++) s += idct_c[u][x] * row[u];
            tr[x] = s;
        }
    }
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++) {
            float s = 128.0f; for (int v = 0; v < 8; v++) s += idct_c[v][y] * t[v * 8 + x];
            int px = (int)(s + 0.5f);
            out[(size_t)y * stride + x] = (uint8_t)CLAMP(px, 0, 255);
        }
}
/* Bilinear sample positions of an upsampled axis: index pairs and weights
 * (0..256) for each output coordinate. */
static void axis_map(uint32_t out_n, uint32_t in_n, int num, int den, uint32_t *i0, uint32_t *i1, uint16_t *w) {
    for (uint32_t x = 0; x < out_n; x++) {
        if (num == den) { i0[x] = i1[x] = MIN(x, in_n - 1); w[x] = 0; continue; }
        /* centre of output sample x in input coordinates */
        int64_t pos = ((int64_t)(2 * x + 1) * num - den) * 128 / den;   /* (x + 0.5) * num/den - 0.5, x256 */
        if (pos < 0) pos = 0;
        uint32_t a = (uint32_t)(pos >> 8), frac = (uint32_t)(pos & 255);
        if (a >= in_n - 1) { a = in_n - 1; frac = 0; }
        i0[x] = a; i1[x] = MIN(a + 1, in_n - 1); w[x] = (uint16_t)frac;
    }
}
static int jdec_output(JDec *j, Image *im) {
    idct_init();
    int nc = j->ncomp;
    uint8_t *plane[3] = {0};
    uint32_t stride[3];
    int r = ERR_OK;
    for (int i = 0; i < nc && !r; i++) {
        JComp *c = &j->comp[i];
        stride[i] = c->bw * 8;
        plane[i] = xalloc((size_t)stride[i] * c->bh * 8);
        if (!plane[i]) { r = ERR_MEMORY; break; }
        for (uint32_t by = 0; by < c->bh; by++)
            for (uint32_t bx = 0; bx < c->bw; bx++)
                idct_block(c->coef + ((size_t)by * c->bw + bx) * 64, j->q[c->tq], plane[i] + (size_t)by * 8 * stride[i] + bx * 8, stride[i]);
        xfree(c->coef); c->coef = NULL;
    }
    uint32_t w = j->w, h = j->h;
    if (!r) { im->rgb = xalloc((size_t)w * h * 3); if (!im->rgb) r = ERR_MEMORY; }
    uint32_t *xi = NULL, *yi = NULL; uint16_t *xw = NULL, *yw = NULL;
    if (!r) {
        xi = xalloc(sizeof(uint32_t) * 2 * (w * 3 + h * 3));
        xw = xalloc(sizeof(uint16_t) * (w * 3 + h * 3));
        if (!xi || !xw) r = ERR_MEMORY;
    }
    if (!r) {
        yi = xi + 2 * w * 3; yw = xw + w * 3;
        for (int i = 0; i < nc; i++) {
            JComp *c = &j->comp[i];
            axis_map(w, c->cw, c->h, j->hmax, xi + 2 * w * i, xi + 2 * w * i + w, xw + w * i);
            axis_map(h, c->ch, c->v, j->vmax, yi + 2 * h * i, yi + 2 * h * i + h, yw + h * i);
        }
        int rgb = nc == 3 && (j->adobe ? j->transform == 0 : (j->comp[0].id == 'R' && j->comp[1].id == 'G' && j->comp[2].id == 'B'));
        for (uint32_t y = 0; y < h; y++) {
            uint8_t *o = im->rgb + (size_t)y * w * 3;
            float v[3] = {0, 0, 0};
            for (uint32_t x = 0; x < w; x++) {
                for (int i = 0; i < nc; i++) {
                    const uint32_t *X0 = xi + 2 * w * i, *X1 = X0 + w, *Y0 = yi + 2 * h * i, *Y1 = Y0 + h;
                    const uint8_t *r0 = plane[i] + (size_t)Y0[y] * stride[i], *r1 = plane[i] + (size_t)Y1[y] * stride[i];
                    int wx = xw[w * i + x], wy = yw[h * i + y];
                    int top = r0[X0[x]] * (256 - wx) + r0[X1[x]] * wx, bot = r1[X0[x]] * (256 - wx) + r1[X1[x]] * wx;
                    v[i] = (float)(top * (256 - wy) + bot * wy) * (1.0f / 65536.0f);
                }
                float R, G, B;
                if (nc == 1) R = G = B = v[0];
                else if (rgb) { R = v[0]; G = v[1]; B = v[2]; }
                else {
                    float cb = v[1] - 128.0f, cr = v[2] - 128.0f;
                    R = v[0] + 1.402f * cr; G = v[0] - 0.344136f * cb - 0.714136f * cr; B = v[0] + 1.772f * cb;
                }
                int ri = (int)(R + 0.5f), gi = (int)(G + 0.5f), bi = (int)(B + 0.5f);
                o[0] = (uint8_t)CLAMP(ri, 0, 255); o[1] = (uint8_t)CLAMP(gi, 0, 255); o[2] = (uint8_t)CLAMP(bi, 0, 255);
                o += 3;
            }
        }
    }
    xfree(xi); xfree(xw);
    for (int i = 0; i < 3; i++) xfree(plane[i]);
    return r;
}

/* ---- markers ---- */
static int has_text(const uint8_t *p, uint32_t n, const char *s) {
    uint32_t k = (uint32_t)xstrlen(s);
    for (uint32_t i = 0; i + k <= n; i++) if (!xmemcmp(p + i, s, k)) return 1;
    return 0;
}
static int jdec_sof(JDec *j, const uint8_t *s, uint32_t len, int marker, Image *im) {
    if (j->frame) return ERR_CORRUPT;
    if (len < 6) return ERR_CORRUPT;
    if (s[0] != 8) { im->detail = UNSUP_PRECISION; return ERR_UNSUPPORTED; }
    j->h = rd16be(s + 1); j->w = rd16be(s + 3); j->ncomp = s[5];
    if (!j->h) { im->detail = UNSUP_LAYOUT; return ERR_UNSUPPORTED; }    /* height from DNL */
    if (!j->w) return ERR_CORRUPT;
    if ((uint64_t)j->w * j->h > MAX_PIXELS) return ERR_TOO_LARGE;
    if (j->ncomp == 4) { im->detail = UNSUP_CMYK; return ERR_UNSUPPORTED; }
    if (j->ncomp != 1 && j->ncomp != 3) { im->detail = UNSUP_LAYOUT; return ERR_UNSUPPORTED; }
    if (len < 6u + 3u * (uint32_t)j->ncomp) return ERR_CORRUPT;
    j->progressive = marker == 0xC2;
    j->hmax = j->vmax = 1;
    for (int i = 0; i < j->ncomp; i++) {
        JComp *c = &j->comp[i];
        c->id = s[6 + 3 * i]; c->h = s[7 + 3 * i] >> 4; c->v = s[7 + 3 * i] & 15; c->tq = s[8 + 3 * i];
        if (c->h < 1 || c->h > 4 || c->v < 1 || c->v > 4 || c->tq > 3) return ERR_CORRUPT;
        j->hmax = MAX(j->hmax, c->h); j->vmax = MAX(j->vmax, c->v);
    }
    if (j->ncomp == 1) j->comp[0].h = j->comp[0].v = j->hmax = j->vmax = 1;
    j->mcux = (j->w + 8u * (uint32_t)j->hmax - 1) / (8u * (uint32_t)j->hmax);
    j->mcuy = (j->h + 8u * (uint32_t)j->vmax - 1) / (8u * (uint32_t)j->vmax);
    for (int i = 0; i < j->ncomp; i++) {
        JComp *c = &j->comp[i];
        c->cw = (j->w * (uint32_t)c->h + (uint32_t)j->hmax - 1) / (uint32_t)j->hmax;
        c->ch = (j->h * (uint32_t)c->v + (uint32_t)j->vmax - 1) / (uint32_t)j->vmax;
        c->bw = j->mcux * (uint32_t)c->h; c->bh = j->mcuy * (uint32_t)c->v;
        c->coef = xcalloc((size_t)c->bw * c->bh * 64 * sizeof(int16_t));
        if (!c->coef) return ERR_MEMORY;
    }
    j->frame = 1;
    return ERR_OK;
}
static int jdec_dht(JDec *j, const uint8_t *s, uint32_t len) {
    while (len >= 17) {
        int tc = s[0] >> 4, th = s[0] & 15, total = 0;
        if (tc > 1 || th > 3) return ERR_CORRUPT;
        for (int i = 0; i < 16; i++) total += s[1 + i];
        if (total > 256 || (uint32_t)(17 + total) > len) return ERR_CORRUPT;
        if (!jhuff_build(tc ? &j->ac[th] : &j->dc[th], s + 1, s + 17, total)) return ERR_CORRUPT;
        s += 17 + total; len -= 17u + (uint32_t)total;
    }
    return len ? ERR_CORRUPT : ERR_OK;
}
static int jdec_dqt(JDec *j, const uint8_t *s, uint32_t len) {
    while (len) {
        int pq = s[0] >> 4, tq = s[0] & 15;
        uint32_t size = pq ? 129 : 65;
        if (pq > 1 || tq > 3 || len < size) return ERR_CORRUPT;
        for (int k = 0; k < 64; k++) j->q[tq][jzz[k]] = (uint16_t)(pq ? rd16be(s + 1 + 2 * k) : s[1 + k]);
        s += size; len -= size;
    }
    return ERR_OK;
}

static int jpeg_decode(const uint8_t *d, uint32_t n, Image *im) {
    if (n < 4 || d[0] != 0xFF || d[1] != 0xD8) return ERR_FORMAT;
    JDec *j = xcalloc(sizeof(JDec));
    if (!j) return ERR_MEMORY;
    const uint8_t *icc_part[256] = {0}; uint32_t icc_size[256] = {0}; int icc_count = 0;
    int r = ERR_OK;
    uint32_t pos = 2;
    while (!r) {
        while (pos < n && d[pos] != 0xFF) pos++;          /* tolerate junk between segments */
        while (pos + 1 < n && d[pos + 1] == 0xFF) pos++;   /* fill bytes */
        if (pos + 1 >= n) break;
        int m = d[pos + 1];
        if (m == 0xD9) break;
        if ((m >= 0xD0 && m <= 0xD7) || m == 0x01 || m == 0x00) { pos += 2; continue; }
        if (pos + 4 > n) { r = ERR_CORRUPT; break; }
        uint32_t len = rd16be(d + pos + 2);
        if (len < 2 || pos + 2 + len > n) { r = j->scans ? ERR_OK : ERR_CORRUPT; break; }
        const uint8_t *s = d + pos + 4;
        uint32_t sl = len - 2;
        pos += 2 + len;
        switch (m) {
        case 0xC0: case 0xC1: case 0xC2: r = jdec_sof(j, s, sl, m, im); break;
        case 0xC3: im->detail = UNSUP_PRECISION; r = ERR_UNSUPPORTED; break;               /* lossless */
        case 0xC5: case 0xC6: case 0xC7: im->detail = UNSUP_LAYOUT; r = ERR_UNSUPPORTED; break;   /* hierarchical */
        case 0xC9: case 0xCA: case 0xCB: case 0xCD: case 0xCE: case 0xCF:
            im->detail = UNSUP_ARITHMETIC; r = ERR_UNSUPPORTED; break;
        case 0xC4: r = jdec_dht(j, s, sl); break;
        case 0xDB: r = jdec_dqt(j, s, sl); break;
        case 0xDD: if (sl < 2) r = ERR_CORRUPT; else j->restart = (int)rd16be(s); break;
        case 0xEE:
            if (sl >= 12 && !xmemcmp(s, "Adobe", 5)) { j->adobe = 1; j->transform = s[11]; }
            break;
        case 0xE1:
            if (sl > 6 && !xmemcmp(s, "Exif\0\0", 6)) { if (im->orientation <= 1) im->orientation = exif_orientation(s + 6, sl - 6); }
            else if (sl > 29 && !xmemcmp(s, "http://ns.adobe.com/xap/1.0/", 29) && (has_text(s, sl, "hdrgm:") || has_text(s, sl, "HDRGainMap")))
                im->flags |= FLAG_GAINMAP;
            break;
        case 0xE2:
            if (sl > 14 && !xmemcmp(s, "ICC_PROFILE", 12)) {
                int seq = s[12], count = s[13];
                if (seq >= 1 && seq <= count && !icc_part[seq]) { icc_part[seq] = s + 14; icc_size[seq] = sl - 14; icc_count = count; }
            } else if (sl >= 28 && !xmemcmp(s, "urn:iso:std:iso:ts:21496:-1", 28)) im->flags |= FLAG_GAINMAP;
            break;
        case 0xDA: {
            if (!j->frame || sl < 6) { r = ERR_CORRUPT; break; }
            int ns = s[0];
            if (ns < 1 || ns > 4 || sl < 4u + 2u * (uint32_t)ns) { r = ERR_CORRUPT; break; }
            JComp *sc[4]; int blocks = 0;
            for (int i = 0; i < ns && !r; i++) {
                int id = s[1 + 2 * i], k;
                for (k = 0; k < j->ncomp && j->comp[k].id != id; k++) {}
                if (k == j->ncomp) { r = ERR_CORRUPT; break; }
                sc[i] = &j->comp[k];
                sc[i]->td = s[2 + 2 * i] >> 4; sc[i]->ta = s[2 + 2 * i] & 15;
                if (sc[i]->td > 3 || sc[i]->ta > 3) r = ERR_CORRUPT;
                blocks += sc[i]->h * sc[i]->v;
            }
            if (r) break;
            if (ns > 1 && blocks > 10) { r = ERR_CORRUPT; break; }
            const uint8_t *t = s + 1 + 2 * ns;
            int ss = t[0], se = t[1], ah = t[2] >> 4, al = t[2] & 15;
            if (j->progressive) {
                if (ss > se || se > 63 || (ss == 0 && se != 0) || (ss > 0 && ns != 1) || al > 13) { r = ERR_CORRUPT; break; }
            } else { ss = 0; se = 63; ah = al = 0; }
            for (int i = 0; i < ns; i++) {
                int need_dc = ss == 0 && !ah, need_ac = se > 0;
                if ((need_dc && !j->dc[sc[i]->td].defined) || (need_ac && !j->ac[sc[i]->ta].defined)) r = ERR_CORRUPT;
            }
            if (r) break;
            r = jdec_scan(j, d, n, &pos, sc, ns, ss, se, ah, al);
            if (r == ERR_CORRUPT && j->scans) r = ERR_OK;      /* keep what was decoded */
            j->scans++;
            break;
        }
        }
    }
    if (!r && (!j->frame || !j->scans)) r = ERR_CORRUPT;
    if (!r) r = jdec_output(j, im);
    if (!r) {
        im->kind = KIND_JPEG; im->w = j->w; im->h = j->h;
        if (im->orientation < 1) im->orientation = 1;
        if (j->progressive) im->flags |= FLAG_PROGRESSIVE;
        if (j->ncomp == 1) im->flags |= FLAG_GRAY;
        /* complete ICC profile from its chunks */
        uint32_t total = 0; int complete = icc_count > 0;
        for (int i = 1; i <= icc_count; i++) { if (!icc_part[i]) complete = 0; else total += icc_size[i]; }
        if (complete && total) {
            im->icc = xalloc(total);
            if (im->icc) {
                for (int i = 1; i <= icc_count; i++) { xmemcpy(im->icc + im->icc_len, icc_part[i], icc_size[i]); im->icc_len += icc_size[i]; }
                im->flags |= FLAG_ICC;
            }
        }
    }
    for (int i = 0; i < 4; i++) xfree(j->comp[i].coef);
    xfree(j);
    return r;
}
