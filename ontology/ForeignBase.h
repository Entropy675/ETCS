#ifndef BASE_FOREIGN_H__
#define BASE_FOREIGN_H__
#include "Foreign.h"
#include "RelationalBase.h"

#include <atomic>

// Lineage in the base: RelationalBase is reached once, so Relational_ is in
// the object once (the rule Drawable.h and Thread.h state).
//
// The family answers the reference: BindStandIn resolves the RID, refuses an
// entity that doesn't land (Targets), and keeps the RID; StandIn reads it.
// Apply narrows Relational's: besides landing, the target must BE the bound
// stand-in, so a leaf's ApplyConcrete never sees anything else.
ETCS_SUPERTYPE_BASE(Foreign), public RelationalBase<Derived>
{
    ETCS_MAKE_INSTANCE(Foreign)
    ETCS_DISPATCH_METHOD_CONST( std::string, Speaks);
    ETCS_DISPATCH_METHOD(       bool, Exchange,
        (const std::string&, question), (std::string&, answer), (ETCS::SignalContext, ctx));

    bool BindStandIn(ETCS::RID stand_in) override final
    {
        ETCS::Entity* e = stand_in ? ETCS::etcs_resolve_rid_anywhere(&ETCS::getLoader(), stand_in) : nullptr;
        if (!e || !this->LandsOn(*e)) return false;
        m_stand_in.store(stand_in, std::memory_order_release);
        return true;
    }
    ETCS::RID StandIn() const override final { return m_stand_in.load(std::memory_order_acquire); }

    bool Apply(ETCS::Entity& target, ETCS::SignalContext ctx) override final
    {
        const ETCS::RID bound = StandIn();
        if (bound == 0 || target.getRID() != bound || !this->LandsOn(target)) return false;
        return static_cast<Derived*>(this)->ApplyConcrete(target, ctx);
    }

private:
    std::atomic<ETCS::RID> m_stand_in{0};
};

#endif // BASE_FOREIGN_H__
