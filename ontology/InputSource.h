#ifndef SUPERTYPE_INPUTSOURCE_H__
#define SUPERTYPE_INPUTSOURCE_H__


#include "../core_defs.h"
#include <cstdint>
#include <cstring>
#include <atomic>

#define NUM_KEYS 1024

// for verbose input logging, define this before compiling
// #define ETCS_VERBOSE_INPUT_EVENTS

// ---------------------------------------------------------------
// Ring event record
// ---------------------------------------------------------------


// One input event, keyboard or pointer, in ONE record.
//
// Two kinds in one struct rather than two rings, because a consumer that
// wants both wants them IN ORDER -- a look and a step that arrived together
// are one intent, and two rings would let them separate. The kind is the
// action field, which already distinguished up from down and now also says
// "this is not a key at all".
//
// THAT IS ALSO THE ANSWER TO "GIVE MOTION ITS OWN RING", which is the obvious
// response to motion arriving hundreds of times a second while keys arrive
// twice. The problem a second ring solves is RATE, and rate has a cheaper
// answer that costs no ordering: motion deltas ADD, so a run of them is
// losslessly one event (accumulatePointerDelta below). Two rings would spend
// the ordering guarantee to buy something coalescing gives away.
//
// dx/dy are RELATIVE, in pixels, and only meaningful for INPUT_MOTION. A
// delta rather than a position because the thing that moves the view is the
// movement: an absolute position is unanswerable once the cursor is captured
// and has no edge of the screen to be near, which is exactly the mode a
// first-person look runs in.
struct InputEvent
{
    uint16_t key;    // supports NUM_KEYS up to 65535; 0 for a pointer event
    uint8_t  action; // INPUT_UP / INPUT_DOWN / INPUT_MOTION
    uint8_t  _pad;   // reserved (mods, scancode, etc.)
    int16_t  dx;     // pointer delta, INPUT_MOTION only
    int16_t  dy;
};

static constexpr uint8_t INPUT_UP     = 0;
static constexpr uint8_t INPUT_DOWN   = 1;
static constexpr uint8_t INPUT_MOTION = 2;

static constexpr uint32_t INPUT_SLOT_SIZE = sizeof(InputEvent);

// THE RING'S DEPTH IS ITS OWN DECISION, which is why this is a number and no
// longer 256/sizeof(InputEvent). The ring used to be carved out of an
// ETCS::Buffer, so its capacity was MAX_TAG_BUFFER_SIZE divided by the record
// -- and widening InputEvent from four bytes to eight silently halved it from
// 64 slots to 32, with nothing anywhere saying that had happened. A depth that
// changes as a side effect of an unrelated struct growing is not a depth, it
// is a coincidence.
//
// 128 is chosen against the worst honest case rather than a round number: a
// 1000Hz pointer against a 60Hz drain is ~17 motion events per pass, and with
// coalescing it is one, so 127 usable is headroom for a key burst arriving on
// top of a stall rather than a bet on the mouse.
static constexpr uint32_t INPUT_RING_CAP  = 128;   // 127 usable, one slot of margin
static constexpr uint8_t INPUT_MAX_OBSERVERS = 16;
static constexpr uint8_t INPUT_INVALID_OBSERVER = 0xFF;

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

// ---------------------------------------------------------------
// InputSource
// ---------------------------------------------------------------
//
// Keyboard event distribution -- split out of what used to be
// Window_ (see Window.h's own comment). A single-producer /
// multi-observer ring buffer: one source (e.g. a GLFW key callback)
// pushes events, up to INPUT_MAX_OBSERVERS independent readers each
// track their own tail and get lapped (invalidated, must
// re-register) independently if they fall behind.

class InputSource_ : virtual public ETCS::Entity
{
public:
    InputSource_()
    {
        for (uint8_t i = 0; i < INPUT_MAX_OBSERVERS; ++i)
        {
            m_tails[i].store(0, std::memory_order_relaxed);
            m_tailActive[i] = false;
        }
    }

