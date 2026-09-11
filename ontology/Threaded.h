#ifndef SUPERTYPE_THREADED_H__
#define SUPERTYPE_THREADED_H__

#include "../core_defs.h"
#include "../core/InterfaceWire.h"
#include <atomic>

// ---------------------------------------------------------------------------
// Threaded — an entity that owns a running body and can be asked to stop.
//
// Claims IWireThread, which core/InterfaceWire.h declared and left unclaimed.
// It is claimed now because the gap it named stopped being an argument: a frame
// producer holds a `VulkanSurface&` for a window's lifetime, and every guard
// available to it is a question it must dereference the object to ask. Adding
// another predicate narrows the window between check and use; it cannot close
// it. Measured -- a Retired() check on that loop moved the fault rate by less
// than the noise.
//
// So the problem was not what the loop knew, it was that nothing could tell it
// to stop. Halt is that telling, and the flag it sets is readable without
// touching anything the reclaim is about to invalidate.
//
// Two things and no more, which is the boundary InterfaceWire drew: what shape
// of work you are, and a cooperative stop. No priority, no affinity -- those
// are policy, and policy belongs to whatever schedules.
//
// WHATEVER SCHEDULES IS Thread, which refines this one (ontology/Thread.h). The
// pair reads in one direction: this family is the LESS strict of the two, held
// by anything with a body to stop, while a Thread additionally owns signal
// authority and a closure and can detach children. So VulkanSurface is Threaded
// and is not a Thread -- it has a frame loop, it is not an actor. Claiming
// Thread claims this cumulatively; claiming both Bases is a redundant claim and
// a compile error.
//
// Cooperative, not preemptive. Halt sets a flag; it does not join or cancel. A
// body that never polls Halted() is not stopped by this, which is honest rather
// than weak -- the alternative is killing a thread mid-Vulkan-call.
//
// IWireThread first and non-virtual, load-bearing for the reason every wire
// carries: the runtime reinterprets the registered interface pointer as a wire
// pointer, exact only at offset 0.
// ---------------------------------------------------------------------------
class Threaded_ : public ETCS::IWireThread, virtual public ETCS::Entity
{
public:
    virtual ~Threaded_() = default;

    // What this entity's bodies do to a worker. See ETCS::WorkShape.
    ETCS::WorkShape Shape() const override = 0;

    // Ask every body this entity owns to stop at its next opportunity. Returns
    // whether the request was TAKEN -- so a drain can say which bodies it asked
    // and which were already leaving, rather than calling both a timeout.
    bool Halt() override = 0;

    // What a running body polls. Cheap, and readable without touching state a
    // reclaim is about to take apart -- which is the entire point.
    bool Halted() const override = 0;

    /*
 * THE BODY REPORTING THAT IT ACTUALLY STOPPED, which is a different fact from
 * having been asked to.
 *
 * Halt/Halted was carrying both and could only mean one. The tag it wrote said
 * "halted" while recording a REQUEST, so an entity that had been asked to stop
 * and an entity whose loop had actually left it were indistinguishable -- and
 * a drain that wanted to know whether it was safe to reclaim had nothing to
 * read. Halting is the transition; being stopped is the destination.
 *
 * SAME SHAPE AS Delete/Release (ontology/Lifecycle.h): Halt is a request from
 * outside, Stop is a notification from the body itself, and only the body can
 * make it -- nothing else knows when a loop has left. So this is called BY a
 * body on its way out, not commanded at one.
 *
 * ON THE WIRE (core/InterfaceWire.h), beside the Halt/Halted pair it
 * completes, so the drain can ask it from the loader side -- which is the
 * caller that needs it, since reclaiming what a body was touching depends on
 * knowing the body has gone. Declared here only as the family's restatement of
 * the wire's slot, exactly as Shape/Halt/Halted are.
 */
    bool Stop() override = 0;
    bool Stopped() const override = 0;
};

#endif // SUPERTYPE_THREADED_H__
