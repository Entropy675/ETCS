#ifndef BASE_DIRECTORY_H__
#define BASE_DIRECTORY_H__
#include "Directory.h"

ETCS_SUPERTYPE_BASE(Directory)
{
    ETCS_MAKE_INSTANCE(Directory)
    ETCS_DISPATCH_METHOD(bool, Advertise,
        (const std::string&, name), (const std::string&, kind), (const std::string&, info));
    ETCS_DISPATCH_METHOD(bool, Withdraw, (const std::string&, name));
    ETCS_DISPATCH_METHOD_CONST(std::string, Listing, (const std::string&, kind));
};

#endif // BASE_DIRECTORY_H__
