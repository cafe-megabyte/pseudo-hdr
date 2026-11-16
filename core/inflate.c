/* zlib/DEFLATE decoder (RFC 1950/1951) into a buffer of known maximum size.
 * Huffman decoding uses a 9-bit lookup table with a canonical slow path. */
#include "base.h"

typedef struct {
    uint16_t fast[512];      /* (length << 9 | symbol) + 1, 0 = slow path */
    uint16_t count[16];      /* codes per length */
    uint16_t sym[320];       /* symbols ordered by code */
} InfHuff;
typedef struct {
    const uint8_t *p, *end;
    uint64_t acc; int bits;
    int overrun;             /* bytes read past the end (padding with zeros) */
    uint8_t *out; size_t len, cap;
} Inflate;

static inline void inf_fill(Inflate *s) {
    while (s->bits <= 56) {
        uint64_t c = 0;
        if (s->p < s->end) c = *s->p++; else s->overrun++;
        s->acc |= c << s->bits; s->bits += 8;
    }
}
static inline uint32_t inf_bits(Inflate *s, int n) {
    if (!n) return 0;
    if (s->bits < n) inf_fill(s);
    uint32_t v = (uint32_t)(s->acc & ((1ull << n) - 1));
    s->acc >>= n; s->bits -= n;
    return v;
}
static int inf_build(InfHuff *h, const uint8_t *lens, int n) {
    uint16_t offs[16];
    xmemset(h->count, 0, sizeof h->count);
    xmemset(h->fast, 0, sizeof h->fast);
    for (int i = 0; i < n; i++) h->count[lens[i]]++;
    h->count[0] = 0;
    int left = 1;
    for (int len = 1; len < 16; len++) { left <<= 1; left -= h->count[len]; if (left < 0) return 0; }
    offs[1] = 0;
    for (int len = 1; len < 15; len++) offs[len + 1] = (uint16_t)(offs[len] + h->count[len]);
    for (int i = 0; i < n; i++) if (lens[i]) h->sym[offs[lens[i]]++] = (uint16_t)i;
    /* fast table: canonical codes are MSB-first, the stream is LSB-first */
    uint32_t code = 0; int k = 0;
    for (int len = 1; len <= 9; len++) {
        for (int i = 0; i < h->count[len]; i++, k++, code++) {
            uint32_t rev = 0;
            for (int b = 0; b < len; b++) if (code >> b & 1) rev |= 1u << (len - 1 - b);
            for (uint32_t f = rev; f < 512; f += 1u << len) h->fast[f] = (uint16_t)((len << 9 | h->sym[k]) + 1);
        }
        code <<= 1;
    }
    return 1;
}
static int inf_decode(Inflate *s, const InfHuff *h) {
    if (s->bits < 16) inf_fill(s);
    uint16_t f = h->fast[s->acc & 511];
    if (f) { f--; int len = f >> 9; s->acc >>= len; s->bits -= len; return f & 511; }
    int code = 0, first = 0, index = 0;
    for (int len = 1; len < 16; len++) {
        code |= (int)(s->acc & 1); s->acc >>= 1; s->bits--;
        int count = h->count[len];
        if (code - count < first) return h->sym[index + (code - first)];
        index += count; first += count; first <<= 1; code <<= 1;
    }
    return -1;
}
static const uint16_t len_base[29] = {3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258};
static const uint8_t len_extra[29] = {0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0};
static const uint16_t dist_base[30] = {1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577};
static const uint8_t dist_extra[30] = {0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13};

