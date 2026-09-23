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

    /*
     * ── HIDDEN, AND WHY IT IS HERE ────────────────────────────────────────
     *
     * A drawable had no way to be present and not drawn, and the gap is not
     * cosmetic: a POPUP needs one. The alternative arrangements are all worse
     * in the same way -- each makes "is it showing" a second fact that can
     * disagree with the first. Unparenting and reparenting moves a subtree in
     * and out of a tree that other things hold RIDs into; parking it at a
     * position off the canvas means the thing is still there, still picked,
     * still composited, and "closed" becomes a coordinate. The colour wheel had
     * taken a third route and simply never been drawn at all, so open governed
     * routing while nothing governed appearance.
     *
     * So the fact is stated once, on the node, and the DRAW is what reads it.
     *
     * DrawInto RATHER THAN EVERY CONTAINER'S WALK, which is what makes it
     * reliable: a hidden node draws nothing wherever it is reached from, so no
     * present or future container has to remember to ask. The children of a
     * hidden node are skipped with it, because a container draws its children
     * from inside its own DrawInto.
     *
     * PICKING TOO, and that is not an extra: it was left out of the first version
     * on the reasoning that "not drawn" and "not clickable" are separate
     * questions, and the result was measured within the hour. A popup hidden but
     * still in the pick tree swallowed every pointer sample over its rectangle,
     * so a stroke painted its first dab and then died the moment it crossed an
     * invisible panel. Drawable2D_::PickAt asks this as well.
     *
     * ContainsLocal is untouched, because that is a question about a rectangle
     * rather than about a node's participation -- and a router that keeps its own
     * set of live panes has its own answer. The other way a node declines a
     * pick without lying about its rectangle is the `passthrough` flag, which
     * PickAt asks beside this: drawn, never hit.
     *
     * The expansion of the dispatch macro, written out, because the guard has to
     * sit between the family entry and the leaf's Concrete.
     */
    virtual void DrawIntoConcrete(Surface_* dst) = 0;

    void DrawInto(Surface_* dst) override
    {
        if (m_hidden) return;
        static_cast<Derived*>(this)->DrawIntoConcrete(dst);
    }

    // Marked, because whoever holds a merged copy of this node has to rebuild
    // without it -- appearing and disappearing are changes like any other.
    void SetHidden(bool hidden)
    {
        if (m_hidden == hidden) return;
        m_hidden = hidden;
        etcs_mark_observed(static_cast<Derived*>(this));
    }
    bool Hidden() const override { return m_hidden; }

private:
    bool m_hidden = false;
};

#endif
