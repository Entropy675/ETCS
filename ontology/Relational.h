#ifndef SUPERTYPE_RELATIONAL_H__
#define SUPERTYPE_RELATIONAL_H__

#include "../core_defs.h"
#include <string>

// ---------------------------------------------------------------
// Relational
// ---------------------------------------------------------------
//
// SOMETHING THAT APPLIES A TRANSFORMATION TO SOMETHING ELSE. A law,
// not a place: what a cold room does to what is in it, what a stage
// does to what crosses it, what a stranger's protocol does to a
// question. The ordinary capability is transformation, not symmetric
// transformation -- a way back is a further claim (Symmetric), proved
// per relation, because whether an inverse exists depends on what the
// relation's algebra can represent: a page that turns two pens into
// three has no way back in whole sheep, and in Fixed (Q32.32) a third
// has no exact spelling at all.
//
// A RELATION NAMES WHAT IT LANDS ON: a tag-set (Targets), and it
// applies only to an entity carrying every tag in it (LandsOn, which
// the family answers). That is the chain constraint grown past
// numbers: a relation feeds the next only where its target is what the
// next is. The ontology sees relations in causal structure, not
// instances.
//
// NOT A PLACE. Wrapper is the wire's slot and a container is an
// Environment; either may CARRY a relation (a stage that frames, a
// pocket with no gravity), and a leaf claims both when it does. This
// family is only the law.
//
// THE FAMILY SAYS THREE THINGS.
//
//   Targets   the tag-set this relation lands on, space-separated
//   LandsOn   does this entity carry every target tag (family answers)
//   Apply     the transformation, on a target that lands; false if it
//             doesn't land or the relation refuses
class Relational_ : virtual public ETCS::Entity
{
public:
    virtual ~Relational_() = default;

    virtual std::string Targets() const = 0;
    virtual bool        LandsOn(const ETCS::Entity& target) const = 0;
    virtual bool        Apply(ETCS::Entity& target, ETCS::SignalContext ctx) = 0;
};

#endif // SUPERTYPE_RELATIONAL_H__
