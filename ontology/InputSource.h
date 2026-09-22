#ifndef SUPERTYPE_INPUTSOURCE_H__
#define SUPERTYPE_INPUTSOURCE_H__


#include "../core_defs.h"
#include <cstdint>
#include <cstring>
#include <atomic>

#define NUM_KEYS 1024

// for verbose input logging, define this before compiling
// #define ETCS_VERBOSE_INPUT_EVENTS

// One input event, keyboard or pointer, in one record. The kind is the action
// field; key is 0 for a pointer event.
//
// x/y are ABSOLUTE, in pixels, relative to the window's content area, and only
// meaningful for INPUT_MOTION.
//
// A POSITION RATHER THAN A DELTA, which is load-bearing. A relative control
// needs the pointer locked -- hidden and teleported back to centre as it
// strays -- and that is a request a display may decline: Qubes proxies windows
// from another domain, XWayland answers to a compositor, remote X has no local
// pointer to move. A declined warp does not degrade, it changes what the
// numbers MEAN: the toolkit's accumulator starts reporting distance from the
// window centre as though it were movement, and the screen becomes a joystick
// with nothing in the stack able to tell.
//
// A position asks for none of that, and is strictly more information: a
// consumer wanting relative motion differences two positions itself, while one
// given deltas can never recover where the pointer is. Relativity becomes a
// choice downstream rather than a capability the platform must grant.
struct InputEvent
{
    uint16_t key;    // supports NUM_KEYS up to 65535; 0 for a pointer event
    uint8_t  action; // INPUT_UP / INPUT_DOWN / INPUT_MOTION
    /*
     * WHICH BUTTONS ARE DOWN AS OF THIS EVENT -- bit (1 << button), and 0 on a
     * key event or from any source that does not know.
     *
     * This was the reserved byte, and this is what it was reserved for. A motion
     * event could not say whether it was a DRAG or a bare move, so a consumer
     * holding stroke state had only its own memory of a press to go on -- and
     * that memory is exactly what goes wrong when a drag leaves the window and
     * the release is delivered to somebody else. The platform knows; it simply
     * had nowhere to write it down.
     *
     * A HINT, NOT A CONTRACT, and the top bit is what makes it one. Bits 0..6
     * are the buttons; INPUT_BUTTONS_STATED (bit 7) says a platform actually
     * REPORTED them. Without it a truthful "nothing is held" and "this source
     * cannot tell" are the same zero, and a consumer would either drop real
     * drags on a platform that stays silent or believe phantom ones on a
     * platform that spoke. A source that cannot answer leaves the bit clear
     * and must not be taken as asserting a release; one that can sets it, and
     * from then on its zero means zero. HeldCharge is what turns this into a
     * decision.
     */
    uint8_t  buttons;
    int16_t  x;      // pointer position in the content area, INPUT_MOTION only
    int16_t  y;
};

static constexpr uint8_t INPUT_BUTTONS_STATED = 0x80;
static constexpr uint8_t INPUT_BUTTONS_MASK   = 0x7F;

static inline uint8_t input_button_bit(uint16_t button)
{ return (button < 7) ? static_cast<uint8_t>(1u << button) : 0u; }

// Was `button` down when this event was recorded, as a three-way answer:
// 1 yes, 0 no, -1 the source could not say. -1 is the one callers get wrong by
// treating it as 0; HeldCharge's users are written against all three.
static inline int input_button_state(const InputEvent& ev, uint16_t button)
{
    if (!(ev.buttons & INPUT_BUTTONS_STATED)) return -1;
    return (ev.buttons & input_button_bit(button)) ? 1 : 0;
}

static constexpr uint8_t INPUT_UP     = 0;
static constexpr uint8_t INPUT_DOWN   = 1;
static constexpr uint8_t INPUT_MOTION = 2;
/*
 * MOUSE BUTTONS, and they are POINTER events rather than key events.
 *
 * They were missing entirely -- the pipeline carried keys and positions and
 * nothing else -- so anything that wanted "press here" had to borrow the
 * keyboard, which is how a paint program ended up drawing on any keystroke
 * instead of on a click. Borrowing the keyboard is also wrong in a way that
 * would not have stayed hidden: a button pressed at a position is a different
 * event from a key pressed while a position happens to be current.
 *
 * They travel on the POINTER ring, and carry their own x/y. Both follow from
 * the same fact: a click is a thing that happens SOMEWHERE. Putting it on the
 * key ring would have separated it from the position it means, and the two
 * rings drain independently, so the press could arrive before or after the
 * position it belongs to. Carrying the coordinate on the event removes the
 * question rather than answering it.
 *
 * Not coalesced. Positions supersede one another and buttons never do -- see
 * notePointerAt, which is why the coalescing lives there and not in the ring.
 */
