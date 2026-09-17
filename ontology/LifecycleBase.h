#ifndef BASE_LIFECYCLE_H__
#define BASE_LIFECYCLE_H__
#include "Lifecycle.h"

// The once-only guarantee lives HERE, not in the leaves, and that is the whole
// reason this base exists rather than the family being a bare interface.
//
// Release() is `final`. A leaf writes ReleaseConcrete() and cannot get at the
// guard to weaken it, forget it, or reimplement it slightly differently. If the
// protection were a rule each implementor followed, the first type written in a
// hurry would be the one that runs its teardown twice -- and a double release
// is exactly the failure this family was added to prevent, so leaving it to
// discipline would be building the mechanism and then not using it.
//
// THE FLAG IS ATOMIC because the two entry points are genuinely concurrent. A
// script's Delete runs on the script thread; the arena's reclaim runs wherever
// the closure ended, which may be a pool worker draining a detached edge. The
// exchange is the arbitration: whichever thread gets `false` back does the work
// and the other returns immediately, with no lock and nothing to order.
//
// ESTABLISHED IS SEPARATE AND NOT ATOMIC. It is written once, by the type's own
// Create, on the thread that constructed the object, before anything else can
// reach it -- so there is no race to protect against, and making it atomic
// would suggest one exists. Release is expected to check it: a type whose
// Create failed halfway has nothing to let go of, and touching half-built state
// is how a cleanup path becomes a second crash.
ETCS_SUPERTYPE_BASE(Lifecycle)
{
    ETCS_MAKE_INSTANCE(Lifecycle)

    /*
 * The guard, and the only place the leaf's teardown is reached from.
 *
 * Returns true if THIS call did the work. Both the arena and an explicit
 * Delete call it; the loser gets false and can say so rather than logging a
 * release that did not happen.
 *
 * acq_rel because the winner's writes -- everything ReleaseConcrete does --
 * must be visible to whatever observes Released() afterwards, and the loser
 * must not proceed on the assumption that nothing has happened yet.
 */
    bool Release() override final
    {
        if (m_released.exchange(true, ::std::memory_order_acq_rel)) return false;
        static_cast<Derived*>(this)->ReleaseConcrete();
        // Delete is NOT called from here, and the obvious version of that hangs:
        //
        //     DestroyEvent::operator()  <-  VulkanSurface::DeleteConcrete
        //                               <-  LifecycleBase<VulkanSurface>::Release
        //
        // Delete's body is a blocking request to the loader to destroy this RID,
        // and Release runs from inside the arena walk already destroying it, so
        // the event waits on this very thread. Delete calls Release instead --
        // Release is local and returns.
        //
        /*
         * RELEASING IS A STATE CHANGE, and this is the funnel for it.
         *
         * `override final` and gated on an exchange, so every path that ends a
         * type's hold on what it owns -- DeletableBase::Delete, and
         * etcs_retire_entity's own Release call -- runs this exactly once. That
         * makes it the lifecycle counterpart to tagModifyImpl and addTagImpl:
         * anything observing this entity has just gone out of date, whether or
         * not the entity goes on to leave the tree.
         *
         * From the entity itself, not its parent: what changed is this, and it
         * is still whole here -- etcs_retire_entity's mark starts at the parent
         * only because it fires once the entity has been unlinked. The two are
         * complementary rather than duplicate; MarkObserved is idempotent
         * between reads, so a path taking both costs one extra fetch_or.
         *
         * After ReleaseConcrete, so an observer woken here sees the released
         * state rather than the one being torn down.
         */
        ETCS::Entity::markStateChange(static_cast<Derived*>(this),
                                      static_cast<Derived*>(this)->getRID());
        return true;
    }

    bool Released() const override
    {
        return m_released.load(::std::memory_order_acquire);
    }

    // Marked by the type's own Create, on success and nowhere else. Release
    // reads it to know whether there is anything to undo.
    void Establish()          { m_established = true; }
    bool Established() const override { return m_established; }

private:
    ::std::atomic<bool> m_released{false};
    bool              m_established = false;
};

#endif
