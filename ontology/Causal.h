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
 * reach, a facing -- and therefore a history, because every interaction it
 * has is a change to those rows and every change is one the rows can be
 * hashed over (ontology/OrderVector.h). This is the physics as a trait, and
 * it is claimed the way Animated or Drawable are: an entity says it is one,
 * and nothing else about it has to be true.
 *
 * NOT A DRAWING RELATION, and that is the point of it being its own family.
 * The rows are the constraint; a picture of them is a PROJECTION -- a camera
 * on a scene, a mesh on a device -- and a projection is something a runtime
 * can decline to make. A world of Causal entities with no camera on it runs
 * every interaction it would have run on screen, at whatever rate the machine
 * manages, and lands on the same rows. What is drawn is derived from what is
 * here, never the other way round.
 *
 * ONE RELATION: ADJACENCY, AND ONE EVENT: A CROSSING. Two Causal entities are
 * adjacent when a crossing can pass between them, and a crossing is an
 * OrderVector -- a quantity of energy, at a place, from an identity, with or
 * without a direction -- that leaves one and lands in the other, exactly.
 * There are two ways to be adjacent and nothing else:
 *
 *   standing    a member and its container, which is its Causal parent in
 *               the entity tree. What a member sheds as entropy crosses up
 *               into its container's rows, always. Containment is this
 *               adjacency and no other fact: a container is an entity whose
 *               adjacency set includes its members, and it emits upward in
 *               its turn exactly as they do. At the root there is no parent;
 *               that is the open boundary, and what crosses it is counted
 *               out (CausalBase::EmittedOut) rather than lost.
 *
 *   discovered  two members of one container whose reaches touch
 *               (OrderVector::GapTo <= 0): a contact. Each hands the other
 *               the part of its ordered energy headed along the line between
 *               them (OrderVector::CrossToward), and absorbs what the other
 *               handed it. Decided only by the rows, every interaction, in
 *               the container's frame -- a member's rows are in its
 *               container's frame, which is what makes siblings comparable.
 *
 * A crossing is the one event between two identities: it carries its own
 * uncertainty (OrderVector::derive_uncertainty), the emitter's clock ticks
 * for it, and a draw nested in it reads it.
 *
 * THE FAMILY SAYS FIVE THINGS.
 *
 *   Order4    the record, read: where and how this thing is. The one
 *             authoritative statement; a reader holding a copy is wrong, and
 *             a reader is of ONE state only under TreeMutex() -- outside it
 *             a read may straddle a step.
 *   Interact  one causal interaction over a stated span: settle the entropy
 *             owed for the interval that just ended, then advance the motion
 *             for the one that starts, then the members, then the contacts
 *             among them. In that order (CausalBase says why), on this
 *             entity and everything Causal under it.
 *   Impulse   work done on it from outside -- the only way |K| rises.
 *   Absorb    a crossing that landed here from an adjacent entity. Heat as
 *             heat, an ordered part as a push.
 *   CausalHash / CausalTicks   the history: the rows of this tree as one
 *             number, and this entity's own clock.
 *
 * WHO CALLS Interact IS NOT STATED HERE, on purpose, and the two callers
 * this tree has are both right: a camera projecting a scene calls it with
 * the intervals since it last looked (being observed is an interaction), and
 * a driver stepping a world calls it with a fixed span, a thousand times,
 * with no clock read. The arithmetic is the same; only what the spans ARE
 * differs. What is KEPT of either history is the rows: they are on the tag
 * surface as the value behind "Causal" (CausalBase, Entity::bindValue), so
 * a store keeps them and a resume puts them back -- the family has no
 * record of its own.
 *
 * THE PROTOCOL BELOW THE LINE is the base's, not the family's vocabulary: how
 * one history is kept (one lock per tree, taken at every entry) and how a
 * container that already holds it reaches its members and their crossings.
 * It is on the interface because the hop crosses providers through this
 * vtable; nothing outside CausalBase calls it. What it upholds -- every
 * write to any rows in a tree is in one total order -- is the invariant;
 * the mutex is one way to have it.
 */
/*
 * A SOLID: what a body is when it is not only a reach. Off by default -- a
 * body is a sphere set that exchanges energy along the line between centres
 * and passes through what it touches (the contact above). A body made solid
 * (CausalBase::SetSolid) meets another solid by its shape instead: pushed out
 * of it, the motion along the contact normal bounced (restitution), the motion
 * across it rubbed (friction); an anchored one never moves. The shape is a
 * set of planes (ontology/Plane.h), and a solid meets only solids: what is
 * not solid passes through it. The shape: a sphere (the reach), a box (six planes
 * about the centre, row 3 facing it), or a half-space (one plane through the
 * centre, facing row 3's up; solid below).
 */