static constexpr uint8_t INPUT_BUTTON_DOWN = 3;
static constexpr uint8_t INPUT_BUTTON_UP   = 4;

/*
 * SCROLL, and it travels the POINTER ring for the same reason buttons do.
 *
 * A wheel notch happens AT a position and means something different depending on
 * where -- zoom about here, scroll this pane, not that one -- so it belongs with
 * the channel that carries positions rather than with keys. x and y are the
 * DELTA, not a position, which is the one place this struct's fields change
 * meaning by action; there is no such thing as an absolute scroll
 * (PointerState says the same of its own scroll_x/scroll_y).
 *
 * WHY AN EVENT AS WELL AS PointerState's deltas. The two are not duplicates:
 * Pointer_::ReadPointer answers "how much has it scrolled since I last looked",
 * which is what a per-frame consumer wants, and this answers "a notch happened,
 * here" -- which is what a consumer that only wakes on input can act on. A UI
 * that polled would miss nothing; one that waits on a stream would never see a
 * wheel at all.
 */
static constexpr uint8_t INPUT_SCROLL = 5;

static constexpr uint32_t INPUT_SLOT_SIZE = sizeof(InputEvent);

/*
 * ── HeldCharge: a press believed only while it keeps being confirmed ────────
 *
 * THE PROBLEM. A consumer that holds "the button is down" holds it until a
 * release arrives, and a release is the one event that most easily never does:
 * drag off the canvas, off the window, out of the page, and the platform
 * delivers the mouseup to somebody else. The consumer is then wedged in a
 * gesture the user ended a minute ago, and the damage appears on the NEXT
 * interaction -- a stroke drawn across the picture by a move that was never
 * meant to be a drag.
 *
 * THE SHAPE OF THE FIX, AND WHY IT IS NOT A TIMEOUT. The obvious answer is a
 * deadline: hold it for N milliseconds and give up. That needs something still
 * ticking to notice the deadline pass, and the situation this exists for is
 * precisely the one where nothing is ticking -- no events are arriving, and the
 * thread that would check is blocked on the read that no event is coming to
 * satisfy. Every clock available here is driven by the input that has stopped.
 *
 * So the counter is CAUSAL rather than temporal: it costs one unit per ACCESS,
 * and the access is the clock. Whoever asks whether the button is held pays for
 * asking, and evidence that it really is held tops it back up. Nothing ticks
 * while nobody asks, which is right -- a held state nobody has looked at yet has
 * not been wrong about anything. The failure is not the state being stale in the
 * dark, it is the state still being stale the next time somebody looks, and by
 * the time somebody looks the counter is already draining.
 *
 * WHAT THE CAPACITY MEANS: how many accesses of unconfirmed holding to tolerate
 * before disbelieving the press. Small for a paint stroke, where a phantom drag
 * costs a line across the drawing and a wrongly dropped one costs a re-press.
 * Large for anything that reads held state as a mode and would rather ride out a
 * gap than drop it -- which is the number to raise, and the only one.
 */
class HeldCharge
{
public:
    explicit HeldCharge(uint16_t capacity = 4) : m_capacity(capacity ? capacity : 1) {}

    void SetCapacity(uint16_t capacity)
    { m_capacity = capacity ? capacity : 1; if (m_charge > m_capacity) m_charge = m_capacity; }
    uint16_t capacity() const { return m_capacity; }

    void Press()   { m_charge = m_capacity; }
    void Release() { m_charge = 0; }

    // The platform says the button really is still down: full confidence again.
    // Guarded on being held already, so a confirmation arriving after a release
    // cannot resurrect a gesture that is over.
    void Confirm() { if (m_charge != 0) m_charge = m_capacity; }

    /*
     * THE ACCESS, and it is the only one that should be in a decision. Returns
     * whether the press is still believed, and spends a unit for having asked.
     * Deliberately not const and deliberately not named like a getter: reading
     * this moves it, and a caller that wanted a free look wanted `charge()`.
     */
    bool Held()
    {
        if (m_charge == 0) return false;
        --m_charge;
        return true;
    }

    // For logging and assertions. Does not move the counter, and must not be
    // what a decision is made on -- that is Held().
    uint16_t charge() const { return m_charge; }

private:
    uint16_t m_capacity;
    uint16_t m_charge = 0;
};
static constexpr uint8_t  INPUT_MAX_OBSERVERS = 16;
static constexpr uint8_t  INPUT_INVALID_OBSERVER = 0xFF;

