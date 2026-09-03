#ifndef BUNDLES_H__
#define BUNDLES_H__
// Pointer bundles for mapping ontology-abiding DLL/SOs. Everything here is
// meant to be cheaply copyable.
#include <iostream>
#include <csignal>
#include <charconv>
#include <string_view>
#include <functional>
#include <cstring>
#include <typeinfo>
#include <climits>
#include <unordered_map>
#include <cstdint>
#include <type_traits>
#include <stdexcept>
#include <iomanip>
#include "MemoryArena.h"
// Every path here goes through ETCS_API.h first (it includes Bundles.h;
// Entity.h and EventNode.h reach it ahead of this). Asserted rather than
// assumed, and deliberately given no fallback -- a second definition quietly
// disagreeing with the real one is the failure this epoch exists to remove.
#ifndef ETCS_MAX_MODULE_TAGS
#  error "Bundles.h needs ETCS_API.h first -- ETCS_MAX_MODULE_TAGS is undefined."
#endif
namespace ETCS
{
// TAG_BITS -- the TYPE vocabulary: one bit per contract tag a module declares.
// The same fact as ETCS_MAX_MODULE_TAGS, not a second one, and
// MAX_BOUNDARY_BUFFER_SIZE derives from it too -- so a module can never declare
// more tags than its Tags string can carry across the ABI boundary.
static constexpr size_t TAG_BITS  = ETCS_MAX_MODULE_TAGS;
static constexpr size_t TAG_WORDS = (TAG_BITS + 63) / 64;
static_assert(TAG_BITS <= 256,
    "TAG_BITS beyond 256 exceeds the uint8_t index EventNode::tag_bit_index stores");
// TagMask -- fixed-width bitset. Above the MirrorBuffer include because
// MirrorBuffer carries two and Bundles is what pulls it in.
//
// Fixed width, never per-module: GapReorderBuffer is a member of EventStream,
// whose layout is load-bearing across dlopen (see DLInEvent::reply_to).
//
// TWO BIT SPACES SHARE THIS TYPE and must never be OR'd together:
//
//   TAG scope    -- meaningful only inside the EventNode whose tag_bit_index
//                   assigned it; module N's bit 2 and module M's bit 2 are
//                   unrelated types. Assigned by ETCS_TAG_DECLARE.
//   MODULE scope -- one bit per loaded module, from GetModuleBit. The only
//                   scope the loader's own reorder buffer can reason in.
//
// So a mask crossing to the loader is re-expressed, not forwarded --
// LoaderStream::OriginScopeMask. Every name here says which scope it is in.
struct TagMask
{

    uint64_t w[TAG_WORDS]{};

