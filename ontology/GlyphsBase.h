#ifndef BASE_GLYPHS_H__
#define BASE_GLYPHS_H__
#include "Glyphs.h"

// Both verbs dispatched: measurement and rasterisation are exactly the two
// things only a backend knows, and there is nothing generic to inherit --
// contrast Pixels, whose whole family body is backend-independent, and
// Clippable, where the arithmetic is shared and only the application is not.
//
// A leaf composing this claims to be a text shaper. It composes nothing else
// from here: Glyphs is not a Surface and does not want to be. It produces
// pixels into something that owns them and lets Blit do the rest.
ETCS_SUPERTYPE_BASE(Glyphs)
{
    ETCS_MAKE_INSTANCE(Glyphs)
    ETCS_DISPATCH_METHOD(TextExtent, MeasureText,
                         (const char*, text), (uint32_t, font), (uint32_t, size_px));
    ETCS_DISPATCH_METHOD(TextExtent, RasterizeText,
                         (ETCS::RID, target), (const char*, text),
                         (uint32_t, font), (uint32_t, size_px),
                         (int32_t, x), (int32_t, y),
                         (float, r), (float, g), (float, b), (float, a));
};

#endif
