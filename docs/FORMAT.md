# How exr-viewer shows OpenEXR images

OpenEXR stores scene-linear colour in half or 32-bit floats, with no display curve and no upper limit.
Showing it on an 8-bit screen needs a few decisions, all made automatically.

## What is read

- The header is checked by exr-viewer itself (magic `76 2F 31 01`, version 2, required attributes). Files
  without a colour or luminance channel are declined, so they never get a broken thumbnail.
- Pixels are decoded by the OpenEXR library: scanline and tiled files (full resolution level), every compression.
- Deep files (several samples per pixel, with depth) are flattened: the samples of each pixel are sorted by `Z`
  and composited front to back ("over", premultiplied colours; samples without alpha are opaque). A deep pass
  thus shows as the element alone on a transparent background.
- The image is the display window: data outside it is cropped, and display pixels without data are transparent.
- Layer: the default one (`R`, `G`, `B`, `A` or `Y`). Multilayer renders without it show their `Combined` pass,
  or else the first layer with colour. Multipart files show their first part with colour or luminance (a
  depth-only first part is skipped); multi-view files show their default view.
- Luminance/chroma (`Y`, `RY`, `BY`) images are converted by the library. A file with a single colour channel
  (`R`, `G` or `B` only) is shown in grey.

## Colour pipeline

1. **Alpha.** OpenEXR colours are premultiplied, so they are divided by alpha. Some converters write straight
   alpha: when the values stay within 0..1 and over 1 % of the semi-transparent pixels are brighter than their
   alpha, the file is taken as straight. An alpha channel that is zero everywhere is ignored (image opaque).
2. **Primaries.** When the `chromaticities` attribute differs from sRGB/Rec.709, colours are converted through
   XYZ. A white point that is a real illuminant (D50, ACES D60...) is adapted to D65 (Bradford). An equal-energy
   white marks absolute CIE XYZ values, which are converted without adaptation.
3. **SDR or HDR.** The 99.9th percentile of the brightest channel, `max(R, G, B)` over the visible pixels, is the
   white level: it measures what a display would clip. Up to 1.1 the image is SDR: values are clipped to 0..1
   and nothing else changes. Above, it is HDR.
4. **Tone mapping (HDR only).** Pixels whose brightest channel `m` is up to 0.5 are kept as they are, so shadows
   and mid-tones look exactly like the SDR rendering. Above, a highlight shoulder compresses the range up to the
   white level into 0.5..1: with `x = m - 0.5`, `m' = 0.5 + x (1 + x / c) / (1 + 2x)` and `c = 2 (white - 0.5)²`
   (an extended Reinhard curve with slope 1 at the knee, reaching 1 at the white level). The three channels are
   scaled by `m' / m`, so hue and saturation are kept; pixels brighter than the white level keep their colour
   at full intensity instead of turning white.
5. **Encoding.** sRGB transfer curve, 8 bits, straight alpha BGRA. NaN becomes 0, infinities the largest half.

## Testing

- `tools/make_testdata.py <folder>` writes uncompressed files with their expected images (computed without
  OpenEXR); decode them with `exr2png <file.exr> <file.out.png>` and run the script again with `--check`.
- The official [openexr-images](https://github.com/AcademySoftwareFoundation/openexr-images) set covers the
  compressions, display windows, chromaticities, luminance/chroma, multi-part, multi-view and deep files. Its
  `Damaged` folder holds fuzzing files: every one must be declined without a crash.
- `exrtest` (built with the tools, not packaged) writes variants with the OpenEXR library:
  - `merge-deep <out> <passes...>` merges deep passes: decoding the merge of
    `v2/LowResLeftView/{Balls,Ground,Leaves,Trunks}.exr` matches `composited.exr`;
  - `recompress <in> <out> <compression>` rewrites an image with any compression (none, rle, zips, zip, piz,
    pxr24, b44, b44a, dwaa, dwab, htj2k256, htj2k32, lj2k, zstd): lossless ones must decode to identical pixels;
  - `depth-first <in> <out>` writes a two-part file whose first part only holds depth.

## Limits

- Thumbnails decode the full image (mipmap levels of tiled files are not used yet).
- Non-square pixels (`pixelAspectRatio` other than 1) are not stretched.
- Images over 2^27 pixels (e.g. 16384 x 8192), or deep images over 2^25 samples, are declined.
- Deep volumetric samples (`ZBack` > `Z`) are composited by their front depth, without splitting overlaps.
- The tone mapping is a fixed, neutral curve: it has no exposure control.
