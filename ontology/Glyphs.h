#ifndef SUPERTYPE_GLYPHS_H__
#define SUPERTYPE_GLYPHS_H__


#include "../core_defs.h"
#include <cstdint>

// ---------------------------------------------------------------
// TextExtent
// ---------------------------------------------------------------
//
// What a run of text would occupy if it were drawn. Declared here,
// beside the family that gives it meaning, the same way WindowSize
// lives in Resizable.h.
//
// baseline is the distance from the top of the extent down to the
// baseline, and it is here rather than derivable because it is the
// only thing that lets two runs in different sizes sit on the same
// line. A layout that has width and height but not this can centre
// text in a box and cannot align it with anything.

struct TextExtent
{
    uint32_t width;
    uint32_t height;
    uint32_t baseline;
};

// ---------------------------------------------------------------
// Glyphs
// ---------------------------------------------------------------
//
// Something that knows how text is shaped: how big a run would be,
// and what its pixels are.
//
// THE SPLIT IS A TIMING CONSTRAINT, NOT A RENDERING ONE, and that is
// the whole reason this is one family with two verbs rather than a
// method on Surface. A layout has to know how wide a string is
// BEFORE any pixels exist -- that is what wrapping, autosizing and
// every flex rule are computed from, and it is why Clay refuses to
// run at all without a measure function. Measurement depends on the
// font and the size and on nothing else: no surface, no swapchain,
// no window. Rasterisation depends on all of it. Two capabilities,
// needed by different parties at different moments, and a design
// that only offers "draw this string somewhere" makes the first one
// unreachable.
//
//   Measure  -- pure, callable during layout, no drawable in sight
//   Rasterise -- produces bytes, callable once there is somewhere
//                for them to go
//
// RASTERISE EMITS INTO A Pixels, NOT ONTO A SURFACE. That is the
// part worth arguing for. Text then reaches the screen through the
// Blit that already exists, so it works on a CPU layer and on a
// window's swapchain with no text path added to either, and no
// Surface implementation has to grow a font stack to satisfy the
// family. A glyph run becomes just another blittable region -- which
// under the Drawable framing means a run of text is a Drawable2D
// like everything else, with no special case anywhere.
//
// THE TARGET IS AN RID, not a Pixels_*. A run is rasterised into
// whatever owns pixels -- an ImageSurface today, a provider's own
// glyph atlas tomorrow -- and the family resolves it the way
// everything else here crosses a module boundary. Naming Pixels_
// directly would be right for exactly today's implementations and
// wrong the first time one of them lives somewhere else.
//
// WHAT IS DELIBERATELY NOT HERE. Fonts as entities, font loading,
// fallback chains, shaping for scripts that need it. A font handle
// is an opaque uint32_t as far as this family is concerned: which
// fonts exist and how they are named is a provider's question, and
// answering it here would freeze one provider's answer into the
// ontology. What every implementation must agree on is only that a
// run has a size and can become pixels.

class Glyphs_ : virtual public ETCS::Entity
{
public:
    virtual ~Glyphs_() = default;

    // What this run would occupy at this size. No side effects, no
    // allocation the caller can observe, callable from a layout pass that
    // has not decided where anything goes yet.
    virtual TextExtent MeasureText(const char* text, uint32_t font, uint32_t size_px) = 0;

    // Draw the run into whatever `target` is, at (x, y) in the target's own
    // space, in the given colour. The target is expected to own pixels; an
    // implementation that cannot reach them says so and draws nothing.
    //
    // Returns the extent actually produced, which a caller can compare
    // against what Measure promised -- they should agree, and an
    // implementation where they do not is one that cannot be laid out with.
    virtual TextExtent RasterizeText(ETCS::RID target, const char* text,
                                     uint32_t font, uint32_t size_px,
                                     int32_t x, int32_t y,
                                     float r, float g, float b, float a) = 0;
};

#endif
