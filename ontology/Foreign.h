#ifndef SUPERTYPE_FOREIGN_H__
#define SUPERTYPE_FOREIGN_H__

#include "../core_defs.h"
#include <string>

// ---------------------------------------------------------------
// Foreign
// ---------------------------------------------------------------
//
// A RELATION TO A STRANGER. Refines Relational, exclusive with
// Symmetric: the far side does not speak ETCS, so nothing beyond this
// relation replays, hashes, or witnesses anything, and its way back is
// unwrapped by a reader this runtime does not own. Remote's
// counterpart -- Remote is a far node that IS ETCS.
//
// ONE PER RELATION, NOT PER LANGUAGE. A Foreign is the border post for
// one stranger. Two strangers speaking about one thing (a registrar
// and the public resolvers, both about one name) are two relations and
// two Foreigns; a protocol they share is machinery beneath them, not
// the relation. So the count of Foreigns is the count of foreign
// relations, and reading them is reading everything that can reach the
// graph from outside: what is not behind one is deterministic.
//
// ITS TARGET IS A STAND-IN, by definition. The stranger has no tags, so
// what a Foreign lands on is a local entity standing for the far thing
// (Targets names the surface it must carry; Directory for a name roll).
// The Foreign holds a REFERENCE to it (BindStandIn, StandIn), checked
// against Targets when bound, and Apply lands only on that entity: it
// brings the stand-in's view in line with the stranger. The stand-in
// is a projection inside our Environment -- its shape replays, what it
// says must be asked again -- which is why neither a Foreign nor its
// stand-in's foreign values can be Environmental.
//
//   Speaks       the protocol the stranger reads ("https", "dns", ...)
//   Exchange     one question out, its paired answer back; both strings
//                ETCS-side, their forms the leaf's to document; false =
//                no answer (unreachable, timed out, refused)
//   BindStandIn  the stand-in, by RID; refused unless it LandsOn
//   StandIn      the bound stand-in's RID, 0 when unbound
//
// SECRETS ARE FILLED AT THE EDGE. A question names a secret; the leaf
// resolves it after the trace has seen the line, writes it only onto
// the far wire, and redacts it in its own logs.
class Foreign_ : virtual public ETCS::Entity
{
public:
    virtual ~Foreign_() = default;

    virtual std::string Speaks() const = 0;
    virtual bool        Exchange(const std::string& question, std::string& answer,
                                 ETCS::SignalContext ctx) = 0;
    virtual bool        BindStandIn(ETCS::RID stand_in) = 0;
    virtual ETCS::RID   StandIn() const = 0;
};

#endif // SUPERTYPE_FOREIGN_H__
