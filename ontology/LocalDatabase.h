#ifndef SUPERTYPE_LOCALDATABASE_H__
#define SUPERTYPE_LOCALDATABASE_H__


#include "../core_defs.h"
#include "Database.h"

class LocalDatabase_ : public Database_
{
public:
    LocalDatabase_() {} ;
    virtual ~LocalDatabase_() = default;
};


#endif
