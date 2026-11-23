#ifndef ONTOLOGY_EPOCH_H__
#define ONTOLOGY_EPOCH_H__

#include "../core_defs.h"
#include <cstdint>

// ---------------------------------------------------------------
// Epoch
// ---------------------------------------------------------------
//
// THE EPOCH AS A SEED. Runtimes that interact share an epoch -- a module
// built for another is refused, and so is a link -- and the epoch moves with
// every update. So it is the seed for whatever such runtimes must pick alike
// without saying so: the same on every one of them, and new with each build,
// a meaningful catch in time. EpochSeed is 64 bits of the loader's epoch
// (DynamicLoader.h, ETCS_GetLoaderEpoch), taken once an image; 0 where no
// loader is in the process, which has none to share.
//
// What one session must pick alike past that -- a round, a burst -- comes
// from its own lines, mixed with this.
namespace ETCS {

inline uint64_t EpochSeed()
{
    static const uint64_t seed = [] {
        void* getter = etcs_find_loader_symbol("ETCS_GetLoaderEpoch");
        if (!getter) return uint64_t(0);
        const unsigned char* epoch = reinterpret_cast<const unsigned char* (*)()>(getter)();
        return epoch ? static_cast<uint64_t>(XXH3_64bits(epoch, 32)) : uint64_t(0);
    }();
    return seed;
}

} // namespace ETCS

#endif // ONTOLOGY_EPOCH_H__
