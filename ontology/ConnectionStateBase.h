#ifndef BASE_CONNECTIONSTATE_H__
#define BASE_CONNECTIONSTATE_H__
#include "ConnectionState.h"

ETCS_SUPERTYPE_BASE(ConnectionState)
{
    ETCS_MAKE_INSTANCE(ConnectionState)
    ETCS_DISPATCH_METHOD_CONST( int,        GetClientFd);
    ETCS_DISPATCH_METHOD(       void,       SetClientFd, (int, fd));
    ETCS_DISPATCH_METHOD_CONST( ETCS::RID,  GetPageRID);
    ETCS_DISPATCH_METHOD(       void,       SetPageRID,  (ETCS::RID, rid));
    ETCS_DISPATCH_METHOD_CONST( bool,       IsConnectionOpen);
};
#endif // BASE_CONNECTIONSTATE_H__