    virtual ~InputSource_() = default;

    // Register a new observer, returns id to pass to readNextRingEvent.
    // Returns INPUT_INVALID_OBSERVER if all slots are full.
    uint8_t RegisterObserver()
    {
        for (uint8_t i = 0; i < INPUT_MAX_OBSERVERS; ++i)
        {
            if (!m_tailActive[i])
            {
                // Start reading from current head — don't replay stale events
                m_tails[i].store(m_head.load(std::memory_order_acquire), std::memory_order_release);
                m_tailActive[i] = true;
                return i;
            }
        }
        return INPUT_INVALID_OBSERVER;
    }

    void UnregisterObserver(uint8_t id)
    {
        if (id < INPUT_MAX_OBSERVERS)
            m_tailActive[id] = false;
    }

    /*
 * Returns false if the ring is empty for this observer, or if it was lapped.
 * On lap the observer id is invalidated -- the caller must re-register.
 *
 * HEAD AND TAIL ARE FREE-RUNNING, not slot indices, and that is the whole
 * correctness of this function. They count events ever written and ever read,
 * and only get %CAP'd at the moment a slot is addressed. So `head - tail` in
 * unsigned arithmetic is the TRUE backlog at any distance, and the three cases
 * are exactly distinguishable: zero is empty, up to CAP-1 is readable, more
 * than that is lapped.
 *
 * As slot indices they were not distinguishable, and the failure was silent.
 * A producer that wrote CAP events between two drains brought the index back
 * around to the tail, `tail == head` read as EMPTY, and a full ring of input
 * was dropped with no lap reported -- the reader was told nothing had
 * happened, which is the one answer that was certainly wrong. Only the head
 * being exactly one slot short was ever caught, so the check fired for the
 * near-miss and missed every real overrun.
 */
    bool ReadNextRingEvent(uint8_t observerId, ETCS::Buffer& out)
    {
        if (observerId >= INPUT_MAX_OBSERVERS || !m_tailActive[observerId])
            return false;

        const uint32_t tail = m_tails[observerId].load(std::memory_order_acquire);
        const uint32_t head = m_head.load(std::memory_order_acquire);
        const uint32_t backlog = head - tail;

        if (backlog == 0) return false;                 // empty for this observer

        // One slot of margin: invalidated while the slot about to be read is
        // still intact, rather than after the producer has already begun
        // overwriting it.
        if (backlog > INPUT_RING_CAP - 1)
        {
            m_tailActive[observerId] = false;
            return false;
        }

        InputEvent ev = readSlot(tail);
        m_tails[observerId].store(tail + 1, std::memory_order_release);

        std::memcpy(out.buf, &ev, INPUT_SLOT_SIZE);
        out.written = INPUT_SLOT_SIZE;
        out.read_offset = 0;
        return true;
    }

    const InputState& ViewInput() const { return m_inputSnapshot; }

protected:
    void pushKeyDown(int key) { pushEvent({ static_cast<uint16_t>(key), INPUT_DOWN, 0, 0, 0 }); }
    void pushKeyUp  (int key) { pushEvent({ static_cast<uint16_t>(key), INPUT_UP,   0, 0, 0 }); }

    // A pointer movement, as a delta. Key 0 because there is no key: a
    // consumer switches on the action, and a pointer event that carried a
    // plausible key code would eventually be read as one.
    //
    // CLAMPED, not truncated. A coalesced run can exceed int16 range, and a
    // cast would wrap it -- turning the largest movement the user ever makes
    // into a delta pointing the other way, which is the one error a look
    // control cannot recover from. At the clamp the view is already spinning
    // faster than anyone can follow, so the saturated value is indistinguish-
    // able from the true one.
    void pushPointerDelta(int dx, int dy)
    {
        if (dx == 0 && dy == 0) return;   // not an event
        pushEvent({ 0, INPUT_MOTION, 0, clamp16(dx), clamp16(dy) });
    }

