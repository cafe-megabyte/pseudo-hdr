// Native checks of the Pseudo HDR core against Apple's ImageIO and Core Image.
//
// 1. Writes test images: JPEGs and PNGs through ImageIO (baseline, progressive,
//    greyscale, 4:4:4, orientation, Display P3, 16 bit, alpha, interlaced),
//    hand-made PNGs (bit depths 1/2/4, palettes, tRNS) and a JPEG with
//    restart markers. Files in samples/ are added when present.
// 2. Decodes each with the core and with ImageIO and compares the pixels.
// 3. Exports the HDR JPEG and checks it with Apple's frameworks: ISO 21496-1
//    gain map found, the SDR image of a JPEG source bit-identical to the
//    original, Core Image's HDR rendering equal to SDR times the intended gain.
// 4. Writes the exports and previews as references for tests/wasm-check.js.
#import <Foundation/Foundation.h>
#import <CoreGraphics/CoreGraphics.h>
#import <ImageIO/ImageIO.h>
#import <CoreImage/CoreImage.h>
#include <math.h>
#define PHDR_NATIVE 1
#include "../../core/phdr.c"

static int failures;
static NSString *dir;
#define FAIL(...) do { failures++; printf("FAIL "); printf(__VA_ARGS__); printf("\n"); } while (0)

