#ifndef SUPERTYPE_RENDERABLE_H__
#define SUPERTYPE_RENDERABLE_H__


#include "../core_defs.h"
#include "Raster.h"
#include <cstdint>

// ---------------------------------------------------------------
// Renderable
// ---------------------------------------------------------------
//
// A raster whose pixels live in a rendering device's memory. The
// other half of the split Raster.h describes, and the exact
// complement of Pixels: everything Pixels_ offers is a consequence
// of the bytes being addressable by the host, and none of it is
// true here.
//
// SO THIS FAMILY DECLARES ALMOST NOTHING, which is the honest
// shape rather than an unfinished one. A device-resident raster
// cannot be read, blended or composited from outside the backend
// that owns it -- there is no pointer to hand out and no portable
// way to ask for one. What a consumer CAN establish, and the only
// thing it can act on, is whether the raster is somewhere it can
// already reach. That is DeviceKey().
//
// DEVICEKEY IS OPAQUE ON PURPOSE. Ontology has no business knowing
// what a device is; it does not name Vulkan, and a second backend
// must not have to pretend to be one. The only operation defined on
// the value is equality: two Renderables with the same non-zero key
// are in the same memory and their owner may copy between them
// without a host round trip, and any other combination cannot.
// A backend publishes whatever identifies its device uniquely
// within the process -- VulkanSurface publishes its VkDevice
// handle. Zero means "no device yet", which is what an entity
// answers between construction and Create.
//
// WHAT THIS UNBLOCKS TODAY IS THE REFUSAL, and that is worth
// stating plainly rather than dressing up. VulkanSurface::Blit
// used to say "has no Pixels interface -- only a CPU-backed
// surface can be blitted from yet" to everything that was not a
// CPU raster, which lumped "this is not a picture at all" together
// with "this is a picture on the very device you are drawing on".
// Those have different causes and different fixes. The device-to-
// device copy itself is not here; the seam it lands on is, and it
// is the equality above.
//
// Inherits Raster_ directly and virtually, which is the one place
// in this ontology a lineage belongs in the interface rather than
// in the Base -- Raster.h says why, and why that is what keeps this
// family and Pixels mutually exclusive.
//
// The size accessors are left pure here and dispatched to the leaf
// by RenderableBase, unlike Pixels_ which answers them from the
// buffer it owns. That asymmetry is the split itself: a CPU raster
// HAS its dimensions, a device raster can only ask the backend what
// it made.

class Renderable_ : virtual public Raster_
{
public:
    virtual ~Renderable_() = default;

    virtual uint64_t DeviceKey() const = 0;
};

#endif
