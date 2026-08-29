#ifndef SUPERTYPE_GATE_H__
#define SUPERTYPE_GATE_H__


#include "../core_defs.h"

class Gate_ : virtual public ETCS::Entity
{
public:
    virtual ~Gate_() = default;
    virtual bool Open(const ETCS::Buffer& config) = 0;
    virtual bool Close() = 0;
    virtual bool IsOpen() const = 0;
};

#endif // SUPERTYPE_Gate_H__
