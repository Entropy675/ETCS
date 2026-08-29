#ifndef BASE_Deletable_H__
#define BASE_Deletable_H__
#include "Deletable.h"

ETCS_SUPERTYPE_BASE(Deletable)
{
    ETCS_MAKE_INSTANCE(Deletable)
    ETCS_DISPATCH_METHOD(bool, Delete);
};

#endif
