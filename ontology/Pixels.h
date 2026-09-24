#ifndef SUPERTYPE_PIXELS_H__
#define SUPERTYPE_PIXELS_H__


#include "../core_defs.h"
#include "Observable.h"
#include "Raster.h"
#include <cstdint>
#include <cstring>
#include <vector>

// ---------------------------------------------------------------
// Pixels
// ---------------------------------------------------------------
//
// A raster whose bytes live in CPU memory and can be read and
// written directly. Split from Surface for the same reason
// Presentable was: a swapchain-backed surface has no CPU bytes to
// hand out, and only some surfaces are meant to be edited a pixel
// at a time.
//
// ONE OF TWO, and the other is Renderable (ontology/Renderable.h).
// Everything below is a consequence of the bytes being addressable
// by the host; a device-resident raster has none of it. What the
// two share is that they have a SIZE, which is Raster
// (ontology/Raster.h) -- inherited VIRTUALLY and directly, which
// that header explains: Raster has no base of its own to be reached
// twice through, so this is the one lineage in the ontology that
// belongs in the interface rather than in the Base. The practical
// consequence is that a Pixels_* answers its own dimensions, and
// the two families remain mutually exclusive anyway -- by the
// final-overrider rule, on the two accessors implemented just below.
//
// This family OWNS its buffer rather than declaring an interface to
// one, the same way InputSource_ owns its event ring instead of
// making every window reimplement it. The buffer and the CPU raster
// below are backend-independent by construction, so a second
// rendering backend inherits all of it unchanged and only has to
// implement upload.
//
// FORMAT, stated exactly because a projection layer depends on it:
// 8 bits per channel, R,G,B,A in ascending byte order, NOT
// premultiplied, tightly packed, stride = width * 4, origin
// top-left. Cairo's ARGB32 -- what Pinta's ImageSurface actually
// holds -- is premultiplied BGRA on little-endian, so a
// PintaProvider adapter converts on the way in and out. That
// conversion is the adapter's job on purpose: this family stays the
// one obvious format rather than growing a format enum that every
// backend then has to handle every case of.
//
// DIRTY IS NOT HERE. It used to be: one bool, MarkDirty to set,
// TakeDirty to read-and-clear, and a comment saying one consumer
// was assumed and that two devices sharing an image would need
// revisiting. Two devices sharing an image is paint_two_windows,
// and the second surface's TakeDirty returned false forever.
//
// So a Pixels leaf claims Observable and the edge lives there,
// per observer. What that leaves here is the readable state, which
// is the honest division: the mark says only that there is
// something to read on this side, and this buffer IS the something.
// A writer calls etcs_mark_observed (Observable.h); a reader asks
// TakeObserved(its own RID) and, if told yes, reads PixelData().

class Pixels_ : virtual public Raster_
{
public:
    virtual ~Pixels_() = default;

    // Raster_'s two questions, answered from the buffer's own dimensions
    // rather than from a number kept beside it -- so the size a consumer is
    // told and the size the bytes actually have cannot drift apart. This is
    // also the pair whose overrider a leaf claiming Renderable as well would
    // make ambiguous, which is where the exclusivity is enforced.
    uint32_t PixelWidth()  const override { return m_pw; }
    uint32_t PixelHeight() const override { return m_ph; }

    // Zero-fills (fully transparent). Idempotent for the same size, so
    // re-Allocating an unchanged image is not a silent realloc.
    void Allocate(uint32_t w, uint32_t h)
    {
        if (w == m_pw && h == m_ph && !m_pixels.empty()) return;
        m_pw = w;
        m_ph = h;
        m_pixels.assign(static_cast<size_t>(w) * h * 4, 0);
        etcs_mark_observed(this);
    }

    // Everything that is only true of a host-addressable buffer -- where it
    // starts, how far apart its rows are, and how much of it there is. A
    // device image answers none of these: its row pitch is the driver's
    // business and its bytes have no address in this process. That is the
    // whole of what separates this family from Renderable.
    uint8_t*        PixelData()             { return m_pixels.empty() ? nullptr : m_pixels.data(); }
    const uint8_t*  PixelData()       const { return m_pixels.empty() ? nullptr : m_pixels.data(); }
    uint32_t        PixelStride()     const { return m_pw * 4; }
    size_t          PixelBytes()      const { return m_pixels.size(); }

