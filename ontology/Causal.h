#ifndef SUPERTYPE_CAUSAL_H__
#define SUPERTYPE_CAUSAL_H__

#include "../core_defs.h"
#include "OrderVector.h"
#include "Plane.h"

#include <atomic>
#include <mutex>
#include <thread>
#include <vector>

/*
 * ── CAUSAL ───────────────────────────────────────────────────────────────
 *
 * A THING THAT HAS AN ORDER VECTOR: a place, an energy with a direction, a
 * reach, a facing -- and so a history, since every interaction changes rows the
 * hash runs over (ontology/OrderVector.h). The physics as a trait, claimed like
 * Animated or Drawable.
 *
 * NOT A DRAWING RELATION. The rows are the constraint; a picture is a
 * PROJECTION (a camera on a scene, a mesh on a device) a runtime may decline to
 * make. With no camera, a world runs every interaction it would on screen and
 * lands on the same rows. What is drawn derives from what is here.
 *
 * ONE RELATION: ADJACENCY, AND ONE EVENT: A CROSSING. Two Causal entities are
 * adjacent when a crossing can pass between them; a crossing is an OrderVector
 * (energy, at a place, from an identity, with or without a direction) that
 * leaves one and lands in the other, exactly. Two ways to be adjacent:
 *
 *   standing    a member and its container (its Causal parent). What a member
 *               sheds as entropy crosses up into its container's rows, always;
 *               containment is this adjacency and nothing more, and a container
 *               emits upward in turn. The root's open boundary counts what
 *               crosses it out (CausalBase::EmittedOut).
 *
 *   discovered  two members of one container whose reaches touch
 *               (OrderVector::GapTo <= 0): a contact. Each hands the other the
 *               part of its ordered energy headed along the line between them
 *               (OrderVector::CrossToward) and absorbs what it is handed. Decided
 *               by the rows alone, in the container's frame -- which is what
 *               makes siblings comparable.
 *
 * A crossing carries its own uncertainty (OrderVector::derive_uncertainty); the
 * emitter's clock ticks for it, and a draw nested in it reads it.
 *
 * THE FAMILY SAYS FIVE THINGS.
 *
 *   Order4    the record: where and how this thing is. The one authoritative
 *             statement; a copy is wrong, and a read is of ONE state only under
 *             TreeMutex() -- outside it, it may straddle a step.
 *   Interact  one interaction over a stated span: settle the entropy owed for
 *             the interval just ended, advance the motion for the next, then the
 *             members, then their contacts -- in that order (CausalBase says
 *             why), here and everything Causal below.
 *   Impulse   work done on it from outside -- the only way |K| rises.
 *   Absorb    a crossing that landed here from an adjacent entity. Heat as
 *             heat, an ordered part as a push.
 *   CausalHash / CausalTicks   the history: the rows of this tree as one
 *             number, and this entity's own clock.
 *
 * WHO CALLS Interact IS NOT STATED HERE, on purpose. A camera calls it with
 * the intervals since it last looked (being observed is an interaction); a
 * driver calls it with a fixed span a thousand times, reading no clock. Same
 * arithmetic, different spans. What is KEPT is the rows: the value behind
 * "Causal" on the tag surface (Entity::bindValue), so a store keeps them and a
 * resume puts them back -- the family keeps no record of its own.
 *
 * THE PROTOCOL BELOW THE LINE is the base's, not the family's vocabulary: one
 * lock per tree taken at every entry, and how a container holding it reaches
 * its members and their crossings. On the interface only because the hop
 * crosses providers through this vtable. The invariant is that every write to
 * any rows in a tree is in one total order; the mutex is one way to have it.
 */
/*
 * A SOLID: a body that is more than a reach. Off by default -- a body is a set
 * of spheres that exchanges energy along the line between centres and passes
 * through what it touches. A solid (CausalBase::SetSolid) meets another solid
 * by shape: pushed out, normal motion bounced (restitution), tangential motion
 * rubbed (friction); an anchored one never moves. Shapes are plane sets
 * (Plane.h): a sphere (the reach), a box (six planes about the centre, row 3
 * facing it), or a half-space (one plane through the centre facing row 3's up;
 * solid below). Only solids meet solids.
 */
struct CausalStream;   // a tree's own order, below the family

// The speed of light where no space states one: 2^16 units a second, met only
// on purpose. A power of two, so a multiple of it is a shift.
inline Fixed DefaultLightSpeed() { return Fixed::FromInt(int64_t(1) << 16); }

