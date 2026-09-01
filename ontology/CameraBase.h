#ifndef BASE_CAMERA_H__
#define BASE_CAMERA_H__
#include "Camera.h"
#include "Drawable2DBase.h"

// Composes Drawable2DBase, so a concrete camera owes the whole accumulated
// constraint -- Clear/DrawRect/Blit from Surface, GetSize from Resizable,
// operator< from Orderable, DrawInto from Drawable, Bounds/ContainsLocal
// from the 2D leaf -- plus the five below, and gets "Camera", "Drawable2D",
// "Drawable", "Surface", "Resizable" and "Orderable" interface pointers
// registered for it. Foreign code reaches any of them generically through
// getInterfacePointer, which is what lets a compositor that has never heard
// of this family blit a camera view.
//
// That weight is the point rather than a cost. The four surface verbs are
// exactly what makes a camera view usable without an adapter, and a camera
// that could not be cleared, blitted from, or drawn into a parent would
// have needed every one of them written again somewhere else, as a special
// case, about it.
//
// The 2D leaf is composed here at the FAMILY level for the same reason
// SurfaceBase composes Resizable and Orderable there: every camera is a
// plane, without exception and without a backend for which it is not true.
// Nothing is left to the concrete type that is true of the whole family.
//
// AND IT SETTLES THE EXCLUSION. Reaching Drawable2DBase means holding
// DrawableBase non-virtually, so any camera leaf that also tried to compose
// Drawable3DBase gets an ambiguous base -- a compile error, not a review
// note. A camera cannot be its own scene, and that is now a fact about the
// type system rather than a convention.
ETCS_SUPERTYPE_BASE(Camera), public Drawable2DBase<Derived>
{
    ETCS_MAKE_INSTANCE(Camera)
    ETCS_DISPATCH_METHOD(void,        SetView,  (ViewFrustum, view));
    ETCS_DISPATCH_METHOD(ViewFrustum, GetView);
    ETCS_DISPATCH_METHOD(void,        SetScene, (ETCS::RID, scene));
    ETCS_DISPATCH_METHOD(ETCS::RID,   GetScene);
    ETCS_DISPATCH_METHOD(bool,        Render);
};

#endif