/* ---- test images ---- */
static const size_t TW = 301, TH = 203;
static uint8_t pattern(size_t x, size_t y, int c) {
    double u = (double)x / (TW - 1), v = (double)y / (TH - 1);
    double val;
    if (y < TH / 3) val = c == 0 ? u : c == 1 ? v * 0.8 : 1 - u;                        /* colour ramps */
    else if (y < 2 * TH / 3) val = u;                                                     /* grey ramp */
    else val = ((x / 7 + y / 5) & 1) ? 0.95 - 0.2 * c * u : 0.1 + 0.3 * (c == 2) * v;  /* hard edges */
    if ((x - 220) * (x - 220) + (y - 60) * (y - 60) < 400) val = 1.0;                    /* white disc */
    val += (double)(((x * 2654435761u) ^ (y * 40503u) ^ (unsigned)c * 977u) % 7) / 255.0 - 3 / 255.0;
    return (uint8_t)lround(fmin(fmax(val, 0), 1) * 255);
}
static CGImageRef make_image(int gray, int alpha, int bits16, CFStringRef spaceName) {
    size_t ch = gray ? 1 : 3, comps = ch + (alpha ? 1 : 0), bpc = bits16 ? 2 : 1;
    if (!gray && !alpha && !bits16) comps = 4;             /* RGBX */
    NSMutableData *data = [NSMutableData dataWithLength:TW * TH * comps * bpc];
    uint8_t *p = data.mutableBytes;
    for (size_t y = 0; y < TH; y++) for (size_t x = 0; x < TW; x++) {
        uint8_t *q = p + (y * TW + x) * comps * bpc;
        for (size_t c = 0; c < comps; c++) {
            uint32_t v = c < ch ? pattern(x, y, gray ? 1 : (int)c) : (c == ch && alpha ? (uint32_t)(255 * x / (TW - 1)) : 255);
            if (bits16) { uint32_t w = v * 257 + (c < ch ? (uint32_t)((x * 31 + y) % 200) : 0); w = MIN(w, 65535u); q[c * 2] = (uint8_t)(w >> 8); q[c * 2 + 1] = (uint8_t)w; }
            else q[c] = (uint8_t)v;
        }
    }
    CGColorSpaceRef space = CGColorSpaceCreateWithName(spaceName ? spaceName : gray ? kCGColorSpaceGenericGrayGamma2_2 : kCGColorSpaceSRGB);
    CGBitmapInfo info = (alpha ? kCGImageAlphaLast : comps == 4 ? kCGImageAlphaNoneSkipLast : kCGImageAlphaNone) | (bits16 ? kCGBitmapByteOrder16Big : 0);
    CGDataProviderRef prov = CGDataProviderCreateWithCFData((__bridge CFDataRef)data);
    CGImageRef img = CGImageCreate(TW, TH, 8 * bpc, 8 * bpc * comps, TW * comps * bpc, space, info, prov, NULL, false, kCGRenderingIntentDefault);
    CGDataProviderRelease(prov); CGColorSpaceRelease(space);
    return img;
}
static void write_image(CGImageRef img, NSString *name, CFStringRef type, NSDictionary *props) {
    NSURL *url = [NSURL fileURLWithPath:[dir stringByAppendingPathComponent:name]];
    CGImageDestinationRef d = CGImageDestinationCreateWithURL((__bridge CFURLRef)url, type, 1, NULL);
    CGImageDestinationAddImage(d, img, (__bridge CFDictionaryRef)props);
    if (!CGImageDestinationFinalize(d)) FAIL("ImageIO cannot write %s", name.UTF8String);
    CFRelease(d);
}
/* Minimal PNG writer (stored DEFLATE blocks) for variants ImageIO does not write. */
static uint32_t crc_table[256];
static uint32_t crc32_of(const uint8_t *p, size_t n, uint32_t c) {
    if (!crc_table[1]) for (uint32_t i = 0; i < 256; i++) { uint32_t v = i; for (int k = 0; k < 8; k++) v = v & 1 ? 0xEDB88320u ^ (v >> 1) : v >> 1; crc_table[i] = v; }
    c = ~c; for (size_t i = 0; i < n; i++) c = crc_table[(c ^ p[i]) & 255] ^ (c >> 8); return ~c;
}
static void chunk(NSMutableData *png, const char *type, const void *data, uint32_t len) {
    uint8_t be[4] = {(uint8_t)(len >> 24), (uint8_t)(len >> 16), (uint8_t)(len >> 8), (uint8_t)len};
    [png appendBytes:be length:4];
    NSMutableData *body = [NSMutableData dataWithBytes:type length:4];
    if (len) [body appendBytes:data length:len];
    [png appendData:body];
    uint32_t c = crc32_of(body.bytes, body.length, 0);
    uint8_t cb[4] = {(uint8_t)(c >> 24), (uint8_t)(c >> 16), (uint8_t)(c >> 8), (uint8_t)c};
    [png appendBytes:cb length:4];
}
static NSData *zlib_stored(NSData *raw) {
    NSMutableData *z = [NSMutableData dataWithBytes:"\x78\x01" length:2];
    const uint8_t *p = raw.bytes; size_t n = raw.length, at = 0;
    do {
        size_t k = MIN(n - at, (size_t)65535);
        uint8_t h[5] = {(uint8_t)(at + k == n), (uint8_t)k, (uint8_t)(k >> 8), (uint8_t)~k, (uint8_t)(~k >> 8)};
        [z appendBytes:h length:5]; [z appendBytes:p + at length:k]; at += k;
    } while (at < n);
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < n; i++) { a = (a + p[i]) % 65521; b = (b + a) % 65521; }
    uint32_t ad = b << 16 | a;
    uint8_t ab[4] = {(uint8_t)(ad >> 24), (uint8_t)(ad >> 16), (uint8_t)(ad >> 8), (uint8_t)ad};
    [z appendBytes:ab length:4];
    return z;
}
/* ctype/depth PNG with samples from the pattern, optional palette/tRNS/interlace. */
static void handmade_png(NSString *name, int ctype, int depth, int trns, int interlace) {
    const int chans[7] = {1, 0, 3, 1, 2, 0, 4};
    int ch = chans[ctype];
    uint32_t maxv = (1u << depth) - 1;
    NSMutableData *png = [NSMutableData dataWithBytes:"\x89PNG\r\n\x1a\n" length:8];
    uint8_t ihdr[13] = {0, 0, (uint8_t)(TW >> 8), (uint8_t)TW, 0, 0, (uint8_t)(TH >> 8), (uint8_t)TH, (uint8_t)depth, (uint8_t)ctype, 0, 0, (uint8_t)interlace};
    chunk(png, "IHDR", ihdr, 13);
    if (ctype == 3) {
        uint8_t pal[256 * 3], alpha[256];
        int n = 1 << depth;
        for (int i = 0; i < n; i++) { pal[3 * i] = (uint8_t)(255 * i / (n - 1)); pal[3 * i + 1] = (uint8_t)(255 - 255 * i / (n - 1)); pal[3 * i + 2] = (uint8_t)(i * 37); alpha[i] = (uint8_t)(i % 3 ? 255 : 255 * i / n); }
        chunk(png, "PLTE", pal, (uint32_t)n * 3);
        if (trns) chunk(png, "tRNS", alpha, (uint32_t)n);
    } else if (trns && ctype == 0) { uint8_t k[2] = {0, (uint8_t)(maxv / 3 & 255)}; if (depth == 16) { k[0] = 0x55; k[1] = 0x55; } chunk(png, "tRNS", k, 2); }
    else if (trns && ctype == 2) { uint8_t k[6] = {0, 255, 0, 255, 0, 255}; chunk(png, "tRNS", k, 6); }
    static const int ax[7] = {0, 4, 0, 2, 0, 1, 0}, ay[7] = {0, 0, 4, 0, 2, 0, 1}, adx[7] = {8, 8, 4, 4, 2, 2, 1}, ady[7] = {8, 8, 8, 4, 4, 2, 2};
    NSMutableData *raw = [NSMutableData data];
    for (int pass = 0; pass < (interlace ? 7 : 1); pass++) {
        size_t sx = interlace ? ax[pass] : 0, sy = interlace ? ay[pass] : 0, dx = interlace ? adx[pass] : 1, dy = interlace ? ady[pass] : 1;
        if (TW <= sx || TH <= sy) continue;
        size_t pw = (TW - sx + dx - 1) / dx;
        for (size_t y = sy; y < TH; y += dy) {
            NSMutableData *row = [NSMutableData dataWithLength:1 + (pw * ch * depth + 7) / 8];
            uint8_t *r = row.mutableBytes;
            r[0] = 0;                                     /* filters: covered by ImageIO's PNGs */
            size_t bit = 0;
            for (size_t x = sx; x < TW; x += dx) for (int c = 0; c < ch; c++) {
                uint32_t v = (uint32_t)lround(pattern(x, y, ch == 1 ? 1 : c) / 255.0 * maxv);
                if (ctype == 6 && c == 3) v = (uint32_t)(maxv * x / (TW - 1));
                if (ctype == 4 && c == 1) v = (uint32_t)(maxv * y / (TH - 1));
                if (ctype == 0 && trns && depth == 16 && (x + y) % 9 == 0) v = 0x5555;
                if (ctype == 2 && trns && (x + y) % 11 == 0 && c < 3) v = 255;
                if (depth == 16) { r[1 + bit / 8] = (uint8_t)(v >> 8); r[2 + bit / 8] = (uint8_t)v; }
                else r[1 + bit / 8] |= (uint8_t)(v << (8 - depth - bit % 8));
                bit += (size_t)depth;
            }
            [raw appendData:row];
        }
    }
    NSData *z = zlib_stored(raw);
    chunk(png, "IDAT", z.bytes, (uint32_t)z.length);
    chunk(png, "IEND", NULL, 0);
    [png writeToFile:[dir stringByAppendingPathComponent:name] atomically:NO];
}
/* JPEG with restart markers, written by the core's own encoder. */
static void restart_jpeg(NSString *name) {
    uint8_t *rgb = malloc(TW * TH * 3);
    for (size_t y = 0; y < TH; y++) for (size_t x = 0; x < TW; x++) for (int c = 0; c < 3; c++) rgb[(y * TW + x) * 3 + c] = pattern(x, y, c);
    Buf b = {0};
    jpeg_test_restart = 7;
    jpeg_encode(&b, rgb, TW, TH, 88, JPEG_420, NULL);
    jpeg_test_restart = 0;
    [[NSData dataWithBytes:b.data length:b.len] writeToFile:[dir stringByAppendingPathComponent:name] atomically:NO];
    buf_free(&b); free(rgb);
}

