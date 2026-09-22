#ifndef BASE_Deletable_H__
#define BASE_Deletable_H__
#include "Deletable.h"
#include <atomic>

// The once-only guard lives here rather than in the leaves, same as
// LifecycleBase's: a rule each implementor has to remember is one that will be
// forgotten, and this one is called from a teardown path where a second caller
// is normal.
//
// It is also what lets Release call Delete blindly -- without it, every entity a
// script deleted would get a second destroy on reclaim.
//
// Delete() is final and the leaf writes DeleteConcrete(), which is the shape
// ETCS_DISPATCH_METHOD already generated, so no leaf changes.
//
// The flag latches BEFORE the call, not after. That makes re-entrancy terminate
// (DeleteConcrete fires a DestroyEvent -> reclaim -> Release -> Delete again,
// which must find the door shut). The cost is that a failed DeleteConcrete
// cannot be retried, which is right here: the failure it returns is a refused
// enqueue onto a stream already leaving, and that is terminal.
ETCS_SUPERTYPE_BASE(Deletable)
{
    ETCS_MAKE_INSTANCE(Deletable)

    // Kept from ETCS_DISPATCH_METHOD(bool, Delete): the leaf's own body, still
    // required of every implementor by the pure virtual.
    virtual bool DeleteConcrete() = 0;

    // Returns what the destroy answered on the call that ran it, false after.
    // Both mean the same to a caller: you did not delete this.
    bool Delete() override final
    {
        if (m_deleted.exchange(true, ::std::memory_order_acq_rel)) return false;
        // Release first, if this type has one: Delete is Release plus a destroy
        // request, so both death paths run one ReleaseConcrete exactly once.
        //
        // Only this direction works. Release calling Delete deadlocks -- Delete
        // blocks on the loader to destroy this RID, and the arena's reclaim is
        // what calls Release. Reproduced; see LifecycleBase.
        if (auto* l = dynamic_cast<ETCS::IWireLifecycle*>(static_cast<Derived*>(this)))
            l->Release();
        return static_cast<Derived*>(this)->DeleteConcrete();
    }

    // Has the request path run? Release reads it; so may a work function that
    // would otherwise act on something already destroyed.
    bool Deleted() const { return m_deleted.load(::std::memory_order_acquire); }

private:
    ::std::atomic<bool> m_deleted{false};
};

#endif
