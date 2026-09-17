#ifndef SUPERTYPE_OBSERVABLE_H__
#define SUPERTYPE_OBSERVABLE_H__

#include "../core_defs.h"
#include <utility>
#include <vector>

// ---------------------------------------------------------------------------
// Observable — something records the state of another and wants to know when
// that changes.
//
// The structure was already here twice, written out by hand and differently
// each time: etcs_mark_pixel_path walks the parent chain marking every Pixels
// owner, and Scene3D keeps m_viewers of cameras that projected it and marks
// those. Same causal shape, two spellings, neither reusable -- and a third use
// was about to be written for the merkle hash.
//
// THE DIRTY BIT BELONGS TO THE RELATION, not to either end of it. Not a
// property of the observed (it has no single state of being stale -- it is
// stale to one watcher and current to another), and not one of the observer
// (which holds a cache, not a claim about someone else). What it is, is one
// bit per EDGE, and everything below follows from that:
//
//   - two cameras viewing one scene each have their own edge, so neither can
//     consume the other's invalidation. A single read-and-clear flag on the
//     observed is the bug this shape makes impossible;
//   - an edge that does not exist carries nothing, which is why an
//     unregistered observer is told true: it has no edge to have been told
//     along, so it cannot claim to be current.
//
// EDGES ARE INTRINSIC ONLY FOR PARENT/CHILD, AND THEY RUN BOTH WAYS. Containment
// is one relation carrying two different statements, and each is news to the
// opposite side:
//
//   upward    MarkObserved       "what I contain changed" -- so anything holding
//                                a merged copy of me is stale. Bubbles to the
//                                nearest Observable ancestor, which does the same.
//   downward  MarkObservedBelow  "the frame I hand you changed" -- so anything
//                                you derive FROM me is stale. Marks direct
//                                children; each continues when it acts.
//
// Neither implies the other, which is why they are two calls. A compositor
// rebuilding its own pixels tells its parent something and its children nothing;
// a compositor MOVING tells its children their coordinates shifted and tells its
// parent something else entirely. One call for both would make every recompose
// look like a reparenting to everything underneath it.
//
// Both directions are intrinsic because containment is what makes them true: a
// child no more registers to hear that its parent moved than an ancestor
// registers to hear that a descendant changed. EVERY OTHER edge is declared with
// Observe(): a camera naming a scene it does not contain, a device caching an
// image, a follower tracking a size.
//
// SELF-OBSERVATION IS REGISTERED, not intrinsic, and the distinction is not
// pedantic. A node watching its own subtree is not its own parent; the edge
// from a node to itself has to be stated like any other, which is what
// ObservableBase::ObserveSelf is for. A node that caches something derived
// from below it and skips that registration gets the unregistered answer --
// true, forever -- and rebuilds every frame with nothing to report it.
//
// ETCS::IWireObservable first and non-virtual -- see core/InterfaceWire.h.
// ---------------------------------------------------------------------------
class Observable_ : public ETCS::IWireObservable, virtual public ETCS::Entity
{
public:
    virtual ~Observable_() = default;

    ETCS::ObserverEdge Observe(uint64_t observer_rid) override = 0;
    void Unobserve(uint64_t observer_rid) override = 0;
    void MarkObserved(uint64_t origin_rid) override = 0;
    bool TakeObserved(const ETCS::ObserverEdge& edge) override = 0;
    bool TakeObserved(uint64_t observer_rid) override = 0;

    // NOTHING ELSE. The four above are the wire, answered by the base for
    // every claimant, so this family adds no dispatched method of its own and
    // `ace ontology` correctly reports it as having none -- the same shape
    // Threaded and Lifecycle have.
    //
    // "Is anyone watching" (Observed) and "who" (ObserverRids) both live on
    // ObservableBase instead. They were briefly declared here, which made the
    // tool flag this family as not proving its lineage, and it was right to:
    // a pure virtual no leaf implements is a lineage claim with nothing behind
    // it. Halted went the other way -- onto IWireThread -- because the arena
    // genuinely asks it. Nothing outside this family asks either of these.
};

// This entity's Observable half, or null if it never claimed the family.
// Reached by family name like every other cross-family hop -- a caller holding
// a Pixels_* or a Resizable_* has no static route to it, by design.
inline ETCS::IWireObservable* etcs_observable_of(ETCS::Entity* e)
{
    if (!e) return nullptr;
    void* p = e->getInterfacePointer(ETCS::Buffer("Observable"));
    return p ? static_cast<ETCS::IWireObservable*>(p) : nullptr;
}

/*
 * Mark the nearest Observable at or above `from`.
 *
 * Replaces etcs_mark_pixel_path, which walked to the root marking every Pixels
 * owner it passed. It does not need to: MarkObserved bubbles to ITS nearest
 * Observable ancestor, which does the same, so one call here reaches the same
 * set by composition. The old walk was that recursion written out by hand.
 *
 * Starts AT `from` rather than at its parent, so a writer outside the tree
 * passes the node it wrote to and a node that changed itself passes `this` --
 * both the same statement, as before.
 *
 * Walks up when `from` has no Observable half rather than giving up: a leaf
 * that never claimed the family still gets its ancestors told, so forgetting
 * the claim degrades the signal instead of dropping it.
 */
inline void etcs_mark_observed(ETCS::Entity* from)
{
    for (ETCS::Entity* n = from; n; n = n->getParent())
        if (ETCS::IWireObservable* o = etcs_observable_of(n))
        { o->MarkObserved(from->getRID()); return; }
}

/*
 * The downward counterpart: say that what `from` hands its children has moved.
 *
 * Free-function form for the same reason etcs_mark_observed has one -- a caller
 * holding a Drawable2D_* or writing from outside the tree has no route to the
 * family, and a leaf that never claimed Observable should degrade rather than
 * drop the statement.
 *
 * Deliberately NOT a walk to the leaves. One level, and each child that acts on
 * the mark passes it on; see ObservableBase::MarkObservedBelow.
 */
inline void etcs_mark_observed_below(ETCS::Entity* from)
{
    if (!from) return;
    ::std::vector<::std::pair<ETCS::Buffer, ETCS::RID>> kids;
    from->getTypedChildren(kids);
    for (const auto& entry : kids)
    {
        ETCS::Entity* child = from->getTypedChild(entry.first, entry.second);
        if (!child) continue;
        if (ETCS::IWireObservable* o = etcs_observable_of(child))
            o->MarkObservedLocal(from->getRID());
    }
}

#endif // SUPERTYPE_OBSERVABLE_H__
