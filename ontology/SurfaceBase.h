#ifndef BASE_SURFACE_H__
#define BASE_SURFACE_H__
#include "Surface.h"
#include "ResizableBase.h"
#include "LayerBase.h"

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
// LAYER IS COMPOSED HERE, AND IT IS WHERE ORDERABILITY NOW COMES FROM.
//
// This file used to compose OrderableBase directly, on the argument that
// every surface stands somewhere relative to other surfaces -- that a layer
// stack IS an ordering, and a surface that could not answer "what is above
// me" would need that answer stored somewhere else, by something else, about
// it. The argument was right and was being made one level too low: it is not
// a fact about surfaces, it is the definition of a LAYER (ontology/Layer.h),
// which is an order that has a frame of reference. So the claim moved up and
// Surface refines Layer instead.
//
// Nothing downstream changed. The single-claim rule that used to live here
// lives in LayerBase.h now, one link higher: OrderableBase is composed
// non-virtually, exactly one place in a lineage may claim the causality for
// orderability, and claiming it there excludes every base downstream --
// Surface, Drawable, Drawable2D and Drawable3D all inherit that one claim as
// they always did. The requirement rides along unchanged: a concrete surface
// must declare bool operator<(const T&) const, checked at compile time
// (OrderableBase.h).
//
// What it buys is the leaf that is a layer WITHOUT being a surface -- a paint
// layer, a locale node with no appearance of its own -- which could not exist
// while the two claims were the same claim, and which is the case the family
// was raised for.
//
// A concrete surface therefore gets FOUR interface pointers registered
// automatically -- "Surface", "Resizable", "Layer", "Orderable" (one
// ETCS_MAKE_INSTANCE call here, one inherited from each composed base's own
// ctor) -- foreign code can reach any of them generically via
// getInterfacePointer.
ETCS_SUPERTYPE_BASE(Surface), public ResizableBase<Derived>, public LayerBase<Derived>
{
    ETCS_MAKE_INSTANCE(Surface)
    ETCS_DISPATCH_METHOD(void, Clear,    (float, r), (float, g), (float, b), (float, a));
    ETCS_DISPATCH_METHOD(void, DrawRect, (int32_t, x), (int32_t, y), (uint32_t, w), (uint32_t, h),
                                          (float, r), (float, g), (float, b), (float, a));
    ETCS_DISPATCH_METHOD(void, Blit,     (Surface_*, source), (int32_t, x), (int32_t, y),
                                          (uint32_t, w), (uint32_t, h), (float, opacity));
};

#endif
