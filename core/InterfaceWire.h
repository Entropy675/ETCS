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
// IWireThread — the scheduler's slot. CLAIMED by the Threaded family
// (ontology/Threaded.h), and called by the arena's reclaim funnel, which halts
// an entity's bodies before it releases the state those bodies are using. The
// gap it names stopped being an argument and became a measured crash; see the
// family header for the reproduction that claimed it.
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
//   pool take half of it permanently and nothing anywhere says so.
//
//   IT CANNOT ASK A BODY TO STOP. Shutdown raises a signal and hopes: the
//   closure drain waits a bounded five seconds, then joins; the pool sleeps a
//   101ms "cushion" and then joins. Both are guesses standing in for a request
//   that cannot be made. The self-join abort fixed in this same subsystem was
//   the sharp end of exactly that -- a thread that could not be told to stop,
//   being joined by itself.
//
// So the wire carries what shape of work this is, and a cooperative halt --
// the request and its readback, the same request/readback pair IWireLifecycle
// carries. Deliberately NOT a priority or an affinity: those are policy, and
// policy belongs to whatever schedules, not to the thing being scheduled.
//
// The POOL-side call sites are still outstanding: the arena's reclaim funnel
// halts a body before releasing what it uses, but ThreadPool itself does not
// yet ask. Changing when every stream body in the system is told to stop
// deserves its own pass.
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

    // The readback, and what a running body polls. On the wire rather than the
    // family for the reason IWireLifecycle carries Released().
    virtual bool Halted() const = 0;

    /*
     * THE BODY REPORTING THAT IT ACTUALLY LEFT, which is a different fact from
     * having been asked to.
     *
     * Halt/Halted was carrying both and could only mean one: an entity asked to
     * stop and an entity whose loop had already gone were the same answer, and
     * the tag written for it said "halted" while recording a REQUEST. Halting
     * is the transition, stopped is the destination.
     *
     * SAME SHAPE AS Delete/Release on IWireLifecycle -- Halt is a request from
     * outside, Stop is a notification from the body, and only the body can make
     * it because nothing else knows when a loop has ended. So Stop is called BY
     * a body on its way out, never commanded at one.
     *
     * ON THE WIRE, beside the pair it completes. The drain is the caller that
     * needs it: shutdown_detached_executors and etcs_retire_entity ask bodies
     * to stop from the LOADER side, across the boundary, and "did it actually
     * go" is the question they have to answer before reclaiming anything the
     * body was touching. A readback whose only useful caller sits on the far
     * side of the wire belongs on the wire, for the reason Halted() already
     * gives one line up.
     */
    virtual bool Stop() = 0;
    virtual bool Stopped() const = 0;

    /*
     * Start `script` as a child of this entity. Returns the child's RID, or 0
     * if refused.
     *
     * ON THE WIRE BECAUSE CORE CANNOT DO IT. The dependency runs ontology ->
     * core and never back, so CommandExecutor's detach can talk to a Thread but
     * cannot allocate one -- and a detached script IS a Thread. This is the one
     * capability core structurally lacks and must delegate, which is exactly
     * what a wire is for.
     *
     * It does not contradict this wire's "two things and no more" boundary. That
     * boundary was drawn against POLICY -- priority, affinity, who runs next --
     * and this is not policy. It is "make another of you", answered by the only
     * thing that knows how.
     *
     * A Threaded that is not a Thread returns 0, the way Resizable_::ResizeTo
     * returns false: having a body to stop does not make you an actor, and
     * refusing is the honest answer rather than an absent method. So a caller
     * checks the return instead of first asking what kind of thing it holds.
     */
    virtual uint64_t Detach(const ETCS::Buffer& script) = 0;

    /*
     * This thread's own signal authority, or null if it has none.
     *
     * On the wire for the same reason Detach is: core has to reach it and
     * cannot own it. CommandExecutor's detach has to parent a child job's
     * signals somewhere, and under the entity model that somewhere is the child
     * Thread itself -- but SignalContext is a core type held by an ontology
     * family, so the pointer crosses out through here.
     *
     * BY VALUE, like every other SignalContext in the runtime -- WorkFunc and
     * StreamFunc already take one that way across the DSO boundary. A copy also
     * cannot dangle into an entity's shell after a reclaim, which a pointer
     * could.
     *
     * A Threaded that is not a Thread returns a default-constructed context
     * with no local authority, exactly as Detach returns 0: owning a body to
     * stop does not make you an authority over signals.
     */
    virtual SignalContext Signals() = 0;
};

