#ifndef BASE_SWITCHABLE_H__
#define BASE_SWITCHABLE_H__
#include "Switchable.h"

ETCS_SUPERTYPE_BASE(Switchable)
{
    ETCS_MAKE_INSTANCE(Switchable)
    ETCS_DISPATCH_METHOD(       bool, Start);
    ETCS_DISPATCH_METHOD(       bool, Stop);
    ETCS_DISPATCH_METHOD_CONST( bool, IsStarted);
};

#endif // BASE_SWITCHABLE_H__
