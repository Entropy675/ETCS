#ifndef PROVENANCE_H__
#define PROVENANCE_H__
#include "Buffer.h"

#if !defined(_WIN32)
#include <dlfcn.h>
#endif

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <mutex>
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
 * beneath it), the ONE script-level action it happened inside.
 *
 * ONE ACTION, THE OUTERMOST. A work function that calls others is replayed by
 * replaying it; the calls it makes happen again by themselves. So the frame
 * recorded is the one the executor (or whatever issued the call from outside
 * any call) opened, and everything beneath it is credited to it.
 *
 * RIDS ONLY AT RUNTIME. A frame names its receiver and the entities its
 * payload referred to (@name) by RID, because that is what exists while it
 * runs; the script generated from them (etcs_replay_capture, Entity.h)
 * names them by where they sit in the rebuilt graph instead, and no RID is
 * ever stored.
 *
 * Opt-in by construction: nothing is recorded for an entity with no
 * Environmental entity at or above it, so a type that never claims the
 * family pays one thread-local read per call and nothing else.
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
    // A call opened from C++ (Entity::call) keeps its verb and payload as
    // Buffers -- copied, since the body answers in the same one -- and turns
    // them into strings only if something under it is recorded: most calls
    // change no Environmental entity, and a hot path should not pay two
    // allocations to find that out.
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

/*
 * ONE SLOT PER PROCESS. The executor that opens a frame is the loader's code
 * and the work function that changes a tag is a module's, and each binary
 * has its own copy of every inline static (modules are built hidden). So the
 * frame, the frame counter, the script names and the closing state live in
 * the loader, and a module finds them the way it finds the loader's manifest:
 * ETCS_GetProvenance, looked up in the process's global scope
 * (DynamicLoader.h). With no loader in the process a binary keeps its own.
 */
struct ProvenanceShared
{
    explicit ProvenanceShared(ActionFrame*& (*f)()) : frame(f) {}
    ActionFrame*& (*frame)();                   // the CALLING thread's, in the loader's TLS
    std::atomic<uint64_t>        next_frame{ 0 };
    std::mutex                   names_mu;
    std::map<RID_T, std::string> names;         // see note_script_name
    std::atomic<bool>            closing{ false };
    std::atomic<void (*)()>      closing_hook{ nullptr };
};

inline ActionFrame*& provenance_thread_frame()
{
    static thread_local ActionFrame* f = nullptr;
    return f;
}
inline ProvenanceShared& provenance_local()
{
    static ProvenanceShared p(&provenance_thread_frame);
    return p;
}
inline ProvenanceShared& provenance()
{
    static ProvenanceShared* const p = []() -> ProvenanceShared*
    {
#if !defined(ETCS_LOADER)
  #ifdef _WIN32
        void* sym = reinterpret_cast<void*>(GetProcAddress(GetModuleHandleA(nullptr), "ETCS_GetProvenance"));
  #else
        void* sym = dlsym(RTLD_DEFAULT, "ETCS_GetProvenance");
  #endif
        if (sym) return static_cast<ProvenanceShared*>(reinterpret_cast<void* (*)()>(sym)());
#endif
        return &provenance_local();
    }();
    return *p;
}

inline ActionFrame*& current_action_frame() { return provenance().frame(); }

// Opens a frame if none is open on this thread; the outermost one wins.
struct ActionScope
{
    ActionFrame  frame;
    ActionFrame* saved = nullptr;
    bool         outer = false;
    explicit ActionScope(ActionLine line)
    {
        saved = current_action_frame();
        if (saved) return;
        outer       = true;
        frame.line  = std::move(line);
        frame.id    = ++provenance().next_frame;
        current_action_frame() = &frame;
    }
    // The lazy form (ActionFrame::settle).
    ActionScope(RID_T receiver, const Buffer& verb, const Buffer& payload)
    {
        saved = current_action_frame();
        if (saved) return;
        outer               = true;
        frame.line.receiver = receiver;
        frame.lazy_verb     = verb;
        frame.lazy_payload  = payload;
        frame.lazy          = true;
        frame.id            = ++provenance().next_frame;
        current_action_frame() = &frame;
    }
    ~ActionScope() { if (outer) current_action_frame() = saved; }
    ActionScope(const ActionScope&) = delete;
    ActionScope& operator=(const ActionScope&) = delete;
};

/*
 * The name a script gave an entity it made at global scope -- what a rebuilt
 * scene calls it again, so a resumed session answers to the names it had.
 * A hint, not an identity: the last script to make that RID wins.
 */
inline void note_script_name(RID_T rid, const std::string& name)
{
    auto& p = provenance();
    std::lock_guard<std::mutex> lock(p.names_mu);
    p.names[rid] = name;
}
inline void forget_script_name(RID_T rid)
{
    auto& p = provenance();
    std::lock_guard<std::mutex> lock(p.names_mu);
    p.names.erase(rid);
}
inline std::string script_name(RID_T rid)
{
    auto& p = provenance();
    std::lock_guard<std::mutex> lock(p.names_mu);
    auto it = p.names.find(rid);
    return it == p.names.end() ? std::string() : it->second;
}

/*
 * CLOSING. What keeps this runtime (DatabaseProvider's Persistence) saves
 * as it goes; when the loader leaves -- `exit`, Ctrl+C, a drain -- it says so
 * here once, BEFORE anything is torn down, and the keeper saves one last
 * time and then never again this run: what a teardown looks like half-way
 * is not a scene anyone left.
 */
inline bool runtime_closing() { return provenance().closing.load(std::memory_order_acquire); }
inline void set_closing_hook(void (*f)()) { provenance().closing_hook.store(f); }
inline void runtime_close()
{
    if (provenance().closing.exchange(true)) return;
    if (auto f = provenance().closing_hook.load()) f();
}

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