/* ---- reference decoding ---- */
/* ImageIO's stored-orientation pixels as RGB, composited onto white in the
 * image's own colour space (no colour conversion). */
static uint8_t *apple_rgb(NSData *file, size_t index, size_t *w, size_t *h) {
    CGImageSourceRef src = CGImageSourceCreateWithData((__bridge CFDataRef)file, NULL);
    CGImageRef img = src ? CGImageSourceCreateImageAtIndex(src, index, NULL) : NULL;
    if (src) CFRelease(src);
    if (!img) return NULL;
    *w = CGImageGetWidth(img); *h = CGImageGetHeight(img);
    CGColorSpaceRef space = CGImageGetColorSpace(img);
    int gray = CGColorSpaceGetModel(space) == kCGColorSpaceModelMonochrome;
    if (CGColorSpaceGetModel(space) == kCGColorSpaceModelIndexed) space = CGColorSpaceGetBaseColorSpace(space);
    size_t n = *w * *h, comps = gray ? 1 : 4;
    uint8_t *px = calloc(n, comps), *rgb = malloc(n * 3);
    CGContextRef ctx = CGBitmapContextCreate(px, *w, *h, 8, *w * comps, space,
        gray ? (CGBitmapInfo)kCGImageAlphaNone : (CGBitmapInfo)(kCGImageAlphaNoneSkipLast | kCGBitmapByteOrder32Big));
    CGContextSetFillColorWithColor(ctx, CGColorGetConstantColor(kCGColorWhite));
    CGContextFillRect(ctx, CGRectMake(0, 0, *w, *h));
    CGContextDrawImage(ctx, CGRectMake(0, 0, *w, *h), img);
    CGContextRelease(ctx); CGImageRelease(img);
    for (size_t i = 0; i < n; i++) for (int c = 0; c < 3; c++) rgb[i * 3 + c] = px[i * comps + (gray ? 0 : c)];
    free(px);
    return rgb;
}
static int open_core(NSData *file) {
    uint8_t *copy = xalloc(file.length);
    memcpy(copy, file.bytes, file.length);
    int r = engine_open(copy, (uint32_t)file.length);
    if (r) xfree(copy);
    return r;
}
/* Header tables/frame segments and scan data of a JPEG (metadata excluded). */
static NSData *jpeg_core(NSData *file) {
    Seg seg[SEG_MAX]; int nseg = 0, eoi = 0; uint32_t at = 0, end = 0;
    if (jpeg_layout(file.bytes, (uint32_t)file.length, seg, &nseg, &at, &end, &eoi)) return nil;
    NSMutableData *d = [NSMutableData data];
    for (int i = 0; i < nseg; i++) if (seg[i].group == 2) [d appendBytes:(const uint8_t *)file.bytes + seg[i].at length:seg[i].len];
    [d appendBytes:(const uint8_t *)file.bytes + at length:end - at];
    return d;
}
/* Length of the primary image according to the MPF index. */
static NSUInteger mpf_primary_len(NSData *file) {
    const uint8_t *p = file.bytes;
    for (NSUInteger i = 0; i + 90 < file.length; i++)
        if (p[i] == 0xFF && p[i + 1] == 0xE2 && !memcmp(p + i + 4, "MPF", 4)) return rd32be(p + i + 8 + 50 + 4);   /* TIFF header, MP entry 1: attribute, size */
    return file.length;
}
/* JPEG_MAX_NOTE: single JPEG pixels are not compared by their maximum
 * difference. ImageIO itself decodes the identical compressed data of the
 * original and of the exported file differently (it chooses different
 * decoders), by up to ~60 levels at sharp chroma edges; the mean stays below
 * one level. The compressed data is compared byte for byte instead. */
