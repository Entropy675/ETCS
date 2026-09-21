#ifndef BASE_DRAWABLE2D_H__
#define BASE_DRAWABLE2D_H__
#include "Drawable2D.h"
#include "DrawableBase.h"

// The 2D leaf of the Drawable lineage. Composing DrawableBase is what makes
// the obligation cumulative -- a type reaching this base owes Clear,
// DrawRect, Blit, GetSize and DrawInto as well as the two dispatched below,
// and gets "Drawable2D", "Drawable", "Surface" and "Resizable" interface
// pointers registered for it.
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

protected:
    /*
     * Where this node's PARENT sits on the destination, composed from every
     * Drawable2D ancestor's own origin. This is the upward half of the contract
     * doing its work: no node stores an absolute position, so a node moving
     * moves its subtree, and a subtree grafted onto a different parent lands
     * wherever that parent is with nothing rewritten.
     *
     * Stops at the first ancestor that is not a Drawable2D -- a Window, an
     * Instance, whatever else an entity may be nested under. That boundary is
     * exactly "the outermost drawable", which is the node whose coordinates are
     * the destination's own.
     *
     * AND AT THE FIRST ANCESTOR THAT IS A RASTER, without adding its origin. A
     * node with a raster of its own is a COORDINATE ORIGIN: its children state
     * their points in its space, and that raster IS that space, so the offset
     * between them is zero. Whatever that node is nested inside is its own
     * problem, resolved once, when it is blitted. Without this rule a
     * composited subtree would be drawn at its screen position inside a buffer
     * that starts at its own top-left, which is the same picture translated by
     * however deep the tree happened to be.
     *
     * ASKED AS Raster, NOT Pixels, which is the point of that family existing
     * (ontology/Raster.h). "Is this node an origin" is a question about whether
     * it HAS a raster, not about whose memory the raster sits in -- and while
     * every raster in this system was CPU-backed the two had the same answer. A
     * device-resident ancestor is just as much an origin, and was walked
     * straight past.
     *
     * Walked per draw rather than cached. It is O(depth) on a chain that is
     * three or four deep in practice, and a cached transform is a second copy
     * of the answer -- the thing this whole arrangement exists to not have.
     *
     * ON THE BASE AND PROTECTED, unlike ToParent/ToLocal/Pick next door on the
     * interface. Every 2D leaf walks this at draw time and all of them have to
     * reach the same answer -- children that are interchangeable only until
     * their coordinates mean different things are not interchangeable -- so the
     * walk is the family's, written once, rather than a rule each leaf is
     * trusted to restate. Kept off the interface because a caller holding a
     * Drawable2D_* has no business asking a node for an absolute position;
     * public, it would be the first step toward somebody storing one.
     */
    Point2D parentAbsoluteOrigin()
    {
        Point2D acc{0, 0};
        for (ETCS::Entity* node = getParent(); node; node = node->getParent())
        {
            void* d2 = node->getInterfacePointer(ETCS::Buffer("Drawable2D"));
            if (!d2) break;
            if (node->getInterfacePointer(ETCS::Buffer("Raster"))) break;  // origin
            const Rect2D pb = static_cast<Drawable2D_*>(d2)->Bounds();
            acc.x += pb.x;
            acc.y += pb.y;
        }
        return acc;
    }
};

#endif
