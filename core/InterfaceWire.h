#ifndef INTERFACE_WIRE_H__
#define INTERFACE_WIRE_H__

#include "Buffer.h"
#include "SignalContext.h"

namespace ETCS
{

// ---------------------------------------------------------------------------
// InterfaceWire — the slots the RUNTIME itself calls, one per ontology family
// that means something to it.
//
// THE PROBLEM THIS NAMES. Core sits underneath the ontology: ontology.h
// includes core, so core can never name a family type. But some families are
// not merely descriptive -- the runtime has to CALL them. A wrapper stage has
// to be invoked on every packet; a released entity has to be told before its
// memory goes. Without something in core to call through, each of those
// arrives as its own ad-hoc arrangement: a virtual bolted onto Entity, a
// function pointer the ontology installs at load, a reinterpret_cast justified
// in a comment somewhere else.
//
// SO THE ARRANGEMENT IS THE THING, and it gets a name and one home. A wire is
// a pure-virtual interface declared HERE, in core, which an ontology family
// inherits as its FIRST NON-VIRTUAL BASE. The runtime then takes the interface
// pointer the family already registers under its bare name
// (ETCS_MAKE_INSTANCE) and reinterprets it as the wire -- which is exact,
// because under the Itanium C++ ABI a first non-virtual base subobject sits at
// offset 0, so the two pointers are bit-identical.
//
// THAT OFFSET-ZERO REQUIREMENT IS A REAL DEPENDENCY on each family's declared
// base order, not a style note. Getting it wrong produces a silently
// mis-adjusted pointer at runtime rather than a compile error, which is why it
// is stated here once, next to every wire, instead of being rediscovered at
// each use site.
//
// The rule for whether a family needs one is narrow: does the RUNTIME call it,
// as against other entities calling it? Surface, Drawable and Orderable are
// called by their peers and need no wire. Wrapper and Lifecycle are called by
// the transport and the arena, which are underneath the ontology and have
// nothing else to reach them through.
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// WireScope — a wrapper stage's own declaration of which transport
// strategies it applies to. Checked once per stage, at chain-resolution
// time (resolveWrapChain, DynamicLoader.h) — never per packet — since a
// wrapper's applicability to a given strategy is a fixed classification
// (Scope() is const), not something that varies call to call.
//
// NetworkOnly is the fit for anything whose whole purpose IS the wire
// (TLSWrapper, WebsocketWrapper) — LMAX never leaves the process, so
// wrapping it would be pure overhead with nothing to protect against.
// All is the fit for anything orthogonal to transport (access control,
// auditing) that should apply uniformly regardless of strategy.
// ---------------------------------------------------------------------------
enum class WireScope : uint8_t
{
    None        = 0,
    LMAX        = 1 << 0,
    Pipe        = 1 << 1,
    Socket      = 1 << 2,
    NetworkOnly = Pipe | Socket,
    All         = LMAX | Pipe | Socket,
};

// ---------------------------------------------------------------------------
// IWireWrapper — the transport's slot. Fulfilled by the Wrapper family
// (ontology/Wrapper.h), called by MirrorBuffer on every framed payload and by
// DynamicLoader when it resolves a chain.
//
// The original wire, and the one the pattern is named after: it was already
// doing exactly this -- declared in core, inherited first by Wrapper_,
// reinterpreted from the registered interface pointer -- before there was a
// word for it.
// ---------------------------------------------------------------------------
struct IWireWrapper
{
    virtual ~IWireWrapper() = default;
    virtual void Wrap(ETCS::MBuffer& io, ETCS::SignalContext ctx) = 0;
    virtual void Unwrap(ETCS::MBuffer& io, ETCS::SignalContext ctx) = 0;
    virtual void Close(ETCS::MBuffer& io, ETCS::SignalContext ctx) { (void)io; (void)ctx; }
    virtual ETCS::WireScope Scope() const = 0;
};

// ---------------------------------------------------------------------------
// IWireLifecycle — the arena's slot. Fulfilled by the Lifecycle family
// (ontology/Lifecycle.h), called by MemoryArena immediately before an entity's
// memory is reclaimed, on every path that reclaims it.
//
// WHY THE RUNTIME NEEDS THIS ONE. An entity can die two ways -- a script's
// Delete, or the closure that made it ending -- and the second path had no way
// to tell the type it was happening. The destructor is too late: by then the
// object is coming apart and the graph around it may be gone, so the things a
// release needs to do (unbind from something holding you, resolve a peer, stop
// a stream) are exactly the things it can no longer do.
//
// Release() returns whether THIS call did the work, so the two entry points can
// tell each other apart; the exactly-once guarantee is the family's, not the
// caller's (ontology/LifecycleBase.h).
// ---------------------------------------------------------------------------
struct IWireLifecycle
{
    virtual ~IWireLifecycle() = default;
    virtual bool Release() = 0;
    virtual bool Released() const = 0;
    virtual bool Established() const = 0;
};

// ---------------------------------------------------------------------------
// IWireThread — the scheduler's slot. DECLARED, NOT YET CLAIMED: there is no
// Threaded family and ThreadPool does not consult it. It is here because this
// header is where a wire is pre-declared before it is wired, and because the
// gap it names is one this codebase has already been bitten by three times.
//
// THE PATTERN IT COMPLETES. Each wire pairs a core subsystem with the one
// question it has to ask an entity that it has no other way to reach:
//
//     IWireWrapper    <-> MirrorBuffer   what do I do to this payload
//     IWireLifecycle  <-> MemoryArena    you are about to stop existing
//     IWireThread     <-> ThreadPool     what shape of work are you, and stop
//
// WHAT THE POOL ACTUALLY LACKS, which is the test of whether this is a real
// slot or a pleasing symmetry:
//
//   IT CANNOT TELL A PASSING BODY FROM A HELD ONE. A work function that
//   returns promptly and a stream producer that loops for a window's lifetime
//   are scheduled identically, so two long-lived producers on a four-worker
//   pool take half of it permanently and nothing anywhere says so. Every
//   comment in this codebase that warns about that warns a HUMAN, because
//   there is no field for it.
//
//   IT CANNOT ASK A BODY TO STOP. Shutdown raises a signal and hopes: the
//   closure drain waits a bounded five seconds, then joins; the pool sleeps a
//   101ms "cushion" and then joins. Both are guesses standing in for a request
//   that cannot be made. The self-join abort fixed in this same subsystem was
//   the sharp end of exactly that -- a thread that could not be told to stop,
//   being joined by itself.
//
// So the wire would carry two things and no more: what shape of work this is,
// and a cooperative halt that reports whether it took. Deliberately NOT a
// priority or an affinity -- those are policy, and policy belongs to whatever
// schedules, not to the thing being scheduled.
//
// Left unclaimed on purpose. Adding the interface is cheap and reversible;
// adding call sites inside ThreadPool changes when every stream body in the
// system is asked to stop, and that deserves its own pass rather than riding
// along with a memory-release change.
// ---------------------------------------------------------------------------
enum class WorkShape : uint8_t
{
    // Returns promptly; borrows a worker and gives it back. The pool can
    // oversubscribe these freely.
    Passing = 0,
    // Loops for as long as the entity lives -- a stream producer, a frame
    // clock, a poll loop. Occupies a worker rather than borrowing one, so a
    // pool that schedules more of these than it has workers deadlocks with no
    // error anywhere.
    Held    = 1,
};

struct IWireThread
{
    virtual ~IWireThread() = default;

    // What this entity's bodies do to a worker. Const and fixed: it is a
    // classification, like WireScope, not something that varies per call.
    virtual WorkShape Shape() const = 0;

    // Cooperative stop. Returns whether the request was taken -- so a drain
    // can report which bodies acknowledged and which it is about to wait on
    // blindly, instead of treating both the same and calling it a timeout.
    virtual bool Halt() = 0;
};

} // namespace ETCS

#endif