struct CausalStream;   // a tree's own order, below the family

// The speed of light where no space states one: 2^16 units a second, far
// enough off that a scene only meets it by approaching it on purpose. A power
// of two: a multiple of it is a shift.
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

    // This entity's own clock: crossings committed (OrderVector.h, "emission
    // is the clock"). Not a count of visits.
    virtual uint64_t CausalTicks() const = 0;

    // The rows of this entity and everything Causal under it, as one number:
    // what two runtimes compare. The members compose in canonical order --
    // by what they are, then creation order between twins -- which is the
    // order the step walks them (CausalBase::kidsLocked).
    virtual uint64_t CausalHash() = 0;

    // Was anything in this subtree moving when the last interaction ended?
    // For an observer deciding whether to look again: a member in flight is
    // motion the container's own rows do not show. Read without the lock.
    virtual bool Moving() const = 0;

    // A write to the rows from outside an interaction -- a teleport, a pose, a
    // push -- said once it is made: the change climbs the tree as every other
    // state change does (Entity::markStateChange). What keeps an answer until
    // its subtree changes reads that climb, so a write not said is a write it
    // does not see (CausalBase, A STILL SUBTREE).
    virtual void RowsWritten() = 0;

    // ── the environment ─────────────────────────────────────────────────
    //
    // EVERY CAUSAL THING IS AN ENVIRONMENT for what fits in it, and what holds
    // a thing is decided by fit, not by who made it: the universe holds the
    // earth, which holds the person, which holds the pen in their pocket. A
    // member is in its container while its reach lies inside the space the
    // container provides (Space; zero, solid, holds nothing); after the
    // contacts each interaction, a member that no longer fits moves to its
    // container's container, and one that fits inside a sibling's space
    // moves into the smallest such sibling (CausalBase::fitLocked); an
    // anchored member is held where it was put, and fit moves it nowhere. A
    // move is the entity changing parents (Entity::moveTo), its position
    // restated in the new frame -- frames are translations: a member's
    // position is relative to its container's.

    // The environment this one is in: its container, or null at the open
    // boundary.
    virtual Causal_* Environment() = 0;
    // The origin of the frame this entity's rows are stated in, in the
    // topmost environment's frame: every container's position, summed. A
    // member's place in the world is Basis + its own position.
    virtual void Basis(Fixed& x, Fixed& y, Fixed& z) = 0;
    // The interior this provides, as a radius about its own position.
    virtual Fixed Space() const = 0;

    // ── the space's parameters ──────────────────────────────────────────
    //
    // A CONTAINER STATES WHAT ITS SPACE IS LIKE, and what is inside is subject
    // to it: the radius above is one parameter (what fits), the field is
    // another (what everything inside falls along). They are cumulative down
    // the tree by DEFAULT -- a container that states no field passes on the
    // one it is in, so the earth's gravity reaches the pen in the pocket --
    // and only by default: a container may state its own, zero included,
    // whatever its parent's is, and may provide more room than it takes up in
    // its parent. A pocket dimension is a parameter, not a contradiction.
    //
    // The field this one's members feel, and the container that states it
    // (null: no field anywhere above). The work it does is counted there.
    virtual Causal_* FieldUnder(Fixed& gx, Fixed& gy, Fixed& gz) = 0;
    virtual void CountFieldWorkUnder(Fixed joules) = 0;
    // The speed of light in this space: the fastest anything inside it moves
    // (CausalBase::SetLightSpeed) -- stated here, else the container's, else
    // DefaultLightSpeed at the open boundary.
    virtual Fixed LightUnder() = 0;
    // What this body is when it touches another solid (CausalSolid).
    virtual CausalSolid Solid() = 0;
    // The closeness check: this environment's members whose reach comes
    // within `r` of a point of its frame, in canonical order.
    virtual void Near(Fixed x, Fixed y, Fixed z, Fixed r, ::std::vector<Causal_*>& out) = 0;
    // The members of this one's environment whose reach touches this one's
    // (the contact gate), in canonical order.
    virtual void Adjacent(::std::vector<Causal_*>& out) = 0;
    // Take `member` in, from wherever it is: a move, its position restated
    // in this frame. False when it cannot move (not movable, a module
    // apart, or this is inside it).
    virtual bool Contain(Causal_* member) = 0;

    // ── the protocol (see above) ────────────────────────────────────────
    virtual ::std::recursive_mutex& TreeMutex() = 0;
    // The tree's stream (CausalStream): the topmost entity's, as the lock is.
    virtual CausalStream& TreeStream() = 0;
    // The interaction for a caller holding the tree's lock: the entropy owed
    // over `commit_dt`, the motion over `step_dt` (one span for a driver;
    // an observer measures the two with different ceilings), the members,
    // the contacts.
    virtual void InteractUnder(Fixed commit_dt, Fixed step_dt) = 0;
    // Did this entity's last interaction leave it and everything under it as
    // it found them, with nothing written into it or crossed into its rows
    // since? From under the lock: what a container's own stillness is made of.
    virtual bool StillUnder() = 0;
    // A crossing arriving from under the lock.
    virtual void AbsorbUnder(const OrderVector& crossing) = 0;
    // The contact crossing this entity hands a neighbour along n, from under
    // the lock: OrderVector::CrossToward, with this entity's clock ticked for
    // it. Zero energy when nothing was headed that way.
    virtual OrderVector CrossTowardUnder(Fixed nx, Fixed ny, Fixed nz, Fixed span) = 0;
    // Row 0's position, set from under the lock: a move's restatement in the
    // new frame (Contain).
    virtual void PlaceUnder(Fixed x, Fixed y, Fixed z) = 0;
    // A solid contact, from under the container's lock: the rows to move and
    // the mass that reads them, and the surface this body rests on after it
    // (a resting body feels only the field's part along that surface).
    virtual OrderVector& RowsUnder() = 0;
    virtual Fixed MassUnder() = 0;
    virtual void SupportUnder(bool resting, Fixed nx, Fixed ny, Fixed nz) = 0;
    // THE PEG: where this body was when the interaction in hand began, in its
    // container's frame. From it to where the step put the body is its smear
    // (etcs_causal_constraints.md §14), which is what a solid contact is
    // found on; a contact that cuts the smear pegs the body again where it
    // was cut, so what is left of the smear starts there.
    virtual void PegUnder(Fixed& x, Fixed& y, Fixed& z) = 0;
    virtual void RepegUnder(Fixed x, Fixed y, Fixed z) = 0;
    // The field as a relation (Causal is Relational: every space is a law for
    // what is in it). FieldOnUnder is the space's half: resolve the field
    // (FieldUnder), hand it to the member, count the work where it is stated.
    // AccelerateUnder is the body's half: what it lets through (resting takes
    // only the part along its surface), returning the work done. Both from
    // under the lock.
    virtual void FieldOnUnder(Causal_& member, Fixed dt) = 0;
    virtual Fixed AccelerateUnder(Fixed gx, Fixed gy, Fixed gz, Fixed dt) = 0;
};

