#ifndef PROVENANCE_H__
#define PROVENANCE_H__
#include "Buffer.h"

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

/*
 * PROVENANCE -- how an entity came to have the tags it has.
 *
 * An entity that claims Environmental (ontology/Environmental.h) can be
 * rebuilt: locally from its own record, or as a reflection on the far side of
 * a MirrorBuffer. What rebuilds it is not a copy of its state but the ETCS
 * actions that produced that state, replayed -- so the runtime records, for
 * each tag that goes on or off an Environmental entity (or a plain entity
 * beneath it), the ONE script-level action it happened inside. The record
 * itself is the family's (IWireEnvironmental, core/InterfaceWire.h): an entity
 * that never claims it carries nothing.
 *
 * ONE ACTION, THE OUTERMOST. A work function that calls others is replayed by
 * replaying it; the calls it makes happen again by themselves. So the frame
 * recorded is the one the executor (or whatever issued the call from outside
 * any call) opened, and everything beneath it is credited to it.
 *
 * CARRIED BY THE CALL, NOT HELD BY THE PROCESS. Each binary keeps its own
 * current frame (a thread-local here is one copy per binary, modules being
 * built hidden). A frame crosses into another binary the way everything else
 * about a call does: the dispatch stamps it on the SignalContext it forwards
 * (WorkBundle::operator()), and the callee's trampoline installs it for the
 * body (ETCS_ACTION_SCOPE, ETCS_API.h). A call arriving with none -- from a
 * thread of a module's own, over a link -- opens its own there.
 *
 * RIDS ONLY AT RUNTIME. A frame names its receiver and the entities its
 * payload referred to (@name) by RID, because that is what exists while it
 * runs; the script generated from them (etcs_replay_capture, Entity.h)
 * names them by where they sit in the rebuilt graph instead, and no RID is
 * ever stored.
 */
namespace ETCS
{

using RID_T = uint64_t;

struct ActionLine
{
    RID_T       receiver = 0;
    std::string verb;            // "spawn" for a child made by the script
    std::string payload;         // as written: @names unresolved
    std::vector<std::pair<std::string, RID_T>> refs;   // @name -> RID at the time
    RID_T       stream_to = 0;   // a stream pair's consumer, when this line was one
    std::string stream_verb;
};

struct ActionRecord
{
    uint64_t                 seq = 0;
    uint64_t                 frame = 0;       // 0: nothing was running -- not replayable
    ActionLine               line;
    std::vector<std::string> created, removed;
};

struct ActionFrame
{
    ActionLine line;
    uint64_t   id = 0;
    // A frame a trampoline opens keeps its verb and payload as Buffers --
    // copied, since the body answers in the same one -- and turns them into
    // strings only if something under it is recorded: most calls change no
    // Environmental entity, and a hot path should not pay two allocations to
    // find that out.
    Buffer lazy_verb, lazy_payload;
    bool   lazy = false;
    void settle()
    {
        if (!lazy) return;
        line.verb    = lazy_verb.toString();
        line.payload = lazy_payload.toString();
        lazy = false;
    }
};

// This binary's frame on this thread.
inline ActionFrame*& current_action_frame()
{
    static thread_local ActionFrame* f = nullptr;
    return f;
}

/*
 * Frame ids without a shared counter: this binary's own count, under high
 * bits taken from where this binary's counter lives -- distinct per binary,
 * so two frames opened in two modules never read as one.
 */
inline uint64_t next_action_frame_id()
{
    static std::atomic<uint32_t> next{ 0 };
    static const uint64_t salt = [] {
        uint64_t a = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(&next));
        a ^= a >> 33; a *= 0xff51afd7ed558ccdULL; a ^= a >> 33;
        return (a << 32) | (1ULL << 63);
    }();
    return salt | ++next;
}

// Installs a frame for this thread if none is; the outermost one wins.
struct ActionScope
{
    ActionFrame  frame;
    ActionFrame* saved = nullptr;
    bool         outer = false;

    // A line stated by whoever runs it (the executor's script lines).
    explicit ActionScope(ActionLine line)
    {
        saved = current_action_frame();
        if (saved) return;
        outer      = true;
        frame.line = std::move(line);
        frame.id   = next_action_frame_id();
        current_action_frame() = &frame;
    }
    // A trampoline's: the frame its caller carried in, if any, else a lazy one
    // of its own naming the call (ActionFrame::settle).
    ActionScope(const ActionFrame* carried, RID_T receiver, const Buffer& verb, const Buffer& payload)
    {
        saved = current_action_frame();
        if (saved) return;
        outer = true;
        if (carried) { current_action_frame() = const_cast<ActionFrame*>(carried); return; }
        frame.line.receiver = receiver;
        frame.lazy_verb     = verb;
        frame.lazy_payload  = payload;
        frame.lazy          = true;
        frame.id            = next_action_frame_id();
        current_action_frame() = &frame;
    }
    ~ActionScope() { if (outer) current_action_frame() = saved; }
    ActionScope(const ActionScope&) = delete;
    ActionScope& operator=(const ActionScope&) = delete;
};

/*
 * Where this runtime keeps what outlives it (Persistence, DatabaseProvider):
 * ETCS_STORE if set, else $XDG_DATA_HOME/etcs, else ~/.local/share/etcs; in a
 * browser, the IndexedDB-backed /persist. One answer, so the loader and the
 * store agree on it without either asking the other.
 */
inline std::string etcs_store_dir()
{
#if defined(__EMSCRIPTEN__)
    return "/persist";
#else
    if (const char* s = std::getenv("ETCS_STORE"); s && *s) return s;
    if (const char* x = std::getenv("XDG_DATA_HOME"); x && *x) return std::string(x) + "/etcs";
    if (const char* h = std::getenv("HOME"); h && *h) return std::string(h) + "/.local/share/etcs";
    return ".etcs";
#endif
}

} // namespace ETCS

#endif // PROVENANCE_H__
