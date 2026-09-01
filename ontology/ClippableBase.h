#ifndef BASE_CLIPPABLE_H__
#define BASE_CLIPPABLE_H__
#include "Clippable.h"

// One dispatch entry: the backend supplies how a rectangle becomes true of
// its own drawing, and inherits the stack discipline and the intersection
// arithmetic from the family (Clippable.h explains why that split falls
// where it does).
//
// Composed by a concrete surface, never by SurfaceBase: clipping is a
// capability some surfaces have, which is precisely the difference between
// this and Resizable -- every surface has a size, not every surface can
// restrict where drawing lands.
ETCS_SUPERTYPE_BASE(Clippable)
{
    ETCS_MAKE_INSTANCE(Clippable)
    ETCS_DISPATCH_METHOD(void, SetScissor, (int32_t, x), (int32_t, y),
                                            (uint32_t, w), (uint32_t, h));
};

#endif
