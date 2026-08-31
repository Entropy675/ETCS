#ifndef BASE_RESIZABLE_H__
#define BASE_RESIZABLE_H__
#include "Resizable.h"

ETCS_SUPERTYPE_BASE(Resizable)
{
    ETCS_MAKE_INSTANCE(Resizable)
    ETCS_DISPATCH_METHOD(WindowSize, GetSize);
};

#endif