    static TagMask bit(size_t index)
    {
        TagMask m;
        if (index < TAG_BITS) m.w[index >> 6] = uint64_t(1) << (index & 63);
        return m;
    }
    // "Collides with everything." For events that change memory topology rather
    // than one type's data, and for any mask that could not be resolved --
    // acquire() only sees a dependency when BOTH sides are non-empty, so an
    // empty mask slips past a live unload as easily as past anything else.
    static TagMask all()
    {
        TagMask m;
        for (size_t i = 0; i < TAG_WORDS; ++i) m.w[i] = ~uint64_t(0);
        return m;
    }
    bool test(size_t index) const
    {
        return index < TAG_BITS && ((w[index >> 6] >> (index & 63)) & 1ull);
    }
    bool any() const
    {
        for (size_t i = 0; i < TAG_WORDS; ++i) if (w[i]) return true;
        return false;
    }
    // Early-out on the first overlapping word -- acquire() calls this once per
    // live slot per event, the one hot path on this type.
    bool intersects(const TagMask& o) const
    {
        for (size_t i = 0; i < TAG_WORDS; ++i) if (w[i] & o.w[i]) return true;
        return false;
    }
    TagMask& operator|=(const TagMask& o)
    {
        for (size_t i = 0; i < TAG_WORDS; ++i) w[i] |= o.w[i];
        return *this;
    }
    friend TagMask operator|(TagMask a, const TagMask& b) { a |= b; return a; }
    explicit operator bool() const { return any(); }
};
static_assert(std::is_trivially_copyable_v<TagMask>,
              "TagMask rides inside GapSlot across DSO boundaries");
} // namespace ETCS
#include "MirrorBuffer.h"
namespace ETCS
{
class Entity;
class Root;      // defined at the bottom of Entity.h; LifetimeOwner needs it
                 // only as a tagged pointer member.
class TagManager;
class EventNode; // registerLoader takes EventNode&, defined in DynamicLoader.h
struct Module;   // ModuleBundle holds a Module*, LifetimeOwner::module()
                 // returns Module&, both ahead of Module's definition below.
struct WorkBundle;
struct ModuleBundle;
#ifdef __GNUC__
#include <cxxabi.h>
inline std::string demangle(const char* name) {
    int status = -1;
    char* demangled = abi::__cxa_demangle(name, NULL, NULL, &status);
    std::string result = (status == 0) ? demangled : name;
    free(demangled);
    return result;
}
#else
inline std::string demangle(const char* name) { return name; }
#endif
template<typename K, typename V>
struct FlatMap {
    struct Pair {
        K first;
        V second;
    };
    Pair* data = nullptr;
    uint32_t size = 0;
    uint32_t capacity = 0;
    static inline MemoryArena* arena = nullptr;
    static void setArena(MemoryArena* arena_ptr) { arena = arena_ptr; }
    size_t count(const K& key) const { 
        return (this->find(key) != this->end()) ? 1 : 0; 
    }
    // Inserts a default value if absent, mimicking std::map.
    V& operator[](const K& key) {
        Pair* it = find(key);
        if (it != end()) return it->second;
    
        if (!arena) {
            std::cerr << "\n[ETCS FATAL ERROR]: FlatMap Arena Violation\n"
                      << "-------------------------------------------\n"
                      << "Map Type: FlatMap<" << demangle(typeid(K).name()) 
                      << ", " << demangle(typeid(V).name()) << ">\n"
                      << "Status:   Attempted insertion but 'arena' pointer is NULL.\n"
                      << "Context:  This usually happens during global static registration.\n"
                      << "Fix:      Call FlatMap::setArena(&arena) before this access.\n"
                      << "-------------------------------------------\n" << std::endl;
            throw std::logic_error("ETCS::FlatMap - Null MemoryArena during insertion.");
        }
        insert(arena, key, V{});
        return find(key)->second; 
    }
    // Read-only path for the validator.
    const V& operator[](const K& key) const {
        const Pair* it = find(key);
        if (it == end()) {
            static const V empty_val{};
            return empty_val;
        }
        return it->second;
    }
    const Pair* find(const K& key) const {
        uint32_t low = 0, high = size;
        while (low < high) {
            uint32_t mid = low + (high - low) / 2;
            if (data[mid].first < key) low = mid + 1;
            else if (key < data[mid].first) high = mid;
            else return &data[mid];
        }
        return end();
    }
    Pair* find(const K& key) {
        return const_cast<Pair*>(static_cast<const FlatMap*>(this)->find(key));
    }
    Pair* begin() { return data; }
    Pair* end()   { return data + size; }
    const Pair* begin() const { return data; }
    const Pair* end()   const { return data + size; }
    void reserve(MemoryArena* arena_ptr, uint32_t new_capacity) {
        if (new_capacity <= capacity) return;
        arena = arena_ptr;
        Pair* new_data = (Pair*)arena->allocateRaw(new_capacity * sizeof(Pair), alignof(Pair));
        if (data && size > 0) {
            std::memcpy((void*)new_data, (const void*)data, size * sizeof(Pair));
        }
        data = new_data;
        capacity = new_capacity;
    }
    void insert(MemoryArena* arena_ptr, K key, V value) {
        arena = arena_ptr;
        if (size >= capacity) {
            reserve(arena, (capacity == 0) ? 8 : capacity * 2);
        }
        uint32_t i = size;
        while (i > 0 && key < data[i-1].first) {
            std::memmove((void*)&data[i], (const void*)&data[i-1], sizeof(Pair));
            i--;
        }
        
        new (&data[i].first) K(key);
        new (&data[i].second) V(value);
        size++;
    }
};
using Manifest = FlatMap<ETCS::Buffer, ETCS::Buffer>; // dependent headers -> hashes

// A HEADER:/ONTOLOGY: entry disagreed -- loader and module built for
// different epochs. Never an ordinary load failure (missing .so, missing
// export): attachModule (DynamicLoader.h) handles this one distinctly.
struct ManifestMismatchException : std::runtime_error
{
    using std::runtime_error::runtime_error;
};

// Logs `theirs` against `ours` row by row and returns true iff a
// HEADER:/ONTOLOGY: (contract) key disagrees -- a plain [DIFF] on any other
// key is informational only. Symmetric by construction: pass your own
// manifest as `ours` regardless of which side you're on. Both the loader
// (Module::validateManifest) and a module's own RegisterDynamicLoader
// handshake (DynamicLoader.h) call this independently against the other
// side's manifest, so a bad build can't slip through by only one side
// checking.
inline bool compareManifests(Manifest& ours, Manifest* theirs, const std::string& label)
{
    bool mismatch_found = false;
    ETCS_LOG("ManifestCheck", "--- Manifest comparison: " << label << " ---");
    ETCS_LOG("ManifestCheck", std::left << std::setw(40) << "Key"
        << std::setw(12) << "Ours(8)" << std::setw(12) << "Theirs(8)" << "Status");
    for (auto const& [key_c, their_hash_c] : *theirs)
    {
        std::string key(key_c);
        std::string their_hash(their_hash_c);
        /*
     * CORE COUNTS AS A CONTRACT, and leaving it out was the hole.
     *
     * ONTOLOGY and HEADER are contracts because they define DISPATCH SHAPES --
     * disagree and a call goes to the wrong place. Core headers were treated as
     * informational on the reasoning that they are implementation, but that is
     * the wrong distinction: core is where the types that CROSS THE DSO BOUNDARY
     * live. RIDListHandle says so against itself -- "a slot added in the middle
     * shifts every later one, and a single translation unit built against the
     * older layout calls the wrong function pointer through a valid-looking
     * self". A layout disagreement is not milder than a contract disagreement,
     * it is the same failure one level down, and it presents worse: a wrong
     * vtable slot usually crashes, a wrong function-pointer slot in a
     * plain struct just quietly does something else.
     *
     * Which is exactly how it was found. Adding a slot to RIDListHandle and
     * rebuilding only the loader left every module calling through the old
     * layout. Nothing crashed. The renderer simply presented zero frames and
     * the window stayed blank -- and the handshake printed CORE:RIDList.h with
     * a visible difference and marked it OK.
     *
     * The cost of this being a hard failure is that a partial rebuild after a
     * core change now refuses to run instead of running wrongly. That is the
     * trade being made deliberately: `ace make modules --force` is a minute,
     * and the alternative is a silent behavioural bug with no symptom pointing
     * anywhere near its cause.
     */
        bool is_contract = (key.rfind("ONTOLOGY:", 0) == 0 || key.rfind("HEADER:", 0) == 0
                         || key.rfind("CORE:", 0) == 0);
        std::stringstream row;
        row << std::left << std::setw(40) << key;
        if (ours.count(key_c))
        {
            std::string o_short = std::string(ours[key_c]).substr(0, 8);
            std::string t_short = their_hash.substr(0, 8);
            row << std::setw(12) << o_short << std::setw(12) << t_short;
            if      (o_short == t_short) row << "[ OK ]";
            else if (is_contract)      { row << "[ FAIL ]"; mismatch_found = true; }
            else                         row << "[ DIFF ]";
        }
        else
            row << std::setw(12) << "N/A" << std::setw(12) << their_hash.substr(0, 8) << "[INFO]";
        ETCS_LOG("ManifestCheck", row.str());
    }
    if (mismatch_found)
        for (auto const& [key_c, their_hash_c] : *theirs)
        {
            std::string key(key_c);
            /*
     * CORE COUNTS AS A CONTRACT, and leaving it out was the hole.
     *
     * ONTOLOGY and HEADER are contracts because they define DISPATCH SHAPES --
     * disagree and a call goes to the wrong place. Core headers were treated as
     * informational on the reasoning that they are implementation, but that is
     * the wrong distinction: core is where the types that CROSS THE DSO BOUNDARY
     * live. RIDListHandle says so against itself -- "a slot added in the middle
     * shifts every later one, and a single translation unit built against the
     * older layout calls the wrong function pointer through a valid-looking
     * self". A layout disagreement is not milder than a contract disagreement,
     * it is the same failure one level down, and it presents worse: a wrong
     * vtable slot usually crashes, a wrong function-pointer slot in a
     * plain struct just quietly does something else.
     *
     * Which is exactly how it was found. Adding a slot to RIDListHandle and
     * rebuilding only the loader left every module calling through the old
     * layout. Nothing crashed. The renderer simply presented zero frames and
     * the window stayed blank -- and the handshake printed CORE:RIDList.h with
     * a visible difference and marked it OK.
     *
     * The cost of this being a hard failure is that a partial rebuild after a
     * core change now refuses to run instead of running wrongly. That is the
     * trade being made deliberately: `ace make modules --force` is a minute,
     * and the alternative is a silent behavioural bug with no symptom pointing
     * anywhere near its cause.
     */
        bool is_contract = (key.rfind("ONTOLOGY:", 0) == 0 || key.rfind("HEADER:", 0) == 0
                         || key.rfind("CORE:", 0) == 0);
            if (is_contract && ours.count(key_c) && ours[key_c] != their_hash_c)
                ETCS_LOG("FATAL", "Interface mismatch (" << label << ") -- Key: " << key
                    << "  Ours: " << ours[key_c] << "  Theirs: " << their_hash_c);
        }
    return mismatch_found;
}
using MakeFunc = ETCS::Entity*(*)(ETCS::Buffer&); // constructs child through referent entity*
using MakeChildFunc = ETCS::Entity*(*)(ETCS::Entity*);
using WorkFunc = void(*)(ETCS::Entity*, ETCS::Buffer&, ETCS::SignalContext);
using StreamFunc = void(*)(ETCS::Entity*, ETCS::MBuffer&, ETCS::SignalContext);
// ---------------------------------------------------------------------------
// Scope — per-entity registry of every in-flight stream call's SignalContext.
//
// ORDERED, not a hash map, and that ordering is load-bearing: entries sit in
// creation order, and a call's POSITION among the live entries sharing its
// label is the only explicit record this structure keeps of the causal
// sequence that produced them. Reading the list top to bottom tells you which
// Listen started first. That is also the addressing scheme the shell uses --
// (label, index), two separate inputs, never one compound key.
//
// Position SHIFTS when an earlier sibling exits, deliberately. The question a
// shell user can act on is "which of the ones running right now", not "which
// slot did a since-dead call occupy", so a stable-with-holes numbering would
// preserve a fact about the dead at the cost of the fact about the living.
// The volatility this introduces is the same one RID lookups already have
// everywhere else here, handled the same way: re-read the list, then act.
//
// Identity is therefore NOT position. Each entry carries a monotonic `id`,
// assigned at registration and never reused, which is what ScopeTag holds and
// unregisters by. This is not redundancy -- ScopeTag is MOVE-constructed into
// the pool lambda in DEFINE_STREAM_FUNC_PRODUCE, so its own address changes
// after it registers, and any identity derived from position would be
// invalidated by an unrelated sibling exiting. The id is internal and never
// displayed; drift in it is meaningless.
//
// The REPL-visible flag is now "active_scope_<label>" with no address at all,
// shared by every live call of that label. Removing it (Entity::removeTag)
// interrupts ALL of them -- the coarse verb. Targeting one goes through
// interruptAt(label, index) instead. Two granularities, two interfaces, rather
// than one interface that could only ever express the fine one.
//
// A plain by-value member on Entity (Entity::scope_), not an addTag'd child:
// it needs none of the ontology machinery (T::TAG, the blocking AddTagEvent
// round-trip, RIDList registration, RID addressability) and exists only so
// teardown can reach every in-flight call's context.
//
// No lock of its own -- protected by Entity's m_tagMutex, same as
// flags_/tags/typed_children_. Every method assumes the caller holds it.
// ---------------------------------------------------------------------------
struct Scope
{
    // Each scope owns its OWN interrupt flag, and the context handed to the
    // developer body points local authority at that flag while inheriting the
    // caller's authority through `up`.
    //
    // Previously Scope stored a copy of the CALLER's context. For a REPL
    // dispatch that comes from WIRE_CONTEXT(), whose .interrupt is null, so
    // the old `if (ctx.interrupt)` guard was false and interruptOne/All
    // silently did nothing. Had it been non-null it would have been
    // &g_sig_int -- interrupting one scope would have raised a process-wide
    // SIGINT.
    struct Entry
    {
        SignalFlag flag{0};
        // Owned snapshot of the caller's context, NOT a pointer to it:
        // DEFINE_STREAM_FUNC_PRODUCE moves its ScopeTag into a pool lambda
        // that runs after the trampoline frame has returned.
        SignalContext parent_snapshot;
        SignalContext ctx;   // ctx.up == &parent_snapshot
        std::string label;   // bare, no prefix and no address -- "Listen"
        uint64_t    id = 0;  // see the struct comment: identity, not position
    };
    // unique_ptr elements, not Entry by value. Two independent requirements
    // force it: ctx.interrupt points at &entry.flag and ctx.up at
    // &entry.parent_snapshot, both handed to a body that may run for a
    // server's lifetime, so vector growth must not move them; and SignalFlag
    // (std::atomic) is neither copyable nor movable, so a by-value vector
    // could not reallocate at all. Indirection buys insertion order and O(1)
    // indexed access that the previous unordered_map could not provide.
    std::vector<std::unique_ptr<Entry>> entries;
    uint64_t next_id_ = 1;
    struct Registration
    {
        SignalContext ctx;
        uint64_t      id             = 0;
        bool          first_of_label = false; // caller should addTag the flag
    };
    struct Removal
    {
        bool        found         = false;
        bool        last_of_label = false;    // caller should removeTag the flag
        std::string label;
    };
    // `label` is the bare action name ("Listen"), never a prefixed or
    // address-bearing string. first_of_label tells ScopeTag whether to raise
    // the shared flag -- computed here, under the caller's lock, rather than
    // by a separate check-then-act on ScopeTag's side that two concurrent
    // registrations could interleave.
    Registration registerContext(const std::string& label, const SignalContext& incoming)
    {
        Registration r;
        r.first_of_label = true;
        for (const auto& e : entries)
            if (e->label == label) { r.first_of_label = false; break; }
        auto entry = std::make_unique<Entry>();
        entry->label           = label;
        entry->id              = next_id_++;
        entry->parent_snapshot = incoming;
        entry->ctx             = incoming;
        entry->ctx.setParent(&entry->parent_snapshot);
        entry->ctx.interrupt   = &entry->flag;
        r.ctx = entry->ctx;
        r.id  = entry->id;
        entries.push_back(std::move(entry));
        return r;
    }
    // By id, never by position -- see the struct comment. found == false is
    // not an error: interruptLabel/interruptAll may have won the race against
    // this call's natural completion, which is fine either way.
    Removal unregisterContext(uint64_t id)
    {
        Removal r;
        for (auto it = entries.begin(); it != entries.end(); ++it)
        {
            if ((*it)->id != id) continue;
            r.found = true;
            r.label = (*it)->label;
            entries.erase(it);   // shifts every later entry down by one --
                                 // the intended renumbering, see struct comment
            break;
        }
        if (!r.found) return r;
        r.last_of_label = true;
        for (const auto& e : entries)
            if (e->label == r.label) { r.last_of_label = false; break; }
        return r;
    }
    // The COARSE verb -- every live call carrying this label. What removing
    // the shared "active_scope_<label>" flag now means. Returns how many were
    // signalled, so tagModifyImpl knows whether this key was a scope at all or
    // should fall through to ordinary flag removal.
    size_t interruptLabel(const std::string& label)
    {
        size_t n = 0;
        for (auto& e : entries)
            if (e->label == label) { e->flag.store(1, std::memory_order_release); ++n; }
        return n;
    }
    // The FINE verb -- one specific call, addressed by its position among the
    // live entries sharing `label`, in creation order. Both verbs only ever
    // REQUEST; removal from this registry happens in ~ScopeTag, when the call
    // actually returns.
    bool interruptAt(const std::string& label, size_t index)
    {
        size_t seen = 0;
        for (auto& e : entries)
        {
            if (e->label != label) continue;
            if (seen++ != index)   continue;
            e->flag.store(1, std::memory_order_release);
            return true;
        }
        return false;
    }
    // Signals every registered scope. Called from drainEntityScopes before
    // destruction begins, and again from ~Entity() as a last resort where
    // there is no longer anyone to wait.
    void interruptAll()
    {
        for (auto& e : entries)
            e->flag.store(1, std::memory_order_release);
    }
    // Read-only enumeration for the shell. index is computed here, on read,
    // exactly as interruptAt resolves it -- so what a user sees and what a
    // subsequent kill targets are produced by the same walk, and can only
    // disagree if something exited in between (which is the accepted
    // volatility, not a discrepancy in the addressing).
    struct View
    {
        std::string label;
        size_t      index       = 0;
        bool        interrupted = false; // already asked to stop, hasn't returned yet
    };
    void collect(std::vector<View>& out) const
    {
        std::unordered_map<std::string, size_t> counters;
        for (const auto& e : entries)
            out.push_back(View{e->label, counters[e->label]++,
                               e->flag.load(std::memory_order_acquire) != 0});
    }
    bool empty() const { return entries.empty(); }
};
// ---------------------------------------------------------------------------
// ActivePairModuleMask / PairScope — MODULE-scope mask of the stream pair whose
// body is running on THIS thread; empty outside one.
//
// Set by the trampoline around the call body rather than by ScopeTag, because
// PRODUCE move-constructs its ScopeTag into a pool lambda -- the constructing
// thread is not the body's thread, and the body's thread is the one that fires
// the loader events this has to reach.
//
// Every module-side *Event::operator()() copies it into
// DLInEvent::origin_extra_mask, which OriginScopeMask ORs into the loader's
// ordering mask. So while a cross-module pair is live, an event stepping up
// from either side is ordered against BOTH modules and a RequestUnload for the
// far side cannot commute past a call still using it. Saved and restored, not
// cleared, so nesting unwinds.
inline ETCS::TagMask& ActivePairModuleMaskRef()
{
    thread_local ETCS::TagMask m{};
    return m;
}
inline const ETCS::TagMask& ActivePairModuleMask() { return ActivePairModuleMaskRef(); }
struct PairScope
{
    ETCS::TagMask saved;
    explicit PairScope(const ETCS::TagMask& m) : saved(ActivePairModuleMaskRef())
    { ActivePairModuleMaskRef() = m; }
    ~PairScope() { ActivePairModuleMaskRef() = saved; }
    PairScope(const PairScope&)            = delete;
    PairScope& operator=(const PairScope&) = delete;
};
// ScopeTag — RAII guard around one stream call's body, auto-injected by
// DEFINE_STREAM_FUNC_PRODUCE/_CONSUME. Construction flips the REPL-visible
// "active_scope_<label>_<addr>" flag AND registers this call's context into
// the owning entity's Scope; destruction reverses both.
//
// Move-constructible, not move-assignable. That is what lets PRODUCE's
// trampoline construct (and register) one on the CALLING thread before
// enqueue, then transfer it into the pool lambda -- closing the window where
// a body is enqueued but not yet registered. The move nulls the source's `e`
// so the husk's destructor is a no-op rather than a double-unregister.
// ---------------------------------------------------------------------------
struct ScopeTag
{
    static constexpr const char* kPrefix = "active_scope_";
    ETCS::Entity* e;
    ETCS::Buffer  tag;
    // The derived context for this call (see Scope::registerContext). Held by
    // value and copied on move: it is only pointers, and the flag it points at
    // lives in the entity's Scope map, never in this guard.
    ETCS::SignalContext scope_ctx;
    // Registry identity, assigned by Scope::registerContext and COPIED on
    // move. This is why identity can't be `this`: PRODUCE's trampoline
    // move-constructs this guard into a pool lambda after it has already
    // registered, so its address changes while the registry entry does not.
    // Nor can it be position -- an unrelated sibling exiting renumbers every
    // later entry, and this guard's destructor would then unregister someone
    // else's call. Never displayed anywhere; the shell addresses scopes by
    // (label, index) instead.
    uint64_t scope_id = 0;
    // TAG-scope bits beyond this entity's own closure. The pair's tag mask for
    // a stream call; empty for every other ScopeTag, hence the default. OR'd
    // with myTagClosure() by Entity::addTag/removeTag's mask overloads, and
    // held so the destructor's removeTag matches the constructor's addTag. Tag
    // scope is correct here: TagModifyEvent never leaves its module.
    ETCS::TagMask extra_mask;
    ScopeTag(ETCS::Entity* entity, const char* label, const ETCS::SignalContext& ctx,
             const ETCS::TagMask& extra = ETCS::TagMask{});
    ~ScopeTag();
    ScopeTag(const ScopeTag&)            = delete;
    ScopeTag& operator=(const ScopeTag&) = delete;
    ScopeTag(ScopeTag&& other) noexcept
        : e(other.e), tag(other.tag), scope_ctx(other.scope_ctx),
          scope_id(other.scope_id), extra_mask(other.extra_mask)
    {
        other.e = nullptr;
    }
    const ETCS::SignalContext& ctx() const { return scope_ctx; }
    static bool anyActive(ETCS::Entity* entity);
};
using HashFunc = HASH_TYPE(*)();
using ModuleFunc = Manifest* (*) (ETCS::BBuffer&);
using ThreadpoolCleanFunc = void(*)();
struct WorkBundle
{
    ETCS::Buffer module_tag = "";
    ETCS::Buffer work_tag = "";
    const void* workFunc = nullptr;  // type-erased; WorkFunc/StreamFunc differ
    const HASH_TYPE hash = 0;
    const bool isStream = false;
    
