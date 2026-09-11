#ifndef BASE_WRAPPER_H__
#define BASE_WRAPPER_H__
#include "Wrapper.h"

// The locality rule rides in a private empty base -- see ETCS::WrapperIsLocal
// (Wrapper.h) for why it cannot be a static_assert in this body, and how that
// went unnoticed until the family had its first concrete leaf. AFTER Wrapper_,
// so Wrapper_ stays the first non-virtual base and the offset-0 pointer
// identity MirrorBuffer depends on is untouched.
ETCS_SUPERTYPE_BASE(Wrapper), private ETCS::WrapperIsLocal<Derived>
{
    ETCS_MAKE_INSTANCE(Wrapper)
    ETCS_DISPATCH_METHOD(       void, Wrap,      (ETCS::MBuffer&, io), (ETCS::SignalContext, ctx));
    ETCS_DISPATCH_METHOD(       void, Unwrap,    (ETCS::MBuffer&, io), (ETCS::SignalContext, ctx));
    ETCS_DISPATCH_METHOD(       void, Close,     (ETCS::MBuffer&, io), (ETCS::SignalContext, ctx));
    ETCS_DISPATCH_METHOD_CONST( ETCS::WireScope, Scope);
};
#endif
