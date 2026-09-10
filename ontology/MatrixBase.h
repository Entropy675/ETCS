#ifndef BASE_MATRIX_H__
#define BASE_MATRIX_H__
#include "Matrix.h"
#include "WrapperBase.h"

// Composes WrapperBase, which is the whole reason a chain exists rather than
// having to be built: a leaf claiming this gets the "Wrapper" tag and interface
// pointer for free, and MirrorBuffer's own resolution -- typed children tagged
// Wrapper, in attach order, filtered by Scope() -- finds it with nothing
// registered anywhere. Attach order is the chain order.
//
// LINEAGE IN THE BASE, not in Matrix_. The supertype-base macro inherits its
// interface non-virtually, so an interface that also inherited its parent's
// would put Wrapper_ in the object twice -- the rule Drawable.h and Thread.h
// both state. It matters more than usual here: Wrapper_ declares IWireWrapper
// as its FIRST non-virtual base so that the registered "Wrapper" pointer is
// bit-identical to an IWireWrapper*, which is what MirrorBuffer reinterprets.
// Reaching Wrapper_ through WrapperBase keeps that offset-0 relationship
// exactly as it was; reaching it twice would not.
//
// A concrete Matrix therefore owes Wrap, Unwrap and Scope -- and owes NO
// relationship between the first two. See Matrix.h: the two sides of a
// boundary are two spaces, and arriving is not undoing.
ETCS_SUPERTYPE_BASE(Matrix), public WrapperBase<Derived>
{
    ETCS_MAKE_INSTANCE(Matrix)
};

#endif