    // REPLACES every pixel, including alpha -- this is what a surface's
    // Clear means, and it is deliberately not FillRect over the whole
    // buffer: blending a colour onto whatever was there would leave the old
    // contents showing through at any alpha below 1, which is the opposite
    // of clearing.
    void ClearTo(float r, float g, float b, float a)
    {
        if (m_pixels.empty()) return;
        const uint8_t px[4] = { toByte(r), toByte(g), toByte(b), toByte(a) };
        for (size_t i = 0; i < m_pixels.size(); i += 4)
            ::std::memcpy(m_pixels.data() + i, px, 4);
        etcs_mark_observed(this);
    }

    // Source-over fill of an axis-aligned rect, clipped to the buffer.
    // The one raster primitive this family provides, because it is the
    // one every backend's offscreen Clear/DrawRect reduces to.
    void FillRect(int32_t x, int32_t y, uint32_t w, uint32_t h,
                   float r, float g, float b, float a)
    {
        if (m_pixels.empty() || a <= 0.0f) return;

        int32_t x0 = x < 0 ? 0 : x;
        int32_t y0 = y < 0 ? 0 : y;
        int64_t x1 = static_cast<int64_t>(x) + w;
        int64_t y1 = static_cast<int64_t>(y) + h;
        if (x1 > m_pw) x1 = m_pw;
        if (y1 > m_ph) y1 = m_ph;
        if (x0 >= x1 || y0 >= y1) return;

        const uint8_t sr = toByte(r), sg = toByte(g), sb = toByte(b), sa = toByte(a);
        for (int64_t py = y0; py < y1; ++py)
        {
            uint8_t* row = m_pixels.data() + (static_cast<size_t>(py) * PixelStride());
            for (int64_t px = x0; px < x1; ++px)
                blendPixel(row + px * 4, sr, sg, sb, sa);
        }
        etcs_mark_observed(this);
    }

    /*
     * The second raster primitive, and it is here for the same reason the first
     * one is: a DISC IS WHAT A NIB IS, everywhere, and every caller that wanted
     * one was reaching for rectangles to approximate it.
     *
     * Both roundings were in the tree at once and they disagreed. A paint layer
     * kept dx^2+dy^2 <= r^2 and committed a circle; the live preview of that same
     * dab drew one 2r-square DrawRect, so the nib under the pointer was square
     * and the stroke turned round as soon as the document re-rendered. The other
     * way out is a span per row through the surface verb -- exactly round, but
     * 2r+1 dispatched calls, and with each of them marking (see above) that is
     * 2r+1 invalidations for one dab, which a compositor above then takes as
     * 2r+1 separate changes.
     *
     * ONE CALL, ONE MARK, ONE SHAPE. Same test as the layer's, so the preview and
     * the mark it becomes are the same discrete circle rather than two roundings
     * of one idea. A device-backed destination still has to span it, because it
     * has no host address to write -- that is a backend's problem, not a reason
     * for the host path to be wrong.
     */
    void FillDisc(int32_t cx, int32_t cy, uint32_t radius,
                  float r, float g, float b, float a)
    {
        if (m_pixels.empty() || a <= 0.0f || radius == 0) return;

        const int64_t rad = radius;
        const int64_t r2  = rad * rad;
        const uint8_t sr = toByte(r), sg = toByte(g), sb = toByte(b), sa = toByte(a);

        int64_t dy0 = -rad, dy1 = rad;
        if (cy + dy0 < 0)    dy0 = -cy;
        if (cy + dy1 > m_ph - 1) dy1 = static_cast<int64_t>(m_ph) - 1 - cy;

        for (int64_t dy = dy0; dy <= dy1; ++dy)
        {
            // Half-chord at this row: the largest dx with dx^2 + dy^2 <= r^2.
            int64_t k = 0;
            while ((k + 1) * (k + 1) + dy * dy <= r2) ++k;

            int64_t x0 = cx - k, x1 = cx + k;
            if (x0 < 0) x0 = 0;
            if (x1 > static_cast<int64_t>(m_pw) - 1) x1 = static_cast<int64_t>(m_pw) - 1;
            if (x0 > x1) continue;

            uint8_t* row = m_pixels.data() + (static_cast<size_t>(cy + dy) * PixelStride());
            for (int64_t px = x0; px <= x1; ++px)
                blendPixel(row + px * 4, sr, sg, sb, sa);
        }
        etcs_mark_observed(this);
    }

