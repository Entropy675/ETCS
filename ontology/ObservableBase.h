#ifndef BASE_OBSERVABLE_H__
#define BASE_OBSERVABLE_H__
#include "Observable.h"
#include <atomic>
#include <vector>

// ---------------------------------------------------------------------------
// Tunables. Local to this family on purpose -- the width is a property of how
// this trait stores its edges, not of the runtime's shape, so it does not
// belong in ETCS_API.h where it would read as something other subsystems are
// meant to care about.
//
// EDGE_BITS is one machine word and should stay one: the whole design is that
// every edge's state is a single atomic load or store. CHAIN_SLOTS are the
// reserved tail used to extend capacity past one word (see the chain note
// below); DIRECT_SLOTS is what is left for real observers.
// ---------------------------------------------------------------------------
#define ETCS_OBSERVABLE_EDGE_BITS     64
#define ETCS_OBSERVABLE_CHAIN_SLOTS    8
#define ETCS_OBSERVABLE_DIRECT_SLOTS  (ETCS_OBSERVABLE_EDGE_BITS - ETCS_OBSERVABLE_CHAIN_SLOTS)

// ---------------------------------------------------------------------------
// The edge registry, and the bookkeeping every observed type would otherwise
// write itself -- the same reason Pixels_ owns its buffer and Resizable_ owns
// its size.
//
// BIT POSITION IS EDGE IDENTITY. m_slot[i] and bit i of m_dirty/m_occupied are
// the same edge, index-aligned, one to one. That alignment is what makes every
// operation on the edge SET a mask op on a single word:
//
//     mark all but the cause    m_dirty |= m_occupied & ~originBit
//     is anything stale         m_dirty != 0
//     is anyone watching        m_occupied != 0
//
// and it is why carrying provenance costs nothing here. "A change is news to
// every edge except the one that caused it" is one shift and one OR, where the
// vector this replaces had to compare every entry's RID.
//
// SET IS THE DEFAULT STATE OF AN EDGE. A fresh edge has never been read, so it
// has nothing to claim and must answer yes; a CLEARED bit is the exceptional
// state, meaning "this reader is current as of now". That is also why a reused
// slot inheriting a set bit is correct by construction rather than by luck --
// it is the initial condition, not a leftover. The true/false naming runs
// against the direction entropy actually flows, which is the only thing
// confusing about it.
//
// NO MUTEX. Two atomic words and a slot table, so marking -- the hot path, hit
// by every Pixels_ write and every tag transition -- is one OR rather than a
// lock acquire plus a walk over a heap-allocated vector.
// ---------------------------------------------------------------------------
ETCS_SUPERTYPE_BASE(Observable)
{
    ETCS_MAKE_INSTANCE(Observable)

    /*
 * Register, and hand back the observer's own end of the edge.
 *
 * RETURNING THE HANDLE IS THE TRAIT DOING ITS JOB. A slot the caller had to
 * remember and index correctly would be a convention, and a trait exists to
 * abolish conventions -- the boundary between observer and observed is part of
 * the causal structure this family defines, so this family defines what one
 * looks like (ObserverEdge, Observable.h) rather than leaving each type to
 * invent its own bookkeeping.
 *
 * PUBLICATION ORDER IS LOAD-BEARING: the dirty bit is set BEFORE the RID lands
 * in the slot. A concurrent reader then either does not see the slot at all
 * (unregistered, told true, safe) or sees it already dirty. The other order
 * admits a window where an edge reads as registered-and-clean, which is an
 * under-report -- the one thing this structure must never do.
 */
    ETCS::ObserverEdge Observe(uint64_t observer_rid) override
    {
        if (!observer_rid) return ETCS::ObserverEdge{};

        // Already registered? Hand back the edge it already has.
        for (unsigned i = 0; i < ETCS_OBSERVABLE_DIRECT_SLOTS; ++i)
            if (m_slot[i].load(std::memory_order_acquire) == observer_rid)
                return ETCS::ObserverEdge{observer_rid, static_cast<uint8_t>(i)};

        for (unsigned i = 0; i < ETCS_OBSERVABLE_DIRECT_SLOTS; ++i)
        {
            uint64_t expected = 0;
            const uint64_t bit = 1ull << i;
            m_dirty.fetch_or(bit, std::memory_order_release);      // dirty first
            if (m_slot[i].compare_exchange_strong(expected, observer_rid,
                                                  std::memory_order_acq_rel))
            {
                m_occupied.fetch_or(bit, std::memory_order_release);
                if (observer_rid == static_cast<Derived*>(this)->getRID())
                    m_selfSlot.store(static_cast<int8_t>(i), std::memory_order_release);
                return ETCS::ObserverEdge{observer_rid, static_cast<uint8_t>(i)};
            }
        }
        // Full. Refused loudly rather than aliased silently -- see the chain
        // note in Observable.h for how capacity is meant to grow.
        ETCS_LOG("Observable", "RID:" << static_cast<Derived*>(this)->getRID()
                 << " has no free edge slot for observer RID:" << observer_rid
                 << " (" << ETCS_OBSERVABLE_DIRECT_SLOTS << " direct slots in use).");
        return ETCS::ObserverEdge{};
    }

    // Clears the bit as well as the slot, so a slot's bit always describes its
    // CURRENT occupant rather than usually doing so.
    void Unobserve(uint64_t observer_rid) override
    {
        if (!observer_rid) return;
        for (unsigned i = 0; i < ETCS_OBSERVABLE_DIRECT_SLOTS; ++i)
        {
            if (m_slot[i].load(std::memory_order_acquire) != observer_rid) continue;
            const uint64_t bit = 1ull << i;
            m_slot[i].store(0, std::memory_order_release);
            m_occupied.fetch_and(~bit, std::memory_order_release);
            m_dirty.fetch_and(~bit, std::memory_order_release);
            if (m_selfSlot.load(std::memory_order_acquire) == static_cast<int8_t>(i))
                m_selfSlot.store(-1, std::memory_order_release);
            return;
        }
    }

    /*
 * Mark every registered observer but the cause, then hand the same statement to
 * the nearest Observable ancestor.
 *
 * Only the NEAREST -- it does the same for its own, so the change reaches the
 * root by composition rather than by this walking the whole chain. That is
 * also what makes the propagation correct under re-parenting: nobody holds a
 * path, everyone holds one edge.
 *
 * origin_rid travels UNCHANGED through every hop. It is a property of the
 * change, not of the hop: re-deriving it as "me" at each level would make a
 * compositor skip its own edge when a descendant changed, which is exactly the
 * stale-forever case this exists to prevent.
 */
    void MarkObserved(uint64_t origin_rid) override
    {
        MarkObservedLocal(origin_rid);
        for (ETCS::Entity* n = static_cast<Derived*>(this)->getParent(); n; n = n->getParent())
        {
            void* p = n->getInterfacePointer(ETCS::Buffer("Observable"));
            if (!p) continue;
            static_cast<ETCS::IWireObservable*>(p)->MarkObserved(origin_rid);
            return;
        }
    }

    /*
 * Mine only, no walk. The primitive both directions are built from.
 *
 * The self-slot cache is what keeps this O(1). Nearly every origin in the
 * system is the marking entity itself (etcs_mark_observed passes from->getRID()),
 * and without the cache that RID would be searched for across every slot and
 * usually not found -- a full walk on the hottest path in the family, to
 * discover nothing. With it, the common case is a load and a mask.
 */
    void MarkObservedLocal(uint64_t origin_rid) override
    {
        uint64_t exclude = 0;
        if (origin_rid)
        {
            if (origin_rid == static_cast<Derived*>(this)->getRID())
            {
                const int8_t s = m_selfSlot.load(std::memory_order_acquire);
                if (s >= 0) exclude = 1ull << static_cast<unsigned>(s);
            }
            else
            {
                for (unsigned i = 0; i < ETCS_OBSERVABLE_DIRECT_SLOTS; ++i)
                    if (m_slot[i].load(std::memory_order_acquire) == origin_rid)
                    { exclude = 1ull << i; break; }
            }
        }
        const uint64_t live = m_occupied.load(std::memory_order_acquire);
        m_dirty.fetch_or(live & ~exclude, std::memory_order_release);
    }

    /*
 * THE OTHER DIRECTION OF THE SAME CONTAINMENT EDGE.
 *
 * Containment is one relation carrying two different statements. Upward: "what
 * I contain changed", so whoever holds a merged copy of me is stale. Downward:
 * "the frame I hand you changed", so whatever you derive FROM me is stale.
 * Neither implies the other -- a compositor rebuilding its pixels tells its
 * parent something and its children nothing; a compositor MOVING tells its
 * children something and its parent something else.
 *
 * Both are intrinsic, because containment is what makes them true. A child does
 * not register to be told its parent moved, any more than a parent registers to
 * be told a descendant changed.
 *
 * ONE LEVEL, and the rest by composition -- the same rule MarkObserved follows
 * going up. A child that acts on this mark calls this in turn, so the subtree is
 * reached by everyone holding one edge rather than by anyone walking a path.
 * That keeps it right under re-parenting, and keeps a settled subtree free:
 * marking stops wherever nobody is pulling.
 *
 * What the child does with it is the child's business -- Observable states the
 * causal structure and no more. The intended shape is the lazy pull the rest of
 * this family uses: the bit says there is something readable over here, and the
 * child reads the parent's state when IT next runs, so a derived value is
 * recomputed at the rate the SOURCE changes rather than once per frame forever.
 */
    void MarkObservedBelow()
    {
        std::vector<std::pair<ETCS::Buffer, ETCS::RID>> kids;
        static_cast<Derived*>(this)->getTypedChildren(kids);
        for (const auto& entry : kids)
        {
            ETCS::Entity* child = static_cast<Derived*>(this)->getTypedChild(entry.first, entry.second);
            if (!child) continue;
            void* p = child->getInterfacePointer(ETCS::Buffer("Observable"));
            if (!p) continue;
            static_cast<ETCS::IWireObservable*>(p)->MarkObservedLocal(
                static_cast<Derived*>(this)->getRID());
        }
    }

    /*
 * The fast read: index, verify, test-and-clear. No search anywhere.
 *
 * The verify is not defensive clutter. It is what keeps the RID the real
 * identity and makes the slot a CACHE of the inversion rather than a
 * replacement for it -- a handle into a slot that has since been vacated and
 * reused reads as a mismatch and is reported honestly, instead of silently
 * addressing a stranger's edge.
 */
    bool TakeObserved(const ETCS::ObserverEdge& e) override
    {
        if (!e.valid() || e.slot >= ETCS_OBSERVABLE_DIRECT_SLOTS) return true;
        if (m_slot[e.slot].load(std::memory_order_acquire) != e.observer_rid)
            return true;                       // stale handle: no live edge, no claim
        const uint64_t bit = 1ull << e.slot;
        return (m_dirty.fetch_and(~bit, std::memory_order_acq_rel) & bit) != 0;
    }

    /*
 * The inversion, for a caller holding no handle. Carries a semantic the handle
 * form structurally cannot: an observer with NO edge is told true, because an
 * edge that does not exist carries nothing and so cannot support a claim to be
 * current.
 */
    bool TakeObserved(uint64_t observer_rid) override
    {
        for (unsigned i = 0; i < ETCS_OBSERVABLE_DIRECT_SLOTS; ++i)
            if (m_slot[i].load(std::memory_order_acquire) == observer_rid)
                return TakeObserved(ETCS::ObserverEdge{observer_rid, static_cast<uint8_t>(i)});
        return true;
    }

    /*
 * Watch yourself. A node that caches a result derived from its own subtree is
 * an observer of that subtree like any other, and the subtree is below it, so
 * the bubble already arrives here -- this is what gives it somewhere to land.
 *
 * Not a compositor convenience: a merkle hash is exactly this shape, a cache of
 * everything underneath that recomputes when its own edge is set, so
 * self-observation is the general form and the compositor's raster is its first
 * user.
 *
 * No clear is needed after a self-write. The node's own marks carry origin=itself
 * and are excluded at the source, so its own edge only ever carries somebody
 * else's change.
 */
    ETCS::ObserverEdge ObserveSelf()
    { return Observe(static_cast<Derived*>(this)->getRID()); }

    // A snapshot of who is watching, for a caller that has to do something per
    // observer beyond asking whether it changed. Family-level rather than on
    // the wire: the runtime never needs the list, only the answer.
    void ObserverRids(std::vector<uint64_t>& out) const
    {
        for (unsigned i = 0; i < ETCS_OBSERVABLE_DIRECT_SLOTS; ++i)
        {
            const uint64_t r = m_slot[i].load(std::memory_order_acquire);
            if (r) out.push_back(r);
        }
    }

    // One load each, where the vector form had to take a lock and look at a
    // container. Family-level, not on the interface -- see Observable.h.
    bool Observed()  const { return m_occupied.load(std::memory_order_acquire) != 0; }
    bool AnyStale()  const { return m_dirty.load(std::memory_order_acquire)    != 0; }

private:
    std::atomic<uint64_t> m_occupied{0};   // slot i holds a live edge
    std::atomic<uint64_t> m_dirty{0};      // slot i has entropy to flow
    std::atomic<uint64_t> m_slot[ETCS_OBSERVABLE_DIRECT_SLOTS]{};
    // Which slot is this entity's own edge, or -1. Cached because "the origin is
    // me" is nearly every mark in the system; see MarkObservedLocal.
    std::atomic<int8_t>   m_selfSlot{-1};
};

#endif
