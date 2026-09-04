#ifndef EVENTNODE_H__
#define EVENTNODE_H__
#include <unordered_map>
#include <string>
#include <sstream>
#include <cstdlib>
#include "Buffer.h"
#include "Log.h"
#include "RIDList.h"
#include "EventStream.h"
#include "ArenaAllocator.h"
#include "Bundles.h"
namespace ETCS {
// Forward declarations
struct Module;
class  EventNode;
class  Entity;
class  Root;   // defined at the bottom of Entity.h, after this header --
                // see root_registry's own comment below for why LoaderStream
                // needs Root* specifically (not LifetimeOwner) for that one
                // registry despite LifetimeOwner being used elsewhere here.
// ── DLInEvent ─────────────────────────────────────────────────────────────────
// Input event for LoaderStream and ModuleProxy. Carries kind, conjugate_key,
// and result slot pointers from caller to the single-threaded consumer.
// Caller owns the result atomics on the stack — safe because operator()
// spins until the consumer writes them.
struct DLInEvent
{
    enum class Kind : uint8_t { Load, Resolve, Destroy, AddTag, EntityUnload, TagModify,
                                 RequestUnload, Ack, ChangeModule, PairMask };
    Kind                        kind;
    ETCS::Buffer                conjugate_key;   // Load/Resolve: module:tag key.
                                                   // AddTag: the child's tag name (reused —
                                                   // same field, same convention Destroy uses
                                                   // for `rid` below).
    ETCS::RID                   rid         = 0;       // Destroy only — instance to remove
    bool                        destroy_children = false; // Destroy only — see DestroyEvent's own comment
    std::atomic<ETCS::Entity*>* entity_out  = nullptr; // Load
    // Load only — non-null means "this entity is ALREADY constructed" (via
    // spawn<T>/spawn<T>(arena), on the calling thread,
    // before this event ever fired — same principle addTagTrampoline<T>
    // already uses: construct first, since a bare function pointer can't
    // capture arbitrary constructor arguments the way a lambda could).
    // When set, the handler calls attachModule(module_name, *this,
    // spawn_tag) instead of resolving/calling Make() — conjugate_key still
    // carries "module:tag" in the usual form, parsed the same way.
    ETCS::Entity*               prebuilt_entity = nullptr;
    // Load only, vacant-registry case — the entity or Root to bootstrap the
    // module against when NOTHING exists yet at all (see LoadEvent::root
    // for the full rationale). Unused whenever prebuilt_entity is set.
    // LifetimeOwner, not a bare Entity* -- the bootstrap anchor supplied
    // here is, in current practice, always a Root (see CommandExecutor.h's
    // spawn_entity), but addTagImpl's own vacant branch bootstraps against
    // a genuine Entity's getRootAncestor() instead, so this field
    // genuinely receives both kinds depending on the call site.
    ETCS::LifetimeOwner         bootstrap_root;
    // TagModify only — addTag(Buffer flag)/removeTag(Buffer tag), ordered
    // LOCALLY within the emitting entity's own module (see ModuleProxy::
    // on_event's kind-based fork — this is the one kind that never
    // forwards to the loader). conjugate_key carries the flag/tag string
    // being added or removed. tagmodify_impl is a type-erased callback —
    // ModuleProxy has no business knowing Entity's own private members,
    // it just calls whatever Entity::addTag/removeTag captured here,
    // mirroring the same reasoning addTagTrampoline<T> already uses for
    // crossing that same boundary.
    ETCS::Entity*      tagmodify_target = nullptr;
    bool               tagmodify_is_remove = false;
    // Returns whether the surface actually moved -- see Entity::tagModifyImpl.
    // The answer rides home in release_value and is published by on_emit, the
    // same read-then-publish split AddTag uses for rid_out.
    bool              (*tagmodify_impl)(ETCS::Entity*, const ETCS::Buffer&, bool) = nullptr;
    std::atomic<bool>* tagmodify_done = nullptr;
    std::atomic<bool>* tagmodify_changed = nullptr;
    // TagModify only — the emitting entity's own TYPE bit, stamped at the
    // call site from that type's static TAG_MASK (WIRE_TYPE_IDENTITY,
    // ETCS_API.h) rather than looked up by string in whichever EventNode
    // ends up servicing this event.
    //
    // That distinction is load-bearing, not an optimisation.
    // RegisterTagBitIndex is only ever called from
    // ETCS_MODULE_EXPORT_MAIN, which no loader build expands -- so the
    // LOADER's own tag_bit_index is permanently empty, and
    // LoaderStream::on_event's old owner->GetTagBit(...) returned an
    // empty mask for every TagModifyEvent that originated in
    // loader-compiled code. Those serialized against nothing at all.
    // Carrying the bit with the event makes the answer independent of
    // which side handles it, and drops a std::string construction plus a
    // hash lookup off the ordering thread on the way.
    ETCS::TagMask      tagmodify_mask{};
    // PairMask only -- see PairMaskEvent below. conjugate_key carries the
    // first contract tag, this the second; the handler resolves each through
    // type_owner_index and ORs GetModuleBit. Read-only: it mutates nothing but
    // module_bit_index's own first-use assignment, so unlike every other kind
    // here it needs no ack.
    ETCS::Buffer       pairmask_tag_b;
    ETCS::TagMask*     pairmask_out  = nullptr;
    std::atomic<bool>* pairmask_done = nullptr;
    // MODULE-SCOPE bits the ORIGIN wants added, on top of the origin module's
    // own bit. Today that is exactly one thing: the module pair of a stream
    // call whose body this event was fired from inside
    // (ETCS::ActivePairModuleMask, Bundles.h), which is how a cross-module
    // stream call makes both its modules visible to the loader's ordering for
    // the duration.
    //
    // Module scope, not tag scope, and that is the whole point: tag bits are
    // meaningless outside the EventNode that assigned them, so a mask crossing
    // to the loader has to already be expressed in module bits. Stamped by the
    // module-side operator()(), consumed by LoaderStream::OriginScopeMask.
    ETCS::TagMask      origin_extra_mask{};
    // What the completion store publishes, held from the handler until on_emit.
    // Needed only where the completion signal IS the result -- Load (Entity*),
    // Resolve and Destroy (int8_t); every other kind signals with a bare bool
    // and writes its outputs to their own slots.
    //
    // It exists because releasing the caller moved into on_emit (emitting a
    // slot IS the commit, and the commit is what the buffer orders). The
    // handler still computes everything; it no longer decides when the caller
    // may see it.
    uint64_t           release_value = 0;
    // Resolve: entity or Root whose module_ slot gets filled. LifetimeOwner
    // -- ShellREPL's own nav_root (a Root) and CommandExecutor's ordinary
    // module-context resolution (a real Entity in the general case) both
    // route through the identical ResolveEvent/DLInEvent path.
    ETCS::LifetimeOwner         resolve_target;
    std::atomic<int8_t>*        resolve_ok  = nullptr; // Resolve: -1 pending/0 fail/1 ok
    std::atomic<int8_t>*        tri_out     = nullptr; // Destroy: -1 pending, 0 false, 1 true
    // AddTag only — see AddTagEvent below and Entity::addTag<T>() /
    // Entity::addTagTrampoline<T>() in Entity.h. `addtag_trampoline` is
    // captured at the addTag<T> call site, where T is still known, and
    // invoked here on the loader's ordering thread, where T has been
    // erased from this event's own (non-templated) type.
    ETCS::Entity*     addtag_parent      = nullptr;
    ETCS::Entity*     addtag_child       = nullptr;
    RID (*addtag_trampoline)(ETCS::Entity*, ETCS::Entity*, const ETCS::Buffer&) = nullptr;
    std::atomic<RID>*  rid_out           = nullptr; // AddTag result (the assigned RID)
    std::atomic<bool>* ready_out         = nullptr; // AddTag completion flag — RID 0 isn't
                                                       // structurally impossible the way a
                                                       // null Entity* is, so unlike LoadEvent's
                                                       // sentinel trick this needs an explicit
                                                       // flag rather than a reserved result
                                                       // value. Same caution DestroyEvent's
                                                       // int8_t result already applies.
    // EntityUnload only — see EntityUnloadEvent below and Entity::operator
    // delete. Carries the entity operator delete (root case) or an
    // explicit caller (child-target case) wants the ordering thread to
    // process. No boolean/flag needed — the handler branches entirely on
    // target->getParent() (root vs. explicit child target) and
    // target->isDestructed() (already-run vs. never-touched), both of
    // which are properties of the entity itself, not the caller's intent.
    // Always a genuine Entity* -- Root is never arena-resident and never
    // reaches this path at all (see EntityUnloadEvent's own comment).
    ETCS::Entity*      unload_target = nullptr;
    bool               unload_delete_children = false; // see EntityUnloadEvent's own comment
    std::atomic<bool>* unload_done   = nullptr;
    // reply_to — set ONLY by a module-side caller's own operator()()
    // (LoadEvent/ResolveEvent/DestroyEvent/AddTagEvent/EntityUnloadEvent),
    // pointing at &EventNode::getInstance() as seen from THAT module's own
    // compiled code. nullptr for a loader-originated call (the loader
    // never needs to sync back with itself). After the loader finishes
    // its own work for one of these five kinds, if reply_to is non-null
    // it enqueues a lightweight Ack event onto reply_to->stream and
    // blocks on it — a happen-before edge guaranteeing the calling
    // module's own ordering thread has fully processed a sync point
    // before the original blocking call returns to its caller. Safe to
    // cross the dlopen boundary as a raw EventNode* for the same reason
    // getLoader() already is: only ever used to call enqueue(), a method
    // on the shared EventStream<> base whose layout doesn't depend on
    // which Derived (LoaderStream vs ModuleProxy) actually instantiated
    // it. If reply_to->stream is already cleaning up (is_cleaning_up_),
    // enqueue() fails fast and the loader just proceeds without waiting
    // for an ack that would never come.
    ETCS::EventNode*   reply_to = nullptr;
    // RequestUnload only — the ONE, PERMANENT, loader-owned global Module
    // instance to re-check after RequestUnloadEvent's own 200ms delay.
    // Raw pointer is safe: this instance never moves and is never freed
    // until the loader itself exits, regardless of how many times its own
    // lifetime_owner gets reassigned in the meantime.
    ETCS::Module*      request_unload_target = nullptr;
    // false = initial fire (RequestUnloadEvent's own operator()()) --
    // on_event spawns a detached std::thread that sleeps 200ms then
    // re-enqueues this same kind with this set true. true = the delayed
    // recheck itself -- on_event does the actual re-verify-and-unload
    // work synchronously, no further threading involved.
    bool                request_unload_recheck = false;
    // RequestUnload, recheck fire only — lets the spawned 200ms-delay
    // thread (see LoaderStream::on_event's own Kind::RequestUnload case,
    // DynamicLoader.h) know once THIS specific recheck has actually,
    // fully finished (requestUnloadImpl returned -- cleanupModule/
    // dlclose included), so that thread can be genuinely joined rather
    // than fired detached and never waited on by anything at all. Null
    // for the first fire (request_unload_recheck == false) -- that one
    // never needs to be waited on itself, since its own only job is to
    // spawn the delayed recheck.
    std::atomic<bool>* request_unload_done = nullptr;
    // (ack_done removed.) The Ack carries no result slot at all now: nothing
    // waits on it. sendAckIfNeeded enqueues a heap-allocated Ack and returns
    // immediately, and ModuleProxy::on_event deletes it -- see sendAckIfNeeded's
    // own comment (DynamicLoader.h) for the ordering-inversion deadlock the old
    // blocking round-trip caused.
    // ChangeModule only — Root::changeModule()'s own event, and
    // ~Module()'s own relinquish-only use at natural Root destruction
    // (see EventNode::LoaderStream::changeModuleImpl's own comment,
    // DynamicLoader.h). conjugate_key carries the TARGET module name
    // directly -- same convention Resolve already uses (its own
    // conjugate_key IS the module name, not a "module:tag" pair), since
    // Root is never dispatched through any tag, so there's no such pair
    // to parse here. Empty conjugate_key means relinquish-only: give up
    // whatever module this Root currently holds, without reattaching
    // anywhere.
    //
    // Root*, not LifetimeOwner -- this event is exclusively about a
    // Root's own module (Root::changeModule() is a Root-only method, and
    // its ~Module() use is Root-only by the branch's own documented
    // invariant), so there is no second kind this field ever needs to
    // represent.
    ETCS::Root*        changemodule_root = nullptr;
    std::atomic<bool>* changemodule_done = nullptr;
};
struct DLInEventPtr
{
    DLInEvent* ptr; // 8 bytes — fits in any slot size
};
// ── DLState ───────────────────────────────────────────────────────────────────
// Empty state type for the EventStream template. Using EventNode directly as
// State would create a circular type definition (EventNode incomplete when
// LoaderStream is instantiated inside it). Instead, streams store an owner
// pointer set in the EventNode constructor and use it to reach ridMap.
struct DLState {};
// ── Event base and scope-gated event types ────────────────────────────────────
struct Event
{
    ETCS::Buffer conjugate_key;
    explicit Event(const char* key)         : conjugate_key(key) {}
    explicit Event(const ETCS::Buffer& key) : conjugate_key(key) {}
};
// Always compiled — cross-boundary structural events (loader subset).
// operator() bodies defined in DynamicLoader.h after getLoader() is visible.
struct LoadEvent : Event
{
    std::atomic<ETCS::Entity*> result{nullptr};
    // Non-null means "already constructed — just attach a Module
    // reference" (see prebuilt_entity on DLInEvent for the full
    // rationale). nullptr (the default) is the ORIGINAL runtime-string
    // path, unchanged: resolve conjugate_key's tag via Make().
    ETCS::Entity* prebuilt = nullptr;
    // Set by the runtime-string spawn(module,tag,root) — the entity or
    // Root to bootstrap the module against IF the registry turns out
    // vacant and no entity exists yet at all (loadImpl's chicken-and-egg
    // case: the module has to be dlopen'd before Make() can even be
    // resolved). Unused when prebuilt is set — spawn<T>/spawnInPlace<T>/
    // spawn<T>(arena) always construct a genuine root-level entity
    // (parent_ == nullptr by construction), so attachModule uses
    // prebuilt directly with nothing further to bootstrap against.
    // LifetimeOwner, not a bare Entity* -- see DLInEvent::bootstrap_root's
    // own comment for why this needs to hold either kind.
    ETCS::LifetimeOwner root;
    using Event::Event;
    ETCS::Entity* operator()();
};
// ResolveEvent — attaches a module to the given entity's or Root's own
// .module_ slot: proxy onto the live anchor if one exists, or attachModule's
// bootstrap path (published to module_registry immediately) if vacant.
// `target` is the entity/Root whose module_ gets populated; `ok` reports
// success. No caller-owned Module involved at all — every entity/Root
// already has its own module_ member to work on.
struct ResolveEvent : Event
{
    ETCS::LifetimeOwner target;        // entity or Root whose OWN module_ slot gets populated
    std::atomic<int8_t> ok{-1};        // -1 pending, 0 fail, 1 success
    ResolveEvent(const char* key, ETCS::LifetimeOwner out) : Event(key), target(out) {}
    ResolveEvent(const ETCS::Buffer& key, ETCS::LifetimeOwner out) : Event(key), target(out) {}
    bool operator()();
};
// DestroyEvent — entity-granularity, distinct from module-granularity
// concerns entirely. conjugate_key is the same "module:tag" form used to key
// ridMap entries (the format registerLoader absorbs module ridMaps under);
// rid identifies which live instance of that type to remove.
//
// destroyImpl (DynamicLoader.h) resolves this entity's correct parent
// arena — child: getParent()->getArena(); root: module_.parent->module_arena,
// the same split entityUnloadImpl uses — and delegates to
// MemoryArena::deleteEntity, which DOES run the entity's own destructor
// and reclaim its arena footprint immediately; it is not merely a RID-
// list removal. This is the RID-targeted counterpart to EntityUnloadEvent's
// pointer-targeted child case: same underlying deleteEntity call, just
// reached by RID lookup through the RIDListHandle already held in ridMap
// instead of a caller-supplied Entity*.
//
// delete_children (default false) forwards straight to deleteEntity's
// own same-named parameter -- true cascades the target's whole subtree
// instead of reparenting its children. See registerDtor<T>'s own
// comment (MemoryArena.h); meaningless for a global-scope target.
//
// result is int8_t rather than bool specifically so the caller can
// distinguish "not written yet" (-1) from a legitimately false answer (0) —
// same rationale ResolveEvent's own -1/0/1 sentinel above already applies.
struct DestroyEvent : Event
{
    ETCS::RID            rid;
    bool                 delete_children;
    std::atomic<int8_t>  result{-1}; // -1 = pending, 0 = false, 1 = true
    // Optional. Non-null enables the caller-thread scope drain in
    // operator()(); null keeps the old behaviour byte-for-byte. Every real
    // call site is a self-destroy that already holds the pointer -- but
    // resolving RID->Entity* means touching ridMap, which is ordering-thread
    // property, so the event cannot recover it on its own.
    ETCS::Entity* target = nullptr;
    DestroyEvent(const char* key, ETCS::Entity* t, bool children = true);
    DestroyEvent(const ETCS::Buffer& key, ETCS::Entity* t, bool children = true);
    bool operator()();
};
// AddTagEvent — routes Entity::addTag<T>() through the loader's single
// ordering thread instead of executing on whatever thread happens to call
// addTag<T>(). This gives typed-child creation the same global concurrent
// creation order every other entity-creation path (LoadEvent, CreateEvent)
// already has — addTag<T> creates and registers a NEW typed child, so it
// needs that same ordering guarantee, not merely thread-safe bookkeeping
// for the registries it touches.
//
// Always compiled (not gated behind ETCS_LOADER or #ifndef ETCS_LOADER) —
// module-side code is the primary caller (an entity addTag<T>-ing a typed
// child of itself), and loader-side code can use it too if it ever needs
// to. Same visibility as LoadEvent/ResolveEvent/DestroyEvent
// above, for the same reason. Entity-only -- Root never has typed
// children, so this never receives a Root.
struct AddTagEvent : Event
{
    using Trampoline = RID(*)(ETCS::Entity* parent, ETCS::Entity* child, const ETCS::Buffer& tag);
    ETCS::Entity* parent;
    ETCS::Entity* child;
    Trampoline    trampoline;
    std::atomic<RID>  result{0};
    std::atomic<bool> ready {false};
    AddTagEvent(ETCS::Entity* p, ETCS::Entity* c, const ETCS::Buffer& tag_key, Trampoline tramp)
        : Event(tag_key), parent(p), child(c), trampoline(tramp) {}
    RID operator()();
};
// TagModifyEvent — addTag(Buffer flag)/removeTag(Buffer tag), ordered
// LOCALLY within the emitting entity's own module rather than forwarded
// to the loader (see ModuleProxy::on_event's kind-based fork — the ONE
// kind that stays local). impl is a type-erased callback capturing the
// actual mutation logic (Entity::tagModifyImpl) — this event carries no
// knowledge of Entity's own private members, exactly the same reasoning
// addTagTrampoline<T> already uses for crossing that same boundary.
// Entity-only -- Root never has tags/flags_ either.
//
// The ordering mask is ACQUIRED by operator()(), not passed in: the
// emitting type's own bit from the target, narrowed to the running work
// function's causal edge when one has settled (ETCS::CausalEdgeMask,
// Bundles.h). Same-type operations serialize against each other via the
// reorder buffer's existing collision logic; different-type operations
// commit independently.
//
// Acquired at the emit site rather than resolved by the HANDLER, and that
// distinction is a fix rather than a refactor: the handler used to call
// GetTagBit on whichever EventNode serviced the event, and the LOADER's own
// tag_bit_index is never populated by anything, so loader-originated tag
// modifications silently got an empty mask. See DLInEvent::tagmodify_mask.
//
// extra_mask is the only mask a CALLER supplies — TAG-scope bits beyond the
// entity's own, which ScopeTag uses to order a flag against both halves of a
// stream pair. Empty for the ordinary addTag/removeTag.
//
// For removeTag on a tag pointing to an entity relation, the actual child
// deletion still goes through the EXISTING EntityUnloadEvent child-target
// path (which DOES bubble up to the loader, since it touches shared arena
// state) — only the DECISION to remove the tag stays local here.
struct TagModifyEvent : Event
{
    using Impl = bool(*)(ETCS::Entity*, const ETCS::Buffer&, bool);
    ETCS::Entity*     target;
    bool              is_remove;
    Impl              impl;
    ETCS::TagMask     extra_mask;
    std::atomic<bool> done{false};
    std::atomic<bool> changed{false};
    // The fail-shut substitution an empty mask needs — a type with no tag
    // block, so nothing ever assigned its TAG_MASK — is made in operator()(),
    // where the mask is now acquired. Kept out of TAG_MASK's own default,
    // which must stay empty-until-assigned for ETCS_TAG_DECLARE's alias guard.
    TagModifyEvent(ETCS::Entity* t, const ETCS::Buffer& key, bool remove, Impl fn,
                    const ETCS::TagMask& extra = ETCS::TagMask{})
        : Event(key), target(t), is_remove(remove), impl(fn), extra_mask(extra) {}
    /*
 * Returns whether THIS call moved the surface.
 *
 * That makes a tag operation a claim as well as a record, which is what a
 * lifecycle reached from several threads at once needs and what the tag
 * surface was always the right place to put: `removeTag("active")` both
 * states that the window is no longer active AND tells exactly one of the
 * three callers racing to close it that the close is theirs to finish. No
 * second mechanism, and the ordering is the one every other state change in
 * the module already takes.
 */
    bool operator()();
};
// PairMaskEvent — resolve two contract tags into a MODULE-scope mask.
//
// The one scope conversion that cannot be done where it is needed. A stream
// pair connects two tags; the module setting the pair up knows the tags but
// not which modules own them (type_owner_index is loader state), and knows its
// own tag bits but those mean nothing outside it. So the question goes to the
// loader, which answers in module bits.
//
// Blocking, like every other event here, but off the hot path: callers memoize
// per tag pair (Entity::resolvePairModuleMask), so this fires once per distinct
// pair of tags for the life of the process. Nothing invalidates that cache --
// requestUnloadImpl never erases from module_bit_index, so a module keeps its
// bit across unload and reload.
//
// Read-only on the loader side, so no ack: acks exist to order a module's own
// stream after a memory-altering event, and this alters no memory.
struct PairMaskEvent : Event
{
    ETCS::Buffer      tag_b;
    ETCS::TagMask     result{};
    std::atomic<bool> done{false};
    PairMaskEvent(const ETCS::Buffer& tag_a, const ETCS::Buffer& b)
        : Event(tag_a), tag_b(b) {}
    void operator()();
};
// EntityUnloadEvent — the sole entry point into module-scope election and
// arena reclamation, fired from exactly two places:
//
//   1. Entity::operator delete, UNCONDITIONALLY for roots (parent_ ==
//      nullptr) only, never for children. This is always "the first
//      triggering entity dtor" — the destructor chain has already fully
//      completed by the time operator delete runs (ordinary C++ delete
//      semantics), so the handler never calls target's destructor again,
//      only handles election + arena reclamation.
//
//   2. An explicit external caller targeting a CHILD entity directly
//      (never a root — roots are only ever reached via path 1, by
//      established convention; targeting a root explicitly is rejected
//      as a combination that has no safe meaning). Here the destructor
//      may or may not have already run (see Entity::isDestructed()) —
//      the handler calls it now if not, or just unlinks if so, then
//      always reclaims the child's own local_arena_ from its parent's
//      arena regardless.
//
// No boolean/flag parameter — everything the handler needs to branch
// correctly (root vs. child, already-destructed vs. not) is already a
// property of `target` itself, readable via friend access. Blocking
// (LMAX round-trip, ~hundreds of ns) — the caller needs the outcome
// settled before returning. Always a genuine Entity* -- see
// unload_target's own comment on DLInEvent.
struct EntityUnloadEvent : Event
{
    ETCS::Entity*     target;
    bool              delete_children;
    std::atomic<bool> done{false};
    explicit EntityUnloadEvent(ETCS::Entity* t, bool del_children = true)
        : Event(""), target(t), delete_children(del_children) {}
    void operator()();
};
// ChangeModuleEvent — Root::changeModule()'s own event. Always compiled,
// same as LoadEvent/ResolveEvent/DestroyEvent/AddTagEvent/
// EntityUnloadEvent above — a Root can be constructed inside a module's
// own compiled work function just as easily as loader-side code (see
// run_socket_repl's own local Root, CommandExecutor.h), so this needs to
// be callable from either side.
//
// conjugate_key carries the target module name directly -- same
// convention ResolveEvent already uses. Empty means relinquish-only —
// root gives up whatever it currently holds, with no reattach — which
// is exactly ~Module()'s own use at natural Root destruction (see
// EventNode::LoaderStream::changeModuleImpl's own comment, DynamicLoader.h).
struct ChangeModuleEvent : Event
{
    ETCS::Root*       root;
    std::atomic<bool> done{false};
    ChangeModuleEvent(const std::string& target_module, ETCS::Root* r)
        : Event(target_module.c_str()), root(r) {}
    void operator()();
};
// RequestUnloadEvent — the non-blocking counterpart to the old, blocking
// unload path. Fired from a per-entity Module token's own ~Module() the
// moment it discovers it was the elected lifetime_owner AND no survivor
// exists in its own module's arena to hand off to immediately (see
// Module::~Module()'s own comment). Deliberately NOT waited on — enqueue
// and return, full stop. This is what makes it safe to call from
// anywhere, including synchronously inside a memory arena's own teardown
// walk (exactly where the old, blocking EntityUnloadEvent path used to
// deadlock): there is no wait here for anything else to still be alive.
// The loader processes this asynchronously (via ThreadPool, not by
// blocking its own ordering thread for the delay) — see on_event's own
// Kind::RequestUnload case for the 200ms-then-recheck logic.
struct RequestUnloadEvent : Event
{
    ETCS::Module* target;
    explicit RequestUnloadEvent(const std::string& module_name, ETCS::Module* mod)
        : Event(module_name.c_str()), target(mod) {}
    void operator()();
};
// AckEvent — the lightweight sync-back the loader fires onto an
// originating module's own stream after finishing one of Load/Resolve/
// Destroy/AddTag/EntityUnload for it (see DLInEvent::reply_to's own
// comment). Does nothing but log that the operation completed — its
// entire purpose is the ORDERING guarantee of being enqueued and
// processed on the module's own ordering thread, not any actual work.
// Fired FROM the loader's own on_event (never constructed/called
// directly by ordinary code the way other Event types are), so it has no
// public operator() of its own — ModuleProxy::on_event handles
// Kind::Ack directly.
struct AckEvent : Event
{
    explicit AckEvent(const std::string& module_name) : Event(module_name.c_str()) {}
};
// drainEntityScopes -- signal every in-flight scope registered against
// target, then wait for them to actually return. Declared here, DEFINED in
// DynamicLoader.h (needs Entity complete).
//
// CALLER-THREAD ONLY, and that restriction is the whole design. ~ScopeTag
// fires a TagModifyEvent, which per ModuleProxy::on_event's own kind fork is
// the ONE kind that stays LOCAL to the emitting module's ordering thread. A
// drain running inside on_event / destroyImpl / entityUnloadImpl would be
// waiting on the very thread it is occupying. So it runs in
// DestroyEvent::operator()(), before the enqueue.
//
// Returns false if the drain timed out. A false return means DO NOT DESTROY:
// proceeding anyway is the use-after-free this exists to prevent, and a
// visible leak is the better failure.
bool drainEntityScopes(ETCS::Entity* target, const char* who);
#ifndef ETCS_LOADER
// Module-scope superset — entity-level events, proxied upward through
// ModuleProxy into the loader's LoaderStream.
struct CreateEvent : Event
{
    std::atomic<ETCS::Entity*> result{nullptr};
    using Event::Event;
    ETCS::Entity* operator()();
};
// Further type-level events follow the same pattern.
#endif
// ── EventNode ─────────────────────────────────────────────────────────────────
// The scope boundary. Its internal stream serializes all mutations to ridMap
// and, in loader scope, active_bundles and active_modules.
class EventNode
{
public:
    template<typename K, typename V>
    using ArenaMap = std::unordered_map<
        K, V,
        std::hash<K>,
        std::equal_to<K>,
        ArenaAllocator<std::pair<const K, V>>
    >;
    // Deliberately NOT an ArenaMap, unlike the name might suggest at a
    // glance — plain std::unordered_map, default (heap) allocator. There's
    // exactly one of these per module, constructed and destructed as a
    // single static-lifetime unit alongside EventNode itself; arena-
    // backing it bought nothing but a real hazard: EventNode::~EventNode()
    // (= default) destructs this at dlclose()-time, well after
    // Name##_Cleanup() has already explicitly torn the arena down --
    // meaning an ArenaMap's own bucket array would be living inside
    // already-munmap'd pages by the time this container tries to walk it.
    // Managing that ordering explicitly (clearing it before teardown) was
    // the first fix tried; removing the dependency entirely is the better
    // one, since heap memory's lifetime doesn't depend on MemoryArena's
    // teardown schedule at all. Written only from the EventStream consumer
    // thread — no external lock needed.
    /*
 * WHERE THIS DSO'S LOG LINES GO, reachable from outside this DSO.
 *
 * A trampoline rather than a method, for the reason every other trampoline
 * here exists: ETCS::log_to_file is an inline variable, so the loader and each
 * module have their own. A method defined in this header and called through a
 * module's EventNode* would inline the LOADER's copy and set the LOADER's
 * flag. Taking the address where the object is CONSTRUCTED captures whichever
 * DSO actually built this instance, which is the one whose flag is meant.
 *
 * ABOVE `stream`, and that placement is load-bearing rather than tidy. This
 * class has an #ifdef ETCS_LOADER fork in the middle of it -- LoaderStream on
 * one side, ModuleProxy on the other -- so every member declared after that
 * point sits at a DIFFERENT OFFSET in a loader build than in a module build.
 * The loader reads this pointer out of a MODULE's EventNode, so it has to live
 * in the part of the layout both builds agree on: the prefix, with ridMap and
 * scope, which is exactly why those two were already the only things the
 * loader touched. Declared below the fork it was read as garbage and called --
 * a jump to 0x400000000, found the first time this ran.
 */
    void (*set_log_to_file)(bool) = nullptr;
    bool (*get_log_to_file)()     = nullptr;

