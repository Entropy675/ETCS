#ifndef BASE_LAYER_H__
#define BASE_LAYER_H__
#include "Layer.h"
#include "OrderableBase.h"

// Carries the lineage: this is where a Layer becomes an Orderable. See
// Layer.h for why the refinement is that way round -- an order with a
// frame of reference is what a layer IS, so the family cannot be stated
// without the one it refines.
//
// IT IS THE UNIQUENESS TRAIT FOR ORDERABILITY, inherited from the argument
// SurfaceBase used to make. OrderableBase<Derived> is composed
// non-virtually, so if a second base anywhere in a leaf's lineage also
// claimed Orderable, the leaf would hold two OrderableBase subobjects and
// every comparison through them would be ambiguous -- a compile error, not
// a silent double answer. Exactly one place in a lineage may claim the
// causality for orderability, and claiming it HERE excludes every base
// downstream from claiming it again.
//
// That is why it belongs at the top of this lineage rather than partway
// down: Surface, Drawable, Drawable2D and Drawable3D all inherit the single
// claim made here, and a leaf that is a Layer without being a Surface --
// PaintProvider's PaintLayer, a locale node with no appearance of its own --
// makes the same claim once, in the same place.
//
// The requirement rides along unchanged: a concrete layer must declare
// bool operator<(const T&) const, checked at compile time (OrderableBase.h).
// Every other comparison is derived from that one.
//
// NO DISPATCH ENTRIES OF ITS OWN, and that is deliberate rather than an
// omission. Layer's whole increment is two CONCRETE mechanisms -- Subject()
// with a default that is right for almost every leaf, and Neighbourhood()
// which finds the holding list and asks it -- so claiming this family costs
// a leaf nothing it was not already doing. It had to be free: this base sits
// above Surface, and an obligation here would be a new *Concrete on every
// drawable in the tree, written once each to restate a default.
//
// A concrete layer registers TWO interface pointers automatically --
// "Layer" and "Orderable" -- so foreign code can reach either generically
// via getInterfacePointer.
ETCS_SUPERTYPE_BASE(Layer), public OrderableBase<Derived>
{
    ETCS_MAKE_INSTANCE(Layer)
};

#endif
