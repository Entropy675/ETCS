#ifndef TRUST_GATE_H__
#define TRUST_GATE_H__

/*
 * The entity whose Wrapper children stand on the module gate (core/Trust.h),
 * outside the loader's own stages: gate_entity()->addTag<T>(), off the
 * ordering thread like any addTag. Made the first time one is wanted; a
 * loader that never asks has none.
 *
 * Its own header, included last (core_defs.h): an entity type needs
 * WIRE_TYPE_IDENTITY, and every other core header is read before ETCS_API.h
 * reaches it.
 */
#include "Trust.h"

namespace ETCS
{
namespace trust
{
class GateNode : public Entity
{
public:
    WIRE_TYPE_IDENTITY(GateNode)
};

inline Entity* gate_entity()
{
    static std::mutex m;
    std::lock_guard<std::mutex> l(m);
    Gate& g = gate();
    if (!g.entity.load()) g.entity.store(MemoryArena::getInstance().allocate<GateNode>());
    return g.entity.load();
}
} // namespace trust
} // namespace ETCS

#endif // TRUST_GATE_H__
