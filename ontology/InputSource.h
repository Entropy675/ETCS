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
    uint8_t  _pad;   // reserved (mods, scancode, etc.)
    int16_t  x;      // pointer position in the content area, INPUT_MOTION only
    int16_t  y;
};

static constexpr uint8_t INPUT_UP     = 0;
static constexpr uint8_t INPUT_DOWN   = 1;
static constexpr uint8_t INPUT_MOTION = 2;

static constexpr uint32_t INPUT_SLOT_SIZE = sizeof(InputEvent);
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
            tails[i].store(0, std::memory_order_relaxed);
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
                tails[i].store(head.load(std::memory_order_acquire), std::memory_order_release);
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

        const uint32_t t = tails[id].load(std::memory_order_acquire);
        const uint32_t backlog = head.load(std::memory_order_acquire) - t;

        if (backlog == 0) return false;
        if (backlog > CAP - 1) { active[id] = false; return false; }   // lapped

        out = slots[t % CAP];
        tails[id].store(t + 1, std::memory_order_release);
        return true;
    }

    // Never blocks and never consults the tails: a reader that has fallen more
    // than a ring behind finds out on its own read. Making the producer wait on
    // a consumer would put the OS event pump behind the slowest thing in the
    // process.
    void write(const InputEvent& ev)
    {
        const uint32_t h = head.load(std::memory_order_relaxed);
        slots[h % CAP] = ev;
        head.store(h + 1, std::memory_order_release);
    }

    InputEvent            slots[CAP] = {};
    std::atomic<uint32_t> head{ 0 };
    std::atomic<uint32_t> tails[INPUT_MAX_OBSERVERS];
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
        m_pointer.write({ 0, INPUT_MOTION, 0, clamp16(m_pendingX), clamp16(m_pendingY) });
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
        std::memcpy(out.buf, &ev, INPUT_SLOT_SIZE);
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
