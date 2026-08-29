#ifndef SUPERTYPE_SWITCHABLE_H__
#define SUPERTYPE_SWITCHABLE_H__
#include "../core_defs.h"

// Switchable_ — a marker for anything with an externally-toggleable on/off
// causal state. A server, a scheduler, a poller, a recording session:
// anything that can be turned on, turned off, and asked which it currently is.
// Dumb type - a simple causal shape, for something that manages a boundary, see Gate/GateBase
class Switchable_ : virtual public ETCS::Entity
{
public:
    virtual ~Switchable_() = default;

    virtual bool Start() = 0;
    virtual bool Stop()  = 0;
    virtual bool IsStarted() const = 0;
};

#endif // SUPERTYPE_SWITCHABLE_H__
