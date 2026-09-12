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

    /*
 * True if THIS call placed the request, false if one was already standing.
 *
 * THE REQUEST IS RECORDED AS A TAG, because the tag surface is the state
 * surface. HALTING is a transition on it -- the tag "halted" means a stop has
 * been ASKED FOR and the body has not finished leaving; "stopped" replaces it
 * when the body says it has (Stop, below). Two tags because they are two
 * facts, and one of them used to be unrepresentable: an entity asked to stop
 * and an entity that had stopped both read as "halted", so nothing could tell
 * a wind-down from a finished one. It belongs on the tag surface for the same
 * reason a flag does -- and putting it there is what makes it observable, with
 * no Observable code here at all: addTag routes through Entity::tagModifyImpl,
 * which marks the nearest claimant at or above this entity
 * (Entity::markStateChange). A Threaded type that also claims Observable gets
 * that for free; one that does not loses nothing.
 *
 * THE LATCH IS STILL THE ATOMIC, and it is what Halted() reads. Two reasons,
 * both load-bearing:
 *
 *   the exchange is the claim. "Did I place this request" has to be answered
 *   atomically for three genuinely concurrent callers (the arena's retire, a
 *   script's Delete, the body's own poll), and it has to be answered before
 *   the tag write, which can be refused.
 *
 *   Halted() is polled from frame loops -- VulkanSurface::Retired() is the
 *   first question ProduceFrames and ConsumeFrames ask on every tick. That is
 *   an atomic load; hasTag is a mutex and a map lookup.
 *
 * So the flag is the RECORD and the atomic is the LATCH, written together on
 * the one winning transition. Removing the flag afterwards does not un-halt
 * anything -- a halt is one-way, and the flag is a statement about this entity
 * rather than the control input, exactly as removing a family marker does not
 * remove the family.
 *
 * ORDERING-THREAD FALLBACK, and it is not optional. addTag emits a
 * TagModifyEvent and blocks on it, so calling it from an ordering thread
 * deadlocks against the very thread that would service it --
 * ETCS_ASSERT_NOT_ORDERING_THREAD says so, and etcs_retire_entity reaches Halt
 * from exactly there (destroyImpl -> deleteEntity -> reclaimEntity). Same for
 * a live lifetime hold. In those cases the tag is skipped and the mark is made
 * directly, so the observable guarantee holds unconditionally even where the
 * record cannot be written.
 */
    bool Halt() override final
    {
        if (m_halted.exchange(true, ::std::memory_order_acq_rel)) return false;
        ETCS::Entity* self = static_cast<Derived*>(this);
        if (!ETCS::EventNode::on_ordering_thread && ETCS::lifetime_hold_depth == 0)
            self->addTag(ETCS::Buffer("halted"));
        else
            ETCS::Entity::markStateChange(self, self->getRID());
        return true;
    }

    bool Halted() const override
    {
        return m_halted.load(::std::memory_order_acquire);
    }

    /*
 * THE BODY SAYING IT HAS LEFT -- see Threaded.h on why that is a separate
 * fact from having been asked to.
 *
 * REPLACES the tag rather than adding beside it: "halted" is the transition
 * and "stopped" is the destination, so an entity carrying both would be
 * claiming to be mid-wind-down and finished at once. That is the one shape
 * the pair exists to make impossible.
 *
 * IDEMPOTENT, and true only for the caller that made the transition -- the
 * same claim-by-exchange Halt uses, and for the same reason: several bodies
 * under one entity can finish concurrently and only one of them is the one
 * that stopped it.
 *
 * DOES NOT REQUIRE A HALT FIRST. A body that finishes its own work and leaves
 * was never asked to stop and is no less stopped for it; removeTag on a tag
 * that was never written is a no-op, so the ordinary case costs a lookup.
 *
 * SAME ORDERING-THREAD FALLBACK as Halt, and it is not optional there either.
 *
 * final, like Halt: the latch and the tag transition are the family's to keep
 * consistent, and a leaf overriding half of it is how they come apart.
 */
    bool Stop() override final
    {
        if (m_stopped.exchange(true, ::std::memory_order_acq_rel)) return false;
        ETCS::Entity* self = static_cast<Derived*>(this);
        if (!ETCS::EventNode::on_ordering_thread && ETCS::lifetime_hold_depth == 0)
        {
            self->removeTag(ETCS::Buffer("halted"));
            self->addTag(ETCS::Buffer("stopped"));
        }
        else
            ETCS::Entity::markStateChange(self, self->getRID());
        return true;
    }

    bool Stopped() const override final
    {
        return m_stopped.load(::std::memory_order_acquire);
    }

    ETCS::WorkShape Shape() const override { return ETCS::WorkShape::Held; }

    // Refused, because owning a body is not being an actor. A Thread overrides
    // this (ontology/ThreadBase.h); everything else that merely has a loop --
    // VulkanSurface's frame producer, say -- correctly says no. Same shape as
    // Resizable_::ResizeTo defaulting false.
    uint64_t Detach(const ETCS::Buffer&) override { return 0; }

    // No signal authority either, and for the same reason. See IWireThread.
    ETCS::SignalContext Signals() override { return ETCS::SignalContext{}; }

private:
    ::std::atomic<bool> m_halted{false};
    // The destination, latched separately from the request -- an entity can be
    // asked and not yet gone, or gone without ever being asked.
    ::std::atomic<bool> m_stopped{false};
};

#endif