    /*
 * COALESCING, and why motion gets it and keys never can.
 *
 * A pointer reports as fast as its hardware does -- 1000Hz is ordinary -- and
 * nothing downstream consumes at that rate: the drain runs once per poll pass
 * and a view angle is applied once per frame. Every event in between is not
 * extra information, it is the same information split up, because DELTAS ADD.
 * Summing a run and pushing it once is exactly equal to pushing each and
 * having the consumer sum them, which is what the consumer does anyway.
 *
 * Keys are the opposite and this must never be applied to them: a down and an
 * up are not summable, and two presses coalesced into one is a lost keystroke.
 * Additivity is the property being exploited, so it is only offered to the
 * event kind that has it.
 *
 * SINGLE-THREADED BY CONSTRUCTION -- both halves run on whichever thread pumps
 * the OS queue: accumulate from inside the callback, flush once the pump
 * returns. That is one thread, because an OS event queue has one pump, so
 * these need no synchronisation. They are protected rather than public for
 * that reason: only the source doing the pumping may call them.
 */
    void accumulatePointerDelta(int dx, int dy)
    {
        m_pendingDx += dx;
        m_pendingDy += dy;
    }

    void flushPointerDelta()
    {
        if (m_pendingDx == 0 && m_pendingDy == 0) return;
        pushPointerDelta(m_pendingDx, m_pendingDy);
        m_pendingDx = 0;
        m_pendingDy = 0;
    }

private:
    InputState             m_inputSnapshot;

    // The ring's own storage, sized by the ring's own constant. It used to
    // live in an ETCS::Buffer, which is what coupled its depth to a tag
    // buffer -- see INPUT_RING_CAP.
    InputEvent             m_eventRing[INPUT_RING_CAP] = {};

    // Free-running counts of events written and read, NOT slot indices --
    // see ReadNextRingEvent for why that distinction is the correctness.
    std::atomic<uint32_t>  m_head{ 0 };
    std::atomic<uint32_t>  m_tails[INPUT_MAX_OBSERVERS];
    bool                   m_tailActive[INPUT_MAX_OBSERVERS] = {};

    // Pump-thread only; see accumulatePointerDelta.
    int                    m_pendingDx = 0;
    int                    m_pendingDy = 0;

    static int16_t clamp16(int v)
    {
        if (v >  32767) return  32767;
        if (v < -32768) return -32768;
        return static_cast<int16_t>(v);
    }

    void pushEvent(InputEvent ev)
    {
        const uint32_t head = m_head.load(std::memory_order_relaxed);

        // Never blocks and never checks the tails: an observer that has fallen
        // more than a ring behind is detected by its own read (backlog >
        // CAP-1) and invalidated there. Making the producer wait on a reader
        // would put the OS event pump behind the slowest consumer in the
        // process, which is the stall this whole path exists to avoid.
        writeSlot(head, ev);
        m_head.store(head + 1, std::memory_order_release);

        // Also apply to local snapshot for viewInput() queries. Motion is
        // excluded rather than mapped to key 0: InputState is a keyboard
        // snapshot, and feeding it a pointer event would set a held "key"
        // nothing ever releases.
        if      (ev.action == INPUT_DOWN) m_inputSnapshot.applyDown(ev.key);
        else if (ev.action == INPUT_UP)   m_inputSnapshot.applyUp(ev.key);
    }

    // The counters are free-running, so the modulo lives HERE and only here --
    // the one place a count becomes an address.
    void writeSlot(uint32_t count, InputEvent ev)
    {
        m_eventRing[count % INPUT_RING_CAP] = ev;
    }

    InputEvent readSlot(uint32_t count) const
    {
        return m_eventRing[count % INPUT_RING_CAP];
    }
};

#endif
