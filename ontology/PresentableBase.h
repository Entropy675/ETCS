#ifndef BASE_PRESENTABLE_H__
#define BASE_PRESENTABLE_H__
#include "Presentable.h"

ETCS_SUPERTYPE_BASE(Presentable)
{
    ETCS_MAKE_INSTANCE(Presentable)
    ETCS_DISPATCH_METHOD(void, Present);
};

#endif
