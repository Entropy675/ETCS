#ifndef BASE_PARSER_H__
#define BASE_PARSER_H__
#include "Parser.h"

ETCS_SUPERTYPE_BASE(Parser)
{
    ETCS_MAKE_INSTANCE(Parser)
    ETCS_DISPATCH_METHOD(void, Parse, (ETCS::MirrorBuffer&, io), (ETCS::SignalContext, ctx));
    ETCS_DISPATCH_METHOD(bool, Reset);
};
#endif
