#ifndef BASE_GATE_H__
#define BASE_GATE_H__
#include "Gate.h"

ETCS_SUPERTYPE_BASE(Gate)
{
    ETCS_MAKE_INSTANCE(Gate)
    ETCS_DISPATCH_METHOD(       bool, Open, (const ETCS::Buffer&, config));
    ETCS_DISPATCH_METHOD(       bool, Close);
    ETCS_DISPATCH_METHOD_CONST( bool, IsOpen);
};

#endif // BASE_GATE_H__
