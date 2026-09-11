#ifndef BASE_DEVICE_H__
#define BASE_DEVICE_H__
#include "Device.h"

// Nothing composed: a device is not a refinement of anything in this ontology.
// It is not drawn, not ordered, not resizable and owns no pixels -- it is the
// place other things' pixels can be, which is a claim with no parent.
//
// Both entries dispatch to the leaf, because both are properties of a backing
// object this family cannot see.
ETCS_SUPERTYPE_BASE(Device)
{
    ETCS_MAKE_INSTANCE(Device)
    ETCS_DISPATCH_METHOD_CONST(uint64_t, DeviceKey);
    ETCS_DISPATCH_METHOD_CONST(bool,     DeviceReady);
};

#endif
