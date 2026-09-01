#ifndef BASE_DRAWABLE2D_H__
#define BASE_DRAWABLE2D_H__
#include "Drawable2D.h"
#include "DrawableBase.h"

// The 2D leaf of the Drawable lineage. Composing DrawableBase is what makes
// the obligation cumulative -- a type reaching this base owes Clear,
// DrawRect, Blit, GetSize and DrawInto as well as the two below, and gets
// "Drawable2D", "Drawable", "Surface" and "Resizable" interface pointers
// registered for it.
//
// Only Bounds and ContainsLocal are dispatched. ToParent, ToLocal,
// ContainsParent and Pick are concrete on the interface (Drawable2D.h) and
// deliberately kept out of the dispatch set: a dispatched method is one
// every leaf must write, and those four are exactly the ones a leaf should
// inherit rather than reimplement. A node with its own transform still
// overrides ToParent/ToLocal directly -- they are virtual, just not
// required.
ETCS_SUPERTYPE_BASE(Drawable2D), public DrawableBase<Derived>
{
    ETCS_MAKE_INSTANCE(Drawable2D)
    ETCS_DISPATCH_METHOD(Rect2D, Bounds);
    ETCS_DISPATCH_METHOD(bool,   ContainsLocal, (int32_t, x), (int32_t, y));
};

#endif
