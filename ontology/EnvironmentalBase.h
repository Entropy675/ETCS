#ifndef BASE_ENVIRONMENTAL_H__
#define BASE_ENVIRONMENTAL_H__
#include "Environmental.h"

// Claiming the family is what turns provenance on for the entity: from
// construction, every action that changes its tag surface is recorded
// against it (Entity::markEnvironmental, core/Provenance.h).
//
// CaptureState and MigrateTo default to "nothing off the surface" and "no
// renames"; RebuildLocal and ReflectRemote are the type's to say, because
// how an entity comes back -- here, or as a reflection -- is its own.
ETCS_SUPERTYPE_BASE(Environmental)
{
    ETCS_MAKE_INSTANCE(Environmental)
    ETCS_DISPATCH_METHOD(bool, RebuildLocal,  (const ETCS::EnvironmentState&, state));
    ETCS_DISPATCH_METHOD(bool, ReflectRemote, (const ETCS::EnvironmentState&, state));

    void CaptureState(ETCS::EnvironmentState& out) const override
    { static_cast<const Derived*>(this)->CaptureStateConcrete(out); }
    std::string MigrateTo() const override
    { return static_cast<const Derived*>(this)->MigrateToConcrete(); }

    void        CaptureStateConcrete(ETCS::EnvironmentState&) const {}
    std::string MigrateToConcrete() const { return {}; }

    struct MarkEnvironmental { explicit MarkEnvironmental(ETCS::Entity* e) { e->markEnvironmental(); } };
    MarkEnvironmental mark_environmental_{ this };
};

#endif // BASE_ENVIRONMENTAL_H__