// ---------------------------------------------------------------
// InputState
// ---------------------------------------------------------------

struct InputState
{
    bool keys[NUM_KEYS]    = {};
    bool pressed[NUM_KEYS] = {};
    int  mapKeys[NUM_KEYS] = {};

    bool getHeld(int key)    const { return (key >= 0 && key < NUM_KEYS) ? keys[key]    : false; }
    bool getPressed(int key) const { return (key >= 0 && key < NUM_KEYS) ? pressed[key] : false; }

    bool getPressedOnce(int key, bool (&ctx)[NUM_KEYS]) const
    {
        if (key < 0 || key >= NUM_KEYS) return false;
        if (!pressed[key])  { ctx[key] = false; return false; }
        if (ctx[key])       { return false; }
        ctx[key] = true;
        return true;
    }

    void map(int from, int to) { if (from >= 0 && from < NUM_KEYS) mapKeys[from] = to; }
    void unmap(int key)        { if (key  >= 0 && key  < NUM_KEYS) mapKeys[key]  = 0; }

    void applyDown(int key)
    {
        if (key >= 0 && key < NUM_KEYS && mapKeys[key] != 0) key = mapKeys[key];
        if (key < 0 || key >= NUM_KEYS) return;
        if (!keys[key]) pressed[key] = true;
        keys[key] = true;
    }

    void applyUp(int key)
    {
        if (key >= 0 && key < NUM_KEYS && mapKeys[key] != 0) key = mapKeys[key];
        if (key < 0 || key >= NUM_KEYS) return;
        keys[key]    = false;
        pressed[key] = false;
    }

    void flushPressed()
    {
        for (int i = 0; i < NUM_KEYS; ++i)
            if (keys[i]) pressed[i] = false;
    }
};

/*
 * ONE RING PER CHANNEL, and the channels are keyboard and pointer.
 *
 * A pointer reports hundreds of times a second and a keyboard a few. Share one
 * ring and the fast channel's bursts sit between the slow channel's events, so
 * each waits behind the other -- nothing dropped, both late and uneven, which
 * reads as jitter in whatever they drive.
 *
 * The rule is general: synchronise through a shared channel only what has to be
 * synchronised. Nothing here does. A consumer that wants both binds both, and
 * pays for correlation only where it needs correlation.
 *
 * Single producer, up to INPUT_MAX_OBSERVERS independent readers, each with its
 * own tail and lapped independently.
 */
template <uint32_t CAP>
struct InputRing
{
    static_assert(CAP >= 2, "a ring needs a slot and a margin");

    InputRing()
    {
        for (uint8_t i = 0; i < INPUT_MAX_OBSERVERS; ++i)
        {
            tails[i].store(0, ::std::memory_order_relaxed);
            active[i] = false;
        }
    }

    // Starts reading at the current head: a new observer gets what happens
    // next, never a replay of what it missed.
    uint8_t attach()
    {
        for (uint8_t i = 0; i < INPUT_MAX_OBSERVERS; ++i)
            if (!active[i])
            {
                tails[i].store(head.load(::std::memory_order_acquire), ::std::memory_order_release);
                active[i] = true;
                return i;
            }
        return INPUT_INVALID_OBSERVER;
    }

    void detach(uint8_t id) { if (id < INPUT_MAX_OBSERVERS) active[id] = false; }

    /*
     * HEAD AND TAIL COUNT EVENTS, they are not slot indices, and that is the
     * correctness of this function. `head - tail` in unsigned arithmetic is the
     * true backlog at any distance, so empty / readable / lapped are exactly
     * distinguishable. As indices they were not: a producer that wrote CAP
     * events between two drains brought the index back around and the reader
     * was told the ring was empty.
     *
     * One slot of margin, so an observer is invalidated while the slot it is
     * about to read is still intact.
     */
    bool read(uint8_t id, InputEvent& out)
    {
        if (id >= INPUT_MAX_OBSERVERS || !active[id]) return false;

        const uint32_t t = tails[id].load(::std::memory_order_acquire);
        const uint32_t backlog = head.load(::std::memory_order_acquire) - t;

        if (backlog == 0) return false;
        if (backlog > CAP - 1) { active[id] = false; return false; }   // lapped

        out = slots[t % CAP];
        tails[id].store(t + 1, ::std::memory_order_release);
        return true;
    }