// ---------------------------------------------------------------------------
// IWireObservable — the observation slot. Fulfilled by the Observable
// family (ontology/Observable.h).
//
// The general causal structure: something records the state of another and
// wants to know when that changes. Nothing about pixels, cameras or hashes --
// those are USERS of it. The merkle hash in particular is its own independently
// updated structure that happens to fit this shape exactly; it observes and
// marks through here rather than living on this surface.
//
// Dirty is PER OBSERVER, which is the whole reason this is a wire and not a
// bool. Two cameras viewing one scene each need telling once; a single
// read-and-clear flag lets whichever looks first consume the other's
// invalidation.
// ---------------------------------------------------------------------------
// The observer's own end of one edge. Defined in core because it crosses the
// wire: two words, trivially copyable, fixed layout -- the same reason TBuffer
// exists rather than ::std::string.
//
// slot is a CACHE of the RID->position inversion, never a replacement for it.
// The RID stays the identity; the slot makes reading the edge a load and a
// compare instead of a search, and a handle whose slot has been vacated and
// reused fails its verify rather than silently addressing a stranger.
struct ObserverEdge
{
    uint64_t observer_rid = 0;
    uint16_t slot         = 0;
    bool valid() const { return observer_rid != 0; }
};

struct IWireObservable
{
    virtual ~IWireObservable() = default;

    // Register something that watches this entity, and hand back its end of the
    // edge. An observer that is an ancestor does not need this -- see
    // MarkObserved.
    virtual ObserverEdge Observe(uint64_t observer_rid) = 0;
    virtual void Unobserve(uint64_t observer_rid) = 0;

    /*
     * This entity changed. Marks every registered observer, then bubbles to the
     * nearest Observable ancestor, which does the same -- so a change reaches
     * the root by composition rather than by anyone walking the whole tree.
     *
     * ORIGIN IS WHO CAUSED IT, and it is carried unchanged through every hop.
     * The observer whose RID equals it is skipped: you never need telling about
     * a change you made yourself.
     *
     * That one bit of identity is what the bare flag could not express, and its
     * absence was a lost update rather than an inefficiency. A node that writes
     * its own cache marks itself along with everyone else, so it had to clear
     * its own bit afterwards -- and that clear could not tell its own mark from
     * one a concurrent writer had left during the write, so it swallowed it.
     * Demonstrated in the ontology tester. Skipping at the source removes the
     * clear, and with it the race.
     */
    virtual void MarkObserved(uint64_t origin_rid) = 0;

    /*
     * Mark my own observers and STOP -- no walk in either direction.
     *
     * The primitive the DOWNWARD edge is built from. MarkObserved says "what I
     * contain changed", which is news to whoever holds a merged copy of me, so
     * it travels up. The opposite statement -- "the frame I hand my children
     * changed" -- is news to what is below me, and the two cannot be the same
     * call: a compositor recomposing its own pixels is not telling its children
     * their coordinates moved.
     *
     * On the wire because reaching a child's Observable half is a cross-family
     * hop like any other; the walk that uses it is family-level
     * (ObservableBase::MarkObservedBelow).
     */
    virtual void MarkObservedLocal(uint64_t origin_rid) = 0;

    // Read-and-clear through a held edge: index, verify, test-and-clear. No
    // search, and a stale handle is detected rather than aliased.
    virtual bool TakeObserved(const ObserverEdge& edge) = 0;

    // Has this changed since `observer` last looked? Read-and-clear, and only
    // for that observer.
    virtual bool TakeObserved(uint64_t observer_rid) = 0;
};

} // namespace ETCS

#endif