    WorkBundle(ETCS::Buffer m = "", ETCS::Buffer w = "", const void* f = nullptr, HASH_TYPE h = 0, bool stream = false)
        : module_tag(std::move(m)), work_tag(std::move(w)), workFunc(f), hash(h), isStream(stream) {}
    
    bool operator()(ETCS::Entity* child, ETCS::Buffer& data, ETCS::SignalContext ctx = {});
    bool operator()(ETCS::Entity* child, ETCS::MBuffer& data, ETCS::SignalContext ctx = {});
};
// Defined ahead of Module: Module's inline methods construct ModuleBundle by
// value, so it must be complete there. ModuleBundle needs only Module*.
struct ModuleBundle
{
    ETCS::Buffer tag = "";      // e.g. "SomeLocation" dll provides "SomeCitizen"
    Module* owner = nullptr;
    const HASH_TYPE hash = 0;
    const MakeFunc makeFunc = nullptr;
    const Manifest* actions_hashes = nullptr;
    ETCS::FlatMap<ETCS::Buffer, WorkBundle> actions;
    SignalContext ctx;
    ETCS::Buffer tagbuff;
        
    bool isActionStream(const ETCS::Buffer& action) const
    {
        auto it = actions.find(action);
        return it != actions.end() && it->second.isStream;
    }
    StreamFunc getStream(ETCS::Buffer action)
    {
        if (isActionStream(action)) return reinterpret_cast<StreamFunc>(const_cast<void*>(actions[action].workFunc));
        else return nullptr;
    }
    
