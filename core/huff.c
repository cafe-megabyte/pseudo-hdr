/* Entropy-coder helpers of the JPEG encoder: bit writer with 0xFF stuffing
 * and optimal length-limited Huffman tables (ITU T.81 Annex K.2). */
#include "base.h"

typedef struct { Buf *out; uint64_t acc; int bits; } BitW;
static void bitw_put(BitW *w, uint32_t code, int len) {
    w->acc = w->acc << len | (code & ((1u << len) - 1));
    w->bits += len;
    while (w->bits >= 8) {
        uint8_t c = (uint8_t)(w->acc >> (w->bits - 8));
        buf_byte(w->out, c);
        if (c == 0xFF) buf_byte(w->out, 0);
        w->bits -= 8;
    }
}
static void bitw_flush(BitW *w) { if (w->bits) bitw_put(w, 0x7F, 8 - w->bits); w->acc = 0; w->bits = 0; }

/* `freq` has 257 slots: symbols 0..255 plus the reserved one. Produces JPEG
 * counts/symbols. */
static void huff_optimal(const uint32_t *freq_in, uint8_t counts[16], uint8_t *symbols, int *nsym) {
    int32_t freq[257]; int codesize[257], others[257], bits[33];
    for (int i = 0; i < 257; i++) { freq[i] = (int32_t)MIN(freq_in[i], 0x3FFFFFFFu); codesize[i] = 0; others[i] = -1; }
    freq[256] = 1;
    for (int i = 0; i < 33; i++) bits[i] = 0;
    for (;;) {
        int c1 = -1, c2 = -1; int32_t v = 0x7FFFFFFF;
        for (int i = 0; i <= 256; i++) if (freq[i] && freq[i] <= v) { v = freq[i]; c1 = i; }
        v = 0x7FFFFFFF;
        for (int i = 0; i <= 256; i++) if (freq[i] && freq[i] <= v && i != c1) { v = freq[i]; c2 = i; }
        if (c2 < 0) break;
        freq[c1] += freq[c2]; freq[c2] = 0;
        codesize[c1]++; while (others[c1] >= 0) { c1 = others[c1]; codesize[c1]++; }
        others[c1] = c2;
        codesize[c2]++; while (others[c2] >= 0) { c2 = others[c2]; codesize[c2]++; }
    }
    for (int i = 0; i <= 256; i++) if (codesize[i]) bits[MIN(codesize[i], 32)]++;
    for (int i = 32; i > 16; i--)
        while (bits[i] > 0) {
            int j = i - 2;
            while (bits[j] == 0) j--;
            bits[i] -= 2; bits[i - 1]++; bits[j + 1] += 2; bits[j]--;
        }
    int i = 16; while (bits[i] == 0) i--;
    bits[i]--;                                   /* drop the reserved symbol */
    for (int k = 0; k < 16; k++) counts[k] = (uint8_t)bits[k + 1];
    int n = 0;
    for (int len = 1; len <= 32; len++)
        for (int s = 0; s < 256; s++) if (codesize[s] == len) symbols[n++] = (uint8_t)s;
    *nsym = n;
}
static void huff_codes(const uint8_t counts[16], const uint8_t *symbols, uint16_t *code, uint8_t *size) {
    uint32_t c = 0; int k = 0;
    for (int len = 1; len <= 16; len++) {
        for (int i = 0; i < counts[len - 1]; i++) { code[symbols[k]] = (uint16_t)c; size[symbols[k]] = (uint8_t)len; c++; k++; }
        c <<= 1;
    }
}
