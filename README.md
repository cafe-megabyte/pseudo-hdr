# Pseudo HDR

Turns an ordinary **PNG or JPEG** into an **HDR photo with a gain map**, running entirely in
the browser. On HDR displays (iPhone, iPad, MacBook Pro, HDR monitors) the bright parts of
the image then shine brighter than white; everywhere else the image looks exactly as before.
Everything is computed locally in C/WebAssembly; there is no server interaction. After the
first visit the app works offline and can be installed as an app; installed copies update
themselves.

The HDR information is not in the source image and cannot be recovered. Pseudo HDR stretches
the existing brightness range into the HDR range, like pulling an accordion apart.

## Use

- **Open** a PNG or JPEG, or drop it onto the window.
- **Strength:** how much brighter than SDR white the brightest parts become, 1.2× to 16×
  (0.25 to 4 stops; every stop doubles). Displays show as much of it as their current
  headroom allows and scale the rest down smoothly.
- **Threshold:** from which brightness on the image is stretched. At 0 % the whole image is
  stretched evenly (every pixel times the strength, black stays black); at high values only
  the highlights glow. The small curve shows the brightening over the original's brightness.
- **View:** *HDR* or *Original* for comparison.
- **Save HDR JPG** writes `<name>-HDR.jpg`. On touch devices the share sheet opens instead;
  *Save Image* puts it into Photos.
- Strength and threshold are remembered on the device.

The interface is in German or English, following the browser language.

### Where it shows as HDR

- **Photos / Preview on iOS 18, iPadOS 18, macOS 15 and later** (ISO 21496-1 gain map).
- **Chrome** on HDR displays, **Android** (Ultra HDR).
- **Safari 26 / iOS 26** also shows HDR images on web pages, which the app's preview uses.
  Safari still falls back to SDR in some layouts, so the preview can stay SDR on some
  devices; the saved file is HDR regardless. On displays without HDR the header says
  *no HDR display*.
- iOS shows HDR in Photos only with *Settings → Photos → View Full HDR* switched on (the
  default). Messengers and some upload services strip gain maps.

## Build

Requires macOS with Apple's Command Line Tools (`xcode-select --install`), nothing else.
Builds run offline in a sandbox that blocks the network and only allows writes below `build/`.

```sh
make                    # builds the WASM linker, engine.wasm and the website in build/web/
make serve              # http://localhost:8080/ (Ctrl-C to stop)
make check              # checks against Apple's ImageIO and Core Image, WASM cross-check
make clean              # removes build/
make rebuild-toolchain  # rebuilds the linker
```

