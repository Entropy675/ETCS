#ifndef SUPERTYPE_LOCKSTEP_H__
#define SUPERTYPE_LOCKSTEP_H__


#include "../core_defs.h"
#include <cstdint>
#include <string>

// ---------------------------------------------------------------
// Lockstep
// ---------------------------------------------------------------
//
// A TRACE WHOSE TIME IS LINES: mirroring a world that moves on its own.
// Refines Trace. Karts rolling and balls in flight are still only the
// trace, if the steps are in it too: `tick <n>` is a line, every copy
// steps to tick n when it reaches it, and where an input falls between
// two steps is the trace's to say and the same everywhere. A mirror
// following the authority's trace is in lockstep with it, a trip behind;
// nobody negotiates a frame.
//
// THE AUTHORITY'S CLOCK IS THE WORLD'S. Its frame edge (Animated's
// measured interval) decides how many whole ticks are due -- a few at
// most at once, so a stalled frame is time lost rather than a lurch --
// and its tick lines are kept in small batches, and always before any
// other line it takes, so that line lands at the tick it was applied at.
//
// A STEP IS THE LEAF'S AND MUST BE ITS ROWS' ALONE: integers (Fixed),
// the trace's dice, nothing measured on this machine -- or two copies
// fed the same lines drift (etcs_causal_constraints.md §2). Every few
// ticks the authority says where it stands (Trace's `hash` line, at the
// tick) and every mirror checks itself there, so a copy that drifted is
// seen. `tick` is the family's word, like `hash`: no seat may say it.
//
//   At    the tick this copy has stepped to
//   Run   the authority's clock by hand: ticks stepped and kept, what a
//         headless run or a test drives a world with
class Lockstep_ : virtual public ETCS::Entity
{
public:
    virtual ~Lockstep_() = default;

    virtual uint64_t At() = 0;
    virtual void     Run(uint32_t ticks) = 0;
};

#endif // SUPERTYPE_LOCKSTEP_H__
