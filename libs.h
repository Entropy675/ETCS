#ifndef INCLUDE_LIBS_H__
#define INCLUDE_LIBS_H__

#define EXTERNAL_LIBS_INCLUDED

// Define the external header-only libraries that you want to use for this compilation unit
// This is effectively your external (non ontology abiding) dependencies. 
// You will need to upload your bundle of headers in this folder when submitting a module.

#include "libs/picosha2.h"

#define XXH_INLINE_ALL
#include "libs/xxhash.h"
// #include "libs/httplib.h"  -> moved to NetworkHandler module

#endif
