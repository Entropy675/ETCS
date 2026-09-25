#ifndef BASE_RECORD_H__
#define BASE_RECORD_H__
#include "Record.h"

ETCS_SUPERTYPE_BASE(Record)
{
    ETCS_MAKE_INSTANCE(Record)
    ETCS_DISPATCH_METHOD(uint64_t, Append, (const std::string&, author), (const std::string&, line));
    ETCS_DISPATCH_METHOD_CONST(std::string, Head);
    ETCS_DISPATCH_METHOD_CONST(std::string, Since, (uint64_t, seq), (size_t, budget));
};

#endif // BASE_RECORD_H__