    ETCS::Entity* operator()();
    bool operator()(ETCS::Entity* child, const ETCS::Buffer& work, ETCS::Buffer& data, ETCS::SignalContext ctx = {});
    bool operator()(ETCS::Entity* child, const ETCS::Buffer& work, ETCS::MBuffer& data, ETCS::SignalContext ctx = {});
};
// ── LifetimeOwner ─────────────────────────────────────────────────────────────
// A tagged reference to whichever kind of thing is serving as a Module's
// lifetime owner: a genuine Entity, or a bare Root. Also used wherever a caller
// hands either kind to the same Module-attachment machinery -- attachModule's
// `entity`, DLInEvent::bootstrap_root/resolve_target, LoadEvent::root,
// ResolveEvent::target, ExecutionContext::root_entity. All the same role:
// something that IS or MAY BECOME a Module's lifetime owner the moment
// attachModule finds that module vacant.
//
// Not an interface -- no virtual call anywhere, matching the zero-cost-dispatch
// stance. Just the kind-tag idiom DLInEvent::Kind already uses, so Module and
// attachModule don't each assume every owner is an Entity. A bare Root never
// was one; it only appeared to satisfy that assumption while it publicly
// inherited Entity, which is the coupling this removes.
//
// getRID()/module()/asEntity()/asRoot() are DEFINED in Entity.h after Root's
// class body -- the same out-of-line split Module's own methods use.
struct LifetimeOwner
{
    enum class Kind : uint8_t { None, Entity, Root };
    Kind kind = Kind::None;
    union
    {
        ETCS::Entity* as_entity;
        ETCS::Root*   as_root;
    };
    LifetimeOwner()                : as_entity(nullptr) {}
    LifetimeOwner(std::nullptr_t)  : as_entity(nullptr) {}
    // Both check for null explicitly rather than unconditionally setting kind.
    // operator bool() and every `if (survivor)`/`if (entity)` branch rely
    // ENTIRELY on kind, never on the pointer -- so Kind::Entity with a null
    // as_entity would report "present" while holding nothing, and the first
    // .module()/.getRID()/.asEntity() dereferences null. That is exactly what
    // happened when findNextCandidateScope/findRootCandidate genuinely found
    // no survivor (the common case) and returned nullptr into
    // promoteOrVacate(LifetimeOwner).
    LifetimeOwner(ETCS::Entity* e) : kind(e ? Kind::Entity : Kind::None), as_entity(e) {}
    LifetimeOwner(ETCS::Root* r)   : kind(r ? Kind::Root   : Kind::None), as_root(r) {}
    explicit operator bool() const { return kind != Kind::None; }
    // uint64_t, not ETCS::RID -- RID isn't visible this early in the include
    // chain. Entity::getRID() declares uint64_t for the same reason.
    uint64_t getRID() const;
    Module&  module()  const;
    // Throw on kind mismatch rather than returning a garbage reference: every
    // call site has a structural guarantee about which kind it holds
    // (spawn_tag non-empty implies Entity; is_lifetime_owner with a null
    // library_handle implies Root), so a mismatch means that guarantee broke.
    ETCS::Entity& asEntity() const;
    ETCS::Root&   asRoot()   const;
};
// ── Module ────────────────────────────────────────────────────────────────────
// Lives here, not DynamicLoader.h, because Entity needs Module COMPLETE at the
// point class Entity is declared -- Module is Entity's first member now, not a
// separately-allocated pointer.
//
// registerLoader/getTagAddress/validateManifest/~Module are declared here and
// DEFINED in DynamicLoader.h: they need EventNode, SignalContext, and
// Entity::getManifest(), none of which exist yet in this include chain.
struct Module
{
    std::string      name           = "";
private:
    /*
 * PRIVATE so it cannot be closed from outside. Every dlclose/FreeLibrary
 * for a module goes through unmapLibrary() below -- that is the invariant,
 * and this access specifier is what enforces it rather than a convention
 * four call sites each had to remember (three did).
 */
    library_handle_t library_handle = nullptr;
public:
    bool hasLibrary() const { return library_handle != nullptr; }
    // The one way a freshly dlopen'd handle becomes this Module's. Asserts
    // nothing is already mapped: overwriting would leak the old mapping AND
    // strand whatever threads it started.
    void adoptLibrary(library_handle_t h)
    {
        if (library_handle && library_handle != h)
            ETCS_LOG("DynamicLoader:Module",
                "adoptLibrary on '" << name << "' with a handle already mapped -- "
                "the previous mapping is being dropped without unmapLibrary().");
        library_handle = h;
    }
    /*
 * THE teardown path for a mapped module, and the only place a library
 * handle is ever closed. Every step exists because skipping it broke
 * something real, and the order is load-bearing:
 *   1. raise this module's own flags, so work it spawned observes the stop
 *      while its code is still mapped
 *   2. purge ridMap rows keyed "<name>:" -- their RIDLists live in THIS
 *      module's image, so a later bare-RID scan walks unmapped memory.
 *      Before the close, not after: the rows must not stay reachable once
 *      the code behind them is gone
 *   3. _Cleanup -- stops the module's ordering thread and drains its pool.
 *      Skip it and dlclose runs the DSO's static dtors, where ~EventStream
 *      joins a thread nobody told to stop: a hang, not a crash
 *   4. close, and null the handle so a second close is impossible
 * Defined in DynamicLoader.h (needs EventNode complete). `node` may be null
 * only when no EventNode exists to purge from.
 */
    void unmapLibrary(EventNode* node);
    std::string      filename       = "";
    std::vector<ETCS::Buffer> tags;
    bool validBinary = false;
    // Every tag this module exports -> its fully-discovered ModuleBundle.
    //
    // A POINTER, allocated exactly once per module from module_arena and never
    // recreated or copied -- a hand-off copies this pointer, not the map. That
    // is what keeps every live entity's TagEntry::bundle valid across
    // hand-offs: those point INTO this map's entries, and its address never
    // changes while the module is loaded.
    std::unordered_map<std::string, ModuleBundle>* type_catalog = nullptr;
    // This module's per-DSO MemoryArena::getInstance(), resolved once via the
    // Name##_GetArena export and cached here.
    MemoryArena* module_arena = nullptr;
    // The entity or Root whose own module_ member this Module's content
    // currently lives in. Transient in content (changes on every hand-off) but
    // a permanent part of every Module's shape. Read in ~Module()'s
    // Root-relinquish branch.
    LifetimeOwner hosting_entity;
    // Forwarding-proxy parent, always set the moment attachModule touches this
    // token, pointing at the one permanent loader-owned global Module for this
    // name. Every accessor defers to it unconditionally; no per-entity Module
    // holds real state (library_handle, module_arena, type_catalog) anymore.
    // Only the global instance has parent == nullptr.
    Module* parent = nullptr;
    // GLOBAL-INSTANCE-ONLY: which entity's or Root's module_ token is the
    // elected lifetime owner. Kind::None means never claimed, or between
    // owners (the window between the previous owner's ~Module() and either a
    // new spawn claiming it or RequestUnloadEvent finding it still empty).
    LifetimeOwner lifetime_owner;
    // PER-TOKEN-ONLY: true iff THIS token is what lifetime_owner points at on
    // the global instance. Checked in ~Module() to decide whether this
    // token's destruction triggers an election at all -- a token that was only
    // ever a proxy has nothing to do.
    bool is_lifetime_owner = false;
    SignalFlag interrupt{0}; // SIGINT
    SignalFlag terminate{0}; // SIGTERM
    SignalFlag hangup   {0}; // SIGHUP
    SignalFlag pause    {0}; // SIGTSTP
    SignalFlag resume   {0}; // SIGCONT
    SignalFlag user1    {0}; // SIGUSR1
    SignalFlag user2    {0}; // SIGUSR2
    Module(const Module&)             = delete;
    Module& operator=(const Module&)  = delete;
    Module(Module&&)                  = delete;
    Module& operator=(Module&&)       = delete;
    // The only constructor for an Entity-hosted token. Every entity's module_
    // is constructed this way in Entity's own ctor initializer list, giving
    // every entity a valid (if vacant) token from the moment it exists.
    Module(const std::string& modName, Entity& owner)
        : name(modName)
        , library_handle(nullptr)
        , filename(getBinDir() + name + DL_EXTENSION)
        , hosting_entity(&owner)
    {}
    // Root-hosted counterpart. Root no longer derives from Entity, so ordinary
    // overload resolution on *this picks between the two.
    Module(const std::string& modName, Root& owner)
        : name(modName)
        , library_handle(nullptr)
        , filename(getBinDir() + name + DL_EXTENSION)
        , hosting_entity(&owner)
    {}
    // For the ONE permanent global instance per module name (loader-arena
    // allocated). It never lives inside any entity's or Root's module_ member,
    // so hosting_entity stays Kind::None.
    explicit Module(const std::string& modName)
        : name(modName)
        , library_handle(nullptr)
        , filename(getBinDir() + name + DL_EXTENSION)
        , hosting_entity(nullptr)
    {}
    // Defined in DynamicLoader.h. Body is now just dlclose/cleanupModule,
    // reached only by the global instance at process shutdown. Election and
    // vacate logic moved to MemoryArena's run_entity_delete callback, which
    // runs before ~Entity() ever does.
    ~Module();
    // Called by MemoryArena's run_entity_delete for a global-scope entity, and
    // by changeModuleImpl for a Root giving up its module. No-op if this token
    // was never the elected owner. Otherwise: promote survivor, or vacate and
    // fire a non-blocking RequestUnloadEvent. The search itself lives in
    // MemoryArena.h (Entity) or LoaderStream (Root).
    //
    // Body in DynamicLoader.h (needs RequestUnloadEvent).
    void promoteOrVacate(LifetimeOwner survivor);
    // Gives an already-constructed, still-vacant module_ its real identity.
    // Used once, by attachModule's bootstrap case.
    void setIdentity(const std::string& modName)
    {
        name     = modName;
        filename = getBinDir() + name + DL_EXTENSION;
    }
    const std::vector<ETCS::Buffer>& getTags()
    {
        if (parent) return parent->getTags();   // forwarding proxy
        if (tags.size() == 0)
        {
            if (discoverTags(tags) == nullptr)
                ETCS_LOG("DynamicLoader:Module", "Invalid locale...");
        }
        return tags;
    }
    // Use this rather than touching type_catalog directly on any Module that
    // might be a proxy. Anchor-only paths (catalogTypes, loadImpl) touch the
    // member directly since they only run on real anchors.
    std::unordered_map<std::string, ModuleBundle>& catalog()
    {
        return parent ? parent->catalog() : *type_catalog;
    }
    const std::string& getFilename() const
    {
        return filename;
    }
    void* getTagFunction(const std::string& tag)
    {
#ifdef ETCS_LOADER
        if (!library_handle) return nullptr;
#ifdef _WIN32
        return GetProcAddress(library_handle, tag.c_str());
#else
        return dlsym(library_handle, tag.c_str());
#endif
#else
        (void) tag;
        return nullptr;
#endif
    }
    void cleanupModule()
    {
#ifdef ETCS_LOADER
        if (!library_handle || !validBinary) return;
        std::string cleanupSymbol = name + "_Cleanup";
        void* cleanupHandle = getTagFunction(cleanupSymbol);
        reinterpret_cast<ThreadpoolCleanFunc>(cleanupHandle)();
#endif
    }
    // Lazily allocates type_catalog once from module_arena (which must already
    // be set), then fills in any uncatalogued tags. Safe to call repeatedly --
    // existing entries are never rebuilt or moved.
    void catalogTypes()
    {
#ifdef ETCS_LOADER
        if (!type_catalog)
        {
            if (!module_arena)
            {
                ETCS_LOG("DynamicLoader:Module",
                    "catalogTypes: module_arena not set for '" << name
                    << "' -- cannot allocate persistent type_catalog.");
                return;
            }
            type_catalog = module_arena->allocate<std::unordered_map<std::string, ModuleBundle>>();
        }
        for (const auto& tag : getTags())
        {
            std::string tag_str = tag.toString();
            if (type_catalog->find(tag_str) == type_catalog->end())
                type_catalog->emplace(tag_str, getTagAddress(tag_str));
        }
#endif
    }
    Manifest* discoverTags(std::vector<ETCS::Buffer>& vec)
    {
#ifdef ETCS_LOADER
        void* moduleFuncAddr = getTagFunction(name);
        if (!moduleFuncAddr)
            throw std::runtime_error(
                "Failed to find module discovery function '" + name + "' in " + getFilename());
        ModuleFunc moduleFunc = reinterpret_cast<ModuleFunc>(moduleFuncAddr);
        ETCS::BBuffer buff;
        Manifest* hashes = moduleFunc(buff);
        if (!hashes)
            throw std::runtime_error("Discover tags on " + name + " failed to provide a valid Manifest.");
        if (validateManifest(hashes))
            throw ManifestMismatchException("manifest mismatch loading " + name);
        std::stringstream ss(buff.toString());
        std::string tag_string;
        ETCS_LOG("DynamicLoader:Module", "Discovering tags provided by " << name);
        while (ss >> tag_string)
        {
            ETCS::Buffer tag_token(tag_string);
            vec.push_back(tag_token);
            ETCS_LOG("DynamicLoader:Module", " - tag: " << tag_token);
        }
        return hashes;
#endif
        (void)vec;
        return nullptr;
    }
    // Manifest tokens are UNQUALIFIED ("<Action>_Work" / "<Action>_Stream").
    // The exported SYMBOLS are Type-qualified to avoid collisions when two
    // tags in one module share an action name -- the qualification is
    // reconstructed here, where `tag` is already in scope.
    Manifest* discoverActions(std::string tag, ETCS::FlatMap<ETCS::Buffer, WorkBundle>& actions)
    {
#ifdef ETCS_LOADER
        using WorkFuncResolver   = WorkFunc (*)();
        using StreamFuncResolver = StreamFunc (*)();
        std::string workFuncSymbol = tag + "_List";
        void* workAddr = getTagFunction(workFuncSymbol);
        if (!workAddr)
            throw std::runtime_error("Failed to find '" + workFuncSymbol + "' in " + name);
        ModuleFunc moduleFunc = reinterpret_cast<ModuleFunc>(workAddr);
        ETCS::BBuffer buff;
        Manifest* actionsHashes = moduleFunc(buff);
        if (!actionsHashes)
            throw std::runtime_error(
                "Discover actions on " + name + "." + tag + " failed to provide a valid Manifest.");
        std::stringstream ss(buff.toString());
        std::string workToken;
        ETCS_LOG("DynamicLoader:Module", "Got actions: ");
        while (ss >> workToken)
        {
            std::string baseName;
            bool isStream;
            if (workToken.size() > 5 &&
                workToken.compare(workToken.size() - 5, 5, "_Work") == 0)
            {
                isStream = false;
                baseName = workToken.substr(0, workToken.size() - 5);
            }
            else if (workToken.size() > 7 &&
                     workToken.compare(workToken.size() - 7, 7, "_Stream") == 0)
            {
                isStream = true;
                baseName = workToken.substr(0, workToken.size() - 7);
            }
            else
            {
                ETCS_LOG("DynamicLoader:Module",
                    "Malformed action token (expected '<Action>_Work' or '_Stream'): " << workToken);
                continue;
            }
            std::string foundAction      = tag + "_" + baseName + (isStream ? "_Stream" : "_Work");
            std::string foundActionsHash = tag + "_" + baseName + "_GetHash";
            void* foundWorkAddr = getTagFunction(foundAction);
            void* foundHashAddr = getTagFunction(foundActionsHash);
            if (!foundWorkAddr) {
                ETCS_LOG("DynamicLoader:Module",
                    "Could not find work function for token in discovery: " << foundAction);
                continue;
            }
            if (!foundHashAddr) {
                ETCS_LOG("DynamicLoader:Module",
                    "Could not find hash of work function for token in discovery: " << foundActionsHash);
                continue;
            }
            using HashFunc = HASH_TYPE(*)();
            const void* actualWork = isStream
                ? reinterpret_cast<const void*>(reinterpret_cast<StreamFuncResolver>(foundWorkAddr)())
                : reinterpret_cast<const void*>(reinterpret_cast<WorkFuncResolver>(foundWorkAddr)());
            HASH_TYPE actualWorkHash = reinterpret_cast<HashFunc>(foundHashAddr)();
            actions.insert(&ETCS::MemoryArena::getInstance(), ETCS::Buffer(baseName),
                {ETCS::Buffer(tag), ETCS::Buffer(baseName), actualWork, actualWorkHash, isStream});
            ETCS_LOG("DynamicLoader:Module", baseName << " [Stream:" << isStream << "]"
                << " WorkAddr: " << foundWorkAddr
                << " Hash: " << std::hex << std::showbase << actualWorkHash << std::dec);
        }
        std::cout << " --- " << "\n";
        return actionsHashes;
#endif
        (void) tag; (void) actions;
        return nullptr;
    }
    // Defined in DynamicLoader.h -- needs Entity::getManifest().
    bool validateManifest(Manifest* dllManifest);
    // Defined in DynamicLoader.h -- needs EventNode complete and
    // RootSignalContext().
    bool registerLoader(EventNode& st);
    // Defined in DynamicLoader.h -- same reasons as registerLoader.
    ETCS::ModuleBundle getTagAddress(const std::string& tag);
private:
    static const std::string& getBinDir() {
        static const std::string dir = []() -> std::string {
#if defined(_WIN32) || defined(_WIN64)
            char path[MAX_PATH];
            DWORD len = GetModuleFileNameA(NULL, path, MAX_PATH);
            if (len == 0) return ".\\";
            char* lastSep = strrchr(path, '\\');
            if (!lastSep) lastSep = strrchr(path, '/');
            if (lastSep) *(lastSep + 1) = '\0';
            else return ".\\";
            return std::string(path);
#else
            char path[PATH_MAX];
            ssize_t len = readlink("/proc/self/exe", path, sizeof(path) - 1);
            if (len == -1) return "./";
            path[len] = '\0';
            char* lastSlash = strrchr(path, '/');
            if (lastSlash) *(lastSlash + 1) = '\0';
            else return "./";
            return std::string(path);
#endif
        }();
        return dir;
    }
};
}
#endif