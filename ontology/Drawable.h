#ifndef SUPERTYPE_DRAWABLE_H__
#define SUPERTYPE_DRAWABLE_H__


#include "../core_defs.h"
#include "Surface.h"
#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

// ---------------------------------------------------------------
// Drawable
// ---------------------------------------------------------------
//
// A Surface that is also a NODE: it occupies addressable space
// inside a parent, it can be realised onto some other surface, and
// the things nested inside it are Drawables in exactly the same
// sense, addressing space on IT the way it addresses space on its
// own parent.
//
// This is a refinement of Surface, not a sibling of it. A Surface
// is a region of addressable pixels; a Drawable is such a region
// that additionally knows how to put itself somewhere and what is
// nested within it. Every Drawable is a Surface -- the obligation
// is cumulative, and structurally so: DrawableBase composes
// SurfaceBase, so a type cannot claim this family without also
// satisfying Clear/DrawRect/Blit and being registered under
// "Surface" for anything reaching it generically.
//
// WHY THE REFINEMENT LIVES IN THE BASE AND NOT IN THIS HEADER.
// The obvious spelling -- `class Drawable_ : virtual public
// Surface_` -- does not survive contact with the supertype-base
// macro, which inherits its interface NON-virtually. A Drawable
// base composing a Surface base would then reach Surface_ twice,
// once virtually through this interface and once directly through
// the composed base, and every inherited call would be ambiguous.
// So the rule this family and its two leaves follow, uniformly, is:
//
//     an interface declares only its own INCREMENT,
//     a Base carries the LINEAGE.
//
// Nothing depends on the implicit Drawable_* -> Surface_* pointer
// conversion that the other spelling would have bought, because
// nothing in this runtime reaches across families by static_cast:
// it goes through getInterfacePointer("Surface") /
// resolve_in_family<Surface_>, which returns the correctly-adjusted
// base subobject whatever the inheritance shape underneath. The
// conversion was never the mechanism.
//
// It also buys the exclusivity rule as a COMPILE ERROR rather than
// a convention. Drawable2D and Drawable3D are siblings, and
// etcs_ontology_constraint_sets.md says a leaf may hold at most one
// member of a family. Because both of their bases compose this one
// non-virtually, a type inheriting both gets two Drawable subobjects
// and every call through them is ambiguous. The incoherent claim
// stops being something a reviewer has to catch.
//
// WHAT IS DELIBERATELY NOT HERE. Coordinates. A 2D drawable
// addresses (x, y) and a 3D one addresses (x, y, z); there is no
// honest way to state "where are you in your parent" once for both,
// and a base class that answered in one dimensionality would make
// the other lie. So the shared constraint is the one thing that IS
// dimension-free -- that a drawable can be REALISED onto a surface
// -- and the addressing lives one level down, in each leaf, where
// its dimension is known. That split is also the seam the camera
// projection crosses: a Drawable3D realises itself by projecting to
// a Drawable2D first (see Drawable3D.h), and everything downstream
// of that point is 2D code that never learns a camera was involved.

class Drawable_ : virtual public ETCS::Entity
{
public:
    virtual ~Drawable_() = default;

    // Realise this drawable, and everything nested inside it, onto dst.
    //
    // Takes a Surface_ rather than a Drawable_ because the destination
    // needs no tree of its own -- a window's swapchain is a legitimate
    // place to land and is not a Drawable. The source side is where the
    // nesting lives; the destination is just somewhere pixels go.
    virtual void DrawInto(Surface_* dst) = 0;

    /*
 * Z ORDER, stated by the node rather than inferred from the tree.
 *
 * The first draft of drawChildren claimed insertion order was painter's
 * order, on the strength of getTypedChildren reporting "in addTag call
 * order". It is not, and the test caught it: that ordering is over
 * distinct tag TYPES, and the RIDs within one type come out of an
 * unordered map. A parent holding a BoxNode and a TriangleNode paints
 * them grouped by type, and two boxes paint in whatever order the hash
 * happened to produce -- stable within a run, meaningless as a claim
 * about what is on top of what.
 *
 * So the order is a property of the node. Lower draws first, which means
 * higher ends up on top -- and Pick (Drawable2D.h) walks the identical
 * ordering backwards, which is what keeps "what you see" and "what you
 * clicked" the same answer. Ties keep registry order, which is
 * deterministic within a run but not meaningful: two siblings that must
 * stack a particular way say so.
 *
 * Concrete, and 0 by default: the overwhelming majority of nodes do not
 * overlap their siblings and should not have to answer this. Not routed
 * through the dispatch macro for the same reason ToParent/ToLocal are
 * not -- a dispatched method is one every leaf must write.
 */
    virtual int32_t Order() { return 0; }

    /*
 * Public rather than protected, because its second caller is not a
 * subclass: Drawable2D_::Pick needs the same ordered list read
 * backwards, and Drawable2D_ does not inherit this interface -- the
 * lineage lives in the Bases (see the header comment above). So Pick
 * asks for its own Drawable half by family, exactly the way foreign
 * code would, and calls this on it. Reaching a sibling family through
 * getInterfacePointer is the rule here even when the sibling is
 * yourself.
 */
    void collectDrawableChildren(std::vector<Drawable_*>& out)
    {
        std::vector<std::pair<ETCS::Buffer, ETCS::RID>> kids;
        getOrderedTypedChildren(kids);
        out.reserve(kids.size());
        for (const auto& entry : kids)
        {
            ETCS::Entity* child = getTypedChild(entry.first, entry.second);
            if (!child) continue;
            void* iface = child->getInterfacePointer(ETCS::Buffer("Drawable"));
            if (!iface) continue;
            out.push_back(static_cast<Drawable_*>(iface));
        }
        std::stable_sort(out.begin(), out.end(),
                         [](Drawable_* a, Drawable_* b) { return a->Order() < b->Order(); });
    }

protected:
    /*
 * The recursion, written once. A leaf's DrawInto draws ITSELF and then
 * calls this, rather than each leaf re-deriving how to walk its own
 * children -- the same reason Pixels_ owns its buffer and Resizable_
 * owns its listener list at the family level instead of per backend.
 *
 * Painter's order by Order(), ascending -- see it above for why the
 * child registry's own enumeration order could not supply this.
 *
 * TWO ORDERINGS, one per question, and they compose rather than
 * compete. getOrderedTypedChildren has each tag's own RIDList already
 * sorted by the LEAF's operator< (which Orderable requires of every
 * drawable, OrderableBase.h) -- a real relation, but only within one
 * concrete type, because two unrelated leaf types have no comparison
 * between them and requiring one would be requiring a lie. The merge
 * across tags is the cross-type question, and it needs a scalar every
 * member can answer whatever it is: Order(). stable_sort, so the
 * within-tag order the lists already established survives wherever
 * Order() ties.
 *
 * A child that is not a Drawable is skipped rather than treated as an
 * error. Entities nest for many reasons -- a Surface's Instance, a
 * window's input source -- and "is part of the picture" is precisely
 * the question getInterfacePointer answers here. A tree with mixed
 * children draws the drawable ones and ignores the rest, which is the
 * behaviour that lets a Drawable own non-visual machinery without
 * having to hide it somewhere else.
 *
 * Resolved per call, not cached. Children come and go (Delete, a
 * reparent), and a cached vector of Drawable_* is a dangling pointer
 * waiting for the first script that removes a layer mid-run.
 */
    void drawChildren(Surface_* dst)
    {
        if (!dst) return;
        std::vector<Drawable_*> ordered;
        collectDrawableChildren(ordered);
        for (Drawable_* child : ordered) child->DrawInto(dst);
    }
};

#endif
