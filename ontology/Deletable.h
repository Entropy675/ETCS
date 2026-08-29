#ifndef SUPERTYPE_DELETABLE_H__
#define SUPERTYPE_DELETABLE_H__


#include "../core_defs.h"

class Deletable_ : virtual public ETCS::Entity
{
public:
    virtual ~Deletable_() = default;
    virtual bool Delete() = 0;
};

#endif // SUPERTYPE_DELETABLE_H__
