#ifndef SUPERTYPE_TRACE_H__
#define SUPERTYPE_TRACE_H__


#include "../core_defs.h"
#include <cstdint>
#include <string>
#include <vector>

// ---------------------------------------------------------------
// Trace
// ---------------------------------------------------------------
//
// A STATE THAT IS ITS LINES. A chess board, a battle, a document: each
// starts as made and from then on moves only as lines are applied, in
// order -- and keeps them, numbered from that start. The lines it kept
// are its trace, and a copy that follows the trace from its first line
// is the same state. Record (ontology/Record.h) is lines kept apart from
// any state; a trace is the state's own.
//
// ONE COPY IS THE AUTHORITY, the others mirror it. The authority applies
// a line as it TAKES it -- from its own hand, or from a SEAT: a stream
// into it from another runtime (`mine.Emit() -> theirs.Take()`), whose
// every message after the first is that seat's line. Nothing in a line
// says who wrote it; the stream it came on does, and the order the
// authority read the streams in is the order. Judging is the
// MirrorBuffer's: the room publishes Take and Trace and nothing else, a
// seat can only speak for itself, and the leaf refuses whatever that seat
// may not say (ApplyConcrete).
//
// A SEAT IS AN IDENTITY. Over a link the stream carries what its link
// proved (SignalContext::peer), and the seat is that key's -- its lines
// are authored as the key's fingerprint, whatever the seat calls itself;
// the first message gives only a LABEL to show people, and a NONCE the
// seat's own mirror knows itself by. The same key coming back on a new
// stream takes its seat again (no second opening); the old stream's
// lines and its ending are then nobody's. A stream from this runtime
// carries no peer, and is named by its label, as it is this runtime's
// own code speaking. `seat <nonce> <label>` is the family's line for a
// seat sitting, before the leaf's opening: every copy learns the label,
// and the mirror whose nonce it is learns which seat is its own.
//
// A MIRROR follows (`theirs.Trace(0) -> mine.Follow()`): every line
// applied as its author, never decided -- its own lines leave on its
// seat stream and come back as the authority's. Kept, too, so a mirror
// can be followed in turn.
//
// A LINE IS APPLIED OR REFUSED, never repaired: "<seq> <author> <line>".
// The authority says where it stands every so often, as a line --
// `hash <at> <hash>`, the leaf's hash of what it holds, at a point (a
// sequence; a tick, for Lockstep) -- and a mirror reaching that line
// hashes itself: one that differs is REPORTED (DiffersConcrete), not
// negotiated. `hash` and `seat` are the family's words; a leaf's lines
// never use them.
//
// A TRACE IS A SESSION'S. Every stream takes the GENERATION when it
// starts, and ends once the copy has begun another session rather than
// feed an old trace into the new state.
//
//   Follow      one line of the authority's trace, applied here; false
//               once the stream's generation is not this copy's
//   Lines       this copy's lines from `from`, as many as are kept and
//               waiting briefly for one; false once stale
//   Sit/Take/Rise   a seat's stream opening (a token for it, 0 if
//               refused), its line, its ending
//   NextEmit    a mirror's next message for its seat, or nothing yet
//   Ended       the trace this mirror followed ended under it
//   Generation  the session's number, for a stream starting now
//   Owner       whose trace this is (the authority's name)
//   Snapshot    "<at> <hash>", the latest
class Trace_ : virtual public ETCS::Entity
{
public:
    virtual ~Trace_() = default;

    virtual bool        Follow(uint64_t gen, const std::string& msg) = 0;
    virtual bool        Lines(uint64_t gen, uint64_t from, std::vector<std::string>& out) = 0;
    virtual uint64_t    Sit(uint64_t gen, const std::string& seat, const std::string& label, const std::string& nonce) = 0;
    virtual bool        Take(uint64_t gen, const std::string& seat, uint64_t token, const std::string& line) = 0;
    virtual void        Rise(uint64_t gen, const std::string& seat, uint64_t token) = 0;
    virtual bool        NextEmit(uint64_t gen, std::string& out, bool& stale) = 0;
    virtual void        Ended(uint64_t gen) = 0;
    virtual uint64_t    Generation() = 0;
    virtual std::string Owner() = 0;
    virtual std::string Snapshot() = 0;
};

#endif // SUPERTYPE_TRACE_H__