static void compare(const char *label, const uint8_t *a, const uint8_t *b, size_t n, double mean_max, int max_max) {
    double sum = 0; int worst = 0;
    for (size_t i = 0; i < n; i++) { int d = abs((int)a[i] - (int)b[i]); sum += d; if (d > worst) worst = d; }
    double mean = sum / n;
    int ok = mean <= mean_max && worst <= max_max;
    if (!ok) failures++;
    printf("%s %s: mean |diff| %.3f, max %d\n", ok ? "ok  " : "FAIL", label, mean, worst);
}

/* ---- HDR output ---- */
static void check_hdr(NSString *name, NSData *source, NSData *out, double stops) {
    const char *label = name.UTF8String;
    CGImageSourceRef src = CGImageSourceCreateWithData((__bridge CFDataRef)out, NULL);
    NSDictionary *info = src ? CFBridgingRelease(CGImageSourceCopyAuxiliaryDataInfoAtIndex(src, 0, kCGImageAuxiliaryDataTypeISOGainMap)) : nil;
    NSDictionary *props = src ? CFBridgingRelease(CGImageSourceCopyPropertiesAtIndex(src, 0, NULL)) : nil;
    if (src) CFRelease(src);
    NSDictionary *desc = info[(NSString *)kCGImageAuxiliaryDataInfoDataDescription];
    size_t w = S.im.w, h = S.im.h;
    if ([desc[@"Width"] unsignedLongValue] != w || [desc[@"Height"] unsignedLongValue] != h) { FAIL("%s: ImageIO finds no ISO 21496-1 gain map of %zux%zu", label, w, h); return; }
    int orientation = props[(NSString *)kCGImagePropertyOrientation] ? [props[(NSString *)kCGImagePropertyOrientation] intValue] : 1;
    if (orientation != S.im.orientation) FAIL("%s: orientation %d, expected %d", label, orientation, S.im.orientation);
    /* SDR: a JPEG source keeps its compressed data, so ImageIO decodes identical pixels */
    size_t aw, ah, bw, bh;
    uint8_t *after = apple_rgb(out, 0, &bw, &bh);
    if (!after || bw != w || bh != h) { FAIL("%s: SDR image not decodable", label); free(after); return; }
    if (!engine_reencodes()) {
        /* tables, frame and scans byte for byte; ImageIO's pixels agree too
         * (not necessarily bit for bit: Apple picks different JPEG decoders
         * for differently structured files) */
        NSData *a = jpeg_core(source), *b = jpeg_core([out subdataWithRange:NSMakeRange(0, mpf_primary_len(out))]);
        if (!a || ![a isEqual:b]) FAIL("%s: compressed SDR image differs from the original", label);
        else printf("ok   %s: compressed SDR image identical to the original (%lu bytes)\n", label, (unsigned long)a.length);
        uint8_t *before = apple_rgb(source, 0, &aw, &ah);
        if (before) compare([NSString stringWithFormat:@"%@: ImageIO SDR before/after", name].UTF8String, after, before, w * h * 3, 1.0, 255);
        free(before);
    } else compare([NSString stringWithFormat:@"%@: SDR JPEG vs source", name].UTF8String, after, S.im.rgb, w * h * 3, 3.5, 48);
    /* HDR: Core Image applies the gain map; compare with SDR x intended gain */
    uint8_t *codes = malloc(w * h);
    gain_codes(S.im.rgb, w * h, &S.curve, codes);
    CIImage *sdr = [CIImage imageWithData:out options:@{kCIImageApplyOrientationProperty: @NO}];
    CIImage *hdr = [CIImage imageWithData:out options:@{kCIImageExpandToHDR: @YES, kCIImageApplyOrientationProperty: @NO}];
    CIContext *ctx = [CIContext contextWithOptions:@{kCIContextWorkingFormat: @(kCIFormatRGBAf)}];
    CGColorSpaceRef lin = CGColorSpaceCreateWithName(kCGColorSpaceExtendedLinearSRGB);
    float *a = malloc(w * h * 16), *b = malloc(w * h * 16);
    [ctx render:sdr toBitmap:a rowBytes:w * 16 bounds:sdr.extent format:kCIFormatRGBAf colorSpace:lin];
    [ctx render:hdr toBitmap:b rowBytes:w * 16 bounds:hdr.extent format:kCIFormatRGBAf colorSpace:lin];
    CGColorSpaceRelease(lin);
    /* Row order of Core Image bitmaps: whichever matches (top-down or bottom-up). */
    double err = 1e9, worst = 0, top = 0;
    for (int flip = 0; flip < 2; flip++) {
        double e_sum = 0, e_max = 0, t = 0; size_t used = 0;
        for (size_t y = 0; y < h; y++) for (size_t x = 0; x < w; x++) {
            size_t i = (flip ? h - 1 - y : y) * w + x, c = y * w + x;
            double ys = 0.2126 * a[i * 4] + 0.7152 * a[i * 4 + 1] + 0.0722 * a[i * 4 + 2];
            double yh = 0.2126 * b[i * 4] + 0.7152 * b[i * 4 + 1] + 0.0722 * b[i * 4 + 2];
            if (ys < 0.01) continue;
            double got = log2(yh / ys), e = fabs(got - stops * codes[c] / 255.0);
            e_sum += e; used++; e_max = fmax(e_max, e); t = fmax(t, got);
        }
        e_sum /= used ? used : 1;
        if (e_sum < err) { err = e_sum; worst = e_max; top = t; }
    }
    int ok = hdr.contentHeadroom > exp2(stops) * 0.97 && err < 0.03 && top > stops * 0.95;
    if (!ok) failures++;
    printf("%s %s: headroom %.2f (%.2f expected), gain error mean %.4f / max %.3f stops, brightest +%.2f stops\n", ok ? "ok  " : "FAIL",
           label, hdr.contentHeadroom, exp2(stops), err, worst, top);
    free(a); free(b); free(codes); free(after);
}

