#ifndef BASE_THREAD_H__
#define BASE_THREAD_H__
#include "Thread.h"
#include "ThreadedBase.h"
#include <atomic>
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
 * This thread's own signal authority, by value.
 *
 * COPIED, NOT HANDED OUT BY POINTER. A SignalContext is a set of pointers and
 * two Buffers -- the runtime already passes it by value across the DSO boundary
 * in every WorkFunc and StreamFunc, and IWireWrapper takes one that way. There
 * is nothing to protect: a caller that can ask for the signals is entitled to
 * them, and a copy cannot dangle the way a pointer into an entity's shell can.
 *
 * IT IS THE ENTITY'S OWN CONTEXT, not a second one beside it. Entity::ctx_
 * already exists and is already maintained -- reparentChildrenTo is its single
 * writer and repoints the PASSIVE `provider` edge whenever ownership moves
 * (core/SignalContext.h). A private context here would get none of that upkeep
 * while looking like it had.
 *
 * LOCAL FLAGS ARE WHAT MAKE A JOB INDIVIDUALLY STOPPABLE. Without them the
 * context has no authority of its own and every signal check falls straight
 * through to the parent chain, so a targeted terminate would stop the whole
 * tree instead of one job -- which is precisely the property DetachedExecutor
 * carried its own three flags for.
 *
 * Wired here rather than in a constructor because ETCS_MAKE_INSTANCE already
 * defines ThreadBase(). Idempotent, so calling it repeatedly costs three
 * stores.
 */
    ETCS::SignalContext Signals() override
    {
        ETCS::SignalContext& c = static_cast<Derived*>(this)->getContext();
        c.interrupt = this->ensureFlag(m_interrupt);
        c.terminate = this->ensureFlag(m_terminate);
        c.user1     = this->ensureFlag(m_user1);

        /*
 * A THREAD IS A CLOSURE BOUNDARY. Root is the entry point for the ontology;
 * a Thread is the entry point for lifetimes and signals, so everything it
 * runs and everything it detaches belongs to its closure and a raise inside
 * that closure stops HERE rather than reaching g_sig_int
 * (SignalContext::closure_root). Closing a window used to end the runtime for
 * exactly this reason: nothing named an edge, so every closure was the
 * process.
 *
 * Still parented to the process root, and that is not a contradiction: reads
 * cross the boundary in both directions, so a real SIGTERM still stops
 * everything inside here. The marker bounds who a raise REACHES, not who
 * hears one.
 */
        c.closure_root = true;
        c.setParent(&ETCS::RootSignalContext());
        return c;
    }

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

    /*
 * THE FLAGS DO NOT LIVE IN THIS ENTITY, and that is the whole point.
 *
 * A SignalContext is copied and carried -- a detached job holds one for as long
 * as it runs -- so the bits it points at have to outlive the entity that
 * published them. An entity member cannot: reclaimEntity memsets the WHOLE
 * outer shell once ~T() has run and pushes it onto the exact-(size, alignment)
 * free list, where the next allocate of the same concrete type takes it. A
 * running thread's copy would then be pointing at a DIFFERENT LIVE THREAD'S
 * stop flag -- not a crash, a silent misrouting, which is the same hazard
 * SignalContext::provider documents for itself.
 *
 * So they come from the ROOT arena. That memory is released only when the
 * module is unloaded, which cannot happen while anything still holds the
 * lifetime token -- so a flag is guaranteed to outlive every reader of it.
 *
 * DetachedExecutor got this for free by accident of shape: its flags were
 * members of a heap object the registry held until join_all(). Moving the
 * authority onto an entity is what made the lifetime need stating.
 *
 * Never individually freed. Three atomics per Thread, arena-owned, reclaimed
 * wholesale at teardown -- the cost of a bounded leak against a class of bug
 * that reads as an unrelated subtree stopping for no reason.
 */
    ETCS::SignalFlag* ensureFlag(std::atomic<ETCS::SignalFlag*>& slot)
    {
        if (ETCS::SignalFlag* f = slot.load(std::memory_order_acquire)) return f;
        ETCS::SignalFlag* fresh =
            ETCS::MemoryArena::getInstance().allocate<ETCS::SignalFlag>(0);
        ETCS::SignalFlag* expected = nullptr;
        if (slot.compare_exchange_strong(expected, fresh, std::memory_order_acq_rel))
            return fresh;
        return expected;   // lost the race; ours is arena-owned and simply unused
    }

    std::atomic<ETCS::SignalFlag*> m_interrupt{nullptr};
    std::atomic<ETCS::SignalFlag*> m_terminate{nullptr};
    std::atomic<ETCS::SignalFlag*> m_user1{nullptr};
};

#endif
