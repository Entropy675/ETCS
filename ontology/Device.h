#ifndef SUPERTYPE_DEVICE_H__
#define SUPERTYPE_DEVICE_H__


#include "../core_defs.h"
#include <cstdint>

// ---------------------------------------------------------------
// Device
// ---------------------------------------------------------------
//
// Somewhere other than host memory that work can be done and
// pixels can live. A GPU is the one that exists; the family says
// nothing about which, because nothing above it needs to know.
//
// CAPABILITY AS STRUCTURE, which is the whole reason this is a
// family and not a flag. THERE IS ALWAYS A CPU. Host memory is the
// floor every raster in this system already stands on, so it needs
// no declaration -- and that makes a device strictly an ADDITION,
// which in this ontology is a child. An entity that can reach a
// device is an entity with a Device under it, and one that cannot
// is one without: the capability is a fact about the entity graph,
// enumerable by the same typed-child walk everything else uses,
// on the state surface like every other origin-affixed tag, and
// marking its observers when it appears or goes.
//
// A flag would have been none of that. It would have been a second
// copy of an answer the graph already holds, settable to a value
// the graph disagrees with, and invisible to every walk.
//
// SO "WHICH DEVICE" IS THE SAME MECHANISM AS "WHETHER". Two
// Devices under one entity is two devices available to it, with no
// new concept and no list to maintain beside the children -- the
// same reason Thread.h gives for a thread's jobs being its
// children rather than rows in a registry.
//
// DeviceKey is the SAME value a Renderable on this device answers
// (ontology/Renderable.h): equality across the two is what says a
// raster is somewhere this entity can already reach, which is the
// only question either of them can act on.
//
// NOT A RASTER, and the distinction is the point of both families
// existing. A Renderable is pixels that live on a device; a Device
// is the place they live. The thing that has one is usually
// neither -- a camera with a Device child owns no pixels at all
// (RenderProvider/Camera3D.h).

class Device_ : virtual public ETCS::Entity
{
public:
    virtual ~Device_() = default;

    // Zero until the backing device exists, so "declared" and "usable" are
    // never the same answer -- an entity spawned and not yet Created is the
    // ordinary case, not an error.
    virtual uint64_t DeviceKey()   const = 0;
    virtual bool     DeviceReady() const = 0;
};

#endif
