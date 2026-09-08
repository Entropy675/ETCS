#ifndef BASE_THREAD_H__
#define BASE_THREAD_H__
#include "Thread.h"
#include "ThreadedBase.h"
#include <mutex>

// Composes ThreadedBase, so claiming Thread claims the cooperative stop too and
// a leaf cannot hold one without the other. Same lineage-in-the-Base rule
// DrawableBase follows for Surface, and it buys the same compile-time
// exclusivity: a type that claimed both ThreadBase and ThreadedBase would carry
// two ThreadedBase subobjects and every Halt() through them would be ambiguous.
//
// The closure and the signal context live here rather than in the leaves --
// bookkeeping every actor needs and none of them should write twice, the same
// argument Pixels_ makes for owning its buffer and ObservableBase for owning
// its watcher list.
ETCS_SUPERTYPE_BASE(Thread), public ThreadedBase<Derived>
{
    ETCS_MAKE_INSTANCE(Thread)
    ETCS_DISPATCH_METHOD(ETCS::Buffer, Script);
    // Overrides the wire's refusing default (ThreadedBase) and hands the work
    // to the leaf, which is the only thing that knows how to make one of itself.
    uint64_t Detach(const ETCS::Buffer& script) override
    {
        if (this->Halted()) return 0;   // a thread in teardown spawns nothing
        return static_cast<Derived*>(this)->DetachConcrete(script);
    }

    /*
 * This thread's own signal authority.
 *
 * SignalContext already carries the two edges this needs (core/SignalContext.h):
 * `up`, the ACTIVE call chain, and `provider`, the PASSIVE ownership chain that
 * "follows the ownership edge, always". An entity Thread gets the second one
 * maintained for free, because reparentChildrenTo is already the single writer
 * of it -- which is exactly what DetachedExecutor's hand-rolled local_sig,
 * parented once at creation to the process root, could not track.
 */
    ETCS::SignalContext&       Signals()       { return m_sig; }
    const ETCS::SignalContext& Signals() const { return m_sig; }

    /*
 * The closure: a name bound to a copy of its data.
 *
 * BY VALUE, AND THAT IS THE POINT. A detached job outlives the statement that
 * launched it, so a pointer or reference into the launching scope is a read of
 * dead stack by the time the job runs. Copying into a fixed Buffer is what lets
 * the thread carry its inputs into whatever lifetime it enters.
 *
 * Returns false if the data does not fit, rather than truncating. A silently
 * shortened SQL query is a script that half-works with nothing to say why, and
 * "fail explicitly at the binding" is the behaviour the rvalue syntax wants
 * when a [data] blob exceeds the standard buffer.
 */
    bool Bind(const ETCS::Buffer& name, const char* data, size_t len)
    {
        if (!data || len >= ETCS::Buffer::bufsize) return false;
        std::lock_guard<std::mutex> lock(m_closureMutex);
        for (auto& e : m_closure)
            if (e.name == name) { e.data = ETCS::Buffer(data); return true; }
        m_closure.push_back(Binding{name, ETCS::Buffer(data)});
        return true;
    }

    bool Lookup(const ETCS::Buffer& name, ETCS::Buffer& out) const
    {
        std::lock_guard<std::mutex> lock(m_closureMutex);
        for (const auto& e : m_closure)
            if (e.name == name) { out = e.data; return true; }
        return false;
    }

    // Copy this thread's closure onto a child. Called by a leaf's Detach after
    // it has allocated the child -- the family cannot do it itself, because
    // only the leaf knows how it makes one.
    void InheritClosureTo(ThreadBase<Derived>& child) const
    {
        std::lock_guard<std::mutex> lock(m_closureMutex);
        std::lock_guard<std::mutex> clock(child.m_closureMutex);
        child.m_closure = m_closure;
    }

    size_t ClosureSize() const
    {
        std::lock_guard<std::mutex> lock(m_closureMutex);
        return m_closure.size();
    }

private:
    struct Binding { ETCS::Buffer name; ETCS::Buffer data; };
    mutable std::mutex   m_closureMutex;
    std::vector<Binding> m_closure;
    ETCS::SignalContext        m_sig;
};

#endif
