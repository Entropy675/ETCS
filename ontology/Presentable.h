#ifndef SUPERTYPE_PRESENTABLE_H__
#define SUPERTYPE_PRESENTABLE_H__


#include "../core_defs.h"

// ---------------------------------------------------------------
// Presentable
// ---------------------------------------------------------------
//
// Something whose accumulated contents can be handed to a display.
// Split out of Surface for the same reason Resizable was split out
// of Window: it is not universal. Only a surface bound to a window
// (a swapchain, in the Vulkan backend) can present; an offscreen
// image surface has nowhere to present TO, and making it carry a
// no-op Present() would be a family lying about what its members
// can do.
//
// Deliberately one method. Everything about HOW a frame is
// assembled belongs to Surface_; this names only the boundary
// crossing at the end of it.

class Presentable_ : virtual public ETCS::Entity
{
public:
    virtual ~Presentable_() = default;

    virtual void Present() = 0;
};

#endif
