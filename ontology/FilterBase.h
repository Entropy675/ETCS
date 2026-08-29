#ifndef BASE_FILTER_H__
#define BASE_FILTER_H__
#include "Filter.h"
ETCS_SUPERTYPE_BASE(Filter)
{
    ETCS_MAKE_INSTANCE(Filter)
    ETCS_DISPATCH_METHOD_CONST( bool, Accepts, (ETCS::Buffer&, io));
};
#endif // BASE_FILTER_H__
