#ifndef SUPERTYPE_REMOTEDATABASE_H__
#define SUPERTYPE_REMOTEDATABASE_H__


#include "../core_defs.h"
#include "Database.h"

class RemoteDatabase_ : public Database_
{
public:
    RemoteDatabase_() {} ;
    virtual ~RemoteDatabase_() = default;
    virtual void DisconnectRemote() = 0;
    //virtual bool connectRemote(ETCS::MirrorBuffer& data) = 0;
};

#endif
