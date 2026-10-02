#ifndef SUPERTYPE_CAUSAL_H__
#define SUPERTYPE_CAUSAL_H__

#include "../core_defs.h"
#include "OrderVector.h"

#include <mutex>

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
 * differs -- and an observed span, once measured and made Fixed, is a
 * recorded input (CausalBase::ObservedTape), so an observed history replays.
 *
 * THE PROTOCOL BELOW THE LINE is the base's, not the family's vocabulary: how
 * one history is kept (one lock per tree, taken at every entry) and how a
 * container that already holds it reaches its members and their crossings.
 * It is on the interface because the hop crosses providers through this
 * vtable; nothing outside CausalBase calls it. What it upholds -- every
 * write to any rows in a tree is in one total order -- is the invariant;
 * the mutex is one way to have it.
 */
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
    // what two runtimes compare. The members compose as a multiset of their
    // own hashes, so the same tree built in another order is the same number.
    virtual uint64_t CausalHash() = 0;

    // ── the protocol (see above) ────────────────────────────────────────
    virtual ::std::recursive_mutex& TreeMutex() = 0;
    // The interaction for a caller holding the tree's lock: the entropy owed
    // over `commit_dt`, the motion over `step_dt` (one span for a driver;
    // an observer measures the two with different ceilings), the members,
    // the contacts.
    virtual void InteractUnder(Fixed commit_dt, Fixed step_dt) = 0;
    // A crossing arriving from under the lock.
    virtual void AbsorbUnder(const OrderVector& crossing) = 0;
    // The contact crossing this entity hands a neighbour along n, from under
    // the lock: OrderVector::CrossToward, with this entity's clock ticked for
    // it. Zero energy when nothing was headed that way.
    virtual OrderVector CrossTowardUnder(Fixed nx, Fixed ny, Fixed nz, Fixed span) = 0;
};

#endif // SUPERTYPE_CAUSAL_H__
