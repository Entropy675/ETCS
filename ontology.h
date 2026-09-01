#ifndef ONTOLOGY_H__
#define ONTOLOGY_H__


// Include all modules available in this version of the ontology, and how they are loaded


#include "core/DynamicLoader.h"

// !! ALERT, whenever you add a new Base, you must include it here for it to be visible to other types.
// If you do not, you will get an error: expected template-name before ‘<’ token
//   10 |     public UnregisteredBase<UserInstance>
// Wherever your use of it is.

// these ones are a bit more questionable (too general?):
#include "ontology/ConnectionStateBase.h"
#include "ontology/HtmlPageBase.h"
#include "ontology/WindowBase.h"
#include "ontology/InputSourceBase.h" 

// these ones may be turned into just Database with a flag:
#include "ontology/LocalDatabaseBase.h"
#include "ontology/RemoteDatabaseBase.h"

// Lower suspicion graphical/window ontology types (mostly RenderProvider/WindowProvider):
#include "ontology/ResizableBase.h"
#include "ontology/SurfaceBase.h"
#include "ontology/PresentableBase.h"
#include "ontology/PixelsBase.h"

// The Drawable lineage: Surface refined into a node that occupies space in
// a parent and nests. DrawableBase composes SurfaceBase, and the two leaf
// bases compose DrawableBase, so the include order below is the lineage
// order -- and each header includes what it refines anyway, so this list is
// for visibility (see the ALERT above), not for ordering.
#include "ontology/DrawableBase.h"
#include "ontology/Drawable2DBase.h"
#include "ontology/Drawable3DBase.h"

// standard ontlogy supertype bases:
#include "ontology/DeletableBase.h"
#include "ontology/EphemeralBase.h"
#include "ontology/ParserBase.h"
#include "ontology/WrapperBase.h"
#include "ontology/GateBase.h"
#include "ontology/SwitchableBase.h"
#include "ontology/FilterBase.h"

#define ONTOLOGY_H__VER_0 // maybe we will replace this with a hash of this file

#endif
