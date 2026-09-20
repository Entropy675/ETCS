#ifndef SUPERTYPE_SCALEDCOMPOSITE_H__
#define SUPERTYPE_SCALEDCOMPOSITE_H__

#include "Pixels.h"

#include <algorithm>
#include <cstdint>

/*
 * WHAT Surface_::Blit's w/h WERE ALWAYS FOR.
 *
 * IN THE ONTOLOGY, NOT IN A PROVIDER, because it is Pixels_ arithmetic and
 * nothing else -- no backend, no device, no module's private types. Every
 * implementor of Blit owes the same answer, and so does every caller that
 * projects Pixels_ into someone else's Pixels_ without going through Blit
 * (PaintLayer::BlitTo cannot: a layer is not a surface). Written once in
 * RenderProvider, it was immediately copied into PaintProvider, which is what
 * a shared primitive living in one module's tree always produces.
 *
 * Every CPU-side implementor took the pair and threw it away -- `(void)w;
 * (void)h;` with a note that this path is 1:1 and a resampler would be the
 * wrong place to pay for it. That was true while the only caller composited
 * layers at their own size, and it stopped being true the moment a projected
 * document had to reach a surface: a scale has to happen SOMEWHERE, and with
 * this one refusing it, the caller grew its own.
 *
 * What the caller grew is the reason this header exists. PaintLayer::BlitTo
 * walked the SOURCE and emitted one virtual DrawRect per sample, so its
 * fidelity was bounded by how many calls it could afford -- every 4th pixel at
 * 100% zoom, drawn as a 5x5 block. The picture was shown at a sixteenth of the
 * resolution it was stored at, thin marks were either missed or fattened into
 * squares, and it only reached 1:1 at 400% zoom, which is backwards.
 *
 * DESTINATION-DRIVEN is what fixes that, and it is also the only form that can
 * be correct: one output pixel is written exactly once, its source found by
 * inverse mapping. Magnified, neighbouring outputs land on the same input;
 * shrunk, inputs are skipped -- which is decimation, and is what shrinking IS.
 *
 * NEAREST, stated rather than defaulted. This is the blit a paint program shows
 * a document through, and a magnified pixel should look like a pixel; smoothing
 * is a filter somebody asks for, not something the transport does on the way
 * past. A smoothed variant belongs beside this one, not instead of it.
 */
static inline void render_composite_scaled(Pixels_& dst, const Pixels_& src,
                                           int32_t x, int32_t y,
                                           uint32_t dw, uint32_t dh,
                                           float opacity)
{
    const uint32_t sw = src.PixelWidth();
    const uint32_t sh = src.PixelHeight();
    if (sw == 0 || sh == 0 || dw == 0 || dh == 0 || opacity <= 0.0f) return;

    uint8_t* dp = dst.PixelData();
    const uint8_t* sp = src.PixelData();
    if (!dp || !sp) return;

    const uint32_t dstw = dst.PixelWidth();
    const uint32_t dsth = dst.PixelHeight();
    const uint32_t dstride = dst.PixelStride();
    const uint32_t sstride = src.PixelStride();
    const float o = (opacity > 1.0f) ? 1.0f : opacity;

    // Clipped to the destination up front, so the inner loop carries no bounds
    // test beyond the source lookup it cannot avoid.
    const int64_t x0 = std::max<int64_t>(0, x);
    const int64_t y0 = std::max<int64_t>(0, y);
    const int64_t x1 = std::min<int64_t>(dstw, static_cast<int64_t>(x) + dw);
    const int64_t y1 = std::min<int64_t>(dsth, static_cast<int64_t>(y) + dh);

    for (int64_t dy = y0; dy < y1; ++dy)
    {
        // Inverse map, in source pixels. Integer truncation IS the nearest
        // sample for a non-negative ratio, and doing it per row rather than per
        // pixel keeps the division out of the inner loop.
        const int64_t sy = ((dy - y) * sh) / dh;
        if (sy < 0 || sy >= static_cast<int64_t>(sh)) continue;
        const uint8_t* srow = sp + static_cast<size_t>(sy) * sstride;
        uint8_t*       drow = dp + static_cast<size_t>(dy) * dstride;

        for (int64_t dx = x0; dx < x1; ++dx)
        {
            const int64_t sx = ((dx - x) * sw) / dw;
            if (sx < 0 || sx >= static_cast<int64_t>(sw)) continue;
            const uint8_t* s = srow + static_cast<size_t>(sx) * 4;
            const uint32_t sa = static_cast<uint32_t>(s[3] * o);
            if (sa == 0) continue;
            uint8_t* d = drow + static_cast<size_t>(dx) * 4;
            if (sa >= 255)
            {
                d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = 255;
                continue;
            }
            // Source-over on NON-premultiplied bytes, which is what Pixels_
            // stores (ontology/Pixels.h) -- the same arithmetic blendPixel
            // does, written out because this one has no access to it.
            const uint32_t inv = 255u - sa;
            const uint32_t da  = d[3];
            const uint32_t oa  = sa + (da * inv) / 255u;
            if (oa == 0) { d[0] = d[1] = d[2] = d[3] = 0; continue; }
            d[0] = static_cast<uint8_t>((s[0] * sa + d[0] * da * inv / 255u) / oa);
            d[1] = static_cast<uint8_t>((s[1] * sa + d[1] * da * inv / 255u) / oa);
            d[2] = static_cast<uint8_t>((s[2] * sa + d[2] * da * inv / 255u) / oa);
            d[3] = static_cast<uint8_t>(oa);
        }
    }
}