static void run_case(NSString *name, double stops, double threshold, NSMutableString *cases) {
    NSString *path = [name containsString:@"/"] ? name : [dir stringByAppendingPathComponent:name];
    NSData *file = [NSData dataWithContentsOfFile:path];
    if (!file) { FAIL("missing %s", name.UTF8String); return; }
    NSString *label = name.lastPathComponent;
    int r = open_core(file);
    if (r) { FAIL("%s: core cannot open it (%d/%d)", label.UTF8String, r, S.error_detail); return; }
    size_t w, h;
    uint8_t *ref = apple_rgb(file, 0, &w, &h);
    if (!ref || w != S.im.w || h != S.im.h) FAIL("%s: size %ux%u, ImageIO %zux%zu", label.UTF8String, S.im.w, S.im.h, w, h);
    else {
        int png = S.im.kind == KIND_PNG;
        compare([NSString stringWithFormat:@"%@: decoder vs ImageIO", label].UTF8String, S.im.rgb, ref, w * h * 3,
                png ? 0.2 : 1.2, png ? 1 : 255);   /* JPEG: see JPEG_MAX_NOTE */
    }
    free(ref);
    engine_set_params(stops, threshold);
    if ((r = engine_export())) { FAIL("%s: export failed (%d)", label.UTF8String, r); return; }
    NSData *out = [NSData dataWithBytes:S.out.data length:S.out.len];
    NSString *base = [label stringByReplacingOccurrencesOfString:@"." withString:@"-"];
    [out writeToFile:[dir stringByAppendingPathComponent:[base stringByAppendingString:@"-HDR.jpg"]] atomically:NO];
    check_hdr(label, file, out, S.curve.stops);
    if ((r = engine_preview(640, 480))) { FAIL("%s: preview failed (%d)", label.UTF8String, r); return; }
    [[NSData dataWithBytes:S.phdr.data length:S.phdr.len] writeToFile:[dir stringByAppendingPathComponent:[base stringByAppendingString:@"-preview.jpg"]] atomically:NO];
    CGImageSourceRef ps = CGImageSourceCreateWithData((__bridge CFDataRef)[NSData dataWithBytes:S.phdr.data length:S.phdr.len], NULL);
    NSDictionary *pi = ps ? CFBridgingRelease(CGImageSourceCopyAuxiliaryDataInfoAtIndex(ps, 0, kCGImageAuxiliaryDataTypeISOGainMap)) : nil;
    if (ps) CFRelease(ps);
    uint32_t ow = S.im.orientation >= 5 ? S.im.h : S.im.w, oh = S.im.orientation >= 5 ? S.im.w : S.im.h;
    if (!pi || S.pw > 640 || S.ph > 480 || llabs((long long)S.pw * oh - (long long)S.ph * ow) > (long long)MAX(ow, oh))
        FAIL("%s: preview %ux%u (image %ux%u) without valid gain map", label.UTF8String, S.pw, S.ph, ow, oh);
    [cases appendFormat:@"%@|%.17g|%.17g|%@\n", path, stops, threshold, base];
}

