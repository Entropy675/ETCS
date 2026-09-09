#ifndef BASE_RENDERABLE_H__
#define BASE_RENDERABLE_H__
#include "Renderable.h"

// Nothing composed, and in particular no RasterBase -- there is none, on
// purpose (ontology/Raster.h). Renderable_ inherits Raster_ directly and
// virtually, so a leaf claiming this base answers "Raster" as well, from the
// constructor Raster_ carries for exactly that.
//
// All three entries dispatch to the leaf, because all three are properties of
// a device image this family cannot see. PixelWidth/PixelHeight are Raster_'s
// pure virtuals; declaring them with the same signature overrides them (a
// derived declaration matching a base virtual is an override whether or not
// `virtual` is written), so a backend supplies the size the same way it
// supplies everything else -- once, in the Concrete, from the image it made.
ETCS_SUPERTYPE_BASE(Renderable)
{
    ETCS_MAKE_INSTANCE(Renderable)
    ETCS_DISPATCH_METHOD_CONST(uint64_t, DeviceKey);
    ETCS_DISPATCH_METHOD_CONST(uint32_t, PixelWidth);
    ETCS_DISPATCH_METHOD_CONST(uint32_t, PixelHeight);
};

#endif
