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
        if (m_released.exchange(true, std::memory_order_acq_rel)) return false;
        static_cast<Derived*>(this)->ReleaseConcrete();
        return true;
    }

    bool Released() const override
    {
        return m_released.load(std::memory_order_acquire);
    }

    // Marked by the type's own Create, on success and nowhere else. Release
    // reads it to know whether there is anything to undo.
    void Establish()          { m_established = true; }
    bool Established() const override { return m_established; }

private:
    std::atomic<bool> m_released{false};
    bool              m_established = false;
};

#endif
