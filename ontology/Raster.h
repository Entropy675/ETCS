#ifndef SUPERTYPE_RASTER_H__
#define SUPERTYPE_RASTER_H__


#include "../core_defs.h"
#include <cstdint>

// ---------------------------------------------------------------
// Raster
// ---------------------------------------------------------------
//
// A grid of pixels with a size. WHERE those pixels live is the next
// question down, and it is the one that splits: Pixels (ontology/
// Pixels.h) holds them in host memory and hands out the bytes;
// Renderable (ontology/Renderable.h) holds them in a device's
// memory and hands out nothing but the identity of that device.
//
// THIS EXISTS BECAUSE CONSUMERS KEPT ASKING THE WRONG ONE. Four
// places in RenderProvider walk the parent chain looking for "the
// nearest ancestor that owns a raster" to fix a coordinate origin,
// and all four asked for "Pixels" -- which was the same question
// only for as long as every raster in the system was CPU-backed.
// The blit path had the same conflation from the other side: it
// asked for Pixels, and refused everything else with "only a
// CPU-backed surface can be blitted from yet", one message covering
// two unrelated causes. Size is not a CPU fact. It is the one thing
// every raster can answer no matter whose memory it sits in, so it
// is what this is.
//
// THERE IS NO RasterBase, AND THERE MUST NOT BE. A supertype base
// is what a leaf CLAIMS, and claiming this one would produce a leaf
// that is a raster and nothing else -- a thing with a size, no
// bytes, no device, and no answer to how it reaches a screen. That
// is not an incomplete type, it is an incoherent one. Contrast
// Threaded/Thread, where a base at either level is meaningful
// because "has a body that can be asked to stop" is a complete
// claim on its own; here the parent is a CONSTRAINT SET, satisfied
// only by one of its two children, never on its own account.
//
// SO THE LINEAGE IS IN THE INTERFACES HERE, not in the bases --
// the opposite of Drawable/Surface and Thread/Threaded, and for
// exactly the reason those two state. The rule they follow is
// mechanical, not stylistic: the supertype-base macro inherits its
// interface NON-virtually, so an interface that also inherited its
// parent interface would put that parent in the object twice. That
// hazard needs a parent BASE to exist. With none, Pixels_ and
// Renderable_ can inherit Raster_ virtually and directly, which is
// what makes a Pixels_* answer its own size rather than sending the
// caller back for a second lookup.
//
// AND IT IS WHAT MAKES THE TWO EXCLUSIVE, by the final-overrider
// rule rather than by a duplicated subobject. Both children inherit
// this virtually, so a leaf claiming both collapses to ONE Raster_
// -- with PixelWidth() overridden in two sibling branches and
// neither dominating. That has no unique final overrider and does
// not compile, which is the correct answer: two answers to one
// question is not a richer raster, it is two rasters.
//
// REGISTERS ITSELF, in the constructor below, because there is no
// ETCS_MAKE_INSTANCE to do it -- that macro lives in the base this
// family deliberately does not have. An interface pointer and
// nothing else: no type tag and no family RIDList, and both
// absences say the same thing the paragraph above does. You are a
// Pixels or you are a Renderable; "Raster" is how either is
// ADDRESSED, not something to be. etcs_supertype_fanout skips any
// family the module does not publish a list for (Entity.h), so an
// interface pointer standing alone is a case that machinery already
// handles rather than one it has to be taught.

class Raster_ : virtual public ETCS::Entity
{
public:
    Raster_()
    {
        this->registerInterfacePointer(ETCS::Buffer("Raster"),
            static_cast<void*>(static_cast<Raster_*>(this)));
    }
    virtual ~Raster_() = default;

    virtual uint32_t PixelWidth()  const = 0;
    virtual uint32_t PixelHeight() const = 0;

    // Not "a blank picture" -- a raster that has not been sized yet,
    // and therefore has nothing to read at all. Every consumer that
    // reaches a foreign raster checks this before touching it, so it
    // is stated once here instead of as two comparisons at each site.
    bool RasterEmpty() const { return PixelWidth() == 0 || PixelHeight() == 0; }
};

#endif
