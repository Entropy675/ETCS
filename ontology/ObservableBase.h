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

    // A snapshot of who is watching, for a caller that has to do something per
    // observer beyond asking whether it changed. Family-level rather than on
    // the wire: the runtime never needs the list, only the answer.
    void ObserverRids(std::vector<uint64_t>& out) const
    {
        std::lock_guard<std::mutex> lock(m_obsMutex);
        out.reserve(out.size() + m_observers.size());
        for (const auto& o : m_observers) out.push_back(o.rid);
    }

    bool Observed() const override
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