int main(int argc, const char **argv) {
    @autoreleasepool {
        (void)argc; (void)argv;
        dir = @"build/internal/tests";
        [[NSFileManager defaultManager] createDirectoryAtPath:dir withIntermediateDirectories:YES attributes:nil error:nil];
        /* curve properties: monotonic HDR output, linear stretch at threshold 0 */
        for (int t = 0; t <= 95; t += 5) {
            Curve c; curve_build(&c, 2.0, t / 100.0);
            double last = -1;
            for (int i = 1; i <= 4096; i++) {
                double out = (i / 4096.0) * exp2(2.0 * c.code[i] / 255.0);
                if (out < last) { FAIL("curve not monotonic at threshold %d %%", t); break; }
                last = out;
            }
            if (c.code[4096] != 255) FAIL("white does not get the full gain at threshold %d %%", t);
            if (t == 0) for (int i = 1; i <= 4096; i++) if (c.code[i] != 255) { FAIL("threshold 0 is not a linear stretch"); break; }
        }
        printf("ok   curve: monotonic for thresholds 0..95 %%, linear stretch at 0 %%\n");

        CGImageRef rgb = make_image(0, 0, 0, NULL), rgba = make_image(0, 1, 0, NULL), rgb16 = make_image(0, 0, 1, NULL);
        CGImageRef gray = make_image(1, 0, 0, NULL), p3 = make_image(0, 0, 0, kCGColorSpaceDisplayP3);
        write_image(rgb, @"baseline.jpg", CFSTR("public.jpeg"), @{(NSString *)kCGImageDestinationLossyCompressionQuality: @0.85});
        write_image(rgb, @"best.jpg", CFSTR("public.jpeg"), @{(NSString *)kCGImageDestinationLossyCompressionQuality: @1.0});
        write_image(rgb, @"progressive.jpg", CFSTR("public.jpeg"), @{(NSString *)kCGImageDestinationLossyCompressionQuality: @0.8,
            (NSString *)kCGImagePropertyJFIFDictionary: @{(NSString *)kCGImagePropertyJFIFIsProgressive: @YES}});
        write_image(gray, @"gray.jpg", CFSTR("public.jpeg"), @{(NSString *)kCGImageDestinationLossyCompressionQuality: @0.85});
        write_image(rgb, @"rotated.jpg", CFSTR("public.jpeg"), @{(NSString *)kCGImagePropertyOrientation: @6});
        write_image(p3, @"p3.jpg", CFSTR("public.jpeg"), @{(NSString *)kCGImageDestinationLossyCompressionQuality: @0.9});
        restart_jpeg(@"restart.jpg");
        write_image(rgb, @"rgb8.png", CFSTR("public.png"), nil);
        write_image(rgba, @"rgba8.png", CFSTR("public.png"), nil);
        write_image(rgb16, @"rgb16.png", CFSTR("public.png"), nil);
        write_image(gray, @"gray8.png", CFSTR("public.png"), nil);
        write_image(p3, @"p3.png", CFSTR("public.png"), nil);
        write_image(rgba, @"interlaced.png", CFSTR("public.png"), @{(NSString *)kCGImagePropertyPNGDictionary: @{(NSString *)kCGImagePropertyPNGInterlaceType: @1}});
        CGImageRelease(rgb); CGImageRelease(rgba); CGImageRelease(rgb16); CGImageRelease(gray); CGImageRelease(p3);
        handmade_png(@"gray1.png", 0, 1, 0, 0);
        handmade_png(@"gray2-interlaced.png", 0, 2, 0, 1);
        handmade_png(@"gray4.png", 0, 4, 0, 0);
        handmade_png(@"gray16-trns.png", 0, 16, 1, 0);
        handmade_png(@"palette4-trns.png", 3, 4, 1, 0);
        handmade_png(@"palette8-interlaced.png", 3, 8, 1, 1);
        handmade_png(@"rgb8-trns.png", 2, 8, 1, 0);
        handmade_png(@"graya16.png", 4, 16, 0, 0);
        handmade_png(@"rgba16-interlaced.png", 6, 16, 0, 1);

        NSMutableString *cases = [NSMutableString string];
        NSArray *names = @[@"baseline.jpg", @"best.jpg", @"progressive.jpg", @"gray.jpg", @"rotated.jpg", @"p3.jpg", @"restart.jpg",
            @"rgb8.png", @"rgba8.png", @"rgb16.png", @"gray8.png", @"p3.png", @"interlaced.png", @"gray1.png", @"gray2-interlaced.png",
            @"gray4.png", @"gray16-trns.png", @"palette4-trns.png", @"palette8-interlaced.png", @"rgb8-trns.png", @"graya16.png", @"rgba16-interlaced.png"];
        int k = 0;
        for (NSString *name in names) { run_case(name, 0.5 + (k % 6) * 0.5, (k * 13 % 96) / 100.0, cases); k++; }
        /* the round trip: an exported HDR JPEG opens again and gets a new gain map */
        run_case(@"baseline-jpg-HDR.jpg", 1.0, 0.3, cases);
        if (!(S.im.flags & FLAG_GAINMAP)) FAIL("existing gain map not detected");
        for (NSString *f in [[NSFileManager defaultManager] contentsOfDirectoryAtPath:@"samples" error:nil]) {
            NSString *ext = f.pathExtension.lowercaseString;
            if ([@[@"jpg", @"jpeg", @"png"] containsObject:ext]) run_case([@"samples" stringByAppendingPathComponent:f], 2.0, 0.6, cases);
        }
        [cases writeToFile:[dir stringByAppendingPathComponent:@"cases.txt"] atomically:NO encoding:NSUTF8StringEncoding error:nil];
        session_reset();
        if (failures) { printf("%d native check(s) failed.\n", failures); return 1; }
        printf("Native checks passed.\n");
        return 0;
    }
}