struct CausalSolid
{
    enum Shape : uint8_t { Off = 0, Sphere = 1, Box = 2, HalfSpace = 3 };
    Shape shape = Off;
    Fixed hx, hy, hz;              // a box's half extents (the sphere's radius is the reach)
    Fixed restitution, friction;   // [0,1]: the normal motion kept; the share of the load that rubs
    bool  anchored = false;        // never moves: holds what lands on it (CausalBase::SetAnchored)
};

class Causal_ : virtual public ETCS::Entity
{
public:
    virtual ~Causal_() = default;

    virtual const OrderVector& Order4() const = 0;

    // One interaction over `dt` seconds, here and below.
    virtual void Interact(Fixed dt) = 0;

    // Energy in, along a direction.
    virtual void Impulse(Fixed dx, Fixed dy, Fixed dz, Fixed joules) = 0;

    // A crossing from an adjacent entity, arriving.
    virtual void Absorb(const OrderVector& crossing) = 0;

    // This entity's own clock: crossings committed ("emission is the clock"), not visits.
    virtual uint64_t CausalTicks() const = 0;

    // The rows of this entity and everything Causal under it, as one number --
    // what two runtimes compare. Members compose in canonical order (by kind,
    // then creation order between twins), the order the step walks them.
    virtual uint64_t CausalHash() = 0;

    // Was anything in this subtree moving when the last interaction ended? A
    // member in flight is motion the container's rows don't show. Lock-free.
    virtual bool Moving() const = 0;

    // A write to the rows from outside an interaction -- a teleport, a pose, a
    // push -- said once it is made: the change climbs the tree as every other
    // state change does (Entity::markStateChange). What keeps an answer until
    // its subtree changes reads that climb, so a write not said is a write it
    // does not see (CausalBase, A STILL SUBTREE).
    virtual void RowsWritten() = 0;

    // ── the environment ─────────────────────────────────────────────────
    //
    // EVERY CAUSAL THING IS AN ENVIRONMENT for what fits in it, and fit -- not
    // who made it -- decides what holds a thing (universe, earth, person, pen in
    // a pocket). A member stays while its reach lies inside its container's Space
    // (zero: solid, holds nothing). After the contacts each interaction, a member
    // that no longer fits moves up a level, and one that fits inside a sibling's
    // space moves into the smallest such sibling (CausalBase::fitLocked); an
    // anchored member stays put. A move is a reparent (Entity::moveTo) with the
    // position restated: frames are translations, positions relative.

    // The container, or null at the open boundary.
    virtual Causal_* Environment() = 0;
    // The origin of this entity's frame in the topmost frame: every container's
    // position, summed. A member's world place is Basis + its own position.
    virtual void Basis(Fixed& x, Fixed& y, Fixed& z) = 0;
    // The interior this provides, as a radius about its own position.
    virtual Fixed Space() const = 0;

    // ── the space's parameters ──────────────────────────────────────────
    //
    // A CONTAINER STATES WHAT ITS SPACE IS LIKE: the radius (what fits), the
    // field (what everything inside falls along). Cumulative down the tree by
    // DEFAULT -- unstated, the field above passes on, so the earth's gravity
    // reaches the pen -- but a container may state its own, zero included, and
    // provide more room than it takes up. A pocket dimension is a parameter.
    //
    // The field this one's members feel, and the container stating it (null:
    // none above). Its work is counted there.
    virtual Causal_* FieldUnder(Fixed& gx, Fixed& gy, Fixed& gz) = 0;
    virtual void CountFieldWorkUnder(Fixed joules) = 0;
    // The speed of light here: the fastest anything inside moves -- stated here
    // (SetLightSpeed), else the container's, else DefaultLightSpeed.
    virtual Fixed LightUnder() = 0;
    // What this body is when it touches another solid (CausalSolid).
    virtual CausalSolid Solid() = 0;
    // The closeness check: this environment's members whose reach comes
    // within `r` of a point of its frame, in canonical order.
    virtual void Near(Fixed x, Fixed y, Fixed z, Fixed r, ::std::vector<Causal_*>& out) = 0;
    // The members of this one's environment whose reach touches this one's
    // (the contact gate), in canonical order.
    virtual void Adjacent(::std::vector<Causal_*>& out) = 0;
    // Take `member` in from wherever it is: a move, its position restated here.
    // False when it can't (not movable, a module apart, or this is inside it).
    virtual bool Contain(Causal_* member) = 0;

