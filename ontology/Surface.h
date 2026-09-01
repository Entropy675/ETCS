#ifndef SUPERTYPE_SURFACE_H__
#define SUPERTYPE_SURFACE_H__


#include "../core_defs.h"
#include <cstdint>

// ---------------------------------------------------------------
// Surface
// ---------------------------------------------------------------
//
// A rectangular region of addressable pixels that can be drawn onto.
//
// Named for what it IS in the causal graph rather than for the
// direction data moves through it. This family was called "Target"
// first, which named only the output-destination role -- and under
// that name the case that matters most downstream, an OFFSCREEN
// region that is never presented, reads as a contradiction. A
// PintaProvider layer is exactly that region. Both neighbours
// already use this word for this thing: Vulkan's VkSurfaceKHR is
// the window as the graphics system sees it, and Cairo/Pinta's core
// type is ImageSurface, so `requires canvas [Surface]` reads
// correctly from either side.
//
// A surface says nothing about where its contents CAME from. Bytes
// painted a pixel at a time, 2D primitives rasterised by a device, or
// the projection of a 3D scene onto a camera's image plane at some
// depth from the origin -- all of them produce the same thing, a 2D
// region of pixels, and displaying that region is what showing a
// camera view IS. That last case is the reason this family is worth
// getting right now rather than after: it is not a cousin of the
// editor's canvas, it is literally the same family, differing only in
// what fills it.
//
// What is deliberately NOT here, because none of it is universal:
//   - Present()   -- only a window-bound surface can present, see
//                    Presentable.h
//   - pixel bytes -- only a CPU-backed surface has them to hand out,
//                    see Pixels.h
// Both are separate families a concrete surface composes when it
// actually has them, the same way Resizable was split out of Window,
// and the four combinations are four real things rather than an
// accident of factoring:
//
//   Surface + Presentable + Resizable   a window's swapchain -- what
//                                       you are looking at
//   Surface + Pixels      + Resizable   a CPU layer -- a Pinta layer
//   Surface               + Resizable   a device-side offscreen target
//                                       -- render-to-texture, and the
//                                       slot a camera projection lands
//                                       in. Absent BOTH optional
//                                       families, which is exactly what
//                                       it is: nothing to present to,
//                                       no bytes on the CPU side.
//   Surface + Presentable + Pixels      a directly-mapped framebuffer
//
// Nothing has to pretend, and the third row is empty today on purpose
// rather than by omission.

class Surface_ : virtual public ETCS::Entity
{
public:
    virtual ~Surface_() = default;

    virtual void Clear(float r, float g, float b, float a) = 0;
    virtual void DrawRect(int32_t x, int32_t y, uint32_t w, uint32_t h,
                           float r, float g, float b, float a) = 0;

    // Composite another surface into this one -- the primitive a layered 2D
    // editor is built out of, called once per layer in order.
    //
    // The source is a Surface_, not a bare Entity* and not a Pixels_. Every
    // surface is drawable BY definition -- that is what makes it a surface
    // -- so the constraint this call places on its source is exactly
    // "is a surface", stated in the type. What an implementation then needs
    // to READ off it depends on both sides: today's CPU-backed sources are
    // read through their Pixels_ (Pixels.h) via getInterfacePointer, and a
    // future device-side offscreen surface would be read by copying its
    // image instead, with no CPU bytes anywhere. Naming Pixels_ here would
    // have frozen the first of those into the contract.
    virtual void Blit(Surface_* source, int32_t x, int32_t y,
                       uint32_t w, uint32_t h, float opacity) = 0;
};

#endif
