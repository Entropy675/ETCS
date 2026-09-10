#ifndef SUPERTYPE_WRAPPER_H__
#define SUPERTYPE_WRAPPER_H__


#include "../core_defs.h"

// Wrapper_ — family interface for anything that transforms bytes crossing
// a MirrorBuffer boundary in place: framing, masking, encryption,
// compression. Wrap = this entity's own logical payload -> wire bytes;
// Unwrap = wire bytes -> logical payload. In-place on the same Buffer,
// matching every other in-place mutation convention here (WORK_FUNC_TYPED's
// own by-reference OUT fields) rather than returning a new one.
namespace ETCS
{
/*
 * The locality rule, in a base rather than in WrapperBase's own body.
 *
 * WrapperBase<Derived> is instantiated as part of a leaf's BASE CLAUSE, where
 * Derived is still being defined -- so a static_assert in the class body has to
 * complete an incomplete type and does not compile. That went unnoticed because
 * nothing had ever claimed the family: the Wrapper base did not compile the
 * first time a concrete wrapper existed.
 *
 * A destructor body is instantiated when the leaf is destroyed, by which point
 * Derived is complete, so the check still happens at compile time and still
 * happens for every leaf. Empty, so it costs nothing, and declared AFTER
 * Wrapper_ in WrapperBase's base list so Wrapper_ stays the first non-virtual
 * base -- the offset-0 relationship MirrorBuffer reinterprets depends on it.
 */
template <typename D>
struct WrapperIsLocal
{
    ~WrapperIsLocal()
    {
        static_assert(!ETCS::IsRemote<D>::value,
            "Wrapper_-derived types must never be Remote -- wrapping happens "
            "local to the wire it's about to cross, never across another hop.");
    }
};
}

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
