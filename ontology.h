#ifndef ONTOLOGY_H__
#define ONTOLOGY_H__


// Include all modules available in this version of the ontology, and how they are loaded


#include "core/DynamicLoader.h"

// Not a family: the two-row spatial/energy primitive the 3D side states its
// motion in, reserved in the shape of the 4x4 it grows into. Included here
// rather than only from Drawable3D.h because it is the vocabulary a physical
// leaf of ANY family will state itself in, the way Point2D/Rect2D already are
// for the flat one.
#include "ontology/OrderVector.h"

// !! ALERT, whenever you add a new Base, you must include it here for it to be visible to other types.
// If you do not, you will get an error: expected template-name before ‘<’ token
//   10 |     public UnregisteredBase<UserInstance>
// Wherever your use of it is.

// these ones are a bit more questionable (too general?):
#include "ontology/ConnectionStateBase.h"
#include "ontology/HtmlPageBase.h"
#include "ontology/WindowBase.h"
#include "ontology/InputSourceBase.h" 
#include "ontology/PointerBase.h"

// these ones may be turned into just Database with a flag:
#include "ontology/LocalDatabaseBase.h"
#include "ontology/RemoteDatabaseBase.h"

// Lower suspicion graphical/window ontology types (mostly RenderProvider/WindowProvider):
#include "ontology/ResizableBase.h"
#include "ontology/ClippableBase.h"
#include "ontology/SurfaceBase.h"
#include "ontology/PresentableBase.h"
#include "ontology/PixelsBase.h"

// The Drawable lineage: Surface refined into a node that occupies space in
// a parent and nests. DrawableBase composes SurfaceBase, the two leaf bases
// compose DrawableBase, and CameraBase composes the 2D leaf -- a camera is
// a plane like any other, differing only in that a Drawable3D fills it. So
// the include order below is the lineage order -- and each header includes
// what it refines anyway, so this list is for visibility (see the ALERT
// above), not for ordering.
#include "ontology/DrawableBase.h"
#include "ontology/Drawable2DBase.h"
#include "ontology/Drawable3DBase.h"
#include "ontology/CameraBase.h"
#include "ontology/GlyphsBase.h"

// standard ontlogy supertype bases:
#include "ontology/DeletableBase.h"
// Lifecycle sits beside Deletable and answers a different question -- Delete is
// a request from outside, Release is a notification from the runtime that is
// guaranteed to happen exactly once whichever way the entity dies. See
// ontology/Lifecycle.h.
#include "ontology/LifecycleBase.h"
#include "ontology/EphemeralBase.h"
#include "ontology/ParserBase.h"
#include "ontology/WrapperBase.h"
#include "ontology/GateBase.h"
#include "ontology/SwitchableBase.h"
#include "ontology/FilterBase.h"

#define ONTOLOGY_H__VER_0 // maybe we will replace this with a hash of this file

#endif
