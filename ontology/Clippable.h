#ifndef SUPERTYPE_CLIPPABLE_H__
#define SUPERTYPE_CLIPPABLE_H__


#include "../core_defs.h"
#include <algorithm>
#include <cstdint>
#include <vector>

// ---------------------------------------------------------------
// Clippable
// ---------------------------------------------------------------
//
// A surface that can restrict where drawing lands: push a
// rectangle, draw, pop it back.
//
// Its own family rather than more required surface area on
// Surface, because not every surface can honour one and a family
// is a claim about what a type CAN do. A window's swapchain sets a
// scissor; a CPU layer bounds its span writes; a future
// device-side offscreen target does whichever its backend
// prefers. A surface that cannot do any of that simply does not
// compose this, and a caller that needs clipping asks for
// "Clippable" and finds out.
//
// FOUR SCALARS, NOT A RECT STRUCT, and that is deliberate. Rect2D
// belongs to the Drawable2D leaf (ontology/Drawable2D.h) and
// carries its meaning -- a node's bounds in its PARENT's space.
// Reusing it here would either drag the whole Drawable lineage
// into a family any Surface should be able to compose, or
// duplicate a geometry type so the two can drift. Surface_ already
// states rectangles as four scalars for the same reason.
//
// THE ARITHMETIC IS THE FAMILY'S; APPLYING IT IS THE BACKEND'S.
// PushClip intersects with whatever is already in effect and PopClip
// restores the previous state -- that is stack discipline plus
// rectangle intersection, identical for every backend, and written
// once here rather than re-derived per implementation (the same
// argument that put the buffer in Pixels_ and the listener set in
// Resizable_). What a backend must supply is the one thing only it
// knows: how to make the effective rectangle true of its own
// drawing. That is SetScissor, and it is dispatched.
//
// NESTING INTERSECTS, IT DOES NOT REPLACE. A child clip can only
// ever shrink the region -- which is what makes this composable
// with the Drawable tree, where a node's children address space on
// it and can never legitimately draw outside it. Push the parent's
// bounds, draw the subtree, pop: the guarantee holds however deep
// the subtree goes, because every level intersects again.
//
// An EMPTY clip (zero width or height) is a legitimate state, not
// an error: a child entirely outside its parent clips to nothing
// and draws nothing. Backends must treat it as "draw nothing"
// rather than as "no clip", which is the one way this can go
// silently wrong.

class Clippable_ : virtual public ETCS::Entity
{
public:
    virtual ~Clippable_() = default;

    // Make the given rectangle the region drawing is confined to. Called by
    // PushClip/PopClip whenever the effective region CHANGES -- not once per
    // draw -- so a backend that sets device state pays only for real changes.
    virtual void SetScissor(int32_t x, int32_t y, uint32_t w, uint32_t h) = 0;

    /*
 * Intersect and push. The new region is the intersection of the requested
 * rectangle with whatever is already in effect, so a caller can push
 * without first asking what it is nested inside -- which is the property
 * that lets a recursive draw push at every level and stay correct.
 */
    void PushClip(int32_t x, int32_t y, uint32_t w, uint32_t h)
    {
        ClipRect next{x, y, w, h};
        if (!m_clipStack.empty()) next = intersect(m_clipStack.back(), next);
        m_clipStack.push_back(next);
        SetScissor(next.x, next.y, next.w, next.h);
    }

    /*
 * Restore the region in effect before the matching PushClip.
 *
 * Popping an empty stack is a no-op rather than an error. An unbalanced
 * pop is a bug in the caller, but the useful failure is a wrong picture,
 * not a crash inside a draw loop -- and a backend that has just been told
 * to clip to nothing would be the thing that crashed.
 */
    void PopClip()
    {
        if (m_clipStack.empty()) return;
        m_clipStack.pop_back();
        if (m_clipStack.empty()) { ClearClip(); return; }
        const ClipRect& top = m_clipStack.back();
        SetScissor(top.x, top.y, top.w, top.h);
    }

    // "No clip" as a distinct state from "a clip that happens to be big":
    // a backend can disable scissoring entirely rather than set it to the
    // full extent, and the two are not the same instruction on every device.
    virtual void ClearClip() { SetScissor(0, 0, UINT32_MAX, UINT32_MAX); }

    bool HasClip() const { return !m_clipStack.empty(); }

    // The region currently in effect, for a backend that reads it at draw
    // time rather than tracking SetScissor calls. Both are legitimate; a CPU
    // rasteriser usually wants this one.
    void CurrentClip(int32_t& x, int32_t& y, uint32_t& w, uint32_t& h) const
    {
        if (m_clipStack.empty()) { x = 0; y = 0; w = UINT32_MAX; h = UINT32_MAX; return; }
        const ClipRect& top = m_clipStack.back();
        x = top.x; y = top.y; w = top.w; h = top.h;
    }

protected:
    struct ClipRect { int32_t x; int32_t y; uint32_t w; uint32_t h; };

    // Intersection in signed space, clamped at zero. Written out rather than
    // done with min/max on unsigned widths, where an empty result underflows
    // into an enormous one -- which would turn "clips to nothing" into
    // "clips to everything", the exact inversion this family must not make.
    static ClipRect intersect(const ClipRect& a, const ClipRect& b)
    {
        const int64_t ax0 = a.x, ay0 = a.y;
        const int64_t ax1 = ax0 + static_cast<int64_t>(a.w), ay1 = ay0 + static_cast<int64_t>(a.h);
        const int64_t bx0 = b.x, by0 = b.y;
        const int64_t bx1 = bx0 + static_cast<int64_t>(b.w), by1 = by0 + static_cast<int64_t>(b.h);

        const int64_t x0 = std::max(ax0, bx0), y0 = std::max(ay0, by0);
        const int64_t x1 = std::min(ax1, bx1), y1 = std::min(ay1, by1);
        if (x1 <= x0 || y1 <= y0)
            return ClipRect{ static_cast<int32_t>(x0), static_cast<int32_t>(y0), 0, 0 };
        return ClipRect{ static_cast<int32_t>(x0), static_cast<int32_t>(y0),
                         static_cast<uint32_t>(x1 - x0), static_cast<uint32_t>(y1 - y0) };
    }

    std::vector<ClipRect> m_clipStack;
};

#endif