static int inf_block(Inflate *s, const InfHuff *lit, const InfHuff *dist) {
    for (;;) {
        int sym = inf_decode(s, lit);
        if (sym < 0 || s->overrun > 8) return ERR_CORRUPT;
        if (sym < 256) {
            if (s->len >= s->cap) return ERR_CORRUPT;
            s->out[s->len++] = (uint8_t)sym;
            continue;
        }
        if (sym == 256) return ERR_OK;
        sym -= 257;
        if (sym >= 29) return ERR_CORRUPT;
        uint32_t len = len_base[sym] + inf_bits(s, len_extra[sym]);
        int ds = inf_decode(s, dist);
        if (ds < 0 || ds >= 30) return ERR_CORRUPT;
        uint32_t d = dist_base[ds] + inf_bits(s, dist_extra[ds]);
        if (d > s->len || len > s->cap - s->len) return ERR_CORRUPT;
        uint8_t *o = s->out + s->len;
        const uint8_t *from = o - d;
        for (uint32_t i = 0; i < len; i++) o[i] = from[i];
        s->len += len;
    }
}
/* Inflates a zlib stream into out[0..cap); *len receives the size. */
static int zlib_inflate(const uint8_t *src, size_t n, uint8_t *out, size_t cap, size_t *len) {
    if (n < 2 || (src[0] & 15) != 8 || (src[0] >> 4) > 7 || (src[0] << 8 | src[1]) % 31 || (src[1] & 0x20)) return ERR_CORRUPT;
    Inflate s = {.p = src + 2, .end = src + n, .out = out, .cap = cap};
    InfHuff *lit = xalloc(sizeof(InfHuff) * 2);
    if (!lit) return ERR_MEMORY;
    InfHuff *dist = lit + 1;
    int last = 0, r = ERR_OK;
    while (!last && !r) {
        last = (int)inf_bits(&s, 1);
        uint32_t type = inf_bits(&s, 2);
        if (type == 0) {
            inf_bits(&s, s.bits & 7);                      /* to a byte boundary */
            uint32_t a = inf_bits(&s, 16), b = inf_bits(&s, 16);
            if ((a ^ 0xFFFF) != b) { r = ERR_CORRUPT; break; }
            if (a > s.cap - s.len) { r = ERR_CORRUPT; break; }
            for (uint32_t i = 0; i < a; i++) s.out[s.len++] = (uint8_t)inf_bits(&s, 8);
            if (s.overrun > 8) r = ERR_CORRUPT;
        } else if (type == 1) {
            uint8_t lens[320];
            for (int i = 0; i < 144; i++) lens[i] = 8;
            for (int i = 144; i < 256; i++) lens[i] = 9;
            for (int i = 256; i < 280; i++) lens[i] = 7;
            for (int i = 280; i < 288; i++) lens[i] = 8;
            for (int i = 0; i < 30; i++) lens[288 + i] = 5;
            inf_build(lit, lens, 288); inf_build(dist, lens + 288, 30);
            r = inf_block(&s, lit, dist);
        } else if (type == 2) {
            uint32_t hlit = inf_bits(&s, 5) + 257, hdist = inf_bits(&s, 5) + 1, hclen = inf_bits(&s, 4) + 4;
            static const uint8_t order[19] = {16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15};
            uint8_t cl[19] = {0}, lens[320] = {0};
            for (uint32_t i = 0; i < hclen; i++) cl[order[i]] = (uint8_t)inf_bits(&s, 3);
            if (!inf_build(lit, cl, 19)) { r = ERR_CORRUPT; break; }
            for (uint32_t i = 0; i < hlit + hdist;) {
                int sym = inf_decode(&s, lit);
                if (sym < 0) { r = ERR_CORRUPT; break; }
                if (sym < 16) { lens[i++] = (uint8_t)sym; continue; }
                uint32_t rep; uint8_t v = 0;
                if (sym == 16) { if (!i) { r = ERR_CORRUPT; break; } v = lens[i - 1]; rep = 3 + inf_bits(&s, 2); }
                else if (sym == 17) rep = 3 + inf_bits(&s, 3);
                else rep = 11 + inf_bits(&s, 7);
                if (i + rep > hlit + hdist) { r = ERR_CORRUPT; break; }
                while (rep--) lens[i++] = v;
            }
            if (r) break;
            /* Incomplete codes are allowed (e.g. a single distance code);
             * their unused codes fail when they occur. */
            if (!lens[256] || !inf_build(lit, lens, (int)hlit) || !inf_build(dist, lens + hlit, (int)hdist)) { r = ERR_CORRUPT; break; }
            r = inf_block(&s, lit, dist);
        } else r = ERR_CORRUPT;
    }
    xfree(lit);
    *len = s.len;
    return r;
}
