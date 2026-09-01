#ifndef SUPERTYPE_POINTER_H__
#define SUPERTYPE_POINTER_H__


#include "../core_defs.h"
#include <cstdint>

// ---------------------------------------------------------------
// PointerState
// ---------------------------------------------------------------
//
// Where the pointer is and what is held down, as one value, because
// every consumer needs them together: "is the cursor over this node
// with the primary button down" is one question, and answering it
// from two calls invites the two to be read a frame apart.
//
// Position is in the SURFACE's coordinate space -- the same space a
// root Drawable2D addresses -- so it composes with ToLocal down the
// tree with no conversion step (Drawable2D_::Pick takes exactly
// this). A device that reports in its own units converts once, at
// the edge, which is where a unit conversion belongs.
//
// buttons is a bitmask rather than named members: a mouse has three,
// a stylus has two and a barrel, a pad has however many the driver
// exposes, and a struct that named them would be describing one
// device. Bit 0 is the primary button on every device that has one,
// which is the only assignment worth fixing.
//
// scroll_x / scroll_y are DELTAS since the last read, not absolute
// positions -- there is no such thing as an absolute scroll -- and
// reading them is what consumes them.

struct PointerState
{
    int32_t  x;
    int32_t  y;
    uint32_t buttons;
    float    scroll_x;
    float    scroll_y;
};

// ---------------------------------------------------------------
// Pointer
// ---------------------------------------------------------------
//
// Something with a positional cursor: a mouse, a stylus, a touch
// contact, a controller-driven crosshair.
//
// Split from InputSource for the same reason Resizable was split
// from Window. InputSource carries a STREAM of discrete events --
// key down, key up -- and that is the right shape for keys, which
// have no state between presses worth polling. A pointer is the
// opposite: it has a position at all times, that position is
// meaningful whether or not anything changed, and the question
// asked of it is almost always "where is it now" rather than "what
// happened". Forcing it through an event ring means every consumer
// reconstructs the current position by replaying deltas, and any
// consumer that misses an event is wrong until the next one.
//
// So: InputSource for what happened, Pointer for where things are.
// A window composes both, and they do not overlap.
//
// This is what a UI layer needs from the world -- Clay's
// Clay_SetPointerState wants exactly these fields -- and what a
// paint tool needs, and what Drawable2D_::Pick was written to be
// fed. One family, three consumers that would otherwise each invent
// their own.

class Pointer_ : virtual public ETCS::Entity
{
public:
    virtual ~Pointer_() = default;

    // The current state. Reading the scroll deltas CONSUMES them, so two
    // readers of one pointer will not both see the same wheel motion -- which
    // is the honest behaviour for a delta and the reason this returns a value
    // rather than exposing the fields.
    virtual PointerState ReadPointer() = 0;

    // Whether the pointer is currently within this source's own region --
    // false when the cursor has left the window, which is different from
    // "at the edge" and cannot be recovered from the coordinates alone.
    virtual bool PointerInside() = 0;
};

#endif
