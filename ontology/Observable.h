#ifndef SUPERTYPE_OBSERVABLE_H__
#define SUPERTYPE_OBSERVABLE_H__

#include "../core_defs.h"

// ---------------------------------------------------------------------------
// Observable — something records the state of another and wants to know when
// that changes.
//
// The structure was already here twice, written out by hand and differently
// each time: etcs_mark_pixel_path walks the parent chain marking every Pixels
// owner, and Scene3D keeps m_viewers of cameras that projected it and marks
// those. Same causal shape, two spellings, neither reusable -- and a third use
// was about to be written for the merkle hash.
//
// TWO KINDS OF OBSERVER, and only one of them needs registering. An ancestor
// observes its descendants by construction, so a change bubbles up the parent
// chain on its own. Anything else -- a camera naming a scene it does not
// contain -- says so with Observe().
//
// DIRTY IS RELATIVE TO AN OBSERVER, not a property of the observed. Two
// cameras viewing one scene must each be told once; a single read-and-clear
// flag lets whichever looks first consume the other's invalidation. That is
// the bug the per-observer form exists to make impossible.
//
// ETCS::IWireObservable first and non-virtual -- see core/InterfaceWire.h.
// ---------------------------------------------------------------------------
class Observable_ : public ETCS::IWireObservable, virtual public ETCS::Entity
{
public:
    virtual ~Observable_() = default;

    void Observe(uint64_t observer_rid) override   = 0;
    void Unobserve(uint64_t observer_rid) override = 0;
    void MarkObserved() override                   = 0;
    bool TakeObserved(uint64_t observer_rid) override = 0;

    // Is anything watching? A node with no observers can skip work whose only
    // purpose is to be looked at.
    virtual bool Observed() const = 0;
};

#endif // SUPERTYPE_OBSERVABLE_H__