Apple Clang compiles C to `wasm32`. The only other tool is the small WebAssembly linker
`wcc` from [XCC](https://github.com/tyfkda/xcc) (MIT). It is bundled as an unmodified
source archive and built at the start of the first `make`; see
[vendor/SOURCE_ARCHIVES.md](vendor/SOURCE_ARCHIVES.md). No Node, Emscripten, CMake or Rust.

## Deploy

Copy all of `build/web/` to a static HTTPS host, including `releases/`. Subdirectories
work. Serve `.wasm` as `application/wasm`. Revalidate `index.html`, `service-worker.js`,
`site.webmanifest`, `favicon.ico` and `apple-touch-icon.png`; everything under
`releases/` can be cached forever.

The service worker downloads a new release completely before activating it. Without an
open image the app switches to the new release at once; with an image open, it switches
on the next start. Older releases are then deleted.

## How it works

### The accordion curve
A gain map stores, per pixel, how much an HDR display brightens it: in linear light
`HDR = SDR × 2^(stops × m)` with `m` between 0 and 1. Pseudo HDR derives `m` from the
pixel's brightness `v` (luminance, sRGB-encoded, 0…1) and the threshold `t`:

```
m(v) = smoothstep(t − d, t + d, v),   d = min(t, 1 − t) / 2
```

Below `t − d` nothing changes, above `t + d` pixels get the full strength, in between the
gain rises smoothly. At `t = 0` every pixel gets the full strength, which is a linear stretch
of the whole range. For every setting the HDR brightness grows monotonically with the SDR
brightness, so the order of tones is kept and nothing inverts. The gain is one value per
pixel (applied equally to red, green and blue), so hues stay as they are.

### The file
- **JPEG sources keep their compressed image data bit for bit.** Quantisation and Huffman
  tables, frame and scans are copied unchanged; only metadata is added. Viewers without HDR
  show exactly the original.
- **PNG sources** become a JPEG at quality 95 with 4:4:4 chroma. Transparency is composited
  onto white. An embedded RGB ICC profile and EXIF (`eXIf`) are carried over.
- **Greyscale sources** become a colour JPEG, because Apple ignores gain maps behind
  greyscale JPEGs.
- The **gain map** is a greyscale JPEG (quality 90) at full resolution behind the primary
  image in an MPF container, described twice:
  - **ISO 21496-1**: Apple Photos/Preview (iOS 18, macOS 15 and later), Chrome.
  - **Ultra HDR XMP** (Adobe gain map): Android, Chrome.
  It uses the primary image's colour space and orientation; offsets are zero.
- Kept: EXIF (including orientation), ICC profiles, IPTC and other APP segments, comments.
  Dropped: the source's XMP (it is replaced by the HDR directory), an existing gain map or
  MPF index and anything after the primary image's end (old gain maps, camera previews).

### Decoding
Own decoders in freestanding C: PNG (every colour type and bit depth, Adam7, `tRNS`,
`iCCP`, `eXIf`) with a DEFLATE decoder, and JPEG (baseline, extended and progressive,
any sampling factors, restart intervals, greyscale, YCbCr, Adobe RGB). Chroma is upsampled
bilinearly at the sample centres. The decoded pixels are only used for the gain map and the
preview.

### Preview
The preview is the HDR JPEG itself at screen size, shown in a plain `<img>`: on browsers
that render gain maps it looks exactly like the saved file. The page avoids CSS transitions
and positioned or transformed images, which make Safari fall back to SDR.

### Supported files
- **PNG** up to 65535 pixels per side.
- **JPEG** as above.
- **Not supported** (with a message): CMYK/YCCK JPEGs, arithmetic coding, 12-bit, lossless
  and hierarchical JPEG, PNGs that already are HDR (`cICP` with PQ or HLG), more than
  200 megapixels. HEIF/HEIC and AVIF are not supported.

## Checks

`make check` builds `build-support/tools/core-check.m`, which compiles the core natively and
compares it with Apple's frameworks:

- Test images written by ImageIO (baseline, best quality, progressive, greyscale, EXIF
  rotation, Display P3, 8/16-bit PNG, alpha, interlaced), hand-made PNGs (1/2/4/16-bit grey,
  palettes, `tRNS`, interlaced) and a JPEG with restart markers.
- Decoders against ImageIO: PNGs identical (±1 for 16-bit alpha), JPEGs within one level on
  average. ImageIO itself decodes identical JPEG data differently depending on the file,
  so single JPEG pixels are not compared.
- Exports: ImageIO finds the ISO gain map at full size; for JPEG sources the compressed
  image data is byte-identical to the original; Core Image's HDR rendering equals SDR times
  the intended gain (mean error below 0.03 stops) and reports the expected headroom; the
  orientation is kept.
- The accordion curve is monotonic for every threshold and a linear stretch at 0 %.
- Re-opening an exported file replaces its gain map.
- `tests/wasm-check.js` repeats every export and preview with `engine.wasm` in
  JavaScriptCore and requires byte-identical results.

JPEG and PNG files in `samples/` (not part of the repository) are added to the checks.

## Layout

- `core/`: the engine in freestanding C11 (DEFLATE, PNG, JPEG decoder and encoder, gain map,
  HDR container, session), built natively and as WebAssembly.
- `web/`: user interface, worker, service worker and the WASM adapter (`native/`).
- `build-support/`: build scripts, XCC patch and small Objective-C host tools.
- `assets/`: `icon.png` (transparent) and `icon-maskable.png` (full bleed);
  `build-support/tools/icon-art.m` draws both.
- `tests/`, `vendor/`.

Build system, offline packaging and updater are taken from IR Converter.

Proudly generated with Claude