    std::unordered_map<ETCS::Buffer, RIDListHandle> ridMap;
    // -----------------------------------------------------------------------
    // Tag bit index — maps each of THIS module's own contract tags (the
    // exact same, ordered, space-separated Tags string ETCS_MODULE_EXPORT_MAIN
    // already declares — see ETCS_API.h) to a bit position, 0..TAG_BITS-1.
    // One source of truth: the macro-generated static-init populates this
    // from that SAME string, so there's no hand-maintained duplicate to
    // drift out of sync.
    //
    // NO LONGER ON THE DISPATCH PATH. A TagModifyEvent now carries its own
    // mask, stamped at the call site from the emitting type's static
    // TAG_MASK (WIRE_TYPE_IDENTITY, ETCS_API.h), computed at COMPILE time
    // by ETCS::etcs_tag_index over the same Tags string. This map remains
    // for DecodeTagClosureMask -- naming which types were actually
    // colliding when diagnosing a reorder stall -- which is the one job
    // that genuinely needs the reverse mapping at runtime.
    //
    // That split also fixed a real hole rather than merely moving work:
    // RegisterTagBitIndex is only ever called from module-compiled code,
    // so the LOADER's own copy of this map is permanently empty, and
    // LoaderStream::on_event's old GetTagBit lookup returned an empty
    // mask for every tag modification originating loader-side.
    //
    // Populated exactly once, at static-init time, from module-compiled
    // code (see ETCS_MODULE_EXPORT_MAIN) — meaning it lands on THIS
    // module's own per-DSO EventNode singleton naturally, the same way
    // every other per-module static already does.
    std::unordered_map<std::string, uint8_t> tag_bit_index;
    std::vector<std::string>                 bit_tag_names; // reverse — index is the bit position
    // Overflow is FATAL, not truncating. A tag past the budget used to get
    // no entry here at all, so GetTagBit returned an empty mask, so
    // acquire() set no blocked_by_ bits for it -- meaning two operations
    // on the SAME type stopped serializing against each other. That is
    // precisely the guarantee the mask exists to provide, lost silently,
    // at dispatch time, several layers away from the declaration that
    // caused it.
    //
    // The real guard is the static_assert on the Tags string in
    // ETCS_MODULE_EXPORT_MAIN, which fails the BUILD. This is the backstop
    // for anything reaching here another way.
    void RegisterTagBitIndex(const std::vector<std::string>& ordered_tags)
    {
        if (ordered_tags.size() > TAG_BITS)
        {
            ETCS_LOG("EventNode:" << scope,
                     "FATAL: " << ordered_tags.size() << " contract tags declared, TAG_BITS is "
                     << TAG_BITS << ". Tags past the budget would silently stop serializing "
                     "against themselves.");
            std::abort();
        }
        for (size_t i = 0; i < ordered_tags.size(); ++i)
        {
            tag_bit_index[ordered_tags[i]] = static_cast<uint8_t>(i);
            bit_tag_names.push_back(ordered_tags[i]);
        }
    }
    // An empty mask if tag_name isn't in this module's own bit index. That
    // still means "fully independent", and it is still the correct answer
    // for a FOREIGN tag -- a different module has its own EventNode and
    // its own reorder buffer, so there is nothing here to order against.
    // It is no longer reachable by overflow (see RegisterTagBitIndex), and
    // no longer reachable by an empty loader-side index either, since
    // dispatch no longer calls this at all.
    TagMask GetTagBit(const std::string& tag_name) const
    {
        auto it = tag_bit_index.find(tag_name);
        return it != tag_bit_index.end() ? TagMask::bit(it->second) : TagMask{};
    }
    // Decodes a tag_closure_mask back into the type names it represents —
    // for diagnosing a collision after the fact ("which two types were
    // actually colliding"), not for anything on the hot dispatch path.
    std::vector<std::string> DecodeTagClosureMask(const TagMask& mask) const
    {
        std::vector<std::string> result;
        for (size_t i = 0; i < bit_tag_names.size(); ++i)
            if (mask.test(i))
                result.push_back(bit_tag_names[i]);
        return result;
    }
    // Set true for the duration of on_event's own execution -- lets
    // ~Entity() (Entity.h) detect "am I already running ON this ordering
    // thread" (e.g. reached via a module's own Name##_Cleanup() ->
    // cleanupTypedEntities() bulk sweep, itself triggered by ANOTHER
    // entity's own module-transfer cascade) and call entityUnloadImpl
    // directly instead of firing a blocking EntityUnloadEvent that would
    // otherwise deadlock waiting on this same thread to service it.
    // Module-side code never needs this: its own ~Entity() always
    // forwards to the LOADER's ordering thread, which is always a
    // genuinely different thread, so there's no reentrancy risk to guard
    // against there at all.
    inline static thread_local bool on_ordering_thread = false;
    // An ordering loop's on_event may EMIT to another stream, but must never
    // WAIT on one: that thread's own forward progress can be exactly what the
    // awaited event is ordered behind, which is a deadlock with no timeout and
    // no diagnosis (it presents as a spin into the watchdog, three threads
    // away from the cause). addTagImpl already states this rule informally --
    // "we are already on the ordering thread, so an event round-trip to
    // ourselves would deadlock"; this makes it mechanical instead of
    // remembered.
    #define ETCS_ASSERT_NOT_ORDERING_THREAD(who)                                \
        assert(!ETCS::EventNode::on_ordering_thread &&                          \
               who ": blocking wait attempted from an ordering thread -- emit "  \
               "an event instead of waiting for one.")
#ifdef ETCS_LOADER
    const char* scope = "Root";
    // ── LoaderStream ──────────────────────────────────────────────────────────
    // DLState is the template State to avoid the circular type problem.
    // owner is set in the EventNode constructor, giving the consumer access
    // to ridMap without EventNode needing to be complete at instantiation time.
    // on_event and impl method bodies live in DynamicLoader.h.
    struct LoaderStream : ETCS::EventStream<LoaderStream, DLState, DLInEventPtr>
    {
        EventNode* owner = nullptr; // wired in EventNode ctor
        // module name -> current live anchor Module, or nullptr (vacant).
        // NON-OWNING: every REAL anchor Module lives inside its hosting
        // entity's own local_arena_ — the first entity spawned of a
        // module IS that module's global scope. Written ONLY by this
        // stream's consumer thread (attachModule publish,
        // entityUnloadImpl's election, registry clearing); read from any
        // thread. Vacancy is a null VALUE, never row erasure. A loaded
        // .so always corresponds to exactly one canonical entry here the
        // instant it's loaded — resolveImpl (a thin wrapper over
        // attachModule) publishes here on a vacant bootstrap exactly like
        // any other attachModule call does; there is no separate,
        // unpublished "browse" path anymore.
        ArenaMap<std::string, Module*> module_registry{
            ArenaAllocator<std::pair<const std::string, Module*>>(
                &MemoryArena::getInstance())};
        // module name -> that module's own per-DSO MemoryArena::getInstance()
        // (see Name##_GetArena, ETCS_API.h). Deliberately PERMANENT and
        // separate from module_registry: module_arena outlives any single
        // hosting entity (it has the SAME static storage duration as the
        // module's own dlopen'd .so, for as long as this process runs),
        // so an entity dying WITHOUT ever having been the active host
        // still needs to reach it safely — going through the (possibly
        // currently-vacant) module_registry for this would be wrong, and
        // caching it on any individual Entity would dangle the instant a
        // different entity wins a later election. Resolved once, on
        // first contact with a module (whether via a real spawn or a
        // mere browse), and never cleared.
        ArenaMap<std::string, MemoryArena*> module_arena_registry{
            ArenaAllocator<std::pair<const std::string, MemoryArena*>>(
                &MemoryArena::getInstance())};
        // module name -> that module's PERSISTENT type_catalog pointer.
        // Populated once, the first time any Module::catalogTypes() ever
        // runs for that name, and REUSED (never rebuilt) on every
        // subsequent module-scope election — this is what makes the
        // pointer-stability guarantee on Entity::TagEntry::bundle actually
        // hold across a hand-off: the NEW anchor Module struct gets this
        // SAME pointer assigned directly, never a freshly-allocated map.
        ArenaMap<std::string, std::unordered_map<std::string, ModuleBundle>*> type_catalog_registry{
            ArenaAllocator<std::pair<const std::string, std::unordered_map<std::string, ModuleBundle>*>>(
                &MemoryArena::getInstance())};
        // ── Module-order bit index ──────────────────────────────────────
        // The loader-scope counterpart to EventNode's own tag_bit_index
        // (module scope, per-type). A module gets a bit assigned the
        // FIRST time any structural event (Load/Resolve/Destroy/AddTag)
        // touches it, in first-contact order — deterministic for a given
        // .etcs trace, since that order is itself just "the order things
        // got referenced," same guarantee tag_bit_index's own first-use
        // assignment relies on.
        //
        // Wraps at TAG_BITS (module TAG_BITS+1 shares module 1's bit,
        // etc.), and the wrap stays deliberate here in a way it never was
        // for tags: module count is genuinely unbounded, and sharing a
        // bit over-serializes two unrelated modules, which is slow and
        // correct. Tags could not use this trick, because there the
        // collision domain IS the type -- two types sharing a bit would
        // serialize things that should be independent, but two operations
        // on one type failing to share a bit would fail to serialize
        // things that must not be.
        ArenaMap<std::string, uint8_t> module_bit_index{
            ArenaAllocator<std::pair<const std::string, uint8_t>>(
                &MemoryArena::getInstance())};
        std::vector<std::string> bit_module_names;
        TagMask GetModuleBit(const std::string& module_name)
        {
            auto it = module_bit_index.find(module_name);
            if (it != module_bit_index.end())
                return TagMask::bit(it->second);
            uint8_t idx = static_cast<uint8_t>(bit_module_names.size() % TAG_BITS);
            module_bit_index[module_name] = idx;
            bit_module_names.push_back(module_name);
            return TagMask::bit(idx);
        }
        // OriginScopeMask — THE module-scope-to-loader-scope conversion, applied
        // to every event that steps up. A module-scope mask is tag bits and is
        // meaningless here; this is what those bits become on the way up. It is
        // never OR'd with the incoming tag mask, it REPLACES it.
        //
        // Two parts. reply_to->scope is the module the event was fired from,
        // set by every module-compiled *Event::operator()() and left null by
        // loader-originated ones (which contribute nothing -- the loader is
        // already the scope they are in). origin_extra_mask is what that module
        // asked to be coupled with, which today means the far side of a live
        // cross-module stream call.
        //
        // reply_to->scope reads the MODULE's own EventNode across the dlopen
        // boundary. Already done, by this same file: Module::registerLoader
        // reads node->scope and node->ridMap off exactly this pointer. It is a
        // string literal in the module's .rodata, live as long as the module
        // is, and `scope` sits at the same offset in both the ETCS_LOADER and
        // module builds of EventNode (same type, same position, opposite arms
        // of one #ifdef) -- so this reads the field it means to.
        TagMask OriginScopeMask(const DLInEvent& evt)
        {
            TagMask m = evt.origin_extra_mask;
            if (evt.reply_to && evt.reply_to->scope)
                m |= GetModuleBit(evt.reply_to->scope);
            return m;
        }
        // ── Root registry ────────────────────────────────────────────────
        // module name -> every live Root entity currently attached to it.
        // Root* directly (not LifetimeOwner) — this registry is
        // exclusively about finding a SIBLING ROOT to hand a lifetime
        // token to (see findRootCandidate below); it is never asked to
        // hold or compare against a genuine Entity, so there is no second
        // kind for it to tag. Forward-declared above (class Root;) since
        // Root itself is defined at the bottom of Entity.h, well after
        // this header.
        //
        // This is the Root-specific counterpart to
        // MemoryArena::findNextCandidateScope: an ordinary arena-resident
        // entity dying while holding a module's lifetime token gets a
        // sibling search over its own arena's dtor chain for free
        // (registerDtor<T>'s run_entity_delete callback). Root can't use
        // that at all — it's typically stack-allocated (see Root's own
        // class comment, Entity.h) and so NEVER appears in any arena's
        // own dtor chain. Membership here is opt-in and explicit: only
        // Roots that have gone through Root::changeModule (which
        // includes ~Module()'s own relinquish-only use — see
        // changeModuleImpl's own comment, DynamicLoader.h) are ever
        // tracked. A Root that only ever attaches via the ordinary
        // resolve/spawn path (ResolveEvent, CmdSetModule, etc.) is
        // invisible here and can't currently be found as a hand-off
        // candidate.
        ArenaMap<std::string, std::vector<Root*>> root_registry{
            ArenaAllocator<std::pair<const std::string, std::vector<Root*>>>(
                &MemoryArena::getInstance())};
        void registerRoot(const std::string& module_name, Root* root)
        {
            root_registry[module_name].push_back(root);
        }
        void unregisterRoot(const std::string& module_name, Root* root)
        {
            auto it = root_registry.find(module_name);
            if (it == root_registry.end()) return;
            auto& vec = it->second;
            for (auto vit = vec.begin(); vit != vec.end(); ++vit)
            {
                if (*vit == root) { vec.erase(vit); break; }
            }
        }
        // findRootCandidate — the Root-registry counterpart to
        // MemoryArena::findNextCandidateScope. Returns any OTHER live
        // Root already attached to module_name, or nullptr if none.
        Root* findRootCandidate(const std::string& module_name, Root* exclude)
        {
            auto it = root_registry.find(module_name);
            if (it == root_registry.end()) return nullptr;
            for (Root* e : it->second)
                if (e != exclude) return e;
            return nullptr;
        }
        // CRTP callbacks — bodies in DynamicLoader.h. RequestUnload's own
        // 200ms delay uses a detached std::thread that re-enqueues onto
        // this same stream when it wakes, not DispatchKind::Async/
        // ThreadPool/on_completion -- this stays a trivial no-op.
        ETCS::DispatchResult on_event(DLState&, const DLInEventPtr& evt, uint64_t seq);
        void on_completion(DLState&, ETCS::WorkResult*, uint64_t) {}
        // Resolved BEFORE dispatch, from the event alone. Body in
        // DynamicLoader.h -- it needs parseConjugateOriginKey. Public, like
        // every CRTP callback here: EventStream reaches it through
        // static_cast<Derived*>(this).
        ETCS::TagMask mask_for(DLState&, const DLInEventPtr& evt);
        // THE commit. Every blocking caller spins on a flag in its own stack
        // frame, so that flag's release-store is the moment the event is
        // observably finished -- not on_event returning. Doing it here rather
        // than in the handler is what puts the reorder buffer in charge of it.
        //
        // The usual stack-lifetime hazard inverts: the caller's frame is alive
        // precisely BECAUSE it is still spinning on this flag, and this store
        // is the last touch on that memory. Every other output (rid_out,
        // pairmask_out) was written by the handler and is published by this
        // release, ordered by the caller's own acquire-load.
        void on_emit(DLState&, ETCS::GapSlot& slot)
        {
            DLInEvent* e = static_cast<DLInEvent*>(slot.result);
            if (!e) return;   // Ack, and anything with nobody waiting
            switch (e->kind)
            {
                case DLInEvent::Kind::Load:
                    if (e->entity_out)
                        e->entity_out->store(reinterpret_cast<ETCS::Entity*>(e->release_value),
                                             std::memory_order_release);
                    break;
                case DLInEvent::Kind::Resolve:
                    if (e->resolve_ok)
                        e->resolve_ok->store(static_cast<int8_t>(e->release_value),
                                             std::memory_order_release);
                    break;
                case DLInEvent::Kind::Destroy:
                    if (e->tri_out)
                        e->tri_out->store(static_cast<int8_t>(e->release_value),
                                          std::memory_order_release);
                    break;
                case DLInEvent::Kind::AddTag:
                    // ready_out, not rid_out: AddTagEvent spins on the flag,
                    // since RID 0 is not impossible the way a null Entity* is.
                    // rid_out was written relaxed by the handler and is
                    // published by this store.
                    if (e->ready_out)
                        e->ready_out->store(true, std::memory_order_release);
                    break;
                case DLInEvent::Kind::EntityUnload:
                    if (e->unload_done)  e->unload_done->store(true, std::memory_order_release);
                    break;
                case DLInEvent::Kind::TagModify:
                    // Answer before flag: the caller spins on the flag, so the
                    // store that releases it must be the last touch here.
                    if (e->tagmodify_changed)
                        e->tagmodify_changed->store(e->release_value != 0,
                                                    std::memory_order_relaxed);
                    if (e->tagmodify_done) e->tagmodify_done->store(true, std::memory_order_release);
                    break;
                case DLInEvent::Kind::ChangeModule:
                    if (e->changemodule_done) e->changemodule_done->store(true, std::memory_order_release);
                    break;
                case DLInEvent::Kind::RequestUnload:
                    // Only the recheck fire has a waiter; the initial one is
                    // heap-allocated, deleted by its handler, and passes no
                    // completion.
                    if (e->request_unload_done)
                        e->request_unload_done->store(true, std::memory_order_release);
                    break;
                case DLInEvent::Kind::PairMask:
                    if (e->pairmask_done) e->pairmask_done->store(true, std::memory_order_release);
                    break;
                case DLInEvent::Kind::Ack:
                    break;
            }
        }
        // Impl methods — only called from on_event (single-threaded consumer).
        // Access owner->ridMap directly; no getInstance() calls inside consumer.
        ETCS::Entity* loadImpl   (const std::string& conjugate_key,
                                   ETCS::LifetimeOwner bootstrap_root);
        // Attaches (or re-attaches) a module to an entity's or Root's own
        // .module_ slot with no spawn_tag — proxy onto the live anchor if
        // one exists, or the module's real anchor if vacant. A thin
        // wrapper over attachModule now: there is no separate "transient
        // browse" path anymore (bindModule, formerly here, is gone) — a
        // module resolved this way is published to module_registry
        // immediately, the same as any other attachModule bootstrap,
        // since a loaded .so must always correspond to exactly one
        // canonical registry entry the instant it's loaded, regardless
        // of who's currently holding the entity/Root that hosts it.
        bool          resolveImpl(const std::string& module_name, ETCS::LifetimeOwner entity);
        bool          destroyImpl(const std::string& conjugate_key, ETCS::RID rid,
                                   bool delete_children = true);
        // Runs the type-erased addTag<T> trampoline on this (the ordering)
        // thread. See AddTagEvent above and Entity::addTagTrampoline<T>()
        // in Entity.h.
        RID           addTagImpl (ETCS::Entity* parent, ETCS::Entity* child,
                                  const ETCS::Buffer& tag, AddTagEvent::Trampoline trampoline);
        // THE Kind::EntityUnload handler — see EntityUnloadEvent's own
        // comment for the two paths that reach this (root via operator
        // delete, or an explicit child target) and exactly what each
        // does. Ordering-thread only.
        void          entityUnloadImpl(ETCS::Entity* target, bool delete_children = true);
        // THE single entry point for giving any entity or Root a Module
        // reference for module_name — always a proxy onto the one,
        // permanent global instance (bootstrapping it first if vacant),
        // claiming lifetime_owner iff that's currently empty. spawn_tag
        // is the contract tag to wire entity's own dispatch entry for —
        // pass "" to skip dispatch wiring entirely (used when resolveImpl
        // just wants the module attached, not any specific type spawned
        // from it, and ALWAYS the case when entity holds a Root rather
        // than a genuine Entity, since Root has no dispatch surface at
        // all). Returns success/failure rather than the attached
        // reference -- every caller either already holds the reference it
        // passed in, or only ever checked the old return value for
        // null/non-null. Ordering-thread only.
        bool          attachModule(const std::string& module_name,
                                    ETCS::LifetimeOwner entity,
                                    const std::string& spawn_tag);
        // THE Kind::RequestUnload delayed-recheck handler -- called after
        // the 200ms delay (see DLInEvent::request_unload_recheck's own
        // comment). Re-verifies lifetime_owner is still nullptr before
        // actually unloading; a no-op if anything claimed it in the
        // meantime. Ordering-thread only.
        void          requestUnloadImpl(ETCS::Module* target);
        // THE Kind::ChangeModule handler — see its own definition
        // comment, DynamicLoader.h. Ordering-thread only.
        void          changeModuleImpl(ETCS::Root* root, const std::string& target_module);
        bool          isTypedActionStream(const std::string& origin,
                                          const std::string& conjugate_key);
    private:
        std::pair<std::string, std::string> parseConjugateOriginKey(const std::string& key)
        {
            size_t p = key.find(":");
            if (p == std::string::npos || p == 0 || p == key.length() - 1)
                throw std::runtime_error(
                    "Invalid component key format. Expected 'module_name:tag_type'. Received: " + key);
            return {key.substr(0, p), key.substr(p + 1)};
        }
        // Bare type tag -> owning module name. Built once per module, right
        // after resolveImpl populates that module's type_catalog (see
        // Module::catalogTypes() in DynamicLoader.h). This is the reverse
        // index addTagImpl needs to correctly resolve a typed child's
        // module without assuming it shares its parent's module — the
        // type IS the key into this, exactly as it should be, regardless
        // of which entity happened to addTag<T> it or from where.
        std::unordered_map<std::string, std::string> type_owner_index;
        // Registers every tag in mod->type_catalog as owned by module_name.
        // Logs — does NOT silently overwrite — if a tag name is already
        // claimed by a DIFFERENT module, surfacing a genuine naming
        // collision loudly rather than picking one arbitrarily.
        void registerTypeOwnership(const std::string& module_name, Module* mod);
        // Called from each of Load/Resolve/Destroy/AddTag/EntityUnload's
        // own case in on_event, right before they return -- see
        // DLInEvent::reply_to's own comment (EventNode.h) for the full
        // reasoning. A no-op if evt.reply_to is null (a loader-originated
        // call, never needing to sync back with itself). Blocks on the
        // ack UNLESS reply_to's own stream refuses the enqueue (already
        // cleaning up), in which case there's nothing to wait for and
        // this returns immediately.
        void sendAckIfNeeded(DLInEvent& evt);
    } stream;
#else
    const char* scope = ETCS_MODULE_NAME;
    // ── ModuleProxy ───────────────────────────────────────────────────────────
    // Re-enqueues DLInEvents into the loader's LoaderStream. The result slot
    // pointer travels with the event so the loader writes directly back to
    // the caller's stack — no extra round-trip needed.
    // on_event body defined in DynamicLoader.h after getLoader() is available.
    struct ModuleProxy : ETCS::EventStream<ModuleProxy, DLState, DLInEventPtr>
    {
        EventNode* owner = nullptr; // wired in EventNode's own ctor, mirrors LoaderStream
        ETCS::DispatchResult on_event(DLState&, const DLInEventPtr& evt, uint64_t seq);
        void on_completion(DLState&, ETCS::WorkResult*, uint64_t) {}
        // TagModify is the only kind this stream completes for itself.
        // Everything else is forwarded and carries no completion here, so
        // slot.result is null and this no-ops.
        void on_emit(DLState&, ETCS::GapSlot& slot)
        {
            DLInEvent* e = static_cast<DLInEvent*>(slot.result);
            if (!e) return;
            // Answer before flag -- see LoaderStream::on_emit's own note.
            if (e->tagmodify_changed)
                e->tagmodify_changed->store(e->release_value != 0, std::memory_order_relaxed);
            if (e->tagmodify_done)
                e->tagmodify_done->store(true, std::memory_order_release);
        }
        // Module scope. TagModify is handled locally and its mask is genuine
        // TAG bits, stamped from the emitting type's own closure -- meaningful
        // here and nowhere else. Everything else is a hand-off whose effects
        // land in a scope this stream cannot describe, so all().
        ETCS::TagMask mask_for(DLState&, const DLInEventPtr& ref)
        {
            const DLInEvent& evt = *ref.ptr;
            if (evt.kind == DLInEvent::Kind::TagModify) return evt.tagmodify_mask;
            return ETCS::TagMask::all();
        }
    } stream;
    void RegisterRIDRegistry(const char* name, RIDListHandle handle)
    {
        ridMap[ETCS::Buffer(name)] = handle;
        ETCS_LOG("EventNode:" << scope, "Registered RID MAP for type: " << name << "!");
    }
#endif
    // Liveness, readable AFTER this object has been destroyed.
    //
    // Constant-initialized and trivially destructible, so unlike the instance
    // it guards it has no destructor of its own to be ordered against: it
    // stays valid for as long as this DSO is mapped, which is exactly the
    // window Name##_Cleanup() runs in. It is NOT protection against calling
    // into an already-dlclose'd module -- this flag lives in that module too,
    // and nothing in-process can help there.
    //
    // Name##_Cleanup() (ETCS_API.h) reaches this object from two callers that
    // nothing orders relative to each other: the explicit unload path, while
    // everything is still alive, and process exit, where the LOADER's own
    // MemoryArena static destructor reaches back in via Module::~Module ->
    // cleanupModule. __cxa_atexit is ONE global list torn down in reverse
    // registration order, and the loader's statics register before this module
    // was ever dlopen'd -- so the loader's MemoryArena is destroyed LAST and
    // this EventNode FIRST. The exit-path ridMap.clear() therefore ran against
    // a destroyed unordered_map: reproduced under ASan as a 104-byte
    // heap-use-after-free in _Hashtable::clear(), writing into a bucket array
    // already freed by __run_exit_handlers.
    inline static std::atomic<bool> s_alive{false};
    static bool alive() { return s_alive.load(std::memory_order_acquire); }

    EventNode()
    {
        // Taken HERE rather than as default member initialisers, for the same
        // reason as the placement above: the constructor is what runs in the
        // owning DSO, and these must be that DSO's own functions.
        set_log_to_file = &ETCS::set_log_to_file;
        get_log_to_file = &ETCS::get_log_to_file;
        stream.owner = this; // wire back-pointer now that EventNode is complete --
                              // both LoaderStream and ModuleProxy have this member.
        s_alive.store(true, std::memory_order_release);
    }
    ~EventNode() { s_alive.store(false, std::memory_order_release); }
    static EventNode& getInstance()
    {
#ifdef DEBUG_STATIC_GET_INSTANCE_ORDER
        ETCS_LOG("EventNode", "getInstance()");
#endif
        static EventNode instance;
        return instance;
    }
};
} // namespace ETCS
#endif