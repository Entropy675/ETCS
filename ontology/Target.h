#ifndef SUPERTYPE_TARGET_H__
#define SUPERTYPE_TARGET_H__


#include "../core_defs.h"
#include <cstdint>

// ---------------------------------------------------------------
// Target
// ---------------------------------------------------------------
//
// A 2D render target: something that can be cleared, drawn to with
// basic 2D primitives, and presented. Backend-agnostic on purpose --
// RenderProvider::Target is the first (and for now only) concrete
// implementation (Vulkan), but nothing here names Vulkan, the same
// way HtmlPage_ names nothing about NetworkProvider's own transport.
// Composes Resizable at the ontology-FAMILY level (TargetBase.h),
// not per-concrete-class the way Window_'s implementors do it --
// every render target must handle resize (swapchain-equivalent
// recreation is not an optional per-backend opt-in), so it belongs
// on the family.

class Target_ : virtual public ETCS::Entity
{
public:
    virtual ~Target_() = default;

    virtual void Clear(float r, float g, float b, float a) = 0;
    virtual void DrawRect(int32_t x, int32_t y, uint32_t w, uint32_t h,
                           float r, float g, float b, float a) = 0;
    virtual void Present() = 0;
};

#endif