    // Never blocks and never consults the tails: a reader that has fallen more
    // than a ring behind finds out on its own read. Making the producer wait on
    // a consumer would put the OS event pump behind the slowest thing in the
    // process.
    void write(const InputEvent& ev)
    {
        const uint32_t h = head.load(::std::memory_order_relaxed);
        slots[h % CAP] = ev;
        head.store(h + 1, ::std::memory_order_release);
    }

    InputEvent            slots[CAP] = {};
    ::std::atomic<uint32_t> head{ 0 };
    ::std::atomic<uint32_t> tails[INPUT_MAX_OBSERVERS];
    bool                  active[INPUT_MAX_OBSERVERS] = {};
};

// Distribution of OS input to any number of independent readers. Split out of
// Window_ so that something can be an input source without being a window, and
// a window can be had without dragging a ring buffer along.
class InputSource_ : virtual public ETCS::Entity
{
public:
    virtual ~InputSource_() = default;

    // Each channel is registered for separately: a consumer takes the ones it
    // uses and is not woken by the ones it does not.
    uint8_t RegisterKeyObserver()          { return m_keys.attach(); }
    uint8_t RegisterPointerObserver()      { return m_pointer.attach(); }
    void    UnregisterKeyObserver(uint8_t id)     { m_keys.detach(id); }
    void    UnregisterPointerObserver(uint8_t id) { m_pointer.detach(id); }

    // False when empty for this observer, or when it was lapped -- in which
    // case its id is invalidated and the caller must register again.
    bool ReadNextKeyEvent(uint8_t id, ETCS::Buffer& out)     { return read_into(m_keys, id, out); }
    bool ReadNextPointerEvent(uint8_t id, ETCS::Buffer& out) { return read_into(m_pointer, id, out); }

    const InputState& ViewInput() const { return m_inputSnapshot; }

protected:
    void pushKeyDown(int key) { pushKey({ static_cast<uint16_t>(key), INPUT_DOWN, 0, 0, 0 }); }
    void pushKeyUp  (int key) { pushKey({ static_cast<uint16_t>(key), INPUT_UP,   0, 0, 0 }); }

    /*
 * A BUTTON, WITH THE POSITION IT WAS PRESSED AT.
 *
 * Straight onto the pointer ring rather than through notePointerAt: that path
 * COALESCES, which is exactly right for positions and exactly wrong for
 * these. It also flushes any pending position first, so a click cannot
 * overtake the last movement before it -- the two arrive in the order they
 * happened, which is what a stroke's first sample depends on.
 *
 * The keyboard snapshot is deliberately not touched. It answers getHeld() for
 * KEYS, and a mouse button written into it would be a held key nothing ever
 * releases -- the same reason pushKey keeps pointer events out of it.
 */
    void pushButton(int button, bool down, int x, int y)
    {
        // BEFORE the flush, so the pending position that flushes out carries the
        // mask as it was when that position happened rather than as it is after
        // this button changed it.
        const uint8_t bit = input_button_bit(static_cast<uint16_t>(button));
        flushPointerPosition();
        // The STATED bit is left as it was: a press this object saw is a fact
        // about ONE button, not a report on all of them.
        if (down) m_buttons |= bit;
        else      m_buttons = static_cast<uint8_t>(m_buttons & ~bit);
        m_pointer.write({ static_cast<uint16_t>(button),
                          down ? INPUT_BUTTON_DOWN : INPUT_BUTTON_UP,
                          m_buttons, clamp16(x), clamp16(y) });
    }

    /*
 * WHAT THE PLATFORM SAYS IS DOWN RIGHT NOW, for a backend that can ask (the DOM
 * reports it on every mouse event; GLFW answers glfwGetMouseButton). Called
 * before flushing a position, it is what makes a motion event able to say it is
 * a DRAG -- see InputEvent::buttons.
 *
 * A BACKEND THAT CANNOT ANSWER SIMPLY NEVER CALLS THIS, and the mask then
 * reflects only the presses this object saw itself, which is the old behaviour
 * exactly. That is the honest degradation: a source that cannot observe the
 * truth keeps reporting its own belief, and HeldCharge on the consumer's side is
 * what stops either of them being believed forever.
 */
    void noteButtonMask(uint8_t mask)
    { m_buttons = static_cast<uint8_t>((mask & INPUT_BUTTONS_MASK) | INPUT_BUTTONS_STATED); }
    uint8_t buttonMask() const { return m_buttons; }

