/* Baseline JPEG encoder: colour (4:2:0 or 4:4:4) or greyscale, IJG quality
 * scaling, optimised Huffman tables. Statistics come from a first pass (all
 * rows for small images, every fourth MCU row for large ones); every valid
 * symbol keeps a code, so a partial statistic never makes a symbol
 * unencodable. Input is always a complete in-memory image. */
#include "base.h"

static const uint8_t zigzag[64] = {0,1,8,16,9,2,3,10,17,24,32,25,18,11,4,5,12,19,26,33,40,48,41,34,27,20,13,6,7,14,21,28,
    35,42,49,56,57,50,43,36,29,22,15,23,30,37,44,51,58,59,52,45,38,31,39,46,53,60,61,54,47,55,62,63};
static const uint8_t std_lum[64] = {16,11,10,16,24,40,51,61,12,12,14,19,26,58,60,55,14,13,16,24,40,57,69,56,14,17,22,29,51,87,80,62,
    18,22,37,56,68,109,103,77,24,35,55,64,81,104,113,92,49,64,78,87,103,121,120,101,72,92,95,98,112,100,103,99};
static const uint8_t std_chr[64] = {17,18,24,47,99,99,99,99,18,21,26,66,99,99,99,99,24,26,56,99,99,99,99,99,47,66,99,99,99,99,99,99,
    99,99,99,99,99,99,99,99,99,99,99,99,99,99,99,99,99,99,99,99,99,99,99,99,99,99,99,99,99,99,99,99};

enum { JPEG_GRAY = 0, JPEG_444 = 1, JPEG_420 = 2 };
typedef struct {
    uint32_t w, h;
    int mode;                    /* JPEG_GRAY, JPEG_444, JPEG_420 */
    uint32_t mcu;                /* MCU size in pixels: 8 or 16 */
    uint8_t q[2][64];            /* natural order */
    float qinv[2][64];
    uint32_t freq[4][257];       /* DC lum, AC lum, DC chr, AC chr */
    uint8_t counts[4][16], syms[4][256]; int nsym[4];
    uint16_t code[4][256]; uint8_t size[4][256];
    int pred[3];
    BitW bw;
    float *ybuf, *cbuf;          /* one MCU row of luma and of both chroma planes */
    uint32_t wm;                 /* width rounded up to whole MCUs */
    uint32_t restart, mcus;      /* restart interval (0: none), MCUs written */
} Jpeg;
/* Restart interval of colour images; only the checks set it, to produce
 * test files with RSTn markers like many cameras write. */
static uint32_t jpeg_test_restart;

static float dct_c[8][8];
static int dct_ready;
static void dct_init(void) {
    if (dct_ready) return;
    static const double cosk[9] = {1.0, 0.98078528040323043, 0.92387953251128674, 0.83146961230254524, 0.70710678118654752,
        0.55557023301960218, 0.38268343236508977, 0.19509032201612826, 0.0};
    for (int u = 0; u < 8; u++) for (int x = 0; x < 8; x++) {
        int k = ((2 * x + 1) * u) % 32;      /* cos(k*pi/16) */
        double v = k <= 8 ? cosk[k] : k <= 16 ? -cosk[16 - k] : k <= 24 ? -cosk[k - 16] : cosk[32 - k];
        dct_c[u][x] = (float)(v * (u ? 0.5 : 0.35355339059327376));
    }
    dct_ready = 1;
}
/* Forward DCT + quantisation of one 8x8 block (values already level-shifted). */
static void fdct_quant(const float *blk, const float *qinv, int16_t *out) {
    float tmp[64];
    for (int y = 0; y < 8; y++) for (int u = 0; u < 8; u++) {
        float s = 0; for (int x = 0; x < 8; x++) s += dct_c[u][x] * blk[y * 8 + x];
        tmp[y * 8 + u] = s;
    }
    for (int u = 0; u < 8; u++) for (int v = 0; v < 8; v++) {
        float s = 0; for (int y = 0; y < 8; y++) s += dct_c[v][y] * tmp[y * 8 + u];
        float q = s * qinv[v * 8 + u];
        out[v * 8 + u] = (int16_t)(q < 0 ? q - 0.5f : q + 0.5f);
    }
}
static inline int jcat(int v) { unsigned a = (unsigned)(v < 0 ? -v : v); int n = 0; while (a) { n++; a >>= 1; } return n; }