    // Source-over composite of another Pixels_ into this one at (x, y),
    // scaled by a uniform opacity. Nearest-neighbour, no scaling: a
    // layered editor composites layers at 1:1 and lets the DEVICE scale
    // the finished canvas, so a resampler here would be the wrong place
    // to pay for it.
    void Composite(const Pixels_& src, int32_t x, int32_t y, float opacity)
    {
        if (m_pixels.empty() || src.m_pixels.empty() || opacity <= 0.0f) return;

        for (uint32_t sy = 0; sy < src.m_ph; ++sy)
        {
            int64_t dy = static_cast<int64_t>(y) + sy;
            if (dy < 0 || dy >= m_ph) continue;
            const uint8_t* srow = src.m_pixels.data() + (static_cast<size_t>(sy) * src.PixelStride());
            uint8_t*       drow = m_pixels.data()     + (static_cast<size_t>(dy) * PixelStride());

            for (uint32_t sx = 0; sx < src.m_pw; ++sx)
            {
                int64_t dx = static_cast<int64_t>(x) + sx;
                if (dx < 0 || dx >= m_pw) continue;
                const uint8_t* s = srow + sx * 4;
                uint8_t alpha = static_cast<uint8_t>(s[3] * (opacity > 1.0f ? 1.0f : opacity));
                if (alpha == 0) continue;
                blendPixel(drow + dx * 4, s[0], s[1], s[2], alpha);
            }
        }
        etcs_mark_observed(this);
    }

protected:
    ::std::vector<uint8_t> m_pixels;
    uint32_t             m_pw    = 0;
    uint32_t             m_ph    = 0;

private:
    static uint8_t toByte(float v)
    {
        if (v <= 0.0f) return 0;
        if (v >= 1.0f) return 255;
        return static_cast<uint8_t>(v * 255.0f + 0.5f);
    }

    // Source-over on non-premultiplied RGBA8, integer arithmetic:
    //   out.rgb = (src.rgb * sa + dst.rgb * dst.a * (255 - sa)) / out.a
    // simplified to the common opaque-destination case plus the general
    // one, both kept in 16-bit intermediates so a full-alpha blend is
    // exactly the source and a zero-alpha blend is exactly the dest.
    static void blendPixel(uint8_t* d, uint8_t sr, uint8_t sg, uint8_t sb, uint8_t sa)
    {
        if (sa == 255) { d[0] = sr; d[1] = sg; d[2] = sb; d[3] = 255; return; }

        const uint32_t inv = 255u - sa;
        const uint32_t da  = d[3];
        const uint32_t oa  = sa + (da * inv) / 255u;
        if (oa == 0) { d[0] = d[1] = d[2] = d[3] = 0; return; }

        d[0] = static_cast<uint8_t>((sr * sa + d[0] * da * inv / 255u) / oa);
        d[1] = static_cast<uint8_t>((sg * sa + d[1] * da * inv / 255u) / oa);
        d[2] = static_cast<uint8_t>((sb * sa + d[2] * da * inv / 255u) / oa);
        d[3] = static_cast<uint8_t>(oa);
    }
};

/*
 * A RASTER'S BYTES, WHEN THEY ARE ITS PICTURE -- or null.
 *
 * Writing straight into a destination's Pixels_ is the fast path several
 * drawables take instead of the Surface verbs: one blend over a row instead of
 * a dispatched call per rect. It is only right while those bytes ARE what gets
 * shown. A surface with a Device under it (ontology/Device.h) may be drawing
 * through that device instead, where its host bytes are the floor it falls
 * back to, not the frame -- and a write there never reaches the screen. So the
 * fast path asks this, and a raster with a Device child is written through its
 * verbs, which it answers on whichever side it is drawing.
 *
 * Any Device, ready or not: a device that is still arriving (a browser's
 * comes asynchronously) is one the surface will switch to, and a picture it
 * keeps across that switch has to have been drawn through the verbs.
 */
inline Pixels_* etcs_direct_pixels(Pixels_* px)
{
    if (!px) return nullptr;
    ETCS::Entity* e = static_cast<ETCS::Entity*>(px);
    ::std::vector<::std::pair<ETCS::Buffer, ETCS::RID>> kids;
    e->getTypedChildren(kids);
    for (const auto& k : kids)
    {
        ETCS::Entity* c = e->getTypedChild(k.first, k.second);
        if (c && c->getInterfacePointer(ETCS::Buffer("Device"))) return nullptr;
    }
    return px;
}

inline Pixels_* etcs_direct_pixels(ETCS::Entity* e)
{
    return e ? etcs_direct_pixels(static_cast<Pixels_*>(e->getInterfacePointer(ETCS::Buffer("Pixels"))))
             : nullptr;
}

#endif
