#ifndef SUPERTYPE_EPHEMERAL_H__
#define SUPERTYPE_EPHEMERAL_H__


#include "../core_defs.h"

class Ephemeral_ : virtual public ETCS::Entity
{
public:
    virtual ~Ephemeral_() = default;
    virtual bool Reset() = 0;
    virtual bool IsActive() const = 0;
};

#endif // SUPERTYPE_EPHEMERAL_H__