static void block_stats(Jpeg *j, const int16_t *c, int comp) {
    int t = comp ? 2 : 0;
    int diff = c[0] - j->pred[comp]; j->pred[comp] = c[0];
    j->freq[t][jcat(diff)]++;
    int run = 0;
    for (int k = 1; k < 64; k++) {
        int v = c[zigzag[k]];
        if (!v) { run++; continue; }
        while (run > 15) { j->freq[t + 1][0xF0]++; run -= 16; }
        j->freq[t + 1][run << 4 | jcat(v)]++; run = 0;
    }
    if (run) j->freq[t + 1][0]++;
}
static void block_write(Jpeg *j, const int16_t *c, int comp) {
    int t = comp ? 2 : 0;
    int diff = c[0] - j->pred[comp]; j->pred[comp] = c[0];
    int s = jcat(diff);
    bitw_put(&j->bw, j->code[t][s], j->size[t][s]);
    if (s) bitw_put(&j->bw, (uint32_t)(diff < 0 ? diff - 1 : diff), s);
    int run = 0;
    for (int k = 1; k < 64; k++) {
        int v = c[zigzag[k]];
        if (!v) { run++; continue; }
        while (run > 15) { bitw_put(&j->bw, j->code[t + 1][0xF0], j->size[t + 1][0xF0]); run -= 16; }
        int sz = jcat(v), sym = run << 4 | sz;
        bitw_put(&j->bw, j->code[t + 1][sym], j->size[t + 1][sym]);
        bitw_put(&j->bw, (uint32_t)(v < 0 ? v - 1 : v), sz);
        run = 0;
    }
    if (run) bitw_put(&j->bw, j->code[t + 1][0], j->size[t + 1][0]);
}
static void jpeg_free(Jpeg *j) { xfree(j->ybuf); xfree(j->cbuf); j->ybuf = j->cbuf = NULL; }
static int jpeg_init(Jpeg *j, uint32_t w, uint32_t h, int quality, int mode) {
    xmemset(j, 0, sizeof(*j));
    if (!w || !h || w > 65535 || h > 65535) return ERR_TOO_LARGE;
    dct_init();
    j->w = w; j->h = h; j->mode = mode;
    j->restart = mode == JPEG_GRAY ? 0 : jpeg_test_restart;
    j->mcu = mode == JPEG_420 ? 16 : 8;
    j->wm = (w + j->mcu - 1) / j->mcu * j->mcu;
    quality = CLAMP(quality, 1, 100);
    int scale = quality < 50 ? 5000 / quality : 200 - 2 * quality;
    for (int i = 0; i < 64; i++) {
        int a = (std_lum[i] * scale + 50) / 100, b = (std_chr[i] * scale + 50) / 100;
        j->q[0][i] = (uint8_t)CLAMP(a, 1, 255); j->q[1][i] = (uint8_t)CLAMP(b, 1, 255);
        j->qinv[0][i] = 1.0f / j->q[0][i]; j->qinv[1][i] = 1.0f / j->q[1][i];
    }
    j->ybuf = xalloc((size_t)j->wm * j->mcu * sizeof(float));
    if (mode == JPEG_GRAY) return j->ybuf ? ERR_OK : ERR_MEMORY;
    uint32_t cw = mode == JPEG_420 ? j->wm / 2 : j->wm;
    j->cbuf = xalloc((size_t)cw * 8 * 2 * sizeof(float));
    if (!j->ybuf || !j->cbuf) { jpeg_free(j); return ERR_MEMORY; }
    return ERR_OK;
}

