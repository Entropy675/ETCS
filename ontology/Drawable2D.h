#ifndef SUPERTYPE_DRAWABLE2D_H__
#define SUPERTYPE_DRAWABLE2D_H__


#include "../core_defs.h"
#include "Drawable.h"
#include <cstdint>
#include <utility>
#include <vector>

// ---------------------------------------------------------------
// Point2D / Rect2D
// ---------------------------------------------------------------
//
// Plain geometry, in the same spirit as WindowSize in Resizable.h --
// declared beside the family that gives it meaning rather than in a
// shared header nothing else needs.
//
// Signed origin, unsigned extent: a child may be positioned at a
// negative offset inside its parent (a shape half off the left edge
// is an ordinary thing to draw), but a width of -3 is not a thing at
// all. Same split Surface_::DrawRect already makes.

struct Point2D
{
    int32_t x;
    int32_t y;
};

struct Rect2D
{
    int32_t  x;
    int32_t  y;
    uint32_t w;
    uint32_t h;
};

// ---------------------------------------------------------------
// Drawable2D
// ---------------------------------------------------------------
//
// A Drawable whose addressable space is two-dimensional and stated
// RELATIVE TO ITS PARENT.
//
// The model is self-similar all the way down, and that is the point
// rather than an elegance: the root drawable is a rectangle the size
// of the screen, and it differs from the shapes nested inside it
// ONLY by being the most addressable space there is. It is not a
// special kind of node, it is the node with no parent above it. A
// triangle nested three levels down is the same kind of thing as a
// window-sized rectangle -- both are surfaces you can Clear, DrawRect
// and Blit into, both carve space out of whatever contains them, and
// both hand space to their own children on identical terms.
//
// So there is no separate "shape" concept, no primitive-versus-
// container distinction, and no drawing API that special-cases the
// root. A triangle IS a surface; its extent simply is not
// rectangular, which is what ContainsLocal is for. That is the whole
// difference between a triangle and the screen.
//
// EVERY CHILD ADDRESSES ONLY ITS PARENT'S SPACE. Bounds() is stated
// in the parent's coordinates and nothing here is ever stated in
// screen coordinates -- a node cannot even express a position
// outside the frame of the thing containing it, because it has no
// vocabulary for one. Absolute position is derived by composition
// (ToParent applied up the chain) rather than stored, so a node
// moving moves its entire subtree for free, with nothing to
// invalidate and no second copy of the answer to drift.
//
// ON "LIGHT" SHAPES. Being a Drawable2D means satisfying the whole
// accumulated constraint -- Clear/DrawRect/Blit from Surface,
// GetSize from Resizable, DrawInto from Drawable, plus the two
// below. That is heavier than a struct with three floats in it, and
// it is meant to be: a shape you cannot draw into is not a surface,
// and the entire argument for this family is that it is. Lightness
// comes from SHARING A LEAF, not from thinning the constraint -- one
// generic shape leaf in a provider, implementing the surface verbs
// by clipping to its own ContainsLocal and delegating to its parent,
// serves triangles, circles and paths alike. One implementation,
// many shapes, full membership.
//
// EXCLUSIVE WITH Drawable3D. Siblings under Drawable, so a type may
// hold one or the other and never both -- enforced by the compiler,
// see Drawable.h.

class Drawable2D_ : virtual public ETCS::Entity
{
public:
    virtual ~Drawable2D_() = default;

    // Where this node sits in its PARENT's coordinate space, and how big
    // it is. The root's parent is the screen, so the root's bounds are the
    // screen -- which is the sense in which the root is "just a rectangle".
    virtual Rect2D Bounds() = 0;

    // Is this point, in THIS node's own local space, inside the node's
    // actual shape?
    //
    // The one place a triangle differs from a rectangle. Bounds gives the
    // enclosing box for both; this is what says which of the box's points
    // the node genuinely occupies. A rectangular node answers "yes for
    // anything inside w by h", which is why the root needs no special
    // case: the general answer degenerates to the trivial one.
    virtual bool ContainsLocal(int32_t x, int32_t y) = 0;

    /*
 * The two coordinate mappings, concrete rather than dispatched.
 *
 * Translation by Bounds()'s origin is what parent-relative addressing
 * MEANS, so the default is the definition, not a convenience -- a leaf
 * that accepted the family and then mapped coordinates some other way
 * would not be doing parent-relative addressing at all. Virtual so a
 * node with its own transform (a rotated shape, a scaled viewport) can
 * refine it, non-pure so the overwhelming majority of nodes -- every
 * shape that is merely placed somewhere -- inherit the right answer and
 * implement nothing.
 *
 * Not routed through the dispatch macro deliberately: a dispatched
 * method is one every leaf must define, and these two are exactly the
 * methods most leaves should not have to think about.
 */
    virtual Point2D ToParent(Point2D local)
    {
        const Rect2D b = Bounds();
        return Point2D{ local.x + b.x, local.y + b.y };
    }
    virtual Point2D ToLocal(Point2D parent)
    {
        const Rect2D b = Bounds();
        return Point2D{ parent.x - b.x, parent.y - b.y };
    }

    // Hit test in the PARENT's space -- the composition of the two above
    // with the shape predicate, written once because every caller that
    // asks "is the cursor on this node" needs exactly this and would
    // otherwise assemble it by hand, half of them forgetting the shape
    // and settling for the bounding box.
    bool ContainsParent(Point2D parent)
    {
        const Point2D local = ToLocal(parent);
        return ContainsLocal(local.x, local.y);
    }

    /*
 * Pick: the deepest, last-drawn node containing this point, or `this`
 * when the point is inside this node but inside none of its children.
 * nullptr when the point misses this node entirely.
 *
 * THE SAME LIST drawChildren uses (Drawable_::collectDrawableChildren,
 * ordered by Order()), read backwards. That is not a detail and not a
 * coincidence: the last child painted is the topmost one, so a picker
 * walking forward hands back the node buried underneath whatever the
 * user can actually see. Sharing one traversal is what keeps the two
 * answers the same answer -- two walks with the same rule written twice
 * agree only until somebody edits one of them.
 *
 * Reaches its own Drawable half through getInterfacePointer, because
 * Drawable2D_ does not inherit Drawable_ (Drawable.h explains why the
 * lineage lives in the Bases). Reaching a family by name is the rule
 * here even when the family is your own other half.
 *
 * Recurses through the family interface pointer too, never a concrete
 * type, so a subtree assembled from three different modules' shape
 * leaves picks exactly as well as one module's does.
 */
    Drawable2D_* Pick(Point2D local)
    {
        if (!ContainsLocal(local.x, local.y)) return nullptr;

        void* as_drawable = getInterfacePointer(ETCS::Buffer("Drawable"));
        if (!as_drawable) return this;   // no lineage composed: nothing nested to consult

        std::vector<Drawable_*> ordered;
        static_cast<Drawable_*>(as_drawable)->collectDrawableChildren(ordered);
        for (size_t i = ordered.size(); i-- > 0; )
        {
            void* as_2d = ordered[i]->getInterfacePointer(ETCS::Buffer("Drawable2D"));
            if (!as_2d) continue;        // a 3D child is not pickable in this plane
            Drawable2D_* d = static_cast<Drawable2D_*>(as_2d);
            if (Drawable2D_* hit = d->Pick(d->ToLocal(local))) return hit;
        }
        return this;
    }
};

#endif