    /*
 * A wheel notch, and the accumulator behind PointerState's deltas.
 *
 * Flushes the pending position first, exactly as a button does, so the notch
 * lands in the ring AFTER the position it happened at -- a consumer replaying
 * the stream then has the right position current when the scroll arrives.
 *
 * Deltas are ALSO accumulated for Pointer_::ReadPointer, because the two
 * readers want different things (see INPUT_SCROLL). Reading through
 * takeScrollX/Y consumes them; the event copy does not.
 */
    void pushScroll(float dx, float dy)
    {
        flushPointerPosition();
        m_scroll_x += dx;
        m_scroll_y += dy;
        m_pointer.write({ 0, INPUT_SCROLL, m_buttons,
                          clamp16(static_cast<int>(dx)), clamp16(static_cast<int>(dy)) });
    }

    // Consumed on read -- a delta that survived being read would be applied
    // twice. Pointer_::ReadPointer is the only intended caller.
    float takeScrollX() { const float v = m_scroll_x; m_scroll_x = 0.0f; return v; }
    float takeScrollY() { const float v = m_scroll_y; m_scroll_y = 0.0f; return v; }

    // What the pointer ring last recorded, for a Pointer_ that answers "where is
    // it now" rather than "what happened". Not the ring's tail: a poller must
    // not consume events a stream reader is also owed.
    int32_t currentPointerX() const { return m_pendingX; }
    int32_t currentPointerY() const { return m_pendingY; }

    /*
 * COALESCING, and why the pointer gets it and keys never can.
 *
 * A pointer reports as fast as its hardware does and nothing downstream
 * consumes at that rate. Every report between two reads is not extra
 * information, it is the same information restated, because A POSITION
 * SUPERSEDES THE ONE BEFORE IT. Keeping the last of a run is exactly equal to
 * pushing each and having the consumer keep the last.
 *
 * Keys neither add nor supersede -- two presses coalesced into one is a lost
 * keystroke -- so the property being exploited is offered only to the kind that
 * has it.
 *
 * Both halves run on the thread that pumps the OS queue: record from inside the
 * callback, flush once the queue is drained. One thread by construction, so
 * neither needs synchronisation.
 */
    void notePointerAt(int x, int y)
    {
        m_pendingX = x;
        m_pendingY = y;
        m_pendingMotion = true;
    }

    void flushPointerPosition()
    {
        if (!m_pendingMotion) return;
        m_pointer.write({ 0, INPUT_MOTION, m_buttons,
                          clamp16(m_pendingX), clamp16(m_pendingY) });
        m_pendingMotion = false;
    }

private:
    InputState m_inputSnapshot;

    /*
 * DEPTHS CHOSEN AGAINST EACH CHANNEL'S OWN WORST CASE, not one number for both.
 *
 * Pointer: coalesced to one position per poll pass, and each supersedes the
 * last, so a lapped observer loses only staleness. Sixteen is generous.
 *
 * Keys: bursty, and nothing supersedes anything -- a lost key is a lost key --
 * so this is deep enough to absorb a fast typist against a consumer that
 * stalled for a frame or two.
 */
    InputRing<16>  m_pointer;
    InputRing<128> m_keys;

    // Pump-thread only; see notePointerAt.
    int  m_pendingX = 0;
    int  m_pendingY = 0;
    bool m_pendingMotion = false;
    // Accumulated since the last ReadPointer -- see pushScroll.
    float m_scroll_x = 0.0f;
    float m_scroll_y = 0.0f;
    // Stamped onto every pointer event written below -- see InputEvent::buttons.
    // Pump-thread only, like m_pending*, and for the same reason.
    uint8_t m_buttons = 0;

    void pushKey(InputEvent ev)
    {
        m_keys.write(ev);
        // The snapshot is a KEYBOARD view (getHeld/getPressed), so only key
        // events reach it. A pointer event fed in would set a held "key"
        // nothing ever releases.
        if      (ev.action == INPUT_DOWN) m_inputSnapshot.applyDown(ev.key);
        else if (ev.action == INPUT_UP)   m_inputSnapshot.applyUp(ev.key);
    }

    template <uint32_t CAP>
    static bool read_into(InputRing<CAP>& ring, uint8_t id, ETCS::Buffer& out)
    {
        InputEvent ev{};
        if (!ring.read(id, ev)) return false;
        ::std::memcpy(out.buf, &ev, INPUT_SLOT_SIZE);
        out.written = INPUT_SLOT_SIZE;
        out.read_offset = 0;
        return true;
    }

    static int16_t clamp16(int v)
    {
        if (v >  32767) return  32767;
        if (v < -32768) return -32768;
        return static_cast<int16_t>(v);
    }
};

#endif
