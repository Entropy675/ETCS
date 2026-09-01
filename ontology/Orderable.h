#ifndef SUPERTYPE_ORDERABLE_H__
#define SUPERTYPE_ORDERABLE_H__


#include "../core_defs.h"

// ---------------------------------------------------------------
// Orderable
// ---------------------------------------------------------------
//
// A type whose instances stand in an order relative to each other,
// and which can say when that standing has changed.
//
// The most basic family in the ontology by design: it adds no
// domain meaning at all, only the claim that a comparison exists
// and the one call that re-establishes position after the answer
// moves. Anything that lives in a list somebody reads in order
// composes it -- drawables stacked back to front, a priority queue
// of jobs, entries in a sorted index.
//
// WHERE THE ORDER ACTUALLY GETS USED. RIDList<T> (core/RIDList.h)
// orders by the POINTEE's operator<, detected at compile time. T is
// always a pointer there (its own static_assert says so), and a
// pointer comparison is the built-in one -- you cannot overload
// operator< for two pointers at all, and `a < b` on two T would
// silently order by ADDRESS, deterministically, looking exactly
// like it worked. So the relation lives on the pointee, which for
// an entity's typed-children list IS the concrete leaf: the type
// declares its own operator<, and its own list is ordered by it.
//
// ONE OPERATOR IS REQUIRED, THE REST ARE DERIVED. A leaf declares
// operator< and nothing else; OrderableBase supplies >, <=, >=, ==
// and != from it, so there is exactly one place a type states what
// its order means and no way for five spellings of the same
// question to drift apart. The requirement is checked -- see
// OrderableBase.h.
//
// == IS EQUIVALENCE, NOT IDENTITY, and that distinction is sharper
// here than in ordinary C++ code. Derived from the ordering, a == b
// means "neither precedes the other" -- two DIFFERENT entities with
// the same standing compare equal, which is correct for a sort and
// wrong for anything asking "is this the same thing". Entity
// identity is the RID and only the RID. If a comparison is deciding
// whether two handles name one entity, it wants getRID(), not this.

class Orderable_ : virtual public ETCS::Entity
{
public:
    virtual ~Orderable_() = default;

    /*
 * Re-establish this entity's position in whatever ordered list holds
 * it. Concrete at the family level, like Pixels_'s buffer and
 * Resizable_'s listener set: the mechanism is finding the list and
 * marking its ordered view stale, which is backend-independent and
 * has no business being reimplemented per leaf.
 *
 * WHY IT IS EXPLICIT. Membership changes are already covered without
 * anyone calling anything: a push or a pull along a seam -- an insert
 * or an erase on the list itself -- marks the view stale where it
 * happens (RIDList::insert/remove), so entities arriving and leaving
 * never need a poke. What is NOT covered is the key moving while
 * membership stays put: the entity is still in the same list, the list
 * has not been touched, and the ordering it computed is now a lie. No
 * container can observe that, which is exactly why an ordered
 * container keyed on mutable state is a bug rather than an
 * optimisation. This call is the seam for that case, and the reason
 * the ordering is safe to cache at all.
 *
 * Cheap and idempotent: it marks stale, it does not sort. The next
 * ordered read pays for the rebuild, and a burst of reorders before
 * one read costs one rebuild.
 *
 * A root-level entity is in no parent's list and this is a no-op for
 * it -- not an error. Nothing is holding it in an order.
 */
    void Reorder()
    {
        if (ETCS::Entity* p = getParent()) p->reorderTypedChild(getRID());
    }
};

#endif
