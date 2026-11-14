#ifndef BASE_REMOTE_H__
#define BASE_REMOTE_H__
#include "Remote.h"

// Remote_ stays the first non-virtual base of a leaf that claims this, so the
// registered interface pointer IS the IWireRemote (InterfaceWire.h, offset 0).
ETCS_SUPERTYPE_BASE(Remote)
{
    ETCS_MAKE_INSTANCE(Remote)
    ETCS_DISPATCH_METHOD(bool, RemoteWork,
        (const ETCS::Buffer&, action), (ETCS::Buffer&, data), (const ETCS::SignalContext&, ctx));
    ETCS_DISPATCH_METHOD(int,  RemoteStream,
        (const ETCS::Buffer&, action), (const ETCS::Buffer&, config), (bool, produces),
        (const ETCS::SignalContext&, ctx));
};

#endif // BASE_REMOTE_H__