/*
 * A TREE'S OWN ORDER. An interaction is an event in the stream of the tree it
 * is in -- the topmost entity's, started by the first one posted -- and the
 * stream has no thread: whoever observes the tree runs its loop
 * (ETCS::EventStream::drive), one at a time. A driver waiting on its own line
 * is one such observer; a frame edge stepping what its pictures looked at is
 * another. Every event meets every other (the stream's default mask): one
 * history a tree.
 *
 * THE LOCK IS STILL WHAT EXCLUDES. Verbs and readers are not on the stream
 * yet, and a foreign type steps a tree from inside its lock
 * (KartBattle::stepLocked), so the consumer takes the lock around an event and
 * a poster takes it before it drives: a driver waiting on a poster's event
 * would otherwise wait on the lock that poster holds.
 */
struct CausalEvent
{
    Causal_*            at = nullptr;   // whose interaction: it and everything Causal under it
    Fixed               commit, step;   // the two spans (Causal_::InteractUnder)
    uint32_t            ticks = 1;
    ::std::atomic<bool> done{ false };  // the commit; its poster's frame is free after
};
// The event stays in its poster's frame, which waits on it: only the pointer
// rides the ring.
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

    // Is the calling thread inside one of this stream's events? An interaction
    // asked for from inside a step is part of that step: posted, it would
    // wait behind the event it was asked from.
    bool Inside() const { return m_in.load(::std::memory_order_acquire) == ::std::this_thread::get_id(); }

private:
    ::std::atomic<::std::thread::id> m_in{ ::std::thread::id{} };
};

#endif // SUPERTYPE_CAUSAL_H__
