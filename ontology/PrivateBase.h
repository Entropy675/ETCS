#ifndef BASE_PRIVATE_H__
#define BASE_PRIVATE_H__
#include "Private.h"

ETCS_SUPERTYPE_BASE(Private)
{
    ETCS_MAKE_INSTANCE(Private)
    ETCS_DISPATCH_METHOD_CONST( bool, Holds);
    ETCS_DISPATCH_METHOD(       void, Wipe);
    ETCS_DISPATCH_METHOD_CONST( bool, Reveal,  (std::string&, out));
    ETCS_DISPATCH_METHOD(       bool, Receive, (const std::string&, in));
};

#endif // BASE_PRIVATE_H__
