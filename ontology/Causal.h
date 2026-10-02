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
 * THE FAMILY SAYS FOUR THINGS.
 *
 *   Order4    the record, read: where and how this thing is. The one
 *             authoritative statement; a reader holding a copy is wrong.
 *   Interact  one causal interaction over a stated span: settle the entropy
 *             owed for the interval that just ended, then advance the motion
 *             for the one that starts. In that order (CausalBase says why),
 *             on this entity and everything Causal under it.
 *   Impulse   work done on it from outside -- the only way |K| rises.
 *   Absorb    a crossing that landed here: what a member emitted, arriving
 *             at the thing that contains it. Heat as heat, an ordered part as
 *             a push; the container is what a member's entropy is entropy
 *             INTO.
 *
 * WHO CALLS Interact IS NOT STATED HERE, on purpose, and the two callers
 * this tree has are both right: a camera projecting a scene calls it with
 * the interval since it last looked (being observed is an interaction), and
 * a driver stepping a world calls it with a fixed span, a thousand times,
 * with no clock read. The arithmetic is the same; only what dt IS differs.
 *
 * THE CONTAINMENT IS THE ENTITY TREE. A member's environment is its parent,
 * found by asking the parent whether it is Causal -- so a box inside a room
 * inside a world warms the room, which warms the world, across providers,
 * without any of them naming another's type. At the root the heat leaves the
 * model, counted (CausalBase::EmittedOut), which is what an open system is.
 */
class Causal_ : virtual public ETCS::Entity
{
public:
    virtual ~Causal_() = default;

    virtual const OrderVector& Order4() const = 0;

    // One interaction over `dt` seconds, here and below.
    virtual void Interact(Fixed dt) = 0;

    /*
     * THE TREE'S LOCK, AND THE HOP THAT RUNS UNDER IT. Every write to any
     * rows in a tree of Causal entities is made under the one mutex its
     * topmost Causal entity holds (CausalBase::TreeMutex): a driver stepping
     * the world and a camera observing it take turns on the tree rather
     * than interleaving inside a step, which would be a history nobody ran.
     * Interact takes it; InteractUnder is the same interaction for a
     * container that already holds it, walking its members -- one lock per
     * tick of a tree, not one per member.
     */
    virtual ::std::recursive_mutex& TreeMutex() = 0;
    virtual void InteractUnder(Fixed dt) = 0;
    virtual void AbsorbUnder(const OrderVector& crossing) = 0;   // a member's crossing, from under the lock

    // Energy in, along a direction.
    virtual void Impulse(Fixed dx, Fixed dy, Fixed dz, Fixed joules) = 0;

    // A crossing from a member, arriving.
    virtual void Absorb(const OrderVector& crossing) = 0;

    // This entity's own clock: emissions committed (OrderVector.h, "emission
    // is the clock"). Not a count of visits.
    virtual uint64_t CausalTicks() const = 0;

    // The rows of this entity and everything Causal under it, as one number,
    // in tree order: what two runtimes compare.
    virtual uint64_t CausalHash() = 0;
};

#endif // SUPERTYPE_CAUSAL_H__
