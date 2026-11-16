// Draws assets/icon.png (transparent, free-standing rounded badge) and
// assets/icon-maskable.png (full bleed, motif inside the safe zone).
// Motif: accordion bellows that unfold from dim grey (SDR) on the left to a
// glowing, brighter-than-white end (HDR) on the right.
// Not part of the build; regenerate manually after editing:
//   clang -fobjc-arc -framework Foundation -framework CoreGraphics -framework ImageIO \
//     build-support/tools/icon-art.m -o /tmp/icon-art && /tmp/icon-art assets/icon.png assets/icon-maskable.png
#import <Foundation/Foundation.h>
#import <CoreGraphics/CoreGraphics.h>
#import <ImageIO/ImageIO.h>
#include <math.h>
static CGColorSpaceRef cs;
static CGColorRef rgba(double r, double g, double b, double a) { CGFloat c[4] = {r, g, b, a}; return CGColorCreate(cs, c); }
static void gradient(CGContextRef c, CGPoint a, CGPoint b, int n, const CGFloat *comps, const CGFloat *locs) {
    CGGradientRef g = CGGradientCreateWithColorComponents(cs, comps, locs, n);
    CGContextDrawLinearGradient(c, g, a, b, kCGGradientDrawsBeforeStartLocation | kCGGradientDrawsAfterEndLocation);
    CGGradientRelease(g);
}
static void radial(CGContextRef c, CGPoint p, double r, int n, const CGFloat *comps, const CGFloat *locs) {
    CGGradientRef g = CGGradientCreateWithColorComponents(cs, comps, locs, n);
    CGContextDrawRadialGradient(c, g, p, 0, p, r, 0);
    CGGradientRelease(g);
}
/* Night-blue background in a box. */
static void background(CGContextRef c, CGRect box) {
    CGFloat bg[] = {0.10,0.11,0.17,1, 0.03,0.035,0.06,1}, l[] = {0, 1};
    gradient(c, CGPointMake(0, CGRectGetMaxY(box)), CGPointMake(0, box.origin.y), 2, bg, l);
}
/* Bellows in a unit box [0,1]^2 (y up), mapped to `box`. */
static void bellows(CGContextRef c, CGRect box) {
    CGContextSaveGState(c);
    CGContextTranslateCTM(c, box.origin.x, box.origin.y);
    CGContextScaleCTM(c, box.size.width, box.size.height);
    /* glow behind the bright end */
    CGFloat glow[] = {1.0,0.93,0.70,0.85, 1.0,0.75,0.35,0.35, 1.0,0.6,0.2,0}, gl[] = {0, 0.45, 1};
    radial(c, CGPointMake(0.80, 0.5), 0.42, 3, glow, gl);
    const int N = 8;
    for (int i = 0; i < N; i++) {
        double t0 = (double)i / N, t1 = (double)(i + 1) / N;
        double x0 = 0.08 + 0.78 * t0, x1 = 0.08 + 0.78 * t1;
        /* folds grow in height and width to the right: the accordion opens */
        double h0 = 0.16 + 0.30 * t0 * t0, h1 = 0.16 + 0.30 * t1 * t1;
        double k = (i + 0.5) / N;                       /* brightness: grey -> brighter than white */
        double base = 0.30 + 0.70 * pow(k, 1.4);
        double warm = pow(k, 3.0);
        double shade = i % 2 ? 0.72 : 1.0;              /* alternate fold faces */
        double r = fmin(1, base * shade + 0.05 * warm), g = fmin(1, base * shade * (1 - 0.06 * warm)), b = fmin(1, base * shade * (1 - 0.30 * warm));
        CGMutablePathRef p = CGPathCreateMutable();
        CGPathMoveToPoint(p, NULL, x0, 0.5 - h0); CGPathAddLineToPoint(p, NULL, x1, 0.5 - h1);
        CGPathAddLineToPoint(p, NULL, x1, 0.5 + h1); CGPathAddLineToPoint(p, NULL, x0, 0.5 + h0);
        CGPathCloseSubpath(p);
        CGContextSaveGState(c);
        if (i >= N - 3) CGContextSetShadowWithColor(c, CGSizeZero, box.size.width * 0.05 * (i - N + 4), rgba(1, 0.92, 0.7, 0.9));
        CGContextSetFillColorWithColor(c, rgba(r, g, b, 1));
        CGContextAddPath(c, p); CGContextFillPath(c);
        CGContextRestoreGState(c);
        /* fold edge */
        CGContextSetStrokeColorWithColor(c, rgba(0.02, 0.02, 0.04, 0.35));
        CGContextSetLineWidth(c, 0.006);
        CGContextMoveToPoint(c, x1, 0.5 - h1); CGContextAddLineToPoint(c, x1, 0.5 + h1); CGContextStrokePath(c);
        CGPathRelease(p);
    }
    /* end plate, brightest */
    double xe = 0.86, he = 0.46;
    CGContextSaveGState(c);
    CGContextSetShadowWithColor(c, CGSizeZero, box.size.width * 0.09, rgba(1, 0.95, 0.8, 1));
    CGContextSetFillColorWithColor(c, rgba(1, 0.99, 0.95, 1));
    CGContextAddRect(c, CGRectMake(xe, 0.5 - he, 0.05, 2 * he)); CGContextFillPath(c);
    CGContextRestoreGState(c);
    /* start plate, dim */
    CGContextSetFillColorWithColor(c, rgba(0.34, 0.35, 0.38, 1));
    CGContextAddRect(c, CGRectMake(0.05, 0.5 - 0.16, 0.035, 0.32)); CGContextFillPath(c);
    CGContextRestoreGState(c);
}
static void save(CGContextRef c, const char *path) {
    CGImageRef img = CGBitmapContextCreateImage(c);
    CGImageDestinationRef d = CGImageDestinationCreateWithURL((__bridge CFURLRef)[NSURL fileURLWithPath:@(path)], CFSTR("public.png"), 1, NULL);
    CGImageDestinationAddImage(d, img, NULL); CGImageDestinationFinalize(d); CFRelease(d); CGImageRelease(img);
}
int main(int argc, const char **argv) {
    @autoreleasepool {
        if (argc != 3) return 2;
        const size_t N = 1024;
        cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
        // 1. Transparent icon: rounded badge with a soft shadow.
        CGContextRef c = CGBitmapContextCreate(NULL, N, N, 8, N * 4, cs, kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big);
        CGRect outer = CGRectInset(CGRectMake(0, 0, N, N), 56, 56);
        CGPathRef shape = CGPathCreateWithRoundedRect(outer, 200, 200, NULL);
        CGContextSaveGState(c);
        CGContextSetShadowWithColor(c, CGSizeMake(0, -12), 34, rgba(0, 0, 0, 0.45));
        CGContextAddPath(c, shape); CGContextSetFillColorWithColor(c, rgba(0.05, 0.05, 0.08, 1)); CGContextFillPath(c);
        CGContextRestoreGState(c);
        CGContextSaveGState(c);
        CGContextAddPath(c, shape); CGContextClip(c);
        background(c, outer);
        bellows(c, CGRectInset(outer, 70, 120));
        CGFloat gloss[] = {1,1,1,0.10, 1,1,1,0}, gl[] = {0, 1};
        gradient(c, CGPointMake(0, CGRectGetMaxY(outer)), CGPointMake(0, CGRectGetMidY(outer)), 2, gloss, gl);
        CGContextRestoreGState(c);
        CGContextSetStrokeColorWithColor(c, rgba(1, 1, 1, 0.10)); CGContextSetLineWidth(c, 4);
        CGContextAddPath(c, shape); CGContextStrokePath(c);
        CGPathRelease(shape);
        save(c, argv[1]);
        CGContextRelease(c);
        // 2. Maskable: full-bleed background; the motif stays inside the safe circle (80 %).
        c = CGBitmapContextCreate(NULL, N, N, 8, N * 4, cs, kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big);
        background(c, CGRectMake(0, 0, N, N));
        bellows(c, CGRectMake(N * 0.17, N * 0.20, N * 0.66, N * 0.60));
        save(c, argv[2]);
        return 0;
    }
}
