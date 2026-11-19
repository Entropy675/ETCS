#ifndef SUPERTYPE_PRESENT_H__
#define SUPERTYPE_PRESENT_H__


#include "../core_defs.h"
#include <cstdint>
#include <string>

// ---------------------------------------------------------------
// Present
// ---------------------------------------------------------------
//
// WHO IS HERE IS WHOSE STREAM IS OPEN. A participant is present for as
// long as a stream between it and this entity lasts: it ARRIVES when the
// stream opens and DEPARTS when it ends, however it ends -- a page
// closed, a link dropped, a session left. Nobody says they are here and
// nobody is polled; the edge is the saying. Beside Trace, not in it: a
// Trace's streams are where a session's edges are (a seat's into the
// authority, the trace a mirror follows out of it -- TraceBase.h says
// which), but anything with streams has edges.
//
// THE HALL is the same thing one level up: the owner of a session says
// "<name> <kind> <id> <...>" in the site's Directory (ontology/
// Directory.h), whose entries last exactly as long as the link they
// came over -- an edge again -- kept current, and withdrawn when it goes.
//
//   Arrive    `who`'s stream opened: the presence's generation, or 0 if
//             `who` is here already (a second stream is not a second
//             arrival)
//   Depart    that stream ended; ignored once the generation is another's
//   Here      who is, one name a line
//   Announce  the hall's line on the owner, said again if it changed
class Present_ : virtual public ETCS::Entity
{
public:
    virtual ~Present_() = default;

    virtual uint64_t    Arrive(const std::string& who) = 0;
    virtual void        Depart(uint64_t gen, const std::string& who) = 0;
    virtual std::string Here() = 0;
    virtual void        Announce() = 0;
};

#endif // SUPERTYPE_PRESENT_H__
