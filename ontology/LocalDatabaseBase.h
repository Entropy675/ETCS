#ifndef BASE_LOCALDATABASE_H__
#define BASE_LOCALDATABASE_H__
#include "LocalDatabase.h"

ETCS_SUPERTYPE_BASE(LocalDatabase)
{
    ETCS_MAKE_INSTANCE(LocalDatabase)
    ETCS_DISPATCH_METHOD(void, CloseConnection);
    ETCS_DISPATCH_METHOD(void, CreateConnection, (const ETCS::Buffer&,  db));
    ETCS_DISPATCH_METHOD(bool, ExecuteRaw,       (ETCS::Buffer&,        data));
    ETCS_DISPATCH_METHOD(bool, InitializeSchema, (const ETCS::Buffer&,  schema));
};
#endif
