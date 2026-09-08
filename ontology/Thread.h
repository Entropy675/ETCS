#ifndef SUPERTYPE_THREAD_H__
#define SUPERTYPE_THREAD_H__

#include "../core_defs.h"
#include "../core/SignalContext.h"
#include <vector>

// ---------------------------------------------------------------------------
// Thread — an entity that IS a control thread, rather than one that owns a
// body which happens to run.
//
// A REFINEMENT OF Threaded, and the split is the one Threaded.h already drew
// for itself: "No priority, no affinity -- those are policy, and policy belongs
// to whatever schedules." This is whatever schedules. Threaded is the
// cooperative stop, claimed by anything with a loop in it; Thread is the actor
// that owns signal authority, carries a closure, and can detach children.
//
// So VulkanSurface stays Threaded and is NOT a Thread: it has a frame loop, it
// is not an actor running scripts. That distinction is worth keeping sharp,
// because the two answer different questions and the wire only covers the first.
//
// Lineage lives in ThreadBase (which composes ThreadedBase), not here -- the
// rule Drawable.h states at length and for the same mechanical reason: the
// supertype-base macro inherits non-virtually, so an interface that also
// inherited its parent interface would be reached twice and every call through
// it would be ambiguous. An interface declares only its own INCREMENT.
//
// WHAT THIS REPLACES. core/CommandExecutor.h keeps a DetachedRegistry: a mutex,
// a vector<unique_ptr<DetachedExecutor>>, and an atomic next_id_ handing out
// integers. Every field of that has a counterpart here --
//
//     DetachedExecutor::id          -> the entity's RID
//     DetachedRegistry::executors_  -> typed children
//     DetachedRegistry::next_id_    -> RID allocation
//     DetachedExecutor::local_sig   -> Signals(), on the parent/child edge
//     DetachedExecutor::finished    -> Halted()
//
// -- including the one that reads as an implementation detail: that struct is
// held by unique_ptr specifically so its address stays stable across vector
// growth, and arena-allocated entities have stable addresses by construction.
// It is the third hand-rolled copy of the entity graph found in this ontology,
// after etcs_mark_pixel_path and Scene3D::m_viewers.
//
// THE DAG IS THE PARENT EDGE. A thread that detaches another is that thread's
// parent, so "which jobs did this one start" is getTypedChildren and needs no
// registry to answer. Signal authority follows the same edge rather than a
// separate chain that can disagree with it.
// ---------------------------------------------------------------------------
class Thread_ : virtual public ETCS::Entity
{
public:
    virtual ~Thread_() = default;

    // What this thread is executing -- a script name, a label. Identity of the
    // work, not a handle to it: the work itself is the leaf's business.
    virtual ETCS::Buffer Script() = 0;

    /*
     * Detach is INHERITED FROM THE WIRE (ETCS::IWireThread), not declared here,
     * and ThreadBase is where it stops returning 0.
     *
     * It sits on the wire because core has to call it and cannot implement it:
     * CommandExecutor's detach runs in core, a detached script is a Thread, and
     * ontology depends on core rather than the reverse. See InterfaceWire.h.
     *
     * What a Thread's implementation owes: the child is a child ENTITY, so the
     * DAG is the parent edge; it inherits a COPY of this thread's closure (see
     * ThreadBase::Bind); and a halted thread refuses with 0 rather than
     * spawning into a teardown.
     *
     * A NON-DETACHED Thread has exactly one origin: a Root spawns a Shell. That
     * Shell is then the root of the closure for every script it runs and every
     * child it detaches -- which is what makes the script history a subtree
     * rather than a list, and therefore something the merkle walk can reach.
     */
};

#endif // SUPERTYPE_THREAD_H__
