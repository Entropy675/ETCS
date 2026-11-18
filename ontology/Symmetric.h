#ifndef SUPERTYPE_SYMMETRIC_H__
#define SUPERTYPE_SYMMETRIC_H__

#include "../core_defs.h"
#include <string>

// ---------------------------------------------------------------
// Symmetric
// ---------------------------------------------------------------
//
// A RELATION THAT PROVES ITS WAY BACK. Refines Relational: a way back
// with no way there is not a road, so only a relation can be symmetric.
// Invert undoes Apply on the same target EXACTLY, within the algebra
// the relation names -- not near: a relation that can only approach its
// inverse is Relational twice, not Symmetric. Symmetry is not given; it
// is sought for what it reveals, and claimed per relation.
//
// The inverse may be a method over values the relation keeps (a stored
// matrix and its stored inverse) or a function computed under a bespoke
// algebra; the family does not care which. It cares that Apply then
// Invert leaves the target's related state as it was.
//
// Exclusive with Foreign: a Foreign relation's way back belongs to a
// stranger, so no leaf can prove it.
//
//   Invert    Apply's exact inverse on a target that lands
//   Algebra   where the exactness holds ("Z", "Q32.32", "GF(2)", ...)
class Symmetric_ : virtual public ETCS::Entity
{
public:
    virtual ~Symmetric_() = default;

    virtual bool        Invert(ETCS::Entity& target, ETCS::SignalContext ctx) = 0;
    virtual std::string Algebra() const = 0;
};

#endif // SUPERTYPE_SYMMETRIC_H__
