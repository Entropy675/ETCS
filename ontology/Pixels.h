#ifndef SUPERTYPE_PIXELS_H__
#define SUPERTYPE_PIXELS_H__


#include "../core_defs.h"
#include <cstdint>
#include <cstring>
#include <vector>

// ---------------------------------------------------------------
// Pixels
// ---------------------------------------------------------------
//
// A surface whose bytes live in CPU memory and can be read and
// written directly. Split from Surface for the same reason
// Presentable was: a swapchain-backed surface has no CPU bytes to
// hand out, and only some surfaces are meant to be edited a pixel
// at a time.
//
// This family OWNS its buffer rather than declaring an interface to
// one, the same way InputSource_ owns its event ring instead of
// making every window reimplement it. The buffer, the dirty flag
// and the CPU raster below are backend-independent by construction,
// so a second rendering backend inherits all of it unchanged and
// only has to implement upload.
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
// DIRTY: writers call MarkDirty(); whoever uploads calls TakeDirty(),
// which consumes the flag. One consumer is assumed -- the surface
// that blits these pixels to a device -- which is true today and
// will need revisiting the moment two devices share one image.

class Pixels_ : virtual public ETCS::Entity
{
public:
    virtual ~Pixels_() = default;

    // Zero-fills (fully transparent). Idempotent for the same size, so
    // re-Allocating an unchanged image is not a silent realloc.
    void Allocate(uint32_t w, uint32_t h)
    {
        if (w == m_pw && h == m_ph && !m_pixels.empty()) return;
        m_pw = w;
        m_ph = h;
        m_pixels.assign(static_cast<size_t>(w) * h * 4, 0);
        m_dirty = true;
    }

    uint8_t*        PixelData()             { return m_pixels.empty() ? nullptr : m_pixels.data(); }
    const uint8_t*  PixelData()       const { return m_pixels.empty() ? nullptr : m_pixels.data(); }
    uint32_t        PixelWidth()      const { return m_pw; }
    uint32_t        PixelHeight()     const { return m_ph; }
    uint32_t        PixelStride()     const { return m_pw * 4; }
    size_t          PixelBytes()      const { return m_pixels.size(); }

    void MarkDirty() { m_dirty = true; }
    bool TakeDirty() { bool d = m_dirty; m_dirty = false; return d; }

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
            std::memcpy(m_pixels.data() + i, px, 4);
        m_dirty = true;
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
        m_dirty = true;
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
        m_dirty = true;
    }

protected:
    std::vector<uint8_t> m_pixels;
    uint32_t             m_pw    = 0;
    uint32_t             m_ph    = 0;
    bool                 m_dirty = false;

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
 * WHOEVER HOLDS A MERGED COPY OF WHAT CHANGED IS OUT OF DATE, AND EVERY PIXEL
 * OWNER ABOVE YOU HOLDS ONE.
 *
 * That is the entire upward half of the dirty flag, and it is what lets a
 * compositor skip a whole subtree safely: a node that changes is responsible
 * for saying so, and it says so to exactly the caches that could hold it --
 * the pixel owners on the path from here to the root, and nothing on a
 * sibling branch.
 *
 * Walks PAST a pixel owner rather than stopping at the first. A compositor
 * nested in a compositor caches this node transitively and both must be told.
 * This is the difference between this walk and the coordinate one in
 * Drawable2D: coordinates are relative to the NEAREST origin, staleness
 * propagates to EVERY cache.
 *
 * `from` is included, so a brush that wrote into a buffer from outside the
 * tree passes the node it wrote to; a node that changed itself passes `this`.
 * Both are the same statement. Reached by family name, so this needs to know
 * nothing about any concrete type -- it marks anything that owns pixels,
 * which is exactly the set of things that could have cached the caller.
 */
inline void etcs_mark_pixel_path(ETCS::Entity* from)
{
    for (ETCS::Entity* n = from; n; n = n->getParent())
    {
        void* p = n->getInterfacePointer(ETCS::Buffer("Pixels"));
        if (p) static_cast<Pixels_*>(p)->MarkDirty();
    }
}

#endif
