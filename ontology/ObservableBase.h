#ifndef BASE_OBSERVABLE_H__
#define BASE_OBSERVABLE_H__
#include "Observable.h"
#include <algorithm>
#include <mutex>
#include <vector>

// The registry and the per-observer bits live here, not in the leaves: this is
// bookkeeping every observed type needs and none of them should write twice.
//
// One vector, not a map. An entity has a handful of observers -- a scene has
// one or two cameras -- so a linear scan beats a hash, and the dirty bit rides
// beside the RID rather than in a second container that can disagree with it.
//
// Locked, because the marking side and the reading side are genuinely different
// threads: a script moves a node while a frame edge asks whether it changed.
ETCS_SUPERTYPE_BASE(Observable)
{
    ETCS_MAKE_INSTANCE(Observable)

    void Observe(uint64_t observer_rid) override
    {
        if (!observer_rid) return;
        std::lock_guard<std::mutex> lock(m_obsMutex);
        for (auto& o : m_observers) if (o.rid == observer_rid) return;
        // Starts DIRTY: an observer that has never looked has nothing cached,
        // so its first question must answer yes or it renders nothing.
        m_observers.push_back(Watcher{observer_rid, true});
    }

    void Unobserve(uint64_t observer_rid) override
    {
        std::lock_guard<std::mutex> lock(m_obsMutex);
        m_observers.erase(std::remove_if(m_observers.begin(), m_observers.end(),
                              [observer_rid](const Watcher& w){ return w.rid == observer_rid; }),
                          m_observers.end());
    }

    /*
 * Mark every registered observer, then hand the same statement to the nearest
 * Observable ancestor.
 *
 * Only the NEAREST -- it does the same for its own, so the change reaches the
 * root by composition rather than by this walking the whole chain. That is
 * also what makes the propagation correct under re-parenting: nobody holds a
 * path, everyone holds one edge.
 */
    void MarkObserved() override
    {
        {
            std::lock_guard<std::mutex> lock(m_obsMutex);
            for (auto& o : m_observers) o.dirty = true;
        }
        for (ETCS::Entity* n = static_cast<Derived*>(this)->getParent(); n; n = n->getParent())
        {
            void* p = n->getInterfacePointer(ETCS::Buffer("Observable"));
            if (!p) continue;
            static_cast<ETCS::IWireObservable*>(p)->MarkObserved();
            return;
        }
    }

    // Read-and-clear, for one observer only. An unregistered observer gets
    // true: it has never been told anything, so it cannot assume it is current.
    bool TakeObserved(uint64_t observer_rid) override
    {
        std::lock_guard<std::mutex> lock(m_obsMutex);
        for (auto& o : m_observers)
            if (o.rid == observer_rid) { const bool was = o.dirty; o.dirty = false; return was; }
        return true;
    }

    /*
 * Watch yourself. A node that caches a result derived from its own subtree is
 * an observer of that subtree like any other, and the subtree is below it, so
 * the bubble already arrives here -- this is what gives it somewhere to land.
 *
 * TakeObserved(getRID()) is then the node's own "is my cache stale", which is
 * what the single dirty flag used to answer. Not a compositor convenience: a
 * merkle hash is exactly this shape, a cache of everything underneath that
 * recomputes when its own bit is set, so self-observation is the general form
 * and the compositor's raster is its first user.
 *
 * Registration is explicit and must not be forgotten -- an unregistered
 * observer is told true forever (see TakeObserved), so a node that skips this
 * recomputes every frame and nothing reports it. The ontology tester checks
 * that a self-observing node SETTLES for exactly that reason.
 */
    void ObserveSelf() { Observe(static_cast<Derived*>(this)->getRID()); }

    /*
 * My own write is not news to me. Called by a self-observing node AFTER it
 * rebuilds its cache, to drop the marks that rebuild just caused.
 *
 * The single flag did not need this and the per-observer form does, which is
 * worth stating because it is the one place the two are not equivalent. With
 * one bool the order did the work: the node took the flag (clearing it), then
 * wrote, and the write re-set the flag FOR THE UPLOADER, who took it in turn.
 * One consumer handed it to the next. With independent bits a write marks
 * every observer including this node's own, nobody clears that one, and the
 * node rebuilds every frame forever -- a silent loss of exactly the skip the
 * cache exists for, with correct output the whole time.
 */
    void ClearSelfObserved() { (void)TakeObserved(static_cast<Derived*>(this)->getRID()); }

    // A snapshot of who is watching, for a caller that has to do something per
    // observer beyond asking whether it changed. Family-level rather than on
    // the wire: the runtime never needs the list, only the answer.
    void ObserverRids(std::vector<uint64_t>& out) const
    {
        std::lock_guard<std::mutex> lock(m_obsMutex);
        out.reserve(out.size() + m_observers.size());
        for (const auto& o : m_observers) out.push_back(o.rid);
    }

    // Family-level, not on the interface -- see Observable.h. A node with no
    // observers can skip work whose only purpose is to be looked at.
    bool Observed() const
    {
        std::lock_guard<std::mutex> lock(m_obsMutex);
        return !m_observers.empty();
    }

private:
    struct Watcher { uint64_t rid; bool dirty; };
    mutable std::mutex   m_obsMutex;
    std::vector<Watcher> m_observers;
};

#endif
