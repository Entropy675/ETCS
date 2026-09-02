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
// answer that costs no ordering: a run of positions between two reads is
// answered by the LAST one (notePointerAt below). Two rings would spend the
// ordering guarantee to buy something coalescing gives away.
//
// x/y are ABSOLUTE, in pixels, relative to the window's content area, and only
// meaningful for INPUT_MOTION.
//
// A POSITION RATHER THAN A DELTA, which is the opposite of what this carried
// and is worth the paragraph, because the delta version was not merely
// inconvenient -- it could not be produced correctly on some platforms at all.
//
// A relative control needs the pointer to have somewhere to keep going, so it
// needs the pointer LOCKED: hidden, and teleported back to the centre
// whenever it strays. Toolkits implement that by warping the cursor and
// reporting an accumulated virtual position instead of a real one. Every part
// of that is a request the display may decline -- Qubes proxies windows from
// another domain, XWayland answers to a compositor, remote X has no local
// pointer to move -- and when the warp is declined the accumulator does not
// degrade, it starts reporting the pointer's DISTANCE FROM THE CENTRE as
// though it were a movement. The screen becomes a joystick, and nothing in the
// stack can tell that it happened.
//
// A position asks for none of that. It is what the window already knows,
// needs no cursor to be moved, and is strictly MORE information than a delta:
// a consumer that wants relative motion differences two positions itself,
// while a consumer given deltas can never recover where the pointer is. So the
// primitive is the position, and relativity becomes a choice made downstream
// rather than a capability the platform has to grant.
struct InputEvent
{
    uint16_t key;    // supports NUM_KEYS up to 65535; 0 for a pointer event
    uint8_t  action; // INPUT_UP / INPUT_DOWN / INPUT_MOTION
    uint8_t  _pad;   // reserved (mods, scancode, etc.)
    int16_t  x;      // pointer position in the content area, INPUT_MOTION only
    int16_t  y;
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

    // A pointer position. Key 0 because there is no key: a consumer switches
    // on the action, and a pointer event that carried a plausible key code
    // would eventually be read as one.
    //
    // Clamped rather than cast, so a position outside int16 saturates at the
    // edge instead of wrapping to the far side. Only reachable on a display
    // wider than 32767 pixels, and saturating is the harmless answer there.
    void pushPointerAt(int x, int y)
    {
        pushEvent({ 0, INPUT_MOTION, 0, clamp16(x), clamp16(y) });
    }

    /*
 * COALESCING, and why motion gets it and keys never can.
 *
 * A pointer reports as fast as its hardware does -- 1000Hz is ordinary -- and
 * nothing downstream consumes at that rate: the drain runs once per poll pass
 * and a view angle is applied once per frame. Every report in between is not
 * extra information, it is the same information restated, because A POSITION
 * SUPERSEDES THE ONE BEFORE IT. Keeping the last of a run and pushing it once
 * is exactly equal to pushing each and having the consumer keep the last,
 * which is what the consumer does anyway.
 *
 * (When these carried deltas the same argument held for a different reason --
 * deltas ADD, so a run summed to one event. Superseding is the stronger form:
 * summing is only lossless if nothing is dropped, while the last position is
 * correct even if every earlier one is lost. That robustness is free here and
 * was not free before.)
 *
 * Keys are the opposite and this must never be applied to them: a down and an
 * up neither add nor supersede, and two presses coalesced into one is a lost
 * keystroke. The property being exploited belongs to positions, so it is only
 * offered to the event kind that has it.
 *
 * SINGLE-THREADED BY CONSTRUCTION -- both halves run on whichever thread pumps
 * the OS queue: record from inside the callback, flush once the pump returns.
 * That is one thread, because an OS event queue has one pump, so these need no
 * synchronisation. They are protected rather than public for that reason: only
 * the source doing the pumping may call them.
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
        pushPointerAt(m_pendingX, m_pendingY);
        m_pendingMotion = false;
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

    // Pump-thread only; see notePointerAt.
    int                    m_pendingX = 0;
    int                    m_pendingY = 0;
    bool                   m_pendingMotion = false;

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
