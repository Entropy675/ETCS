#ifndef SUPERTYPE_LAYER_H__
#define SUPERTYPE_LAYER_H__


#include "../core_defs.h"
#include <vector>

// ---------------------------------------------------------------
// Layer
// ---------------------------------------------------------------
//
// ONE OF MANY VIEWS OF THE SAME SUBJECT, STANDING SOMEWHERE.
//
// A refinement of Orderable, and it could not be anything else:
// "where do I stand" is the question Orderable answers, and this
// family is what that answer becomes once the order has a HERE.
// A layer stack, a locale, a history -- each is a set of views of
// one subject, each view positioned by a value, and the only thing
// that distinguishes them is what the value MEASURES.
//
// WHAT IT ADDS TO ORDERABLE, and why it is a family rather than a
// convention: an order alone is total and unbounded -- every member
// stands somewhere relative to every other, and reading it means
// reading all of it. A layer's order has a frame of reference, so
// the useful question is not "give me the whole order" but "give me
// what is NEAR me", and the answer is bounded no matter how large
// the set. That is the increment, and it is the one that pays:
//
//   paint layers   value = depth        index = sorted vector
//   locale         value = position     index = kd-tree
//   history        value = time         index = interval tree
//
// All three are the same call, and -- this is the part worth stating
// plainly -- all three run on the SAME COMPARISON. Orderable's
// operator< is already everything a nearness check needs: a kd-tree's
// lookup is a walk in which each level compares along one axis, so the
// tree does not want a richer relation, it wants to choose which
// coordinate the existing one reads. The parameter the comparison
// answers on is selected BY the lookup, level by level; the leaf still
// declares exactly one operator< and nothing else, exactly as
// Orderable.h requires of everybody.
//
// So no metric, no distance, no second relation appears in this file.
// What varies between the rows above is only the INDEX, and RIDList
// already dispatches its ordered reads through function pointers
// (core/RIDList.h) -- a spatial list fills those in and Neighbourhood
// below does not change a line. The family states the question; the
// list answers it in whatever structure suits the coordinate, using
// the one comparison the leaf was always going to have to declare.
//
// The axis a walk is currently comparing on is therefore the LIST's
// state, not the layer's -- it belongs to the traversal that set it.
// That is not a new constraint: collect_ordered's sort already owns
// the walk it compares inside, and this is the same arrangement with
// more than one axis to choose from.
//
// WHY THIS IS WHERE ORDERABILITY IS CLAIMED for the drawing lineage.
// SurfaceBase used to compose OrderableBase directly, on the argument
// that "a layer stack IS an ordering and a surface that could not say
// what is above it would need that stored elsewhere". That argument is
// this family's, stated one level too low -- so the claim moved up here
// and Surface refines Layer. Every surface is therefore a layer, which
// is what it always meant; nothing downstream of Surface changed, and
// the single-claim rule (LayerBase.h) still holds, now one link higher.
//
// THE CENTRE IS NEVER A PARAMETER, and that is not a simplification --
// it is what the runtime already is. The environment is the scope shared
// locally: everything running here is, by construction, at the causal
// resolution that matters under one nameable space. So "here" is not
// something a caller supplies, it is where the call is being made from,
// and a nearness check is always centred on the locale of the runtime.
// Neighbourhood below therefore asks the list that HOLDS this entity --
// its parent's, never a global one -- which is the same relative
// ordering that already governs sub-windows: they stack against their
// siblings inside their parent, and there is no process-wide answer to
// "which window is on top" because there is no process-wide frame to
// ask it in.
//
// IT IS ALSO WHAT MAKES RESIDENCY EXPRESSIBLE, which is the reason to
// have it at all rather than to keep asking lists for ranges. A family
// whose members are only ever read in bounded neighbourhoods, centred on
// a scope that is already local, is a family whose storage is bounded by
// the neighbourhood rather than by the set: an arena that grows to the
// high-water mark of "how much is near at once" and reuses those
// same-size slots as the frame of reference moves. A layer window
// recycling rows as it scrolls and a world streaming chunks as a camera
// moves are that one pattern, and they are only one pattern because the
// query is one query asked from one place.

class Layer_ : virtual public ETCS::Entity
{
public:
    virtual ~Layer_() = default;

    /*
 * WHAT THIS IS A VIEW OF. Many layers, one subject.
 *
 * CONCRETE, AND ITSELF BY DEFAULT, which is not a placeholder: a layer that
 * is not a view of anything else is a view of itself, and that is the honest
 * answer for the overwhelming majority of them -- a polygon, a text label, a
 * window's root rectangle are each their own subject. Leaves that genuinely
 * are several views of one thing -- a mip chain, a document's per-layer
 * rasters, the same region at two levels of detail -- say so by overriding.
 *
 * Deliberately not dispatched, for the reason Drawable_::Order() and
 * ToParent/ToLocal are not: a dispatched method is one every leaf must write,
 * and this family sits above Surface, so making it an obligation would put a
 * new *Concrete on every drawable in the tree to restate a default.
 */
    virtual ETCS::RID Subject() { return this->getRID(); }

    /*
 * WHAT IS NEAR ME, bounded, in the holding list's own order.
 *
 * Concrete at the family level, exactly like Orderable_::Reorder() and for
 * the same reason: the mechanism is finding the list that holds me and asking
 * it a question, which is backend-independent and has no business being
 * reimplemented per leaf. Reorder() is the write half of that seam -- "my key
 * moved" -- and this is the read half.
 *
 * `before` and `after` are counts, and counts are what the comparison alone can
 * deliver: every index above answers "the n on either side of me" by walking
 * its own structure with operator<, and none of them needs a distance to do it.
 * A radius would need a metric this family deliberately does not have -- see
 * the header note on why the lookup, not the relation, is what varies.
 *
 * Returns this layer's own index within `out`, or SIZE_MAX when it is in no
 * list at all -- a root-level entity, or one already removed. Neither is an
 * error; nothing is holding it in an order, so nothing is near it.
 */
    size_t Neighbourhood(size_t before, size_t after, ::std::vector<ETCS::RID>& out)
    {
        out.clear();
        ETCS::Entity* p = getParent();
        if (!p) return static_cast<size_t>(-1);

        ::std::vector<ETCS::RID> ordered;
        if (!p->collectSiblingOrder(getRID(), ordered)) return static_cast<size_t>(-1);

        size_t self_at = static_cast<size_t>(-1);
        for (size_t i = 0; i < ordered.size(); ++i)
            if (ordered[i] == getRID()) { self_at = i; break; }
        if (self_at == static_cast<size_t>(-1)) return static_cast<size_t>(-1);

        const size_t lo = (self_at > before) ? (self_at - before) : 0;
        const size_t hi = ::std::min(ordered.size(), self_at + after + 1);
        out.assign(ordered.begin() + static_cast<long>(lo),
                   ordered.begin() + static_cast<long>(hi));
        return self_at - lo;
    }

    /*
 * Is anything holding me in an order at all? The cheap half of the question
 * above, for a caller deciding whether a neighbourhood exists before asking
 * for one.
 */
    bool Resident()
    {
        ETCS::Entity* p = getParent();
        if (!p) return false;
        ::std::vector<ETCS::RID> ordered;
        return p->collectSiblingOrder(getRID(), ordered);
    }
};

#endif
