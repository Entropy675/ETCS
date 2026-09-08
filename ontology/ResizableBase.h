#ifndef BASE_RESIZABLE_H__
#define BASE_RESIZABLE_H__
#include "Resizable.h"
#include "ObservableBase.h"

// Composes Observable, by the same argument SurfaceBase.h gives for composing
// Orderable: exactly one place in a lineage may claim a causality, and this is
// the place for this one.
//
// A Resizable's notification half IS observation -- "tells you when its size
// changed" is the general structure with a size attached, and the old listener
// list here was that structure written out by hand. So what is left in
// Resizable_ after the extraction is the size, and the edge lives in the family
// that owns edges. The refinement is real, not a convenience: notifyResize
// marks, PollResize takes.
//
// Reaches every surface for free -- SurfaceBase composes this one, so
// ImageSurface, CompositeDrawable2D and Camera3D are Observable without
// claiming it, which is what keeps them from claiming it TWICE (PixelsBase
// deliberately does not compose it; a leaf holding both would otherwise carry
// two ObservableBase subobjects and every call through them would be
// ambiguous). GLFWWindow and Clayout claim ResizableBase directly and get it
// the same way.
//
// A type that is Observable WITHOUT being Resizable -- a hash node -- claims
// ObservableBase itself. Claiming both is the compile error described above,
// and is meant to be.
ETCS_SUPERTYPE_BASE(Resizable), public ObservableBase<Derived>
{
    ETCS_MAKE_INSTANCE(Resizable)
    ETCS_DISPATCH_METHOD(WindowSize, GetSize);
};

#endif
