#ifndef BASE_THREADED_H__
#define BASE_THREADED_H__
#include "Threaded.h"

// The latch lives here for the reason LifecycleBase and DeletableBase give for
// theirs, and this one is asked for from a teardown path where a second caller
// is normal. Atomic because the callers are genuinely concurrent: the arena's
// retire, a script's Delete, and the body polling Halted() are three threads.
//
// Halt does not call the leaf. Setting the flag is the whole request; a hook
// here would run teardown on whichever thread called Halt, which is the
// cross-thread teardown Lifecycle exists to prevent. A leaf that needs to do
// something on stopping does it in ReleaseConcrete.
//
// Shape defaults to Held: a type claims this because it owns a body that runs.
// A leaf whose bodies really are Passing overrides.
ETCS_SUPERTYPE_BASE(Threaded)
{
    ETCS_MAKE_INSTANCE(Threaded)

    // True if THIS call placed the request, false if one was already standing.
    bool Halt() override final
    {
        return !m_halted.exchange(true, std::memory_order_acq_rel);
    }

    bool Halted() const override
    {
        return m_halted.load(std::memory_order_acquire);
    }

    ETCS::WorkShape Shape() const override { return ETCS::WorkShape::Held; }

    // Refused, because owning a body is not being an actor. A Thread overrides
    // this (ontology/ThreadBase.h); everything else that merely has a loop --
    // VulkanSurface's frame producer, say -- correctly says no. Same shape as
    // Resizable_::ResizeTo defaulting false.
    uint64_t Detach(const ETCS::Buffer&) override { return 0; }

private:
    std::atomic<bool> m_halted{false};
};

#endif