/*
 * The same source-over, from bytes that are not a Pixels_.
 *
 * A published frame is a plain buffer the compositor keeps beside its raster
 * (CompositeDrawable2D::publish), not an entity, so there is nothing to resolve
 * a family on. 1:1 only, because a published frame is drawn at the size it was
 * composed at -- a scale would be a second resample of an already-resampled
 * image, which is exactly the compounding this whole change removes.
 */
static inline void render_composite_raw(Pixels_& dst,
                                        const uint8_t* sp, uint32_t sw, uint32_t sh,
                                        int32_t x, int32_t y, float opacity)
{
    if (!sp || sw == 0 || sh == 0 || opacity <= 0.0f) return;
    uint8_t* dp = dst.PixelData();
    if (!dp) return;
    const uint32_t dstw = dst.PixelWidth();
    const uint32_t dsth = dst.PixelHeight();
    const uint32_t dstride = dst.PixelStride();
    const uint32_t sstride = sw * 4;
    const float o = (opacity > 1.0f) ? 1.0f : opacity;

    const int64_t x0 = std::max<int64_t>(0, x);
    const int64_t y0 = std::max<int64_t>(0, y);
    const int64_t x1 = std::min<int64_t>(dstw, static_cast<int64_t>(x) + sw);
    const int64_t y1 = std::min<int64_t>(dsth, static_cast<int64_t>(y) + sh);

    for (int64_t dy = y0; dy < y1; ++dy)
    {
        const uint8_t* srow = sp + static_cast<size_t>(dy - y) * sstride;
        uint8_t*       drow = dp + static_cast<size_t>(dy) * dstride;
        for (int64_t dx = x0; dx < x1; ++dx)
        {
            const uint8_t* s = srow + static_cast<size_t>(dx - x) * 4;
            const uint32_t sa = static_cast<uint32_t>(s[3] * o);
            if (sa == 0) continue;
            uint8_t* d = drow + static_cast<size_t>(dx) * 4;
            if (sa >= 255) { d[0]=s[0]; d[1]=s[1]; d[2]=s[2]; d[3]=255; continue; }
            const uint32_t inv = 255u - sa;
            const uint32_t da  = d[3];
            const uint32_t oa  = sa + (da * inv) / 255u;
            if (oa == 0) { d[0]=d[1]=d[2]=d[3]=0; continue; }
            d[0] = static_cast<uint8_t>((s[0] * sa + d[0] * da * inv / 255u) / oa);
            d[1] = static_cast<uint8_t>((s[1] * sa + d[1] * da * inv / 255u) / oa);
            d[2] = static_cast<uint8_t>((s[2] * sa + d[2] * da * inv / 255u) / oa);
            d[3] = static_cast<uint8_t>(oa);
        }
    }
}

// Whether a Blit's w/h ask for anything other than the source's own size. Kept
// beside the scaler so every implementor asks the question the same way, and so
// "no scale requested" keeps taking the existing 1:1 path unchanged.
static inline bool render_blit_is_scaled(const Pixels_& src, uint32_t w, uint32_t h)
{
    return (w != 0 && h != 0) && (w != src.PixelWidth() || h != src.PixelHeight());
}

#endif
