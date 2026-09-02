#ifndef BASE_DRAWABLE3D_H__
#define BASE_DRAWABLE3D_H__
#include "Drawable3D.h"
#include "DrawableBase.h"

// The 3D leaf of the Drawable lineage, composing the same DrawableBase its
// 2D sibling does -- which is both what makes the obligation cumulative and
// what makes holding both siblings a compile error rather than a review
// note (see Drawable.h).
//
// A 3D node is still a Surface, and that is not an oversight to be tidied
// away later: DrawInto has to land somewhere, Blit has to be able to read
// from it, and a scene node that could not answer those was never going to
// compose with the rest of the graph. What it draws when asked directly is
// its own business -- projecting through a default camera is the obvious
// answer, and the family does not mandate one.
//
// The dispatch set divides in two. Bounds3D and ContainsLocal3D are the
// exact counterparts of the 2D leaf's Bounds and ContainsLocal, one
// dimension up. DepthFor, DepthAt and Project are one seam, not three
// features: they are the SAME camera question asked at three
// granularities -- whole node, one pixel, the whole frame -- and their
// answers have to agree. That is why depth is dispatched here beside the
// projection rather than through an optional depth base a leaf could
// compose separately: composing it separately is exactly how the three
// answers come from three places and stop matching.
ETCS_SUPERTYPE_BASE(Drawable3D), public DrawableBase<Derived>
{
    ETCS_MAKE_INSTANCE(Drawable3D)
    ETCS_DISPATCH_METHOD(Box3D,        Bounds3D);
    ETCS_DISPATCH_METHOD(bool,         ContainsLocal3D, (Point3D, p));
    ETCS_DISPATCH_METHOD(DepthSpan,    DepthFor,        (Camera_*, camera));
    ETCS_DISPATCH_METHOD(float,        DepthAt,         (Camera_*, camera), (int32_t, x), (int32_t, y));
    ETCS_DISPATCH_METHOD(Drawable2D_*, Project,         (Camera_*, camera));
};

#endif
