#ifndef BASE_POINTER_H__
#define BASE_POINTER_H__
#include "Pointer.h"

// Both dispatched -- a pointer's position comes from the platform and there
// is nothing generic to compute from it here. Contrast Clippable, where the
// family owns the arithmetic and the backend owns only the application.
//
// Composed by a concrete window (or tablet, or touch surface) alongside
// InputSourceBase, never instead of it: the two answer different questions
// and a device that has a cursor and keys claims both.
ETCS_SUPERTYPE_BASE(Pointer)
{
    ETCS_MAKE_INSTANCE(Pointer)
    ETCS_DISPATCH_METHOD(PointerState, ReadPointer);
    ETCS_DISPATCH_METHOD(bool,         PointerInside);
};

#endif
