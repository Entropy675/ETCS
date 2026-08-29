#ifndef SUPERTYPE_FILTER_H__
#define SUPERTYPE_FILTER_H__
#include "../core_defs.h"

// Filter_ — the predicate on a crossing.
// Accepts returns a KEY, not merely a yes/no, and that is the whole design.
// `io` arrives holding a descriptor of the thing trying to cross and leaves
// holding the equivalence class it belongs to; an empty key means declined.
// A bool-only filter can express "I'll take it" but cannot express "these two
// belong TOGETHER" -- and pairing is exactly what a shared board, a match, or
// any rendezvous needs. Returning a class makes two candidates presenting the
// same invite key resolve to the same receiver without the gate ever knowing
// what it is routing to.
//
// CONST, and load-bearingly so: a gate evaluates filters speculatively against
// subscribers that will NOT win the candidate, so a filter that mutates is a
// filter that lies about the crossings it declined. Reserving a seat, binding
// an identity, or anything else that changes state belongs to whatever
// actually receives the crossing afterwards, never here.
class Filter_ : virtual public ETCS::Entity
{
public:
    virtual ~Filter_() = default;
    // true  -> accepted; `io` carries this crossing's equivalence key.
    // false -> declined;  `io` is not read.
    virtual bool Accepts(ETCS::Buffer& io) const = 0;
};
#endif // SUPERTYPE_FILTER_H__
