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

    /*
     * HOW OFTEN THAT CROSSING IS HAPPENING, in presents per second.
     *
     * Here, on the family, and not on a backend -- which is where it was, in
     * two byte-identical copies that a third backend would have had to write a
     * third time. Worse than the duplication: a caption showing a frame rate
     * had no family route to it, so TextLabel resolved the Surface family,
     * checked the tag STRING against its own module's spelling and cast to the
     * concrete platform type to read the field. That is three couplings -- to
     * one module, to one tag name, and to a header include order -- standing in
     * for one method.
     *
     * A PROPERTY OF THE PRESENTING, WHICH IS WHY IT IS ON THIS FAMILY and not
     * on Surface. Surface says how a frame is ASSEMBLED and an offscreen image
     * surface is one; the rate is a fact about the boundary crossing at the
     * end, and only the things that can cross have one. An entity that never
     * presents has no rate rather than a rate of zero, and cannot be asked.
     *
     * Zero until the second present: one crossing establishes no interval. See
     * PresentableBase for the averaging and why it is not the instantaneous
     * value.
     */
    virtual float Fps() const = 0;
};

#endif
