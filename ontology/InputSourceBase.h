#ifndef BASE_INPUTSOURCE_H__
#define BASE_INPUTSOURCE_H__
#include "InputSource.h"

// No ETCS_DISPATCH_METHOD entries: RegisterObserver/UnregisterObserver/
// ReadNextRingEvent/ViewInput are plain non-virtual methods on
// InputSource_ (like Window_::GetHandle()), not per-backend-swappable --
// this file exists to register the "InputSource" interface-pointer
// family and RIDList aggregate so foreign code can reach an InputSource_*
// via getInterfacePointer("InputSource") the same way it reaches Window_.
ETCS_SUPERTYPE_BASE(InputSource)
{
    ETCS_MAKE_INSTANCE(InputSource)
};

#endif
