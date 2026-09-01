#ifndef BASE_SURFACE_H__
#define BASE_SURFACE_H__
#include "Surface.h"
#include "ResizableBase.h"

// Composes Resizable at the FAMILY level, not per-concrete-class (contrast
// GLFWWindow, which multiply-inherits WindowBase/InputSourceBase/
// ResizableBase/DeletableBase itself) -- see Surface.h's own comment for
// why. The supertype-base macro below (defined in core/ETCS_API.h) is
// textual substitution ending mid-base-list, so appending a second base
// here is legal. Both Surface_ and Resizable_ reach ETCS::Entity through
// virtual inheritance, so the diamond collapses to one Entity subobject
// exactly the way GLFWWindow's four-base composition already does today
// -- same proven mechanism, composed one level higher.
//
// Every surface has a size that can change; not every surface can present
// or hand out pixel bytes. That asymmetry is the whole reason Resizable is
// composed HERE while Presentable and Pixels are left for the concrete
// type to add.
//
// CAUTION for future editors of this file: ace_ontology.py's family-name
// scan is a plain regex over raw file text, not a real parser -- it will
// match that macro's invocation anywhere it appears, including inside a
// comment, and take whichever match comes first in the file. Keep any
// prose mention of the macro's call shape from literally reproducing
// "the macro name" immediately followed by a parenthesized identifier
// above the real invocation below, or `ace ontology` mislabels this
// entire family (confirmed by triggering it while writing this file).
//
// A concrete surface therefore gets BOTH a "Surface" and a "Resizable"
// interface pointer registered automatically (one ETCS_MAKE_INSTANCE
// call here, one inherited from ResizableBase<Derived>'s own ctor) --
// foreign code can reach either generically via getInterfacePointer.
ETCS_SUPERTYPE_BASE(Surface), public ResizableBase<Derived>
{
    ETCS_MAKE_INSTANCE(Surface)
    ETCS_DISPATCH_METHOD(void, Clear,    (float, r), (float, g), (float, b), (float, a));
    ETCS_DISPATCH_METHOD(void, DrawRect, (int32_t, x), (int32_t, y), (uint32_t, w), (uint32_t, h),
                                          (float, r), (float, g), (float, b), (float, a));
    ETCS_DISPATCH_METHOD(void, Blit,     (Surface_*, source), (int32_t, x), (int32_t, y),
                                          (uint32_t, w), (uint32_t, h), (float, opacity));
};

#endif
