#ifndef BASE_DRAWABLE_H__
#define BASE_DRAWABLE_H__
#include "Drawable.h"
#include "SurfaceBase.h"

// Carries the lineage: this is where a Drawable becomes a Surface, and
// through SurfaceBase's own composition, a Resizable as well. See
// Drawable.h for why the refinement is spelled here rather than in the
// interface header, and what that buys.
//
// A concrete drawable therefore registers FOUR interface pointers with no
// further work -- "Drawable", "Surface", "Resizable", "Orderable" -- one per
// supertype constructor in the chain, so foreign code can reach it at
// whichever level of specificity it actually needs. A compositor asking for
// "Surface" gets one; a hit-tester asking for "Drawable" gets one; neither
// has to know which leaf answered.
//
// Orderable is composed here rather than left to each leaf because a
// drawable that could not say where it stands relative to its siblings has
// no answer to "what is on top", which is not an optional question for
// something in a picture. It brings a REQUIREMENT with it: every drawable
// leaf must declare operator<, checked at compile time (OrderableBase.h).
// That is what orders the parent's typed-children list -- see Drawable.h on
// why the cross-tag merge is a separate, scalar question.
//
// This base is abstract on purpose even though it has no dispatch entries
// of its own beyond DrawInto: "a Drawable that is neither 2D nor 3D" is not
// a thing to instantiate, it is the shared half of two things that are.
ETCS_SUPERTYPE_BASE(Drawable), public SurfaceBase<Derived>
{
    ETCS_MAKE_INSTANCE(Drawable)
    ETCS_DISPATCH_METHOD(void, DrawInto, (Surface_*, dst));
};

#endif