/* Converts up to `mcu` pixel rows into the MCU buffers (edges replicated). */
static void mcu_load(Jpeg *j, const uint8_t *px, uint32_t rows) {
    uint32_t w = j->w, wm = j->wm, n = j->mcu;
    if (j->mode == JPEG_GRAY) {
        for (uint32_t y = 0; y < 8; y++) {
            const uint8_t *src = px + (size_t)MIN(y, rows - 1) * w;
            float *yr = j->ybuf + (size_t)y * wm;
            for (uint32_t x = 0; x < wm; x++) yr[x] = src[MIN(x, w - 1)] - 128.0f;
        }
        return;
    }
    for (uint32_t y = 0; y < n; y++) {
        const uint8_t *src = px + (size_t)MIN(y, rows - 1) * w * 3;
        float *yr = j->ybuf + (size_t)y * wm;
        for (uint32_t x = 0; x < wm; x++) {
            const uint8_t *p = src + (size_t)MIN(x, w - 1) * 3;
            yr[x] = 0.299f * p[0] + 0.587f * p[1] + 0.114f * p[2] - 128.0f;
        }
    }
    if (j->mode == JPEG_444) {
        for (uint32_t y = 0; y < 8; y++) {
            const uint8_t *src = px + (size_t)MIN(y, rows - 1) * w * 3;
            float *cb = j->cbuf + (size_t)y * wm, *cr = j->cbuf + (size_t)(8 + y) * wm;
            for (uint32_t x = 0; x < wm; x++) {
                const uint8_t *p = src + (size_t)MIN(x, w - 1) * 3;
                float r = p[0], g = p[1], b = p[2];
                cb[x] = -0.168736f * r - 0.331264f * g + 0.5f * b;
                cr[x] = 0.5f * r - 0.418688f * g - 0.081312f * b;
            }
        }
        return;
    }
    uint32_t cw = wm / 2;
    for (uint32_t y = 0; y < 8; y++) {
        const uint8_t *s0 = px + (size_t)MIN(2 * y, rows - 1) * w * 3, *s1 = px + (size_t)MIN(2 * y + 1, rows - 1) * w * 3;
        float *cb = j->cbuf + (size_t)y * cw, *cr = j->cbuf + (size_t)(8 + y) * cw;
        for (uint32_t x = 0; x < cw; x++) {
            uint32_t xa = MIN(2 * x, w - 1), xb = MIN(2 * x + 1, w - 1);
            float r = (s0[xa * 3] + s0[xb * 3] + s1[xa * 3] + s1[xb * 3]) * 0.25f;
            float g = (s0[xa * 3 + 1] + s0[xb * 3 + 1] + s1[xa * 3 + 1] + s1[xb * 3 + 1]) * 0.25f;
            float b = (s0[xa * 3 + 2] + s0[xb * 3 + 2] + s1[xa * 3 + 2] + s1[xb * 3 + 2]) * 0.25f;
            cb[x] = -0.168736f * r - 0.331264f * g + 0.5f * b;
            cr[x] = 0.5f * r - 0.418688f * g - 0.081312f * b;
        }
    }
}
static void code_block(Jpeg *j, const float *src, uint32_t stride, int table, int comp, int write) {
    float blk[64]; int16_t coef[64];
    for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x++) blk[y * 8 + x] = src[(size_t)y * stride + x];
    fdct_quant(blk, j->qinv[table], coef);
    if (write) block_write(j, coef, comp); else block_stats(j, coef, comp);
}
/* Processes one MCU row: statistics (write = 0) or entropy coding. */
static void mcu_row(Jpeg *j, const uint8_t *px, uint32_t rows, int write) {
    mcu_load(j, px, rows);
    uint32_t wm = j->wm, total = (j->w + j->mcu - 1) / j->mcu * ((j->h + j->mcu - 1) / j->mcu);
    for (uint32_t mx = 0; mx < wm / j->mcu; mx++) {
        if (write && j->restart && j->mcus && j->mcus % j->restart == 0 && j->mcus < total) {
            bitw_flush(&j->bw);
            buf_byte(j->bw.out, 0xFF); buf_byte(j->bw.out, (uint8_t)(0xD0 + (j->mcus / j->restart - 1) % 8));
            j->pred[0] = j->pred[1] = j->pred[2] = 0;
        }
        if (write) j->mcus++;
        if (j->mode == JPEG_GRAY) { code_block(j, j->ybuf + mx * 8, wm, 0, 0, write); continue; }
        if (j->mode == JPEG_420) {
            for (int b = 0; b < 4; b++)
                code_block(j, j->ybuf + (size_t)((b >> 1) * 8) * wm + mx * 16 + (b & 1) * 8, wm, 0, 0, write);
            uint32_t cw = wm / 2;
            for (int c = 0; c < 2; c++) code_block(j, j->cbuf + (size_t)c * 8 * cw + mx * 8, cw, 1, 1 + c, write);
        } else {
            code_block(j, j->ybuf + mx * 8, wm, 0, 0, write);
            for (int c = 0; c < 2; c++) code_block(j, j->cbuf + (size_t)c * 8 * wm + mx * 8, wm, 1, 1 + c, write);
        }
    }
}
static void jpeg_tables(Jpeg *j) {
    for (int t = 0; t < (j->mode == JPEG_GRAY ? 2 : 4); t++) {
        if (t & 1) {   /* AC: EOB, ZRL and every run/size pair */
            j->freq[t][0]++; j->freq[t][0xF0]++;
            for (int r = 0; r < 16; r++) for (int s = 1; s <= 10; s++) j->freq[t][r << 4 | s]++;
        } else for (int s = 0; s <= 11; s++) j->freq[t][s]++;
        huff_optimal(j->freq[t], j->counts[t], j->syms[t], &j->nsym[t]);
        huff_codes(j->counts[t], j->syms[t], j->code[t], j->size[t]);
    }
    j->pred[0] = j->pred[1] = j->pred[2] = 0;
}
static void put16be(Buf *b, uint32_t v) { buf_byte(b, (uint8_t)(v >> 8)); buf_byte(b, (uint8_t)v); }
/* Writes an APPn segment: marker, length, identifier (with its NULs), data. */
static int jpeg_app(Buf *out, int n, const char *id, uint32_t id_len, const void *data, uint32_t len) {
    if (id_len + len + 2 > 65535) return 0;
    buf_byte(out, 0xFF); buf_byte(out, (uint8_t)(0xE0 + n)); put16be(out, id_len + len + 2);
    buf_put(out, id, id_len); buf_put(out, data, len);
    return 1;
}
/* ICC profile as APP2 chunks of at most 65519 bytes (ICC.1 Annex B.4). */
static void jpeg_icc(Buf *out, const uint8_t *icc, uint32_t len) {
    if (!icc || !len) return;
    uint32_t chunk = 65519, count = (len + chunk - 1) / chunk;
    if (count > 255) return;
    for (uint32_t i = 0; i < count; i++) {
        uint8_t id[14] = {'I','C','C','_','P','R','O','F','I','L','E',0, (uint8_t)(i + 1), (uint8_t)count};
        uint32_t n = MIN(chunk, len - i * chunk);
        jpeg_app(out, 2, (const char *)id, 14, icc + i * chunk, n);
    }
}
/* Quantisation and Huffman tables, frame and scan header; starts the scan. */
static void jpeg_frame(Jpeg *j, Buf *out) {
    int gray = j->mode == JPEG_GRAY, nq = gray ? 1 : 2, nt = gray ? 2 : 4, nc = gray ? 1 : 3;
    for (int t = 0; t < nq; t++) {
        buf_byte(out, 0xFF); buf_byte(out, 0xDB); put16be(out, 67); buf_byte(out, (uint8_t)t);
        for (int k = 0; k < 64; k++) buf_byte(out, j->q[t][zigzag[k]]);
    }
    uint8_t comps[9] = {1, j->mode == JPEG_420 ? 0x22 : 0x11, 0, 2, 0x11, 1, 3, 0x11, 1};
    if (j->restart) { buf_byte(out, 0xFF); buf_byte(out, 0xDD); put16be(out, 4); put16be(out, j->restart); }
    buf_byte(out, 0xFF); buf_byte(out, 0xC0); put16be(out, 8 + 3 * nc); buf_byte(out, 8);
    put16be(out, j->h); put16be(out, j->w); buf_byte(out, (uint8_t)nc); buf_put(out, comps, 3 * nc);
    static const uint8_t ids[4] = {0x00, 0x10, 0x01, 0x11};
    for (int t = 0; t < nt; t++) {
        buf_byte(out, 0xFF); buf_byte(out, 0xC4); put16be(out, 19 + j->nsym[t]); buf_byte(out, ids[t]);
        buf_put(out, j->counts[t], 16); buf_put(out, j->syms[t], (uint32_t)j->nsym[t]);
    }
    static const uint8_t sos[12] = {0xFF, 0xDA, 0, 12, 3, 1, 0x00, 2, 0x11, 3, 0x11, 0}, gray_sos[8] = {0xFF, 0xDA, 0, 8, 1, 1, 0x00, 0};
    if (gray) buf_put(out, gray_sos, 8); else buf_put(out, sos, 12);
    buf_byte(out, 63); buf_byte(out, 0);
    j->bw.out = out; j->bw.acc = 0; j->bw.bits = 0;
}

