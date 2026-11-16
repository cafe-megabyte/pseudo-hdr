/* Shared definitions for the Pseudo HDR core. The core is freestanding C11:
 * it compiles unchanged for wasm32 (no libc) and natively for the checks. */
#ifndef PHDR_BASE_H
#define PHDR_BASE_H
#include <stdint.h>
#include <stddef.h>

enum {
    ERR_OK = 0,
    ERR_MEMORY = 1,        /* allocation failed */
    ERR_FORMAT = 2,        /* neither PNG nor JPEG / corrupt structure */
    ERR_UNSUPPORTED = 3,   /* valid file, unsupported variant (see detail) */
    ERR_CORRUPT = 4,       /* compressed data damaged or truncated */
    ERR_STATE = 5,         /* API used without an open image */
    ERR_TOO_LARGE = 6      /* more than 65535 pixels per side or MAX_PIXELS */
};
/* Detail codes for ERR_UNSUPPORTED, shown to the user by the web app. */
enum {
    UNSUP_NONE = 0,
    UNSUP_CMYK = 1,        /* four-component (CMYK/YCCK) JPEG */
    UNSUP_ARITHMETIC = 2,  /* arithmetic-coded JPEG */
    UNSUP_PRECISION = 3,   /* 12-bit or lossless JPEG */
    UNSUP_HDR_PNG = 4,     /* PNG that already is HDR (cICP: PQ or HLG) */
    UNSUP_LAYOUT = 5       /* unusual sampling or component layout */
};
#define MAX_PIXELS 200000000u

#undef MIN
#undef MAX
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define CLAMP(v, lo, hi) ((v) < (lo) ? (lo) : (v) > (hi) ? (hi) : (v))

void *xalloc(size_t size);           /* uninitialised; NULL on failure */
void *xcalloc(size_t size);          /* zeroed */
void *xrealloc(void *p, size_t size);
void xfree(void *p);
void xmemcpy(void *d, const void *s, size_t n);
void xmemmove(void *d, const void *s, size_t n);
void xmemset(void *d, int c, size_t n);
int xmemcmp(const void *a, const void *b, size_t n);
size_t xstrlen(const char *s);

double m_exp(double x);
double m_expm1(double x);
double m_log(double x);
double m_pow(double x, double y);
double m_floor(double x);
static inline double m_sqrt(double x) { return __builtin_sqrt(x); }
static inline double m_abs(double x) { return x < 0 ? -x : x; }

/* A growable byte buffer used by all writers. */
typedef struct { uint8_t *data; uint32_t len, cap; int failed; } Buf;
void buf_reserve(Buf *b, uint32_t extra);
void buf_put(Buf *b, const void *p, uint32_t n);
void buf_byte(Buf *b, uint8_t v);
void buf_zero(Buf *b, uint32_t n);
void buf_free(Buf *b);

static inline uint32_t rd16be(const uint8_t *p) { return (uint32_t)p[0] << 8 | p[1]; }
static inline uint32_t rd32be(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
#endif
