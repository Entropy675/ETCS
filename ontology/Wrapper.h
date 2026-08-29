#ifndef SUPERTYPE_WRAPPER_H__
#define SUPERTYPE_WRAPPER_H__


#include "../core_defs.h"

// Wrapper_ — family interface for anything that transforms bytes crossing
// a MirrorBuffer boundary in place: framing, masking, encryption,
// compression. Wrap = this entity's own logical payload -> wire bytes;
// Unwrap = wire bytes -> logical payload. In-place on the same Buffer,
// matching every other in-place mutation convention here (WORK_FUNC_TYPED's
// own by-reference OUT fields) rather than returning a new one.
class Wrapper_ : 
    public ETCS::IWireWrapper, virtual public ETCS::Entity 
{
public:
    virtual ~Wrapper_() = default;
    // Wrapper is the only special case right now in ETCS, because MirrorBuffer wants to know:
    // virtual void Wrap(ETCS::MBuffer& data, ETCS::SignalContext ctx)   = 0;
    // virtual void Unwrap(ETCS::MBuffer& data, ETCS::SignalContext ctx) = 0;
    // virtual ETCS::WireScope Scope() const = 0;
    // thus they are defined within IWireWrapper before MirrorBuffer needs that shape.
};
#endif
