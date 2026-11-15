#ifndef BASE_RELATIONAL_H__
#define BASE_RELATIONAL_H__
#include "Relational.h"

// LandsOn is the family's, final: every space-separated tag in Targets() must
// be on the target (Entity::hasTag routes flags and type tags itself). A leaf
// that could answer it differently would make Targets a description rather
// than the constraint. Apply is written out, not left to the dispatch macro,
// so the landing check is the family's: ApplyConcrete never sees a target the
// relation doesn't land on.
ETCS_SUPERTYPE_BASE(Relational)
{
    ETCS_MAKE_INSTANCE(Relational)
    ETCS_DISPATCH_METHOD_CONST( std::string, Targets);

    virtual bool ApplyConcrete(ETCS::Entity& target, ETCS::SignalContext ctx) = 0;
    bool Apply(ETCS::Entity& target, ETCS::SignalContext ctx) override
    {
        if (!LandsOn(target)) return false;
        return static_cast<Derived*>(this)->ApplyConcrete(target, ctx);
    }

    bool LandsOn(const ETCS::Entity& target) const override final
    {
        const std::string tags = static_cast<const Derived*>(this)->TargetsConcrete();
        size_t at = 0;
        bool any = false;
        while (at < tags.size())
        {
            size_t end = tags.find(' ', at);
            if (end == std::string::npos) end = tags.size();
            if (end > at)
            {
                any = true;
                if (!target.hasTag(ETCS::Buffer(tags.substr(at, end - at)))) return false;
            }
            at = end + 1;
        }
        return any;   // a relation that names nothing lands on nothing
    }
};

#endif // BASE_RELATIONAL_H__