/* Encodes a complete image: RGB (3 bytes per pixel) or, with JPEG_GRAY,
 * one byte per pixel. `app` holds ready-made segments written after SOI. */
static int jpeg_encode(Buf *out, const uint8_t *px, uint32_t w, uint32_t h, int quality, int mode, const Buf *app) {
    Jpeg j;
    int r = jpeg_init(&j, w, h, quality, mode);
    if (r) return r;
    size_t bpp = mode == JPEG_GRAY ? 1 : 3, row = (size_t)w * bpp;
    uint32_t n = j.mcu, step = (uint64_t)w * h > 4000000u ? 4 * n : n;
    for (uint32_t y = 0; y < h; y += step) mcu_row(&j, px + y * row, MIN(n, h - y), 0);
    jpeg_tables(&j);
    buf_byte(out, 0xFF); buf_byte(out, 0xD8);
    if (app && app->len) buf_put(out, app->data, app->len);
    jpeg_frame(&j, out);
    for (uint32_t y = 0; y < h; y += n) mcu_row(&j, px + y * row, MIN(n, h - y), 1);
    bitw_flush(&j.bw); buf_byte(out, 0xFF); buf_byte(out, 0xD9);
    jpeg_free(&j);
    return out->failed ? ERR_MEMORY : ERR_OK;
}
