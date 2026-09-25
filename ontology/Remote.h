#ifndef SUPERTYPE_REMOTE_H__
#define SUPERTYPE_REMOTE_H__


#include "../core_defs.h"

// ---------------------------------------------------------------
// Remote
// ---------------------------------------------------------------
//
// The mark that an entity in this graph is NOT in this runtime -- and
// the wire to where it is. Claimed by a child: the entity above it is
// a SURFACE, a local instance of the same type as a node that lives in
// another runtime (a browser tab, a process across the network), and
// what that surface is asked is asked of the far node instead
// (Entity::setRemoteWire, Entity::call).
//
// THE SURFACE IS THE REAL TYPE, built by the script like anything else
// (`spawn PaintProvider::PaintNode canvas`), so it claims every family
// that type claims, at construction, the ordinary way. What makes that
// honest is that both runtimes run the SAME type: the far node's tag
// hash (ModuleBundle::hash, composed over its actions' source regions)
// must equal the surface's before a Remote child binds, and each verb
// and stream half names its action's hash, which the far side checks
// against its own. A type that differs is not bound, rather than bound
// and misread.
//
// THE WRAPPER CHAIN IS THE AUTHORITY. A node's Wrapper children whose
// scope includes the network (ontology/Wrapper.h) are what every frame
// to or from it passes, and the surface must carry the same ones --
// same types, same code -- or the far side will not bind it, and no
// MirrorBuffer to that node ever opens. So what a runtime can reach of
// another is exactly what it can FULFIL with the type providers it has:
// a node guarded by a wrapper this side cannot construct is out of
// reach, and is influenced only indirectly, through whatever else that
// runtime publishes which does reach it locally. A stage that refuses a
// frame ends the stream (MirrorBuffer::unwrapFrame) or the verb.
//
// THE CALLER STILL SAYS BOTH HALVES. Nothing about a stream pair
// changes when one end is remote: `canvas.State -> mirror.Apply` names
// the producer action and the consumer action exactly as it does
// in-process (ETCS_API.h). The runtime asks the far side for its half
// over IWireRemote (core/InterfaceWire.h) and runs the near half itself,
// on an ordinary StrategySocket MirrorBuffer. A verb is the same with a
// Buffer instead of a socket.
//
// The child's own verbs (binding, reporting) are the only ones answered
// locally, and they are the child's -- so nothing the far node can be
// asked is shadowed by something only this side understands.
//
// ETCS::Remote is the compile-time marker for the same fact
// (core/MirrorBuffer.h, StrategyFor), and a Wrapper can never be one
// (Wrapper.h).

class Remote_ : public ETCS::IWireRemote, public ETCS::Remote, virtual public ETCS::Entity
{
public:
    virtual ~Remote_() = default;
    // Both entries are IWireRemote's, declared there because the runtime
    // calls them before any family type is visible to it.
};

#endif // SUPERTYPE_REMOTE_H__
