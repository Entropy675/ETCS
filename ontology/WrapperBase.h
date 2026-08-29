#ifndef BASE_WRAPPER_H__
#define BASE_WRAPPER_H__
#include "Wrapper.h"

ETCS_SUPERTYPE_BASE(Wrapper)
{
    // A Wrapper_-derived type must never be Remote: wrapping happens
    // local to the wire it's about to cross, never across another hop.
    static_assert(!ETCS::IsRemote<Derived>::value,
        "Wrapper_-derived types must never be Remote -- wrapping happens "
        "local to the wire it's about to cross, never across another hop.");

    ETCS_MAKE_INSTANCE(Wrapper)
    ETCS_DISPATCH_METHOD(       void, Wrap,      (ETCS::MBuffer&, io), (ETCS::SignalContext, ctx));
    ETCS_DISPATCH_METHOD(       void, Unwrap,    (ETCS::MBuffer&, io), (ETCS::SignalContext, ctx));
    ETCS_DISPATCH_METHOD(       void, Close,     (ETCS::MBuffer&, io), (ETCS::SignalContext, ctx));
    ETCS_DISPATCH_METHOD_CONST( ETCS::WireScope, Scope);
};
#endif
