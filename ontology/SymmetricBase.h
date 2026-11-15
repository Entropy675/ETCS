#ifndef BASE_SYMMETRIC_H__
#define BASE_SYMMETRIC_H__
#include "Symmetric.h"
#include "RelationalBase.h"

// Lineage in the base (MatrixBase's pattern): RelationalBase is reached once.
// Invert is written out for the same reason Apply is: the landing check is the
// family's, so InvertConcrete only ever sees a target Apply could have seen.
ETCS_SUPERTYPE_BASE(Symmetric), public RelationalBase<Derived>
{
    ETCS_MAKE_INSTANCE(Symmetric)
    virtual bool InvertConcrete(ETCS::Entity& target, ETCS::SignalContext ctx) = 0;
    bool Invert(ETCS::Entity& target, ETCS::SignalContext ctx) override
    {
        if (!this->LandsOn(target)) return false;
        return static_cast<Derived*>(this)->InvertConcrete(target, ctx);
    }
    ETCS_DISPATCH_METHOD_CONST( std::string, Algebra);
};

#endif // BASE_SYMMETRIC_H__
