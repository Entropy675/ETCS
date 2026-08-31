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
// Ring event record — 4 bytes per slot
// Sentinel slot reserved: effective capacity is 63, not 64
// ---------------------------------------------------------------


struct InputEvent
{
    uint16_t key;    // supports NUM_KEYS up to 65535
    uint8_t  action; // 0 = up, 1 = down
    uint8_t  _pad;   // reserved (mods, scancode, etc.)
};

static constexpr uint8_t INPUT_SLOT_SIZE = sizeof(InputEvent); // 4
static constexpr uint8_t INPUT_RING_CAP  = 256 / INPUT_SLOT_SIZE; // 64 slots, 63 usable
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

    // Returns false if ring is empty for this observer, or if observer was lapped.
    // On lap, observer id is invalidated — caller must re-register.
    bool ReadNextRingEvent(uint8_t observerId, ETCS::Buffer& out)
    {
        if (observerId >= INPUT_MAX_OBSERVERS || !m_tailActive[observerId])
            return false;

        uint8_t tail = m_tails[observerId].load(std::memory_order_acquire);
        uint8_t head = m_head.load(std::memory_order_acquire);

        if (tail == head) return false; // empty for this observer

        // Check for lap: next write slot is (head+1) % CAP, but we detect
        // lap as the head having already passed this tail by checking distance
        uint8_t next = (head + 1) % INPUT_RING_CAP;
        if (next == tail)
        {
            // Head has lapped this tail — invalidate and signal re-register
            m_tailActive[observerId] = false;
            return false;
        }

        InputEvent ev = readSlot(tail);
        m_tails[observerId].store((tail + 1) % INPUT_RING_CAP, std::memory_order_release);

        std::memcpy(out.buf, &ev, INPUT_SLOT_SIZE);
        out.written = INPUT_SLOT_SIZE;  // ← missing
        out.read_offset = 0;
        return true;
    }

    const InputState& ViewInput() const { return m_inputSnapshot; }

protected:
    void pushKeyDown(int key) { pushEvent({ static_cast<uint16_t>(key), 1, 0 }); }
    void pushKeyUp  (int key) { pushEvent({ static_cast<uint16_t>(key), 0, 0 }); }

private:
    InputState             m_inputSnapshot;
    ETCS::Buffer           m_eventRing = {};

    std::atomic<uint8_t>   m_head{ 0 };
    std::atomic<uint8_t>   m_tails[INPUT_MAX_OBSERVERS];
    bool                   m_tailActive[INPUT_MAX_OBSERVERS] = {};

    void pushEvent(InputEvent ev)
    {
        uint8_t head = m_head.load(std::memory_order_relaxed);
        uint8_t next = (head + 1) % INPUT_RING_CAP;

        // Sentinel: next == any active tail means that observer gets lapped on this write.
        // We don't block — we just let readNextRingEvent detect and invalidate lazily.

        writeSlot(head, ev);
        m_head.store(next, std::memory_order_release);

        // Also apply to local snapshot for viewInput() queries
        if (ev.action == 1) m_inputSnapshot.applyDown(ev.key);
        else                m_inputSnapshot.applyUp(ev.key);
    }

    void writeSlot(uint8_t slot, InputEvent ev)
    {
        uint8_t* base = reinterpret_cast<uint8_t*>(m_eventRing.buf) + (slot * INPUT_SLOT_SIZE);
        std::memcpy(base, &ev, INPUT_SLOT_SIZE);
    }

    InputEvent readSlot(uint8_t slot) const
    {
        InputEvent ev{};
        const uint8_t* base = reinterpret_cast<const uint8_t*>(m_eventRing.buf) + (slot * INPUT_SLOT_SIZE);
        std::memcpy(&ev, base, INPUT_SLOT_SIZE);
        return ev;
    }
};

#endif
