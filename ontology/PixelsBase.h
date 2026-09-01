#ifndef BASE_PIXELS_H__
#define BASE_PIXELS_H__
#include "Pixels.h"

// No ETCS_DISPATCH_METHOD entries, same shape as InputSourceBase: every
// method on Pixels_ is a concrete non-virtual implementation on the family
// itself (the buffer, the dirty flag and the CPU raster are
// backend-independent), so there is nothing here for a backend to swap.
// This file exists to register the "Pixels" interface-pointer family, which
// is what lets a device-backed surface reach a foreign image surface's
// bytes via getInterfacePointer("Pixels") with no compile-time dependency
// on whatever concrete type produced them.
ETCS_SUPERTYPE_BASE(Pixels)
{
    ETCS_MAKE_INSTANCE(Pixels)
};

#endif