    // ── the protocol (see above) ────────────────────────────────────────
    virtual ::std::recursive_mutex& TreeMutex() = 0;
    // The tree's stream (CausalStream): the topmost entity's, as the lock is.
    virtual CausalStream& TreeStream() = 0;
    // Interact for a caller holding the lock: entropy over `commit_dt`, motion
    // over `step_dt` (one span for a driver; an observer caps them apart), the
    // members, the contacts.
    virtual void InteractUnder(Fixed commit_dt, Fixed step_dt) = 0;
    // Did this entity's last interaction leave it and everything under it as
    // it found them, with nothing written into it or crossed into its rows
    // since? From under the lock: what a container's own stillness is made of.
    virtual bool StillUnder() = 0;
    // A crossing arriving from under the lock.
    virtual void AbsorbUnder(const OrderVector& crossing) = 0;
    // The contact crossing handed to a neighbour along n (CrossToward), this
    // clock ticked for it. Zero energy when nothing headed that way.
    virtual OrderVector CrossTowardUnder(Fixed nx, Fixed ny, Fixed nz, Fixed span) = 0;
    // Row 0's position: a move's restatement in the new frame (Contain).
    virtual void PlaceUnder(Fixed x, Fixed y, Fixed z) = 0;
    // A solid contact, under the container's lock: the rows to move, the mass
    // that reads them, and the surface it rests on after (resting, a body feels
    // only the field's part along that surface).
    virtual OrderVector& RowsUnder() = 0;
    virtual Fixed MassUnder() = 0;
    virtual void SupportUnder(bool resting, Fixed nx, Fixed ny, Fixed nz) = 0;
    // THE PEG: where this body was when the interaction began, in its
    // container's frame. Peg to step result is its smear (etcs_causal_constraints
    // §14), where a solid contact is found; a contact that cuts it re-pegs there.
    virtual void PegUnder(Fixed& x, Fixed& y, Fixed& z) = 0;
    virtual void RepegUnder(Fixed x, Fixed y, Fixed z) = 0;
    // The field as a relation (Causal is Relational: a space is a law for what is
    // in it). FieldOnUnder is the space's half: resolve the field, hand it to the
    // member, count the work where stated. AccelerateUnder is the body's half:
    // what it lets through (resting: along its surface); returns the work.
    virtual void FieldOnUnder(Causal_& member, Fixed dt) = 0;
    virtual Fixed AccelerateUnder(Fixed gx, Fixed gy, Fixed gz, Fixed dt) = 0;
};

/*
 * A TREE'S OWN ORDER. An interaction is an event in its tree's stream -- the
 * topmost entity's, started by the first post -- and the stream has no thread:
 * whoever observes the tree runs its loop (EventStream::drive), one at a time
 * (a driver waiting on its own line, a frame edge stepping what it looked at).
 * Every event meets every other (the default mask): one history per tree.
 *
 * THE LOCK STILL EXCLUDES: verbs and readers aren't on the stream yet, and a
 * foreign type steps a tree inside its lock (KartBattle::stepLocked). So the
 * consumer locks around an event and a poster locks before it drives -- else a
 * driver could wait on the lock its poster holds.
 */
struct CausalEvent
{
    Causal_*            at = nullptr;   // whose interaction: it and everything Causal under it
    Fixed               commit, step;   // the two spans (Causal_::InteractUnder)
    uint32_t            ticks = 1;
    ::std::atomic<bool> done{ false };  // the commit; its poster's frame is free after
};
// Only the pointer rides the ring; the event waits in its poster's frame.
struct CausalEventPtr { CausalEvent* ptr; };
struct CausalState {};

struct CausalStream : ETCS::EventStream<CausalStream, CausalState, CausalEventPtr>
{
    ETCS::DispatchResult on_event(CausalState&, const CausalEventPtr& ref, uint64_t)
    {
        CausalEvent& e = *ref.ptr;
        const ::std::thread::id outer = m_in.exchange(::std::this_thread::get_id(), ::std::memory_order_acq_rel);
        {
            ::std::lock_guard<::std::recursive_mutex> lk(e.at->TreeMutex());
            for (uint32_t i = 0; i < e.ticks; ++i) e.at->InteractUnder(e.commit, e.step);
        }
        m_in.store(outer, ::std::memory_order_release);
        return { ETCS::DispatchKind::Inline, &e };
    }
    void on_completion(CausalState&, ETCS::WorkResult*, uint64_t) {}
    void on_emit(CausalState&, ETCS::GapSlot& slot)
    {
        if (CausalEvent* e = static_cast<CausalEvent*>(slot.result))
            e->done.store(true, ::std::memory_order_release);
    }

    // Is this thread inside one of this stream's events? An interaction asked for
    // from inside a step is part of it: posted, it would wait behind itself.
    bool Inside() const { return m_in.load(::std::memory_order_acquire) == ::std::this_thread::get_id(); }

private:
    ::std::atomic<::std::thread::id> m_in{ ::std::thread::id{} };
};

#endif // SUPERTYPE_CAUSAL_H__
