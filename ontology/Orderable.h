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
    /*
 * SEARCH IS A PROPERTY OF THIS FAMILY, and of no other, because this family is
 * the only thing that says what a search would be BY.
 *
 * A list is bisectable only in the key it was sorted with, and the key
 * RIDList sorts with is the pointee's operator< -- this relation. A type that
 * claims nothing here leaves its list with no order and therefore no search;
 * one that claims this gets both from the same declaration. So searchability
 * is not an extra thing a type opts into, it is what having an order already
 * means.
 *
 * BY EXEMPLAR, so nothing is added to what a leaf must declare. A leaf still
 * states operator< and nothing else (OrderableBase derives the rest), and the
 * search compares against an instance of the leaf rather than against some
 * separate key type every implementor would have to invent and keep in step
 * with the comparison. The call surface is shaped at this boundary instead --
 * which is what the boundary is for.
 *
 * RETURNS A RANGE. == here is equivalence, not identity (see this file's own
 * note above), so many entities can share a standing and picking one of them
 * would be answering a different question than the one asked.
 *
 * QUALIFIED "Provider:Type", not a bare family, because the comparison belongs
 * to one concrete type. Identity searches fan out across providers -- a RID
 * means the same everywhere -- but an order does not: comparing against
 * another type's relation is not a narrower search, it is a question with no
 * answer. See ETCS::search_in_family.
 */
    template <typename Leaf>
    static size_t Search(const char* qualified_type, const Leaf& exemplar,
                         std::vector<ETCS::RID>& out)
    {
        return ETCS::search_in_family<Leaf>(qualified_type, exemplar, out);
    }

    /*
 * AND THE FORM THAT NEEDS NO TYPE AT ALL: name the exemplar by RID and the
 * list looks it up itself.
 *
 * This is the one most callers want, and it is what keeps the question from
 * being confined to the module that owns the relation. A loader, another
 * provider, or a script verb holds RIDs and Entity pointers, never leaf types
 * -- and "everything that stands where this one stands" is a perfectly good
 * question for any of them to ask. The comparison stays the type's; only the
 * asking is opened up.
 */
    static size_t Search(const char* qualified_type, ETCS::RID exemplar,
                         std::vector<ETCS::RID>& out)
    {
        return ETCS::search_in_family(qualified_type, exemplar, out);
    }

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
