#ifndef BASE_EPHEMERAL_H__
#define BASE_EPHEMERAL_H__
#include "Ephemeral.h"

ETCS_SUPERTYPE_BASE(Ephemeral)
{
    ETCS_MAKE_INSTANCE(Ephemeral)
    ETCS_DISPATCH_METHOD(       bool, Reset);
    ETCS_DISPATCH_METHOD_CONST( bool, IsActive);
};

#endif // BASE_EPHEMERAL_H__
