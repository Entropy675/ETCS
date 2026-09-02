#ifndef SUPERTYPE_LIFECYCLE_H__
#define SUPERTYPE_LIFECYCLE_H__


#include "../core_defs.h"
#include "../core/InterfaceWire.h"
#include <atomic>

// ---------------------------------------------------------------------------
// Lifecycle — a guaranteed, exactly-once release, reachable from both ends.
//
// THE PROBLEM IS NOT DELETION, IT IS THAT THERE ARE TWO WAYS TO DIE. A script
// says X.Delete(); or the closure that made X ends and the arena reclaims it
// (MemoryArena::destroyChildEntitiesFirst). Both are legitimate, either can
// happen first, and until now only the first one ran any of the type's own
// letting-go. So a surface reclaimed by a closure kept a compose root bound, an
// input consumer kept a reference to an entity that no longer existed, and a
// frame edge kept walking a tree being torn down underneath it. None of those
// are bugs in the types; they are the absence of a slot.
//
// SO THE SLOT IS THE FAMILY, and its whole content is: a call that happens
// exactly once, before the memory goes, whichever path got there first.
//
// WHY NOT THE DESTRUCTOR, which is already exactly-once and already ordered?
// Because of WHEN it runs. By the time ~T() executes the object is already
// coming apart -- bases are destructing under it, its vtable is being walked
// back down, and the entity graph around it is in whatever state the cascade
// has reached. Release runs while the object is still whole and the graph is
// still intact, which is the only moment it can do the things it needs to do:
// resolve other entities by RID, unbind itself from something holding it, tell
// a stream to stop. A destructor cannot safely call back into the graph, and
// every job on that list is a call back into the graph.
//
// ---------------------------------------------------------------------------
// WHAT THIS FAMILY DELIBERATELY DOES NOT CARRY: Create.
//
// Pairing Create with Release is the obvious shape and I think it is the wrong
// one, for two reasons.
//
// The first is mechanical: construction has no uniform signature. Surface's
// Create takes an instance and a shader path, Scene3D's takes three extents,
// Camera3D's takes a width and a height. A family method has one signature, so
// a Create constraint either forces a shape none of them have or degenerates
// into a no-argument hook that duplicates a work function every type already
// publishes. Neither earns a row in the dispatch set.
//
// The second is that the two halves are not the same KIND of problem. Release
// is hard because it has two entry points, no ordering guarantee between them,
// and a correctness requirement that it happen exactly once. Construction has
// one entry point -- the script calls it -- and no such requirement. Putting
// them in one family makes the half that needs no help carry the constraints of
// the half that does.
//
// WHAT IS WORTH KEEPING from the constructive side is not the call, it is the
// STATE: whether the thing ever came up. A release running against a Create
// that failed halfway is a real hazard -- half-null Vulkan handles, a buffer
// never allocated -- and it is a question with one answer for every type. So
// the family carries Established() and a Establish() to set it, and the type's
// own Create calls that on success. One bit, no signature, and it is the bit
// Release actually needs.
// ---------------------------------------------------------------------------
//
// IT IS A UNIQUENESS TRAIT, mechanically, exactly as Orderable is
// (SurfaceBase.h). LifecycleBase is composed non-virtually, so two bases in one
// lineage both claiming it give the leaf two subobjects and every call through
// them is ambiguous -- a compile error, not a silent second teardown. Exactly
// one place in a lineage may own a type's release, which is the same thing as
// saying a thing dies once.
//
// AND IT IS NOT Deletable. The two are different questions and both are worth
// having:
//
//   Deletable::Delete()   a REQUEST, from outside: destroy this entity.
//                         Optional -- plenty of things cannot be deleted on
//                         demand -- and it may be refused.
//   Lifecycle::Release()  a NOTIFICATION, from the runtime: you are being
//                         destroyed, let go of what you hold. Guaranteed, not
//                         refusable, and it runs whether or not anyone asked.
//
// So a type that claims both has Delete call Release on its way to firing the
// DestroyEvent, and the arena calls Release regardless of whether Deletable was
// ever claimed. Whichever arrives first does the work; the second finds it
// already done and returns false. That is the "double call protection" the
// whole design turns on, and it lives in the base rather than in each leaf --
// a rule every implementor has to remember is a rule that will be forgotten.

// ETCS::IWireLifecycle FIRST and NON-VIRTUAL, which is load-bearing rather
// than stylistic: MemoryArena reinterprets the interface pointer this family
// registers as an IWireLifecycle*, and that is exact only because a first
// non-virtual base sits at offset 0. Wrapper_ carries the identical
// requirement for the identical reason -- see core/InterfaceWire.h, where the
// rule is stated once for every wire rather than rediscovered at each use.
class Lifecycle_ : public ETCS::IWireLifecycle, virtual public ETCS::Entity
{
public:
    virtual ~Lifecycle_() = default;

    /*
 * Let go of everything this type holds that outlives its own memory.
 *
 * Returns true if THIS call did the work, false if it had already been done.
 * The return is not decoration: it is how a caller distinguishes "I released
 * it" from "something else already had", which is the difference between a
 * teardown log that reads as a sequence and one that reads as a race.
 *
 * Called with the object whole and the graph intact, so it may resolve other
 * entities, unbind itself from things holding it, and stop streams. It must
 * not delete itself -- that is Deletable's job and this is not that question.
 */
    bool Release() override = 0;

    // Has Release already run? A destructor may want to know, and so may a
    // work function that would otherwise operate on a released object.
    bool Released() const override = 0;

    // Did construction ever complete? See this file's header on why the
    // constructive half of the contract is a bit rather than a call.
    bool Established() const override = 0;
};

#endif
