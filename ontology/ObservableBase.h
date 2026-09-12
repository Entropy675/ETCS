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
// every edge's state within a block is a single atomic load or store.
// MAX_BLOCKS bounds the chain, so exhaustion is a loud refusal rather than a
// silent aliasing -- 8 blocks is 512 edges on one entity, which is already far
// past anything this runtime does.
//
// THE CHAIN IS STORAGE, NOT OBSERVATION, and that is why no bits are reserved
// for it. An extension block is this entity's own slot table continued
// elsewhere -- it is reached by pointer, never addressed as an edge, and marks
// propagate into it EAGERLY carrying the original origin. Treating it as an
// observer instead would make an entity's registry an observer of itself, cost
// a real edge per block to say so, and blur the origin across the hop, since a
// lazily-pulled node cannot reconstruct which of several coalesced changes was
// whose. Storage has none of those problems: an RID is a global identity, so
// depth changes where a slot LIVES and never what the edge IS.
// ---------------------------------------------------------------------------
#define ETCS_OBSERVABLE_EDGE_BITS   64   // edges per block -- one machine word
#define ETCS_OBSERVABLE_MAX_BLOCKS   8   // blocks per entity; refuse past this

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
        for (unsigned blk = 0; blk < ETCS_OBSERVABLE_MAX_BLOCKS; ++blk)
        {
            Block* b = this->blockAt(blk, false);
            if (!b) break;
            for (unsigned i = 0; i < ETCS_OBSERVABLE_EDGE_BITS; ++i)
                if (b->rid[i].load(::std::memory_order_acquire) == observer_rid)
                    return this->makeEdge(observer_rid, blk, i);
        }

        for (unsigned blk = 0; blk < ETCS_OBSERVABLE_MAX_BLOCKS; ++blk)
        {
            Block* b = this->blockAt(blk, true);
            for (unsigned i = 0; i < ETCS_OBSERVABLE_EDGE_BITS; ++i)
            {
                uint64_t expected = 0;
                const uint64_t bit = 1ull << i;
                b->dirty.fetch_or(bit, ::std::memory_order_release);   // dirty first
                if (!b->rid[i].compare_exchange_strong(expected, observer_rid,
                                                       ::std::memory_order_acq_rel))
                    continue;
                b->occupied.fetch_or(bit, ::std::memory_order_release);
                if (observer_rid == static_cast<Derived*>(this)->getRID())
                    m_selfEdge.store(static_cast<int32_t>(blk * ETCS_OBSERVABLE_EDGE_BITS + i),
                                     ::std::memory_order_release);
                return this->makeEdge(observer_rid, blk, i);
            }
        }
        // Every block full. Refused loudly rather than aliased silently.
        ETCS_LOG("Observable", "RID:" << static_cast<Derived*>(this)->getRID()
                 << " has no free edge slot for observer RID:" << observer_rid
                 << " (" << (ETCS_OBSERVABLE_MAX_BLOCKS * ETCS_OBSERVABLE_EDGE_BITS)
                 << " edges in use).");
        return ETCS::ObserverEdge{};
    }

    // Clears the bit as well as the slot, so a slot's bit always describes its
    // CURRENT occupant rather than usually doing so.
    void Unobserve(uint64_t observer_rid) override
    {
        if (!observer_rid) return;
        for (unsigned blk = 0; blk < ETCS_OBSERVABLE_MAX_BLOCKS; ++blk)
        {
            Block* b = this->blockAt(blk, false);
            if (!b) return;
            for (unsigned i = 0; i < ETCS_OBSERVABLE_EDGE_BITS; ++i)
            {
                if (b->rid[i].load(::std::memory_order_acquire) != observer_rid) continue;
                const uint64_t bit = 1ull << i;
                b->rid[i].store(0, ::std::memory_order_release);
                b->occupied.fetch_and(~bit, ::std::memory_order_release);
                b->dirty.fetch_and(~bit, ::std::memory_order_release);
                const int32_t idx = static_cast<int32_t>(blk * ETCS_OBSERVABLE_EDGE_BITS + i);
                if (m_selfEdge.load(::std::memory_order_acquire) == idx)
                    m_selfEdge.store(-1, ::std::memory_order_release);
                return;
            }
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
        int32_t excludeIdx = -1;
        if (origin_rid)
        {
            if (origin_rid == static_cast<Derived*>(this)->getRID())
                excludeIdx = m_selfEdge.load(::std::memory_order_acquire);
            else
                excludeIdx = this->findEdge(origin_rid);
        }

        // Eagerly across the whole chain, carrying the same exclusion: the
        // blocks are one table, not a lazy hop, so origin stays exact at depth.
        for (unsigned blk = 0; blk < ETCS_OBSERVABLE_MAX_BLOCKS; ++blk)
        {
            Block* b = this->blockAt(blk, false);
            if (!b) return;
            uint64_t exclude = 0;
            if (excludeIdx >= 0
             && static_cast<unsigned>(excludeIdx) / ETCS_OBSERVABLE_EDGE_BITS == blk)
                exclude = 1ull << (static_cast<unsigned>(excludeIdx) % ETCS_OBSERVABLE_EDGE_BITS);
            const uint64_t live = b->occupied.load(::std::memory_order_acquire);
            b->dirty.fetch_or(live & ~exclude, ::std::memory_order_release);
        }
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
        ::std::vector<::std::pair<ETCS::Buffer, ETCS::RID>> kids;
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
        if (!e.valid()) return true;
        const unsigned blk = e.slot / ETCS_OBSERVABLE_EDGE_BITS;
        const unsigned i   = e.slot % ETCS_OBSERVABLE_EDGE_BITS;
        if (blk >= ETCS_OBSERVABLE_MAX_BLOCKS) return true;
        Block* b = this->blockAt(blk, false);
        if (!b) return true;
        if (b->rid[i].load(::std::memory_order_acquire) != e.observer_rid)
            return true;                       // stale handle: no live edge, no claim
        const uint64_t bit = 1ull << i;
        return (b->dirty.fetch_and(~bit, ::std::memory_order_acq_rel) & bit) != 0;
    }

    /*
 * The inversion, for a caller holding no handle. Carries a semantic the handle
 * form structurally cannot: an observer with NO edge is told true, because an
 * edge that does not exist carries nothing and so cannot support a claim to be
 * current.
 */
    bool TakeObserved(uint64_t observer_rid) override
    {
        const int32_t idx = this->findEdge(observer_rid);
        if (idx < 0) return true;
        return TakeObserved(ETCS::ObserverEdge{observer_rid, static_cast<uint16_t>(idx)});
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
    void ObserverRids(::std::vector<uint64_t>& out) const
    {
        for (unsigned blk = 0; blk < ETCS_OBSERVABLE_MAX_BLOCKS; ++blk)
        {
            Block* b = this->blockAt(blk, false);
            if (!b) return;
            for (unsigned i = 0; i < ETCS_OBSERVABLE_EDGE_BITS; ++i)
            {
                const uint64_t r = b->rid[i].load(::std::memory_order_acquire);
                if (r) out.push_back(r);
            }
        }
    }

    // One load per live block, where the vector form took a lock and looked at
    // a container. Family-level, not on the interface -- see Observable.h.
    bool Observed() const { return this->anyBlockBitSet(&Block::occupied); }
    bool AnyStale() const { return this->anyBlockBitSet(&Block::dirty); }

private:
    // One word of edges, plus the RIDs those bits belong to. The first lives
    // inline; the rest are allocated only if an entity ever exceeds one word.
    struct Block
    {
        ::std::atomic<uint64_t> occupied{0};   // bit i holds a live edge
        ::std::atomic<uint64_t> dirty{0};      // bit i has entropy to flow
        ::std::atomic<uint64_t> rid[ETCS_OBSERVABLE_EDGE_BITS]{};
        ::std::atomic<Block*>   next{nullptr};
    };

    // The first block is inline; the rest come from THIS ENTITY'S OWN local
    // arena, so an extension has the same lifetime as the edges it holds and is
    // reclaimed with the entity rather than by a destructor this base cannot
    // declare (ETCS_MAKE_INSTANCE already defines ~ObservableBase). It is also
    // the only allocator that is correct here: a block is entity-local content,
    // exactly what local_arena_ exists for.
    Block m_head;

    static ETCS::ObserverEdge makeEdge(uint64_t rid, unsigned blk, unsigned bit)
    { return ETCS::ObserverEdge{rid, static_cast<uint16_t>(blk * ETCS_OBSERVABLE_EDGE_BITS + bit)}; }

    // The RID->index inversion, for callers holding no handle. The only search
    // left in the structure, and the handle form exists to skip it.
    int32_t findEdge(uint64_t rid) const
    {
        if (!rid) return -1;
        for (unsigned blk = 0; blk < ETCS_OBSERVABLE_MAX_BLOCKS; ++blk)
        {
            Block* b = this->blockAt(blk, false);
            if (!b) return -1;
            for (unsigned i = 0; i < ETCS_OBSERVABLE_EDGE_BITS; ++i)
                if (b->rid[i].load(::std::memory_order_acquire) == rid)
                    return static_cast<int32_t>(blk * ETCS_OBSERVABLE_EDGE_BITS + i);
        }
        return -1;
    }

    bool anyBlockBitSet(::std::atomic<uint64_t> Block::* which) const
    {
        for (unsigned blk = 0; blk < ETCS_OBSERVABLE_MAX_BLOCKS; ++blk)
        {
            Block* b = this->blockAt(blk, false);
            if (!b) return false;
            if ((b->*which).load(::std::memory_order_acquire) != 0) return true;
        }
        return false;
    }

    // Block at `index`, or null if the chain is shorter. `make` extends it.
    Block* blockAt(unsigned index, bool make) const
    {
        Block* b = const_cast<Block*>(&m_head);
        for (unsigned n = 0; n < index; ++n)
        {
            Block* nxt = b->next.load(::std::memory_order_acquire);
            if (!nxt)
            {
                if (!make) return nullptr;
                ETCS::MemoryArena& arena =
                    const_cast<Derived*>(static_cast<const Derived*>(this))->getArena();
                Block* fresh = arena.allocate<Block>();
                if (!fresh) return nullptr;
                Block* expect = nullptr;
                if (!b->next.compare_exchange_strong(expect, fresh,
                                                     ::std::memory_order_acq_rel))
                    nxt = expect;                    // lost the race; theirs wins,
                                                     // ours is arena-owned and inert
                else nxt = fresh;
            }
            b = nxt;
        }
        return b;
    }
    // This entity's own edge index, or -1. Cached because "the origin is me" is
    // nearly every mark in the system; see MarkObservedLocal.
    ::std::atomic<int32_t>  m_selfEdge{-1};
};

#endif
