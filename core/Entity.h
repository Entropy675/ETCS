#ifndef ENTITY_H__
#define ENTITY_H__
#include "ETCS_API.h"
#include "../libs.h"
#include "Bundles.h"
#include "ArenaAllocator.h"
#include "MemoryArena.h"
#include "RIDList.h"
/*
 * EventNode.h - needed for AddTagEvent (addTag<T>, below), EntityUnloadEvent
 * (operator delete, below), and for the EventNode::ridMap access in
 * ~Entity()'s module-registry cleanup.
 *
 * This is safe DESPITE EventNode.h -> EventStream.h -> ETCS_API.h ->
 * CommandExecutor.h -> Command.h -> Entity.h looking circular on paper:
 * in practice, Entity.h is never reached except via core_defs.h ->
 * ETCS_API.h first (every module .h starts with that), so by the time
 * we're here, ETCS_API.h's include guard is ALREADY set. EventStream.h's
 * own #include "ETCS_API.h" is therefore a no-op, and the chain never
 * actually loops back into Entity.h.
 */
#include "EventNode.h"
#include <iostream>
#include <vector>
#include <mutex>   // resolvePairModuleMask's cache
#include <thread>  // the loader stream pair waits out its producer body
#include <chrono>
namespace ETCS
{
inline ETCS::Buffer source()
{
    return ETCS::Buffer(BASE_SOURCE_STRING);
}
inline HASH_TYPE GenerateEnvironmentSignature(const ETCS::Buffer& uniqueName);
/*
 * -----------------------------------------------------------------------
 * generateRID<Derived>() - process-wide, per-(module,tag)-type unique RID
 * generator. A free function template at namespace scope, NOT a member
 * of Entity (it used to be a protected static member template; moved out
 * so Root -- which no longer derives from Entity -- can still reach it
 * directly, the same way every real ontology leaf type already does via
 * WIRE_TYPE_IDENTITY's own macro expansion).
 *
 * Deliberately NOT marked `static`: this is a template, and non-static
 * free function templates already get the same weak/"vague" linkage a
 * member template does -- exactly one shared instantiation (and
 * therefore one shared `counter`) per (Derived) across every translation
 * unit that instantiates it within the same link unit, deduplicated by
 * the linker. Marking this `static` would give it internal linkage
 * instead, meaning every .cc file that instantiates generateRID<SameType>
 * would get its OWN private copy of `counter` -- silently fragmenting
 * RID generation for any module whose leaf types happen to be
 * referenced from more than one of its own translation units. That
 * would ALSO be a correctness regression against a hard invariant this
 * process depends on: two identical runs of the same .etcs script must
 * produce the identical RID sequence for the identical sequence of type
 * creations (deterministic seed from ETCS_MODULE_NAME + Derived::TAG,
 * deterministic incrementing counter -- no clock, no address, no
 * randomness of any kind enters this computation, ever). A fragmented,
 * per-TU counter would silently break that determinism the instant a
 * module's leaf types were referenced from two .cc files instead of one.
 *
 * Each dlopen'd module .so gets its own independent instantiation
 * regardless (RTLD_LOCAL means no symbol merging across the dlopen
 * boundary, matching how MemoryArena::getInstance()/EventNode::getInstance()
 * are already independently-scoped per module) -- this was already true
 * of the original member-template version and is unaffected by moving it
 * out of Entity.
 */
template<typename Derived>
inline uint64_t generateRID()
{
    static const uint64_t rid_seed = [](){
        char composed[256];
        std::snprintf(composed, sizeof(composed), "%s.%s",
            ETCS_MODULE_NAME, Derived::TAG); // module name is defined in module decl
        return XXH64(composed, std::strlen(composed), 0);
    }();
    static std::atomic<uint64_t> counter{0};
    uint64_t idx = counter.fetch_add(1, std::memory_order_relaxed);
    return XXH64(&idx, sizeof(idx), rid_seed);
}
/*
 * streamPairMask<P,C> - TAG-scope ordering bits for a stream pair: the two
 * types' CLOSURES, not their bare bits, since the call body runs their code and
 * touches what they reach.
 *
 * NOT memoized, deliberately. TAG_CLOSURE grows as the graph is built, so a
 * function-local static would freeze whatever the pair happened to reach the
 * first time this site ran -- a snapshot that silently narrows for the rest of
 * the process. Two atomic reads beside a makePair that allocates a page.
 *
 * TAG_MASK is assigned only by its own module's ETCS_TAG_DECLARE, so a FOREIGN
 * type reads empty here. That is the same-module test -- no module-name branch
 * needed -- and foreign falls to all(), since two modules' tag bits cannot be
 * meaningfully OR'd. Their loader-scope answer is resolvePairModuleMask below.
 */
template<typename P, typename C>
inline ETCS::TagMask streamPairMask()
{
    const ETCS::TagMask a = P::TAG_MASK;
    const ETCS::TagMask b = C::TAG_MASK;
    if (!a.any() || !b.any()) return ETCS::TagMask::all();
    return P::readTagClosure() | C::readTagClosure();
}
/*
 * resolvePairModuleMask - MODULE-scope ordering bits for a stream pair, keyed
 * by contract tag rather than type: the same tags safeBundleFor resolves with,
 * so it answers for the untyped call overload too. That overload is where
 * genuinely cross-module pairs come from -- safeBundleFor finds a bundle
 * wherever it lives, and nothing constrains the halves to one module.
 *
 * One blocking round trip per distinct tag pair, then a hash lookup forever.
 * The cache never needs invalidating: requestUnloadImpl does not erase from
 * module_bit_index, so a module keeps its bit across unload and reload.
 *
 * Per-DSO under -fvisibility=hidden, so each module pays its own first resolve.
 * Mutex-guarded because stream calls run on arbitrary pool workers; holding the
 * lock across a miss collapses concurrent first-resolves of one pair into one.
 *
 * Fails shut twice over: on an ordering thread it does not fire at all (that
 * deadlocks -- see ETCS_ASSERT_NOT_ORDERING_THREAD), and an unresolved tag
 * returns all(). Both cost throughput, neither costs ordering.
 */
inline ETCS::TagMask resolvePairModuleMask(const ETCS::Buffer& tag_a,
                                           const ETCS::Buffer& tag_b)
{
    static std::mutex m;
    static std::unordered_map<std::string, ETCS::TagMask> cache;
    /*
 * \x1f between the halves -- not a character a tag can contain, so "ab"+"c"
 * and "a"+"bc" cannot collide on one entry.
 */
    std::string key = tag_a.toString() + "\x1f" + tag_b.toString();
    std::lock_guard<std::mutex> g(m);
    auto it = cache.find(key);
    if (it != cache.end()) return it->second;
    /*
 * By value, not a reference into the map: a reference would outlive the
 * guard and make lock-free reads a question at all. 32 bytes.
 */
    if (ETCS::EventNode::on_ordering_thread) return ETCS::TagMask::all();
    ETCS::PairMaskEvent evt(tag_a, tag_b);
    evt();
    cache.emplace(std::move(key), evt.result);
    return evt.result;
}

// Defined at the bottom of this file, once EventNode is complete. Declared
// here because addTagTrampoline calls it on a non-dependent Entity*, so the
// name has to be visible where that template is defined, not merely where
// it is instantiated.
class Entity;
inline void etcs_supertype_fanout(Entity* e);

class Entity
{
    /*
 * No friend declarations. Everything DynamicLoader.h needs (setting
 * module identity, reading parent_ for the module-scope election)
 * goes through plain public accessors below instead - see
 * getParent()/setModuleSource(). This matters for a real reason, not
 * just style: friending EventNode::LoaderStream specifically breaks
 * compilation of every MODULE .cc file, since LoaderStream only
 * exists inside EventNode.h's #ifdef ETCS_LOADER block - module code
 * compiles WITHOUT that macro defined, so the friended name doesn't
 * exist there at all. Public accessors have no such visibility
 * mismatch; they're just ordinary members, valid in every
 * translation unit that includes this header.
 */
public:
    /*
 * Every entity's own Module - first member, deliberately, so it's
 * immediately valid the moment any Entity* is dereferenced. Starts
 * vacant (name == "", library_handle == nullptr) for the overwhelming
 * majority of entities that never host anything at all; attachModule
 * (DynamicLoader.h) is what turns a vacant one into a real anchor, a
 * proxy, or the target of a hand-off - see Module's own definition
 * (Bundles.h) for the full reasoning on why it lives there and why
 * it's a direct member now rather than something separately
 * allocated. Constructed with *this as its owner below - see
 * Module's own constructor for why hosting_entity is mandatory.
 */
    Module module_;
private:
    template<typename K, typename V>
    using ArenaMap = std::unordered_map<
        K, V,
        std::hash<K>,
        std::equal_to<K>,
        ArenaAllocator<std::pair<const K, V>>
    >;
    struct TagEntry
    {
        /*
 * Non-owning pointer into the owning Module's (persistent, shared,
 * never-copied) type_catalog entry - NOT a copy. type_catalog is
 * allocated once from the module's own MemoryArena and never
 * moves or gets recreated across a module-scope hand-off, so this
 * pointer stays valid for as long as the module is loaded at all,
 * regardless of which entity currently hosts the anchor Module
 * struct itself.
 */
        ModuleBundle* bundle = nullptr;
        Entity*       child  = nullptr;
    };
    mutable std::mutex                  m_tagMutex;
    /*
 * ---------------------------------------------------------------------
 * owning_arena_ - the arena this entity's OWN outer object and its
 * DestructorRecord were allocated from. NOT the same thing as
 * local_arena_ (which is this entity's own content arena, allocated
 * FROM owning_arena_), and NOT reliably the same as
 * parent_->getArena() any more.
 *
 * It always was for a freshly-addTag'd child: addTag<T> constructs it
 * via getArena().allocate<T>() on the parent, so the parent's own
 * local_arena_ held both the object and its record, and inferring the
 * one from the other was correct by construction. reparentChildrenTo
 * breaks that inference -- deliberately, and correctly. Under this
 * codebase's nesting schema a reparented child's storage genuinely
 * does NOT move: its record stays on the dying entity's local_arena_,
 * which stays alive because it was itself allocated from ITS parent's
 * arena, on up to whatever root is still in scope. Only the
 * typed_children_ bookkeeping moves. So after a reparent,
 * parent_->getArena() names the arena that now TRACKS this child,
 * while its record still sits in the arena that ALLOCATED it -- two
 * different arenas, and every dtor-chain operation wants the second
 * one.
 *
 * Stored rather than re-derived because there is no expression that
 * recovers it after the fact: the original parent may already have
 * been reclaimed, and the arena chain is not walkable downward.
 * ---------------------------------------------------------------------
 */
    MemoryArena*                        owning_arena_;
    /*
 * ---------------------------------------------------------------------
 * local_arena_ - entity-local sub-arena.
 *
 * Allocated from the appropriate PARENT arena based on how this entity
 * is being constructed - see s_pending_parent_arena_ below. A root
 * entity (no parent) draws from the module-wide MemoryArena::getInstance()
 * singleton, exactly as before. A CHILD entity (created via addTag<T>)
 * now draws from its PARENT's own local_arena_ instead - this is the
 * fix for the bug where addTag<T>-created children were previously
 * allocated from the module-wide singleton regardless of parentage,
 * meaning a parent's arena teardown could never reach (or correctly
 * cascade-clean) its own children's memory. Now a child's ENTIRE
 * footprint - its own outer object AND this local_arena_ - lives
 * inside its parent's arena, so evoking the parent's arena tree
 * reaches everything in one place.
 *
 * It is ALSO independently resettable at any time via getArena().reset(),
 * which only touches this entity's own chunks.
 *
 * Must be declared before tags/flags_/typed_children_ below - member
 * init order follows declaration order, and all three allocate out of
 * *local_arena_ via their ArenaAllocator construction in the ctor.
 *
 * Held as a pointer because MemoryArena's copy ctor/assignment are
 * deleted and it has no move ctor either.
 *
 * NOTE: this is exclusively an Entity concept. Root (standalone,
 * below) has no local_arena_ at all -- it never has typed children,
 * tags, or flags_, so there was never anything for a sub-arena to
 * back. An earlier version of Root inherited this field (and the
 * unconditional global-arena allocation in Entity's own constructor
 * below) purely as a side effect of publicly inheriting Entity, with
 * nothing in ~Entity()'s own body ever reclaiming it early -- every
 * Root ever constructed leaked exactly one 4KB MemoryArena off the
 * global arena until actual process exit. Root no longer having this
 * field at all is what closes that leak structurally, rather than by
 * adding cleanup code for a resource Root never actually needed.
 * ---------------------------------------------------------------------
 */
    MemoryArena*                        local_arena_;
    /*
 * Module dispatch table. Populated exclusively by the ETCS_LOADER-gated
 * addTag(tag, bundle, child) overload below - this is what call() reads
 * via tags[tag_type].bundle->(...). bundle is now a pointer (see
 * TagEntry above) rather than an owned copy.
 */
    ArenaMap<ETCS::Buffer, TagEntry>    tags;
    /*
 * Simple string flags - addTag(Buffer). No dispatch participation, just
 * cheap identity/classification markers. Must start with a lowercase
 * ASCII letter or addTag throws.
 */
    ArenaMap<ETCS::Buffer, bool>        flags_;
    /*
 * One type-erased pointer per composed family, keyed by family name.
 * Populated by ETCS_MAKE_INSTANCE's own constructor (ETCS_API.h) via
 * static_cast<Name##_*>(this) -- always a legal, ordinary upcast
 * (Name##Base<Derived> : public Name##_ is non-virtual at THIS link;
 * the virtual inheritance is one level further up, between Name##_
 * and Entity, never touched here). Retrieved generically via
 * getInterfacePointer(family) and cast back to the real interface
 * type ONLY by code that already knows what that family is
 * (MirrorBuffer.h knowing Wrapper_, specifically) -- Entity itself
 * never needs to know what any of these families actually are.
 */
    ArenaMap<ETCS::Buffer, void*>       interface_pointers_;
    /*
 * Typed children - addTag<T>(tag, ...). One RIDList<T*> per distinct
 * tag string, type-erased via RIDListHandle so a single map can hold
 * heterogeneous T's. Lives in local_arena_, so it's torn down with the
 * entity rather than the global arena.
 */
    ArenaMap<ETCS::Buffer, RIDListHandle> typed_children_;
    /*
 * First-attachment order of distinct tag TYPES in typed_children_ -
 * NOT one entry per instance (a second addTag<SameType>() reuses the
 * existing typed_children_ entry's RIDList, it doesn't get a new
 * slot here). Exists purely so a PackPair-capable-tag walk (MirrorBuffer
 * transport auto-detection) can scan a contiguous, arena-backed,
 * insertion-ordered sequence instead of a hash-bucket-ordered map -
 * typed_children_ itself stays untouched (still O(1) find, still the
 * hot path for hasTag()/call()/addTagTrampoline<T> lookups). Appended
 * to in exactly one place: addTagTrampoline<T>'s own first-seen-tag
 * branch, under the same m_tagMutex lock and the same loader-ordering-
 * thread single-writer guarantee that branch already relies on.
 */
    std::vector<ETCS::Buffer, ArenaAllocator<ETCS::Buffer>> typed_child_order_{
        ArenaAllocator<ETCS::Buffer>(local_arena_)
    };
    /*
 * Per-entity registry of every currently-in-flight stream call's own
 * SignalContext -- see Scope's own comment (Bundles.h) for the full
 * reasoning. A plain, default-constructed member; no special init
 * needed. Protected by m_tagMutex, same as tags/flags_/typed_children_
 * -- Scope itself has no lock of its own.
 */
    Scope scope_;

    /*
 * ---------------------------------------------------------------------
 * ctx_ - this entity's own node on the PASSIVE (ownership) signal edge.
 * Carries no local authority of its own: every flag stays null, and it
 * exists purely to be a link in the chain. `provider` points at whatever
 * structurally owns this entity -- the parent entity's own ctx_ for an
 * addTag<T> child (set in addTagTrampoline<T>, rewritten by
 * reparentChildrenTo), or the spawning ModuleBundle's own ctx for a
 * root-level entity (set in ModuleBundle::operator()(), DynamicLoader.h).
 *
 * The passive edge TERMINATES at the bundle -- it does not continue up
 * to the process root on its own. It doesn't need to: the bundle's own
 * ctx already carries the module's local authority with `up` wired to
 * RootSignalContext() (Module::getTagAddress), so a walk arriving here
 * crosses onto the ACTIVE edge at that point and reaches the global
 * flags from there. SignalContext::walk doesn't distinguish the two
 * edges once traversing, so this is automatic rather than something
 * either side has to arrange.
 *
 * Public accessor below (getContext) rather than friending -- same
 * no-friend-declarations policy as getParent()/setModuleSource(), and
 * the two writers (addTagTrampoline<T>, reparentChildrenTo) are both
 * members of this class anyway.
 * ---------------------------------------------------------------------
 */
    SignalContext ctx_;

    EntityLocale locale = EntityLocale::InProcess;
    /*
 * Set at top-level spawn time (ModuleBundle::operator()()) or at
 * addTag<T> registration time (addTagImpl, DynamicLoader.h). Plain
 * strings, deliberately NOT a cached Module* - a cached pointer here
 * would go dangling the instant a module-scope election replaces the
 * current anchor Module instance with a different one (a routine
 * event, not a failure case), and this entity might not be the one
 * that dies first. Every place that actually needs the CURRENT
 * Module* re-resolves it fresh via module_registry[source_module_],
 * which is always safe: if the entry is non-null, that Module object
 * is genuinely alive right now, full stop, by construction of the
 * whole election mechanism.
 */
    ETCS::Buffer source_module_;
    ETCS::Buffer source_tag;
    /*
 * Set true as the FIRST action inside ~Entity(). A no-op operator
 * delete (the child case, below) means the object's own bytes are
 * never reclaimed individually - the flag survives sitting in that
 * still-allocated memory, so a LATER explicit unload targeting this
 * same child (see EventNode::LoaderStream::entityUnloadImpl) can tell
 * "destructor already ran via an earlier ordinary delete" (unlink
 * only - forget) apart from "never touched" (call the destructor AND
 * unlink - evokeDestructor). Calling a destructor twice is undefined
 * behavior regardless of whether its own body happens to be
 * idempotent, so this distinction is load-bearing, not defensive
 * decoration.
 */
    bool destructed_ = false;
    /*
 * Captured by ~Entity() while the object is still whole, read by operator
 * delete after it is not. C++ ends the object's lifetime when the
 * destructor completes, so operator delete reading parent_/owning_arena_
 * through `e` is formally indeterminate -- GCC 14 diagnoses exactly this
 * ("is used uninitialized"), and it stopped being merely formal the moment
 * reclaimEntity began zeroing the shell and handing it to the free list:
 * those bytes can now be a DIFFERENT live entity by the time a later
 * delete reads them, so forget() would unlink someone else's record from
 * someone else's arena.
 *
 * Deliberately still IN the object rather than a side table: operator
 * delete has nothing but the address to key on, and these are read on the
 * one path (an ordinary `delete child`) where the bytes are provably still
 * this object's -- reclaimEntity and delete are mutually exclusive, since
 * reclaim implies nothing will ever call delete on it. What this buys is
 * that the read no longer travels through a member whose value the
 * compiler is entitled to treat as garbage.
 */
    Entity*      delete_parent_ = nullptr;
    MemoryArena* delete_arena_  = nullptr;
    /*
 * Set only when this entity was spawned via a parent's addTag<T>(...).
 * nullptr otherwise. On destruction, if set, this entity removes its
 * own RID from the parent's RIDList (removeTypedChild(rid) alone finds
 * the right bucket - see its own comment for why no separate tag needs
 * to be tracked here at all). Also THE single signal everything else
 * in this class and in entityUnloadImpl/operator delete branches on:
 * parent_ == nullptr is the ENTIRE distinction between "this destructor
 * has already fully run" (root, reached only via ordinary delete) and
 * "this destructor has not run yet" (child, reachable only via an
 * explicit unload target) - see operator delete below for why no
 * separate flag is needed to make that call.
 */
    Entity* parent_     = nullptr;
    RID     parent_rid_ = 0;
    /*
 * Set by addTagTrampoline<T>() (via AddTagEvent, on the loader's
 * ordering thread) iff T's type-provider module registered a
 * module-level RIDList for T at load time (ETCS_TAG_DECLARE /
 * RegisterRIDRegistry) - i.e. iff T is a real ontology leaf, not some
 * ad-hoc internal type someone addTag<T>'d without ever exporting it
 * through ETCS_TAG_BLOCK_*. Lets ~Entity() remove itself from that
 * module-level registry on destruction without needing T again -
 * ~Entity() isn't templated and has no other way to reach it.
 */
    ETCS::Buffer module_registry_key_;
    RID          module_registry_rid_ = 0;
    /*
 * -----------------------------------------------------------------------
 * s_pending_parent_arena_ - the mechanism that lets Entity()'s own
 * (parameterless, unchanged-signature) constructor decide where
 * local_arena_ comes from, without requiring every concrete ontology
 * leaf type's constructor to be rewritten to accept and forward an
 * arena parameter through their own init lists (which would touch
 * code well outside this file). addTag<T> below sets this to the
 * PARENT'S arena immediately before constructing the child, and
 * restores whatever it was beforehand immediately after - a save/
 * restore pair rather than a blind reset, so nested/reentrant
 * addTag<T> calls (a child itself addTag<T>-ing a grandchild during
 * its own construction, however unlikely) still resolve correctly.
 * thread_local because construction can happen on any thread; a
 * static-only version would let concurrent constructions on different
 * threads stomp each other's pending arena.
 */
    static inline thread_local MemoryArena* s_pending_parent_arena_ = nullptr;
public:
    /*
 * setPendingParentArena - lets a DERIVED class's own constructor
 * redirect where a plain, inline-composed Entity-derived MEMBER's
 * own local_arena_ comes from, exactly the same mechanism addTag<T>
 * already uses internally for a typed CHILD (above), exposed here
 * for the structurally identical "composed member" case. See
 * SocketConnectionState's own parser_ member (NetworkProvider) for
 * the motivating case: without this, a composed Entity-derived
 * member gets an ORPHANED local_arena_ off the global singleton,
 * reachable by nothing until the outer type's own arena eventually
 * tears down in full -- and an explicit early reclaim of that
 * orphan, attempted from inside the outer type's own destructor
 * BODY, is unsafe: C++ guarantees a destructor's own body always
 * finishes before any member's own implicit destruction begins, so
 * tearing down a member's own backing arena from the body always
 * runs too early, before that member's own Entity-inherited fields
 * (tags/flags_/interface_pointers_, all living in that same arena)
 * have safely destructed themselves -- a real, reproduced SIGSEGV
 * this session traced to exactly that ordering.
 *
 * Returns the PREVIOUS value, matching addTag<T>'s own save/restore
 * discipline exactly -- callers pass this back to a second call to
 * restore it afterward, rather than blindly resetting to nullptr, so
 * nested/reentrant construction on the same thread still resolves
 * correctly.
 */
    static MemoryArena* setPendingParentArena(MemoryArena* target)
    {
        MemoryArena* previous = s_pending_parent_arena_;
        s_pending_parent_arena_ = target;
        return previous;
    }
private:
    /*
 * allocatePage - compile-time dispatched per Strategy.
 *
 * LMAX:        allocates a single-producer single-consumer ring.
 *              slot_count = 64 is a reasonable default for stream calls;
 *              adjust per call site if the stream depth is known.
 *
 * Pipe/Socket: allocates a SharedPage staging buffer. SharedPage::allocate
 *              sizes to one arena chunk (2MB on huge-page systems), which
 *              comfortably covers any single Buffer payload.
 */
    template<typename Strategy, typename Page>
    Page* allocatePage(uint64_t rid)
    {
        if constexpr (std::is_same<Strategy, ETCS::StrategyLMAX>::value)
        {
            return ETCS::LMAXSequentialSharedPage::allocate(
                getArena(), rid, /* slot_count */ 64);
        }
        else
        {
            return ETCS::SharedPage::allocate(getArena(), rid);
        }
    }
public:

    Entity()
        : module_("", *this)
        , owning_arena_(s_pending_parent_arena_ ? s_pending_parent_arena_
                                                : &MemoryArena::getInstance())
        , local_arena_(owning_arena_->allocate<MemoryArena>(
              DEFAULT_ARENA_START_PAGE, /* performance */ false))
        , tags(ArenaAllocator<std::pair<const ETCS::Buffer, TagEntry>>(local_arena_))
        , flags_(ArenaAllocator<std::pair<const ETCS::Buffer, bool>>(local_arena_))
        , interface_pointers_(ArenaAllocator<std::pair<const ETCS::Buffer, void*>>(local_arena_))
        , typed_children_(ArenaAllocator<std::pair<const ETCS::Buffer, RIDListHandle>>(local_arena_))
    {}
    /*
 * -----------------------------------------------------------------------
 * ~Entity() - pure, ordinary cleanup now. Both the child-reparenting
 * and the module-election/unload-request logic that used to live
 * here have moved to MemoryArena's own run_entity_delete callback
 * (see registerDtor<T>'s own comment, MemoryArena.h), which runs
 * BEFORE this destructor ever executes at all -- triggered via
 * MemoryArena::deleteEntity(), not via this function's own body.
 * That's the actual fix for a real, structural problem: an arena-
 * resident (non-stack) entity's destructor only ever runs when
 * something explicitly walks the arena's own dtor records and calls
 * it (evokeDestructor) -- nothing does that "naturally" the way
 * stack unwinding calls a stack-allocated Root's destructor for
 * free. Putting decision logic (search for a survivor, reparent
 * children) INSIDE this destructor meant it would only ever run
 * whenever something ELSE happened to trigger this destructor in the
 * first place -- often far too late (e.g. only at the whole arena's
 * own final teardown), and in the specific case of reparenting/
 * reclamation logic that itself touched the SAME arena's own dtor
 * chain that a bulk teardown walk might currently be mid-iteration
 * over, genuinely unsafe regardless of timing. Moving it to the
 * arena's own explicit deletion entry point means it's decided once,
 * synchronously, by the one thing that actually controls whether
 * this destructor runs at all -- never re-entered from inside a
 * teardown walk over the same structure.
 */
    virtual ~Entity()
    {
        /*
 * Signal every currently-in-flight stream call's own
 * SignalContext FIRST, before anything else -- see Scope::
 * interruptAll's own comment (Bundles.h) for why .interrupt
 * specifically, and why this needs m_tagMutex: registerScope/
 * unregisterScope (ScopeTag's own ctor/dtor, called from whatever
 * thread is actually running a given stream trampoline -- a pool
 * worker for produce, the calling thread for consume) can run on
 * a genuinely different thread than this destructor (which only
 * ever runs on the loader's own ordering thread, via
 * evokeDestructor/reclaimEntity), so without this lock,
 * interruptAll()'s own walk of scope_.contexts would race a
 * concurrent insert/erase into the SAME underlying unordered_map
 * -- undefined behavior, not merely a stale read.
 *
 * Only closes the "loop still running against reclaimed memory"
 * window down to one loop iteration, not to zero -- a loop
 * mid-iteration at the exact moment this runs could still touch
 * `this` once more before its own next isInterrupted() check.
 * Same, already-accepted property every other volatile
 * sig_atomic_t signal in this codebase already has (a cross-
 * thread signal-handler-safety primitive, not std::atomic) --
 * not a new weakness this introduces.
 * Last resort only. By the time this runs the destructor has already
 * begun, so signalling here cannot prevent a concurrent scope from
 * touching a half-destroyed object -- there is nobody left to wait.
 * The real barrier is drainEntityScopes (DynamicLoader.h), which runs
 * on the CALLER's thread before any destroy is enqueued. This stays
 * for the paths that bypass that entirely (arena bulk teardown,
 * Root leaving scope), where signalling late beats not signalling.
 */
        {
            std::lock_guard<std::mutex> lock(m_tagMutex);
            scope_.interruptAll();
        }
        destructed_ = true;
        /*
 * Unlink our own record HERE, while the object is still alive, rather
 * than from operator delete -- which runs after this destructor
 * completes, i.e. after the object's lifetime has formally ended. Any
 * read through the object at that point is indeterminate (GCC 14
 * diagnoses it directly), and it stopped being merely formal once
 * reclaimEntity began zeroing the shell and returning it to the free
 * list: those bytes can be a DIFFERENT live entity by then, so
 * forget() would unlink someone else's record from someone else's
 * arena.
 *
 * Same conditions the old operator delete applied -- only a CHILD's
 * record is ours to unlink (a root's belongs to whatever spawned it),
 * and forget() rather than evokeDestructor() because the destructor is
 * running right now and must never be invoked twice.
 *
 * Idempotent by construction: forget() on an already-unlinked record
 * returns false and does nothing, so the reclaim paths (which unlink
 * via unlinkRecord before ever running ~T()) are unaffected -- they
 * simply find nothing left to remove.
 */
        if (parent_ != nullptr && owning_arena_ != nullptr)
            owning_arena_->forget(this);
        /*
 * Detach from parent's typed-child tracking before this entity's
 * own state goes away. Parent's removeTypedChild is a no-op if it
 * can't find the entry, so this is safe even if cleanup already
 * happened some other way.
 */
        if (parent_)
            parent_->removeTypedChild(parent_rid_);
        /*
 * Mirror of the above, for the module-level registry addTagTrampoline
 * additionally registers into (see module_registry_key_ comment).
 * Not tied to any arena reset - the module-level RIDList is a plain
 * static with process lifetime, so without this the registry
 * accumulates a dangling entry for every addTag<T>-spawned entity
 * that's ever destroyed, for as long as the process runs.
 */
        if (module_registry_key_.written > 0)
        {
            auto& moduleRidMap = ETCS::EventNode::getInstance().ridMap;
            auto mit = moduleRidMap.find(module_registry_key_);
            if (mit != moduleRidMap.end())
                mit->second.invoke_remove(module_registry_rid_);
        }
    }
    EntityLocale getLocale() { return locale; }
    /*
 * Entity-local arena. Was a proxy straight through to the global
 * singleton; now instance-scoped, and (per the fix above) drawn from
 * whichever arena was appropriate for how this entity was constructed.
 */
    MemoryArena& getArena()              { return *local_arena_; }
    /*
 * The arena holding THIS entity's own record -- see owning_arena_'s
 * own comment. This is what every dtor-chain operation targeting this
 * entity must be called on (forget/evokeDestructor/reclaimEntity/
 * deleteEntity), never parent_->getArena().
 */
    MemoryArena& getOwningArena()        { return *owning_arena_; }

    /*
 * The actual global singleton - used for things that must outlive any
 * single entity (Entity::operator new itself, and anything explicitly
 * module/process-scoped rather than entity-scoped).
 */
    static MemoryArena& getGlobalArena() { return ETCS::MemoryArena::getInstance(); }
    static ThreadPool& getThreadPool()  { return ETCS::ThreadPool::getInstance(); }
    static Manifest& getManifest()
    {
        static Manifest instance;
        return instance;
    }
    /*
 * True once ~Entity() has run on this object. See destructed_'s own
 * comment for exactly why this distinction matters and isn't merely
 * informational.
 */
    bool isDestructed() const { return destructed_; }
    const ETCS::Buffer& getSourceModule() const { return source_module_; }
    const ETCS::Buffer& getSourceTag()    const { return source_tag; }
    /*
 * The single fact everything outside this class needs to branch on:
 * is this entity a root (nullptr) or a child. A plain getter - no
 * friend access needed anywhere for this.
 */
    Entity* getParent() const { return parent_; }
    /*
 * This entity's own passive-edge node -- see ctx_'s own comment. The
 * non-const overload is what ModuleBundle::operator()() (DynamicLoader.h)
 * uses to set a root-level entity's provider at spawn time; the const
 * one is for anything that only wants to READ authority through it
 * (isInterrupted() and friends are all const).
 */
    SignalContext&       getContext()       { return ctx_; }
    const SignalContext& getContext() const { return ctx_; }
    /*
 * Walks parent_ up to the entity with no parent at all. For a
 * top-level entity this is just `this`. For an addTag<T> child, this
 * reaches whatever real, election-visible root eventually owns it -
 * exactly what's needed to bootstrap a not-yet-loaded module a child
 * references (see addTagImpl's vacant branch, DynamicLoader.h): no
 * separate "root" concept needed there, since whatever ancestor this
 * reaches becomes the global instance's lifetime_owner claimant if
 * the module happens to be vacant when attachModule runs against it
 * - the same, uniform first-attach logic regardless of which kind
 * of ancestor it turns out to be. Always returns a genuine Entity* --
 * Root (standalone, below) was never part of this chain, even before
 * it stopped inheriting Entity.
 */
    Entity* getRootAncestor()
    {
        Entity* e = this;
        while (e->getParent()) e = e->getParent();
        return e;
    }
    /*
 * Sets module identity - called from ModuleBundle::operator()() at
 * top-level spawn time, and from addTagImpl at typed-child
 * registration time. A public setter rather than a friend grant for
 * the same reason getParent() is a public getter: it works
 * identically in every translation unit, module or loader, with no
 * ETCS_LOADER-conditional type to friend.
 */
    void setModuleSource(const ETCS::Buffer& tag, const ETCS::Buffer& module_name)
    {
        source_tag     = tag;
        source_module_ = module_name;
    }
    template<class F, class... Args>
    auto schedule(int priority, SignalContext ctx, F&& f, Args&&... args)
        -> std::future<std::invoke_result_t<F, Args...>>
    {
        return getThreadPool().enqueue(
            priority, ctx,
            std::forward<F>(f),
            std::forward<Args>(args)...
        );
    }
    /*
 * -----------------------------------------------------------------------
 * addTag(Buffer) - simple string flags.
 *
 * No dispatch, no ownership tracking. Just a cheap identity/classification
 * marker. Must start with a lowercase ASCII letter - anything else throws,
 * by design, so these can't accidentally collide with conjugate-action
 * tag strings (which are conventionally TitleCase, e.g. "Window") or with
 * typed-child tags.
 *
 * Fires TagModifyEvent rather than mutating flags_ directly - ordered
 * LOCALLY, within this entity's own module's event stream (see
 * ModuleProxy::on_event's fork), never forwarded to the loader. Same-
 * type tag-list operations serialize against each other in causal
 * sequence; different-type operations commit independently.
 *
 * myTagClosure() supplies the ordering bits: this type's own plus every
 * type in its module it holds a reference to (TAG_CLOSURE, accumulated by
 * addTag<T>). Its own bit alone would be wrong -- an op on a node that owns
 * boards touches those boards, through pointers no mask can see at the
 * point of use.
 *
 * Carried on the event rather than resolved by whichever EventNode services
 * it -- see DLInEvent::tagmodify_mask (EventNode.h) for why that lookup
 * silently returned nothing on the loader's side.
 * -----------------------------------------------------------------------
 */
    void addTag(const ETCS::Buffer& flag)
    {
        const char* s = flag.c_str();
        if (!s || !s[0] || s[0] < 'a' || s[0] > 'z')
            throw std::invalid_argument(
                std::string("addTag: flag must start with a lowercase letter: ")
                + (s ? s : ""));
        ETCS::TagModifyEvent{this, flag, false, &Entity::tagModifyImpl, myTagClosure()}();
    }
    /*
 * TAG-scope bits beyond this entity's own closure -- ScopeTag passes a
 * stream pair's mask so the flag orders against BOTH connected types. Same
 * module, so the OR is meaningful; see streamPairMask for why a foreign
 * type never arrives here as bits.
 */
    void addTag(const ETCS::Buffer& flag, const ETCS::TagMask& extra)
    {
        const char* s = flag.c_str();
        if (!s || !s[0] || s[0] < 'a' || s[0] > 'z')
            throw std::invalid_argument(
                std::string("addTag: flag must start with a lowercase letter: ")
                + (s ? s : ""));
        ETCS::TagModifyEvent{this, flag, false, &Entity::tagModifyImpl,
                             myTagClosure() | extra}();
    }
    /*
 * -----------------------------------------------------------------------
 * addTag<T>(args...) - typed child, module-side. Returns T*, not RID.
 *
 * Takes no tag parameter: T alone yields the contract name, via
 * T::CONTRACT_TAG (ETCS_API.h). CONTRACT_TAG and not T::TAG -- TAG is the
 * concrete class name, which a Contract_*.h typedef leaves unchanged.
 * This also fixed a real
 * allocation bug from an earlier version: every actual call site used
 * to construct the child via MemoryArena::getInstance().allocate<Name>()
 * (the module-wide singleton) and pass the already-built pointer in,
 * meaning a child's own outer object was NEVER actually reachable from
 * its parent's arena at all. Now addTag<T> constructs the child ITSELF,
 * from getArena() (this entity's own arena) - via s_pending_parent_arena_,
 * that call also correctly routes the CHILD's own local_arena_ to draw
 * from the SAME parent arena, so a child's entire footprint (outer
 * object + its own local_arena_) lives inside its parent's arena tree,
 * reachable in one place.
 *
 * Returning T* rather than RID preserves full type information all the
 * way back to the call site - the caller already knows T (they wrote
 * it as the template argument), and the pointer is guaranteed valid for
 * the entity's own lifetime (arena-allocated, never individually freed
 * except via the explicit unload machinery), so there's no reason to
 * hand back a type-erased RID when a fully-typed pointer is strictly
 * more useful and no less safe.
 *
 * Construction itself happens on the CALLING thread - unchanged from
 * before. Only the REGISTRATION (typed_children_ insertion, module-
 * level RIDList registration) is serialized through the loader's
 * single ordering thread via AddTagEvent, preserving the existing
 * global concurrent creation ORDER guarantee for that bookkeeping.
 *
 * Blocks the calling thread until the loader's ordering thread has
 * actually performed the registration - same blocking contract every
 * other *Event::operator()() in this codebase already has.
 *
 * Entity-only. Root never has typed children -- it has no arena to
 * draw them from and no dispatch surface to register them into.
 * -----------------------------------------------------------------------
 */
    template<typename T, typename... Args>
    T* addTag(Args&&... args)
    {
        static_assert(std::is_base_of<Entity, T>::value,
                      "addTag<T>: T must derive from Entity");
        MemoryArena* saved = s_pending_parent_arena_;
        s_pending_parent_arena_ = &getArena();
        T* child = getArena().allocate<T>(std::forward<Args>(args)...);
        s_pending_parent_arena_ = saved;
        child->getArena().setScopeTag(T::CONTRACT_TAG);   /*
 * <-- new line
 * CONTRACT_TAG -- not getSourceTag(), still empty here (setModuleSource
 * runs later, inside addTagImpl, as part of this same blocking call),
 * and not T::TAG, the concrete class name a typedef never renames.
 * addTagImpl resolves this one key through type_owner_index and the
 * module catalog, both contract-keyed, then writes it to
 * setModuleSource -- so a wrong key costs the dispatch table, the
 * ridMap entry and the source tag together.
 */
        AddTagEvent evt(this, child, ETCS::Buffer(T::CONTRACT_TAG), &Entity::addTagTrampoline<T>);
        evt();
        return child;
    }

    /*
 * -----------------------------------------------------------------------
 * addTypeTag(Buffer) - bare, origin-less, upper-case is-a marker in
 * `tags` (the SAME map addTag(ModuleBundle&, Entity*) populates with
 * real dispatch entries) - inserted with NO bundle and NO child,
 * purely so hasTag(Buffer("Wrapper"))-style family-membership checks
 * (see MirrorBuffer.h's own wrap-chain resolution, DynamicLoader.h)
 * can tell "this concrete leaf type also IS-A Wrapper_/Parser_/etc"
 * apart from an ordinary dispatch tag, without needing a real
 * ModuleBundle to exist at all. Entity::call()'s own null-bundle
 * guard already handles a bare marker safely if something ever DOES
 * try to dispatch through it directly (logs and returns, no crash).
 *
 * Called exclusively by ETCS_MAKE_INSTANCE's own generated
 * constructor (ETCS_API.h), with #Name always a compile-time
 * TitleCase literal - never runtime/attacker-controlled input - but
 * validated the same way regardless (opposite-case mirror of
 * addTag(Buffer)'s own check), to catch a genuine macro-misuse
 * mistake loudly rather than silently miscategorizing something.
 *
 * Direct insertion, NOT routed through TagModifyEvent the way
 * addTag(Buffer) is: this only ever runs during THIS object's own
 * construction (ETCS_MAKE_INSTANCE's ctor runs after Entity's own
 * base construction has already fully completed, same as every
 * other supertype-family ctor), before any other thread could
 * possibly hold a reference to it yet - TagModifyEvent's whole
 * reason to exist (serializing concurrent, post-construction tag
 * mutations) has nothing to serialize against at this specific point.
 * -----------------------------------------------------------------------
 */
    void addTypeTag(const ETCS::Buffer& type_tag)
    {
        const char* s = type_tag.c_str();
        if (!s || !s[0] || s[0] < 'A' || s[0] > 'Z')
            throw std::invalid_argument(
                std::string("addTypeTag: type tag must start with an uppercase letter: ")
                + (s ? s : ""));
        std::lock_guard<std::mutex> lock(m_tagMutex);
        tags.emplace(type_tag, TagEntry{});
    }

    /*
 * -----------------------------------------------------------------------
 * removeTypedChild(rid) - RID alone, deliberately. RIDs are already
 * globally unique (seeded by module+tag+atomic counter at generation
 * time), so there's exactly one bucket that can ever contain a given
 * RID - no need for the caller to separately track and pass which tag
 * it was registered under. This is also why parent_tag_ no longer
 * exists as a stored member: it had no other use once this stopped
 * needing it.
 * -----------------------------------------------------------------------
 */
    void removeTypedChild(RID rid)
    {
        std::lock_guard<std::mutex> lock(m_tagMutex);
        for (auto& [tag, handle] : typed_children_)
        {
            if (handle.invoke_contains(rid))
            {
                handle.invoke_remove(rid);
                return;
            }
        }
    }


    /*
 * getTypedChildren(out) - read-only enumeration of this entity's own
 * addTag<T> children as (tag, RID) pairs, in addTag call order
 * (typed_child_order_). Same walk shape reparentChildrenTo already
 * uses; added so a caller (ShellREPL's own parent/child navigation)
 * can inspect what's actually live without touching typed_children_
 * directly.
 */
    void getTypedChildren(std::vector<std::pair<ETCS::Buffer, RID>>& out) const
    {
        std::lock_guard<std::mutex> lock(m_tagMutex);
        for (const auto& tag : typed_child_order_)
        {
            auto it = typed_children_.find(tag);
            if (it == typed_children_.end()) continue;
            std::vector<RID> rids;
            it->second.invoke_collect_rids(rids);
            for (RID r : rids) out.emplace_back(tag, r);
        }
    }
    /*
 * getTypedChild(tag, rid) - resolves a single live child by exactly
 * the (tag, RID) pair getTypedChildren() above reports it under.
 * nullptr if the tag was never attached, or the RID is no longer live.
 */
    /*
 * getOrderedTypedChildren(out) - the same enumeration, with each tag's own
 * list reporting in ITS order (RIDList::collect_ordered, which sorts by the
 * pointee's operator< when the concrete type declares one).
 *
 * WITHIN a tag, this is a real order. ACROSS tags it is not, and cannot be
 * from here: children of different concrete types live in different lists,
 * and two unrelated leaf types have no comparison between them -- that is
 * what makes operator< on the leaf a safe thing to require in the first
 * place. Tags come out in first-attachment order, as they always have.
 *
 * A caller that needs one order over a MIXED set has to supply the relation
 * that spans them, which means a scalar every member can answer rather than
 * a pairwise operator (Drawable_::Order() is exactly that). This function
 * gives that caller its input already sorted within each group, so the
 * cross-group step is a stable merge over an almost-sorted sequence instead
 * of a full sort over an arbitrary one.
 */
    void getOrderedTypedChildren(std::vector<std::pair<ETCS::Buffer, RID>>& out) const
    {
        std::lock_guard<std::mutex> lock(m_tagMutex);
        for (const auto& tag : typed_child_order_)
        {
            auto it = typed_children_.find(tag);
            if (it == typed_children_.end()) continue;
            std::vector<RID> rids;
            it->second.invoke_collect_rids_ordered(rids);
            for (RID r : rids) out.emplace_back(tag, r);
        }
    }
    /*
 * reorderTypedChild(rid) - mark the ordered view of whichever of this
 * entity's typed-children lists holds `rid` as stale.
 *
 * The receiving end of Orderable_::Reorder(): a child whose ordering key
 * moved tells its PARENT, because the list is the parent's, not the
 * child's. Searches rather than taking a tag, so a caller that knows only
 * its own RID -- which is every caller, since an entity does not carry the
 * key its parent filed it under -- needs nothing it does not have.
 *
 * Silent when no list holds it: a root-level entity, or one already
 * removed. Neither is an error; nothing is holding it in an order.
 */
    void reorderTypedChild(RID rid)
    {
        std::lock_guard<std::mutex> lock(m_tagMutex);
        for (auto& entry : typed_children_)
            if (entry.second.invoke_contains(rid)) { entry.second.invoke_reorder(); return; }
    }
    Entity* getTypedChild(const ETCS::Buffer& tag, RID rid) const
    {
        std::lock_guard<std::mutex> lock(m_tagMutex);
        auto it = typed_children_.find(tag);
        if (it == typed_children_.end()) return nullptr;
        return it->second.invoke_get(rid);
    }
    /*
 * Takes the BARE label ("Listen"), not a prefixed key -- the shared
 * "active_scope_<label>" flag string is ScopeTag's own business now, and
 * the registry has no reason to store the prefix on every entry.
 */
    Scope::Registration registerScope(const std::string& label, const SignalContext& ctx)
    {
        std::lock_guard<std::mutex> lock(m_tagMutex);
        return scope_.registerContext(label, ctx);
    }
    /*
 * (label, index) -- one specific in-flight call. The fine-grained
 * counterpart to removing the shared flag, which interrupts every call
 * carrying that label. See Scope's own comment for why index is a
 * position rather than an identity.
 */
    bool interruptScopeAt(const std::string& label, size_t index)
    {
        std::lock_guard<std::mutex> lock(m_tagMutex);
        return scope_.interruptAt(label, index);
    }

    /*
 * The coarse form -- every live call carrying `label`. Identical in effect
 * to removing the shared "active_scope_<label>" flag, exposed directly so
 * `kill Listen` doesn't have to go through the tag system to say something
 * the tag system only expresses incidentally. Returns how many were
 * signalled, so a caller can distinguish "asked several to stop" from
 * "there was nothing by that name".
 */
    size_t interruptAllOfLabel(const std::string& label)
    {
        std::lock_guard<std::mutex> lock(m_tagMutex);
        return scope_.interruptLabel(label);
    }

    /*
 * Snapshot for display. Same walk interruptScopeAt uses to resolve an
 * index, so what a caller sees and what it can subsequently target are
 * produced identically.
 */
    void collectScopes(std::vector<Scope::View>& out) const
    {
        std::lock_guard<std::mutex> lock(m_tagMutex);
        scope_.collect(out);
    }
    /*
 * Signals every in-flight scope on this entity. Public because
 * drainEntityScopes (DynamicLoader.h) calls it from the CALLER's thread
 * before a destroy is ever enqueued -- see that function's own comment.
 */
    void interruptAllScopes()
    {
        std::lock_guard<std::mutex> lock(m_tagMutex);
        scope_.interruptAll();
    }
    Scope::Removal unregisterScope(uint64_t scope_id)
    {
        std::lock_guard<std::mutex> lock(m_tagMutex);
        return scope_.unregisterContext(scope_id);
    }
    bool anyScopeActive() const
    {
        std::lock_guard<std::mutex> lock(m_tagMutex);
        return !scope_.empty();
    }
    /*
 * -----------------------------------------------------------------------
 * reparentChildrenTo(newParent) - called by MemoryArena's own
 * run_entity_delete callback (registerDtor<T>, MemoryArena.h), for
 * the non-global-scope case, BEFORE this entity itself is evoked.
 * Re-parents every one of this entity's own addTag'd children up a
 * generation (skip THIS entity, land on newParent directly) rather
 * than orphaning them -- this entity's own local arena is never torn
 * down in that case, so the children remain exactly as reachable as
 * they were, just one hop closer to whatever root eventually owns
 * them. newParent is deliberately whatever the CALLER decides (this
 * entity's own getParent()) rather than assumed here, since the
 * global-scope case (no parent at all) never calls this at all --
 * its own children are evoked, not reparented, unconditionally.
 * -----------------------------------------------------------------------
 */
    void reparentChildrenTo(Entity* newParent)
    {
        /*
 * newParent == this would self-deadlock on the second lock below
 * (std::mutex is not recursive) and is meaningless anyway. nullptr
 * never reaches here from the real caller -- run_entity_delete's
 * non-global branch is gated on getParent() != nullptr -- but this
 * is public, so it's checked rather than assumed.
 */
        if (!newParent || newParent == this) return;
        /*
 * BOTH locks, held together for the whole migration. The old body
 * only took this entity's own, which was sufficient only because it
 * never touched newParent's state at all -- it just rewrote
 * child->parent_ and left the child sitting in a typed_children_
 * list belonging to an entity that was about to be destructed. Now
 * that the child genuinely moves between two maps, a single lock
 * can't cover it.
 *
 * Nested acquisition is safe here because this is the ONLY site in
 * this class that ever holds two entity locks at once -- a lock
 * cycle requires two sites that disagree about ordering, and there
 * is no second site to disagree with. addTagTrampoline<T> holds only
 * the PARENT's lock and reaches into the child by direct field
 * access; ~Entity() closes its own lock scope before calling
 * parent_->removeTypedChild(); tagModifyImpl fires EntityUnloadEvent
 * outside the lock; every scope_/tags/flags_ accessor takes exactly
 * one lock and never calls outward. Holding both (rather than a
 * plan-then-apply split across two disjoint critical sections) is
 * also what keeps a child from ever being observable in NEITHER
 * parent's list by a concurrent getTypedChildren()/getTypedChild()
 * on some other thread.
 */
        std::lock_guard<std::mutex> self_lock(m_tagMutex);
        std::lock_guard<std::mutex> dest_lock(newParent->m_tagMutex);
        for (auto& [tag, handle] : typed_children_)
        {
            /*
 * Snapshot first. invoke_remove below erases from the very map
 * a live iterator would be walking, so the RIDs have to be
 * collected before any mutation begins.
 */
            std::vector<ETCS::RID> child_rids;
            handle.invoke_collect_rids(child_rids);
            if (child_rids.empty()) continue;
            /*
 * Pointer into newParent's own map, not an iterator: emplace()
 * below can rehash, which invalidates iterators but NOT
 * references or pointers to elements (std::unordered_map is
 * node-based, and ArenaMap only changes where those nodes are
 * allocated from, not that guarantee).
 */
            RIDListHandle* dest = nullptr;
            auto dit = newParent->typed_children_.find(tag);
            if (dit != newParent->typed_children_.end())
                dest = &dit->second;
            for (ETCS::RID rid : child_rids)
            {
                Entity* child = handle.invoke_get(rid);
                if (!child || child->parent_ != this) continue;
                if (!dest)
                {
                    /*
 * Lazily minted on the first child that actually
 * migrates under this tag -- deliberately inside the
 * inner loop, not above it, so a tag whose entries all
 * fail the parent_ == this check doesn't leave an empty
 * list (and a spurious typed_child_order_ entry) behind
 * on newParent.
 *
 * Cannot copy `handle` across: the RIDList<T*> object it
 * points at was allocated from THIS entity's own
 * local_arena_ (addTagTrampoline<T>, below), and while
 * that arena does survive as coyote time, it is not
 * newParent's to depend on -- newParent can outlive this
 * entity's arena by an arbitrary margin. invoke_make_in
 * mints a fresh, correctly-typed list in newParent's own
 * arena instead, from the factory captured back when T
 * was still known (RIDList.h).
 */
                    if (!handle.make_in)
                    {
                        ETCS_LOG("Entity", "reparentChildrenTo: tag '" << tag
                                 << "' has no make_in factory -- cannot migrate its "
                                 << child_rids.size() << " child(ren) to newParent; "
                                 << "they will be orphaned in this entity's arena.");
                        break;
                    }
                    RIDListHandle fresh =
                        handle.invoke_make_in(*newParent->local_arena_, tag.c_str());
                    dest = &newParent->typed_children_.emplace(tag, fresh).first->second;
                    /*
 * Mirrors addTagTrampoline<T>'s own first-seen-tag
 * branch exactly -- typed_child_order_ is a type-level
 * record appended only when a tag is genuinely new to
 * that entity, and we only reach here when find() missed.
 */
                    newParent->typed_child_order_.push_back(tag);
                }
                /*
 * Insert BEFORE remove: with both locks held nothing can
 * observe either state, but ordering it this way means an
 * exception from invoke_insert leaves the child still owned
 * by its original list rather than by nothing.
 */
                dest->invoke_insert(rid, child);
                handle.invoke_remove(rid);
                child->parent_ = newParent;
                /*
 * parent_rid_ deliberately untouched -- addTagTrampoline<T>
 * sets it to child->getRID(), which is the same value in any
 * parent's list, so ~Entity()'s own
 * parent_->removeTypedChild(parent_rid_) resolves correctly
 * against newParent without any rewrite here.
 */

                /*
 * Passive edge follows the ownership edge, always. THIS is
 * the load-bearing half: child->parent_ going stale is a
 * lookup miss, but ctx_.provider going stale is a pointer
 * into the dying entity's outer shell, which reclaimEntity
 * is about to zero and hand to the next same-type
 * allocation -- see SignalContext::provider's own comment.
 */
                child->ctx_.setProvider(&newParent->ctx_);
            }
        }
    }
#ifdef ETCS_LOADER
    /*
 * -----------------------------------------------------------------------
 * addTag(bundle, child) - loader-exclusive, module dispatch. Populates
 * this entity's OWN dispatch table entry, keyed by source_tag - NOT a
 * separately-passed tag parameter. Every real call site (
 * ModuleBundle::operator()(), attachModule, addTagImpl) already
 * calls setModuleSource() with this exact same tag value immediately
 * before calling this, so requiring it again here was pure redundancy:
 * the entity itself already knows its own contract tag by the time
 * this runs.
 *
 * bundle is taken BY REFERENCE and its ADDRESS stored - the reference
 * must be to the entry living inside the owning Module's (persistent,
 * shared) type_catalog. Passing a stack-local or any other copy here
 * is a bug by construction: TagEntry::bundle is a non-owning pointer
 * now, not an owned value.
 * -----------------------------------------------------------------------
 */
    void addTag(ModuleBundle& bundle, ETCS::Entity* child = nullptr)
    {
        std::lock_guard<std::mutex> lock(m_tagMutex);
    /*
 * Overwriting assignment, NOT emplace -- addTypeTag (below) can
 * already have inserted a BARE marker (TagEntry{}, null bundle)
 * under this exact same key, for any concrete type whose own
 * supertype family name happens to coincide with its own contract
 * tag string (e.g. WindowProvider's GLFWWindow: ETCS_SUPERTYPE_BASE
 * (Window)'s own generated ctor calls addTypeTag(Buffer("Window"))
 * during construction, BEFORE this ever runs; Contract_WindowProvider
 * .h's own "typedef GLFWWindow Window;" means source_tag here is
 * ALSO "Window"). emplace() silently no-ops on a duplicate key,
 * meaning the bare marker -- inserted first, during construction --
 * would permanently win, leaving tags[tag].bundle null forever: a
 * real, reproduced SIGSEGV this session traced to exactly that (a
 * stream dispatch call reading a null bundle pointer with no
 * guard). The real, dispatch-capable bundle must always take
 * precedence over a marker that only ever existed to make
 * hasTag(Buffer("Wrapper"))-style family checks possible before a
 * real bundle existed at all.
 */
    tags[source_tag] = TagEntry{ &bundle, child };
    }
#endif /*
 * ETCS_LOADER
 * -----------------------------------------------------------------------
 * removeTag(Buffer) - removes an entry from EITHER tags or flags_,
 * whichever actually has it. If the tag points to an entity relation
 * (TagEntry::child is set), that child gets REALLY deleted - via the
 * EXISTING EntityUnloadEvent child-target path, which already handles
 * removing it from ITS parent's typed_children_ bookkeeping and
 * running its full dtor chain on its own local_arena_. Refuses to
 * remove a tag that's foundational to this entity's own type identity
 * (myTag() or its contract/source tag) - those aren't ordinary
 * dispatch entries, they're what makes this entity the type it is.
 *
 * Fires TagModifyEvent, same as addTag(flag) - ordered LOCALLY within
 * this entity's own module (see ModuleProxy::on_event's fork), never
 * forwarded to the loader for the tag-list adjustment itself. The
 * child's own deletion (when applicable) DOES still reach the loader,
 * through EntityUnloadEvent's own separate path - only the DECISION
 * to remove the tag stays local here, matching the global-scope
 * alternative for deleting a leaf being an EXPLICIT EntityUnloadEvent
 * targeting a child directly, not a bare `delete` on its pointer.
 *
 * myTagClosure() supplies the ordering bits -- see addTag(Buffer) above.
 * -----------------------------------------------------------------------
 */
    void removeTag(const ETCS::Buffer& tag)
    {
        ETCS::TagModifyEvent{this, tag, true, &Entity::tagModifyImpl, myTagClosure()}();
    }
    /*
 * See addTag's own overload. ScopeTag's destructor must pass the same extra
 * bits its constructor did, or the pair's removal orders differently from
 * its insertion.
 */
    void removeTag(const ETCS::Buffer& tag, const ETCS::TagMask& extra)
    {
        ETCS::TagModifyEvent{this, tag, true, &Entity::tagModifyImpl,
                             myTagClosure() | extra}();
    }
    /*
 * -----------------------------------------------------------------------
 * hasTag - routes on the first character of `tag` rather than exposing a
 * separate hasFlag. Lowercase-leading strings are addTag(Buffer) flags
 * (flags_); anything else is a module dispatch tag (tags). This mirrors
 * the same split addTag itself already enforces, so a given string can
 * only ever live in one of the two maps and there's no ambiguity about
 * which one hasTag should check.
 * -----------------------------------------------------------------------
 */
    bool hasTag(const ETCS::Buffer& tag) const
    {
        std::lock_guard<std::mutex> lock(m_tagMutex);
        const char* s = tag.c_str();
        bool is_flag = s && s[0] >= 'a' && s[0] <= 'z';
        return is_flag ? (flags_.find(tag) != flags_.end())
                        : (tags.find(tag) != tags.end());
    }
    /*
 * safeBundleFor - the SAME null-bundle guard the plain, non-stream
 * call() overload already has inline (its own "!!! tags[...].bundle
 * is NULL" check), factored out here so every OTHER call() overload
 * that dispatches through tags[tag].bundle shares one implementation
 * instead of each repeating (or, as it turned out, NOT repeating)
 * the identical check by hand.
 *
 * Returns nullptr, having already logged why, if tag_type isn't in
 * tags at all, or IS present but its own .bundle is null -- the
 * second case is exactly what a bare addTypeTag(Buffer) marker
 * looks like before a real ModuleBundle is ever attached under the
 * same key (see that method's own comment) -- and what a real,
 * reproduced SIGSEGV this session traced to: both untyped stream
 * call() overloads below (and the templated one) were calling
 * (*tags[tag_type].bundle)(...) directly, with NO check at all,
 * unlike this plain overload. Dispatching through a null bundle
 * pointer crashes INSIDE ModuleBundle::operator()() itself, where
 * `this` is already null/garbage by the time anything in there
 * could check for it -- the guard has to live at the CALL site,
 * before ever dereferencing it, exactly where this is used.
 */

    ModuleBundle* safeBundleFor(const ETCS::Buffer& tag_type, const char* role)
    {
        auto it = tags.find(tag_type);
        if (it == tags.end())
        {
            ETCS_LOG("Entity::call", "!!! tags.find(" << tag_type << ") missed for "
                     << role << " -- cannot dispatch.");
            return nullptr;
        }
        if (!it->second.bundle)
        {
            ETCS_LOG("Entity::call", "!!! tags[" << tag_type << "].bundle is NULL for "
                     << role << " -- refusing to dispatch (would have crashed).");
            return nullptr;
        }
        return it->second.bundle;
    }
    void getTags(std::vector<ETCS::Buffer>& result) const
    {
        std::lock_guard<std::mutex> lock(m_tagMutex);
        for (auto const& [key, _] : tags) result.push_back(key);
    }
    void getFlags(std::vector<ETCS::Buffer>& result) const
    {
        std::lock_guard<std::mutex> lock(m_tagMutex);
        for (auto const& [key, _] : flags_) result.push_back(key);
    }
    void registerInterfacePointer(const ETCS::Buffer& family, void* ptr)
    {
        std::lock_guard<std::mutex> lock(m_tagMutex);
        interface_pointers_[family] = ptr;
    }
    void* getInterfacePointer(const ETCS::Buffer& family) const
    {
        std::lock_guard<std::mutex> lock(m_tagMutex);
        auto it = interface_pointers_.find(family);
        return it != interface_pointers_.end() ? it->second : nullptr;
    }
    /*
 * The families this entity actually fulfills. Read off
 * interface_pointers_ rather than the tags map on purpose: this map is
 * written by exactly one thing, ETCS_MAKE_INSTANCE's ctor, once per
 * supertype base -- so it is precisely the set of ontology families the
 * type composed, with none of the state tags ("active") or concrete tag
 * names that share the tags map. etcs_supertype_fanout below is its one
 * caller.
 */
    void getInterfaceFamilies(std::vector<ETCS::Buffer>& result) const
    {
        std::lock_guard<std::mutex> lock(m_tagMutex);
        for (auto const& [family, _] : interface_pointers_) result.push_back(family);
    }
    void getAllActions(std::vector<ETCS::Buffer>& all_actions)
    {
        for (auto const& [tag_name, entry] : tags)
        {
            for (auto const& [action_name, work_bundle] : entry.bundle->actions)
            {
                ETCS::Buffer conjugate;
                conjugate.write(tag_name.c_str());
                conjugate.write(".");
                conjugate.write(action_name.c_str());
                all_actions.push_back(conjugate);
            }
        }
    }
    void call(const ETCS::Buffer& conjugateAction, ETCS::Buffer& data, SignalContext ctx = {})
    {
#ifdef ETCS_LOG_FUNCTION_EVOCATION_PATH
        ETCS_LOG("Entity::call", "ENTER conjugateAction=" << conjugateAction.toString()
                 << " data.written=" << data.written << " this=" << (void*)this);
#endif
        auto [tag_type, action] = parseConjugateActionKey(conjugateAction);
#ifdef ETCS_LOG_FUNCTION_EVOCATION_PATH
        ETCS_LOG("Entity::call", "parsed tag_type=" << tag_type << " action=" << action);
#endif
        if (!hasTag(tag_type))
        {
            ETCS_LOG("Entity::call", "Could not find tag " << tag_type << " within tag list, failed action: " << action);
            return;
        }
#ifdef ETCS_LOG_FUNCTION_EVOCATION_PATH
        ETCS_LOG("Entity::call", "hasTag(" << tag_type << ") = true");
#endif
        auto self_it = tags.find(tag_type);
        if (self_it == tags.end())
        {
            ETCS_LOG("Entity::call", "!!! tags.find(" << tag_type
                     << ") missed despite hasTag() reporting true -- inconsistent tags map.");
            return;
        }
#ifdef ETCS_LOG_FUNCTION_EVOCATION_PATH
        ETCS_LOG("Entity::call", "tags.find(" << tag_type << ") succeeded, bundle="
                 << (void*)self_it->second.bundle);
#endif
        if (!self_it->second.bundle)
        {
            ETCS_LOG("Entity::call", "!!! tags[" << tag_type << "].bundle is NULL -- would have crashed here.");
            return;
        }
        bool is_stream = self_it->second.bundle->isActionStream(action);
#ifdef ETCS_LOG_FUNCTION_EVOCATION_PATH
        ETCS_LOG("Entity::call", tag_type << "." << action << " isActionStream=" << is_stream);
#endif
        if (is_stream)
        {
            ETCS_LOG("Entity::call", "Attempted to call stream tag " << tag_type << " within default call buffer, failed action: "
                     << action <<  " - Malformed function call? This should not be called like this, call via MirrorBuffer "
                     << "(or direct mapping between composite work functions).");
            return;
        }
        if (data.written >= MAX_TAG_BUFFER_SIZE)
        {
            ETCS_LOG("Entity::call", "!!! WARNING !!! Tag buffer overflow, action: " << conjugateAction << " received too much data: " << data);
        }
#ifdef ETCS_LOG_FUNCTION_EVOCATION_PATH
        ETCS_LOG("Entity::call", "Received ctx with tag: " << ctx.tag << " and parent: " << ctx.parent
                 << " isNull=" << ctx.isNull());
#endif
        SignalContext forward = ctx;
        if (forward.isNull())
        {
#ifdef ETCS_LOG_FUNCTION_EVOCATION_PATH
            ETCS_LOG("Entity::call", "null SignalContext on " << myTag()
                     << " (source tag " << getSourceTag().toString() << "), proxying via module parent...");
            ETCS_LOG("Entity::call", "about to call mySelf() (looks up tags[" << getSourceTag().toString() << "])...");
#endif
            const ModuleBundle& self_bundle = mySelf();
#ifdef ETCS_LOG_FUNCTION_EVOCATION_PATH
            ETCS_LOG("Entity::call", "mySelf() returned, bundle tag=" << self_bundle.tag);
#endif
            forward = self_bundle.ctx;
            forward.parent = self_bundle.tag;
            forward.tag = getSourceTag();
#ifdef ETCS_LOG_FUNCTION_EVOCATION_PATH
            ETCS_LOG("Entity::call", "forward built: parent=" << forward.parent << " tag=" << forward.tag);
#endif
        }
        else
        {
#ifdef ETCS_LOG_FUNCTION_EVOCATION_PATH
            ETCS_LOG("Entity::call", "non-null SignalContext on " << myTag()
                     << " (source tag " << getSourceTag().toString() << "), classic proxying...");
            ETCS_LOG("Entity::call", "about to call mySelf() (looks up tags[" << getSourceTag().toString() << "])...");
#endif
            const ModuleBundle& self_bundle = mySelf();
#ifdef ETCS_LOG_FUNCTION_EVOCATION_PATH
            ETCS_LOG("Entity::call", "mySelf() returned, bundle tag=" << self_bundle.tag);
#endif
            forward.setParent(&self_bundle.ctx);
            forward.parent = self_bundle.tag;
            forward.tag = getSourceTag();
#ifdef ETCS_LOG_FUNCTION_EVOCATION_PATH
            ETCS_LOG("Entity::call", "forward built: parent=" << forward.parent << " tag=" << forward.tag);
#endif
        }
#ifdef ETCS_LOG_FUNCTION_EVOCATION_PATH
        ETCS_LOG("Entity::call", "Entity call forwarded parent:tag.action - " << forward.parent << ":" << forward.tag << "." << action);
        ETCS_LOG("Entity::call", "about to invoke (*bundle)(this=" << (void*)this
                 << ", action=" << action << ", data.written=" << data.written << ", forward)...");
#endif
        (*self_it->second.bundle)(this, action, data, forward);
#ifdef ETCS_LOG_FUNCTION_EVOCATION_PATH
        ETCS_LOG("Entity::call", "EXIT -- (*bundle)(...) returned normally for " << tag_type << "." << action);
#endif
    }
    /*
 * these type of calls cannot return to the data value, that return value is lost in this stack frame
 * acceptable as const char second param indicates return value is not needed.
 */
    void call(const char* conjugateAction, const char* data = "", SignalContext ctx = {})
    {
        auto [tag_type, action] = parseConjugateActionKey(ETCS::Buffer(conjugateAction));
        if (!hasTag(tag_type))
        {
            ETCS_LOG("Could not find tag " << tag_type << " within tag list, failed action: " << action);
            return;
        }
 
        if(tags[tag_type].bundle->isActionStream(action))
        {
            ETCS_LOG("Attempted to call stream tag " << tag_type << " within default call buffer, failed action: "
                << action << "\n" << "Malformed function call? This should not be called like this, call via MirrorBuffer.");
            return;
        }
        ETCS::Buffer dataBuff(data);
        call(ETCS::Buffer(conjugateAction), dataBuff, ctx);
    }
 
    void call(const char* conjugateAction, ETCS::Buffer& data, SignalContext ctx = {})
    {
        call(ETCS::Buffer(conjugateAction), data, ctx);
    }
    // 2. Action is a Buffer, but Data is a string (aka data return value is dropped in this stack frame)
    void call(const ETCS::Buffer& conjugateAction, const char* data, SignalContext ctx = {})
    {
        ETCS::Buffer dataBuff(data);
        call(conjugateAction, dataBuff, ctx);
    }
 
 
#ifdef ETCS_LOADER
    /*
 * -----------------------------------------------------------------------
 * Untyped stream call - ETCS_LOADER only.
 *
 * The loader's exclusive interface. It is the root of the causal graph -
 * it owns the global sequence, assigns highest-priority events, and
 * dictates runtime causal time. It does not have tag type information at
 * its call sites (the shell executor works in runtime string space), so
 * it always uses StrategyPipe with a SharedPage staging buffer.
 *
 * Modules compiled without ETCS_LOADER cannot reach this overload -
 * the preprocessor removes it entirely, making this a build-time
 * enforcement rather than a runtime check.
 * -----------------------------------------------------------------------
 */
    void call(const ETCS::Buffer& conjugateAction,
              const ETCS::Buffer& conjugateActionRcv,
              const ETCS::Buffer& config,
              SignalContext        ctx = {})
    {
        call(this, conjugateAction, conjugateActionRcv, config, ctx);
    }
    /*
 * Producer action on producer_entity, consumer action on this. Loader
 * always uses Pipe -- it crosses the DSO boundary with no tag type
 * information to justify a ring.
 */
    void call(ETCS::Entity*        producer_entity,
              const ETCS::Buffer&  conjugateAction,
              const ETCS::Buffer&  conjugateActionRcv,
              const ETCS::Buffer&  config,
              SignalContext        ctx = {})
    {
        if (!producer_entity) return;
        auto [tag_type, action]     = parseConjugateActionKey(conjugateAction);
        auto [tag_type_r, action_r] = parseConjugateActionKey(conjugateActionRcv);
        if (!producer_entity->hasTag(tag_type) || !hasTag(tag_type_r))
        {
            ETCS_LOG("[CALL]", "Stream call failed -- producer RID:"
                     << producer_entity->getRID() << " has " << tag_type << ": "
                     << producer_entity->hasTag(tag_type)
                     << ", consumer RID:" << getRID() << " has " << tag_type_r << ": "
                     << hasTag(tag_type_r));
            return;
        }
        ETCS::SharedPage* page = ETCS::SharedPage::allocate(getArena(), getRID());
        ETCS::MirrorBuffer producer(config, ctx);
        ETCS::MirrorBuffer consumer(config, ctx);
        ETCS::MirrorBuffer::makePair<ETCS::StrategyPipe, ETCS::SharedPage>(
            producer, consumer, page,
            producer_entity->getRID(), getRID(), producer_entity);
        /*
 * No compile-time types, so no tag mask -- TagModifyEvent's fail-shut
 * substitution gives these scopes all(). The MODULE mask is still
 * resolvable, and this overload is where it matters: safeBundleFor
 * below finds each bundle wherever it lives, so the two halves may well
 * be in different modules.
 */
        const ETCS::TagMask pair_mod =
            ETCS::resolvePairModuleMask(tag_type, tag_type_r);
        producer.setPairMasks(ETCS::TagMask{}, pair_mod);
        consumer.setPairMasks(ETCS::TagMask{}, pair_mod);
        ETCS::MBuffer transportConsumer;
        ETCS::MBuffer transportProducer;
        consumer.packConsumer(transportConsumer);
        producer.packProducer(transportProducer);
        /*
 * The count is raised HERE, before the enqueue, and not only inside the
 * produce trampoline -- because the window this closes starts before the
 * trampoline exists. This lambda captures producer_entity raw and may not
 * be picked up by a worker for a while; the consumer can finish, the pair
 * can tear down, the script can Delete that entity, and only then does a
 * worker start this and call safeBundleFor on freed tags. Reproduced
 * exactly that way, in ETCS::Entity::safeBundleFor, off closure_probe.
 *
 * Two nested raises, one drop each: this one covers DISPATCH (enqueue ->
 * trampoline returns), the trampoline's own covers the BODY. The body's
 * is raised before this one is released, so the count never dips to zero
 * in between and the pair's frame sees one continuous "busy" from enqueue
 * to the last line of the produce body.
 */
        ETCS::SharedPage* dispatch_token = producer.producerEnter();
        producer_entity->getThreadPool().enqueue(
            ETCS::Priority::High, ctx,
            [producer_entity, tag_type, action, transportProducer, ctx, dispatch_token]() mutable
        {
            ETCS::ProducerLiveGuard _dispatch_live(dispatch_token);
            try
            {
                if (ModuleBundle* b = producer_entity->safeBundleFor(tag_type, "producer"))
                    (*b)(producer_entity, action, transportProducer, ctx);
            }
            catch (const std::exception& e)
            {
                ETCS_LOG("[CALL]", "Producer failed: " << e.what());
            }
        });
        /*
 * "This call returned" used to mean only "the CONSUMER half finished".
 * The produce body kept running on a pool worker afterwards, still
 * holding `self`, and nothing anywhere could tell. For a script edge that
 * is a live crash: the closure drain (run_script) joins the detached
 * script the moment the consumer returns, the next line Deletes the
 * entity, and the produce body is still calling into it -- reproduced as
 * a SIGSEGV inside ProduceFrames on scripts/closure_probe.etcs.
 *
 * NOT the enqueue's future, which is the trap this first fell into: the
 * task enqueued below runs the module's produce TRAMPOLINE, and that
 * trampoline enqueues the actual body onto the MODULE's own pool and
 * returns. Its future is ready almost immediately and says nothing about
 * the body. The flag on the shared page is what the body itself raises
 * and clears (SharedPage::producers_live).
 *
 * Order is load-bearing. The wait comes AFTER the consumer returns (which
 * is what stops draining the pipe, so a blocked producer can make
 * progress) and BEFORE teardownPair (which frees the very page the
 * producer writes through). A producer blocked in writeRaw is woken by
 * the signal, not by the teardown -- every MirrorBuffer wait tests
 * ctx.isInterrupted()/isTerminated() -- and the closure raise that ends a
 * script edge is exactly that signal.
 *
 * Bounded, and a timeout degrades to the OLD behavior rather than to a
 * hang: tear down anyway and say so, naming the action. A produce body
 * that ignores its interrupt is a bug in that body, and this is where it
 * becomes visible instead of becoming a segfault three lines later in
 * someone's script.
 */
        auto wait_for_producer = [&]()
        {
            using namespace std::chrono;
            const auto deadline = steady_clock::now() + seconds(2);
            while (producer.producerBusy())
            {
                if (steady_clock::now() > deadline)
                {
                    ETCS_LOG("[CALL]", "producer " << tag_type << "." << action
                             << " is still running 2s after its consumer returned -- tearing "
                                "the pair down with the body live. It is not observing its "
                                "interrupt.");
                    break;
                }
                std::this_thread::sleep_for(milliseconds(1));
            }
        };
        try
        {
            if (ModuleBundle* b = safeBundleFor(tag_type_r, "consumer"))
                (*b)(this, action_r, transportConsumer, ctx);
        }
        catch (const std::exception& e)
        {
            ETCS_LOG("[CALL]", "Consumer failed: " << e.what());
            wait_for_producer();
            ETCS::MirrorBuffer::teardownPair<ETCS::StrategyPipe, ETCS::SharedPage>(
                producer, consumer, page);
            throw;
        }
        wait_for_producer();
        ETCS::MirrorBuffer::teardownPair<ETCS::StrategyPipe, ETCS::SharedPage>(
            producer, consumer, page);
    }
#endif /*
 * ETCS_LOADER
 * -----------------------------------------------------------------------
 * Typed stream call - available to all translation units.
 *
 * ProducerTag and ConsumerTag are the CRTP leaf tag types. Transport is
 * resolved at compile time via StrategyFor<ProducerTag, ConsumerTag>:
 *
 *   Same tag, not Remote   --> StrategyLMAX
 *     Ring carries Buffer* into this call frame's stack. No payload copy.
 *     Lifetime is guaranteed by this function blocking on consumer
 *     completion - the frame does not unwind until the consumer returns.
 *     makePair takes the LMAXSequentialSharedPage* allocated below.
 *
 *   CalleeTag : Remote     --> StrategySocket
 *     SharedPage staging buffer + connected socket fds.
 *     makePair takes the SharedPage* allocated below.
 *
 *   Otherwise              --> StrategyPipe
 *     SharedPage staging buffer + anonymous pipe fd pair.
 *     makePair takes the SharedPage* allocated below.
 *
 * getRID() is used for both writer and reader so the entity's runtime
 * identity transfers across the transport boundary on both sides.
 *
 * Usage (inside a module, no ETCS_LOADER required):
 *   entity->call<LocalDatabaseTag, LocalDatabaseTag>(
 *       "LocalDatabase.QueryProduce",
 *       "LocalDatabase.RowConsume",
 *       configBuf, ctx);
 * -----------------------------------------------------------------------
 */
    template<typename ProducerTag, typename ConsumerTag>
    void call(const ETCS::Buffer& conjugateAction,
              const ETCS::Buffer& conjugateActionRcv,
              const ETCS::Buffer& config,
              SignalContext        ctx = {})
    {
        call<ProducerTag, ConsumerTag>(this, conjugateAction, conjugateActionRcv, config, ctx);
    }
    /*
 * Producer action on producer_entity, consumer action on this. Each tag is
 * checked on the entity that holds it, rather than both on one.
 *
 * The consumer owns the frame (DEFINE_STREAM_FUNC_CONSUME runs inline,
 * PRODUCE enqueues), so the page comes from this arena and teardownPair
 * runs here, after the consumer returns.
 */
    template<typename ProducerTag, typename ConsumerTag>
    void call(ETCS::Entity*        producer_entity,
              const ETCS::Buffer&  conjugateAction,
              const ETCS::Buffer&  conjugateActionRcv,
              const ETCS::Buffer&  config,
              SignalContext        ctx = {})
    {
        using Strategy = typename ETCS::StrategyFor<ProducerTag, ConsumerTag>::type;
        using Page     = typename ETCS::PageFor<Strategy>::type;
        if (!producer_entity) return;
        auto [tag_type, action]     = parseConjugateActionKey(conjugateAction);
        auto [tag_type_r, action_r] = parseConjugateActionKey(conjugateActionRcv);
        if (!producer_entity->hasTag(tag_type) || !hasTag(tag_type_r))
        {
            ETCS_LOG("[CALL] Stream call failed -- producer RID:"
                     << producer_entity->getRID() << " has " << tag_type << ": "
                     << producer_entity->hasTag(tag_type)
                     << ", consumer RID:" << getRID() << " has " << tag_type_r << ": "
                     << hasTag(tag_type_r));
            return;
        }
        Page* page = allocatePage<Strategy, Page>(getRID());
        ETCS::MirrorBuffer producer(config, ctx);
        ETCS::MirrorBuffer consumer(config, ctx);
        ETCS::MirrorBuffer::makePair<Strategy, Page>(
            producer, consumer, page,
            producer_entity->getRID(), getRID(), producer_entity);
        /*
 * Both halves right after makePair: the only site where ProducerTag and
 * ConsumerTag are both known, and makePair's three specializations
 * would each need two more template parameters to carry one value.
 *
 * Tag scope from the types, module scope from the tag strings -- the
 * strings safeBundleFor resolves with, so the module answer describes
 * the bundles actually connected, not where the TYPES were declared.
 */
        const ETCS::TagMask pair_tag = ETCS::streamPairMask<ProducerTag, ConsumerTag>();
        const ETCS::TagMask pair_mod = ETCS::resolvePairModuleMask(tag_type, tag_type_r);
        producer.setPairMasks(pair_tag, pair_mod);
        consumer.setPairMasks(pair_tag, pair_mod);
        ETCS::MBuffer transportProducer;
        ETCS::MBuffer transportConsumer;
        consumer.packConsumer(transportConsumer);
        producer.packProducer(transportProducer);
        try
        {
            if (ModuleBundle* b = producer_entity->safeBundleFor(tag_type, "producer"))
                (*b)(producer_entity, action, transportProducer, ctx);
        }
        catch (const std::exception& e)
        {
            ETCS_LOG("[CALL] Producer failed: " << e.what());
        }
        try
        {
            if (ModuleBundle* b = safeBundleFor(tag_type_r, "consumer"))
                (*b)(this, action_r, transportConsumer, ctx);
        }
        catch (const std::exception& e)
        {
            ETCS_LOG("[CALL] Consumer failed: " << e.what());
            ETCS::MirrorBuffer::teardownPair<Strategy, Page>(producer, consumer, page);
            throw;
        }
        ETCS::MirrorBuffer::teardownPair<Strategy, Page>(producer, consumer, page);
    }
 
    static std::pair<ETCS::Buffer, ETCS::Buffer> parseConjugateActionKey(const ETCS::Buffer& key)
    {
        const char* raw = key.c_str();
        const char* dot = std::strchr(raw, '.');
        if (!dot || dot == raw || dot == raw + key.written)
            throw std::runtime_error(std::string("Invalid component key format. Expected 'tag_type.action'. Received: ") + raw);
        ETCS::Buffer tag_type, action;
        size_t tag_len = static_cast<size_t>(dot - raw);
        std::memcpy(tag_type.buf, raw, tag_len);
        tag_type.buf[tag_len] = '\0';
        tag_type.written = tag_len;
        const char* action_start = dot + 1;
        size_t action_len = key.written - tag_len - 1;
        std::memcpy(action.buf, action_start, action_len);
        action.buf[action_len] = '\0';
        action.written = action_len;
        return {tag_type, action};
    }
 
    /*
 * Entity objects themselves must come out of the global arena, not
 * local_arena_ - local_arena_ doesn't exist yet at the point operator
 * new runs (it's allocated inside the Entity ctor body's init list,
 * which hasn't executed). Chicken-and-egg, resolved by always routing
 * Entity allocation itself through the global singleton.
 *
 * NOTE: this operator new/delete pair is effectively vestigial for
 * the ACTUAL entity-creation paths in this codebase (_make_##Name via
 * ETCS_TAG_DECLARE, and now addTag<T>, both go through
 * MemoryArena::allocate<T>() directly, never an ordinary new-expression
 * on a concrete leaf type) - they exist for standard-conformance and
 * the unlikely case of direct construction. operator delete's real
 * logic below is what actually matters, and applies REGARDLESS of
 * which allocation path was used, since it triggers on ordinary
 * `delete` of ANY Entity-derived pointer.
 */
    void* operator new(size_t size)
    {
        return getGlobalArena().allocateRaw(static_cast<long long>(size));
    }
    void* operator new(size_t size, void* ptr)
    {
        (void)size;
        return ptr;
    }
    void operator delete(void* ptr, void* place)    { (void)ptr; (void)place; }
    /*
 * -----------------------------------------------------------------------
 * operator delete - child case only now. Root-case module-transfer-or-
 * vacate logic moved into ~Entity()'s own body (see its comment for why:
 * operator delete runs strictly AFTER the complete object, including
 * module_, has already been destroyed - too late to read module_'s
 * fields at all). What's left here is exactly the child-target
 * bookkeeping this always needed regardless: the destructor has already
 * run (unconditionally, by C++ semantics, by the time operator delete
 * is ever reached) - so a CHILD's own outer record on parent_'s arena
 * must be unlinked now via forget() (NOT evokeDestructor - the
 * destructor must never run twice), or the parent's eventual full arena
 * sweep would call it again: undefined behavior, not merely
 * theoretical (GLFWWindow::~GLFWWindow(), for one, is not idempotent).
 *
 * Deliberately does NOT touch local_arena_ here - that is the "coyote
 * time" feature: this entity's OWN grandchildren (anything it
 * addTag<T>'d) remain fully live and addressable, parented to THIS
 * entity's local_arena_, which itself remains reachable through
 * parent_'s arena until something explicitly reaches in (a targeted
 * EntityUnloadEvent on this entity or one of its ancestors, or the
 * eventual cascade when parent_'s own root ancestor is unloaded). A
 * no-op here would leave a double-destruction bug; reaching into
 * local_arena_ here would forfeit that deliberately-preserved window.
 *
 * Root is no longer an Entity at all, so this operator delete is
 * never invoked for one -- there is nothing left to special-case for
 * that.
 * -----------------------------------------------------------------------
 */
    void operator delete(void* ptr)
    {
        (void)ptr;
    }
 
    ID_SIZE_TYPE getID() const                      { return 67; } // will be the sum hash of the types
 
    virtual bool myTagInto(ETCS::Buffer& buffer)
    {
        const char* tag_str = myTag();
        size_t tag_len = std::strlen(tag_str);
        size_t required_space = tag_len + 1;
 
        if (buffer.written + required_space > buffer.bufsize)
        {
            ETCS_LOG("Warning: Failed to copy tag. Need " << required_space
                << " bytes, but only " << buffer.bufsize - buffer.written << " available.");
            return false;
        }
 
        std::memcpy(buffer.buf + buffer.written, tag_str, tag_len);
        buffer.buf[buffer.written + tag_len] = '\0';
        buffer.written += tag_len;
        return true;
    }
 
    virtual const char* myTag()     = 0;
    virtual void* getTrueType()     = 0;
    virtual uint64_t getRID() const = 0;
 
    /*
 * NOT pure, unlike its three siblings above. A type carrying
 * WIRE_TYPE_IDENTITY overrides this with its own static TAG_MASK
 * (ETCS_API.h), but an Entity-derived type WITHOUT that macro still
 * has to compile, and an empty mask -- "independent of everything" --
 * is the correct answer for something that has no contract identity
 * for the reorder buffer to order against in the first place.
 *
 * Declared LAST among the virtuals deliberately: it APPENDS a vtable
 * slot rather than shifting any existing one, so a binary built
 * against the older header mis-dispatches nothing -- it simply never
 * calls this. (That does NOT make a mixed build safe: EventStream's
 * own layout changed in the same epoch, which is a separate and
 * fatal skew. Everything still has to be rebuilt together.)
 *
 * Returns by reference into a function-local static so there is
 * exactly one empty mask in the program rather than one per call.
 */
    virtual const ETCS::TagMask& myTagMask() const
    {
        static const ETCS::TagMask none{};
        return none;
    }
    /*
 * myTagClosure - this type's own bit PLUS every type in its module it holds
 * a reference to (WIRE_TYPE_IDENTITY's TAG_CLOSURE). What addTag/removeTag
 * put on a TagModifyEvent, so what ModuleProxy's buffer admits against.
 *
 * Empty here, like myTagMask: a type with no tag block has nothing to close
 * over, and TagModifyEvent substitutes all() -- keeping the fail-shut
 * decision in the one place that already makes it.
 *
 * noteAcquires records an acquisition, and is called only from
 * addTagTrampoline<T>, the single site where either direction of a typed
 * parent/child reference is created. A no-op here for the same reason.
 *
 * Both appended after myTagMask -- see its comment on vtable slot order.
 */
    virtual ETCS::TagMask myTagClosure() const
    {
        return ETCS::TagMask{};
    }
    virtual void noteAcquires(const ETCS::TagMask&) {}
 
    ETCS::Buffer myTagBuffer()
    {
        return ETCS::Buffer(myTag());
    }
 
    /*
 * mySelf() looks up by getSourceTag() -- the CONTRACT tag (e.g.
 * "LocalDatabase") tags is actually keyed by (see addTag(ModuleBundle&,
 * Entity*)'s own comment: "tags.emplace(source_tag, ...)"), NOT
 * myTagBuffer()/myTag() (the CONCRETE type's own identity, e.g.
 * "SqliteLocalDatabase"). Those two are only the same string for a
 * type that implements its own eponymous contract; for any type
 * implementing a DIFFERENT contract than its own name, myTagBuffer()
 * is simply the wrong key -- operator[] on it silently default-
 * constructs a fresh, empty TagEntry (bundle == nullptr) that this
 * then dereferences, rather than finding the real one. .find() here,
 * with an explicit throw on a miss, fails loudly instead -- ArenaMap's
 * own definition isn't visible in any file this codebase currently
 * has on hand, and .find() (unlike .at(), which has no precedent
 * anywhere in this file) is already confirmed working elsewhere in
 * this same class.
 */
    const ModuleBundle& mySelf()
    {
        auto it = tags.find(getSourceTag());
        if (it == tags.end())
            throw std::runtime_error("mySelf(): no tags entry for source tag '"
                + getSourceTag().toString() + "' on entity of type " + myTag());
        return *(it->second.bundle);
    }
 
private:
    /*
 * -----------------------------------------------------------------------
 * tagModifyImpl - the actual work of addTag(flag)/removeTag(tag),
 * performed here rather than inline in either so it can be captured
 * as a plain function pointer (TagModifyEvent::Impl) with no knowledge
 * of Entity's private members needed on the caller side (ModuleProxy::
 * on_event, LoaderStream::on_event) - same reasoning addTagTrampoline<T>
 * already uses for crossing that same boundary.
 *
 * is_remove == false: adds to flags_ (informational tags only -
 * addTag(flag) never touches tags at all).
 *
 * is_remove == true: checks scope_ FIRST -- see the block below for
 * why that has to run before anything else. Otherwise refuses
 * outright if key matches this entity's own type identity (myTag()
 * or its contract/source tag) - those are foundational, not ordinary
 * dispatch entries. Otherwise checks tags first (module dispatch
 * entries, TitleCase by convention); if found and its TagEntry::child
 * is set (an entity relation, not a bare dispatch entry), that child
 * gets REALLY deleted via the existing EntityUnloadEvent child-target
 * path - fired OUTSIDE the mutex lock, since EntityUnloadEvent's own
 * blocking spin must never happen while holding m_tagMutex. If key
 * isn't in tags at all, falls through to flags_ instead (an
 * informational tag being removed).
 * -----------------------------------------------------------------------
 */
    static void tagModifyImpl(Entity* target, const ETCS::Buffer& key, bool is_remove)
    {
        if (!is_remove)
        {
            std::lock_guard<std::mutex> lock(target->m_tagMutex);
            target->flags_.emplace(key, true);
            return;
        }
 
        /*
 * Scope check FIRST, highest priority -- before even the
 * foundational-name check below (a scope key can never collide
 * with myTag()/getSourceTag() anyway, but this ordering makes
 * the precedence explicit rather than incidental). A hit means
 * `key` names one specific, currently-in-flight stream call's
 * own "active_scope_<label>_<guard-address>" flag -- see
 * ScopeTag's own comment (Bundles.h) for why that's the exact
 * string both scope_'s own registry and this flag are keyed by.
 * Interrupts THAT ONE call's own SignalContext and returns
 * immediately -- deliberately does NOT touch flags_ or scope_
 * itself here; the actual removal from BOTH happens only in
 * ScopeTag's own destructor, whenever the interrupted work
 * function actually notices and returns. This is the literal
 * shape of "removing it from the list via it tearing itself
 * down": this call only ever REQUESTS; the scope is what
 * actually leaves.
 */
        {
            const std::string key_full = key.toString();
            const std::string prefix   = ETCS::ScopeTag::kPrefix;
            if (key_full.size() > prefix.size()
                && key_full.compare(0, prefix.size(), prefix) == 0)
            {
                const std::string label = key_full.substr(prefix.size());
                std::lock_guard<std::mutex> lock(target->m_tagMutex);
                /*
 * COARSE by design: this flag is shared by every live call
 * carrying `label`, so removing it asks all of them to stop.
 * Targeting one goes through Entity::interruptScopeAt(label,
 * index) instead -- a separate interface, because it takes a
 * second input the tag namespace has no way to carry.
 */
                if (target->scope_.interruptLabel(label) > 0)
                    return;
                /*
 * Zero live scopes: fall through. This is the ordinary path
 * when ~ScopeTag removes the flag after unregistering the LAST
 * entry -- there is genuinely nothing left to signal, and the
 * flag itself should now be erased from flags_ below.
 */
            }
        }
 
        std::string key_str = key.toString();
        if (key_str == target->myTag() || key_str == target->getSourceTag().toString())
        {
            ETCS_LOG("Entity", "removeTag: '" << key_str
                     << "' is foundational to this entity's type -- refusing to remove.");
            return;
        }
 
        Entity* child_to_delete = nullptr;
        {
            std::lock_guard<std::mutex> lock(target->m_tagMutex);
            auto it = target->tags.find(key);
            if (it != target->tags.end())
            {
                child_to_delete = it->second.child;
                target->tags.erase(it);
            }
            else
            {
                target->flags_.erase(key);
            }
        }
 
        if (child_to_delete)
            ETCS::EntityUnloadEvent{child_to_delete}();
    }
 
    /*
 * -----------------------------------------------------------------------
 * addTagTrampoline<T>() - the actual work of addTag<T>()'s REGISTRATION
 * half, performed here rather than inline in addTag<T> so it can be
 * captured as a plain function pointer (AddTagEvent::Trampoline) with T
 * fully erased from the event's own type, and invoked later on the
 * loader's ordering thread - see AddTagEvent in EventNode.h and
 * addTagImpl in DynamicLoader.h. UNCHANGED by the arena-allocation fix
 * above - child is already fully constructed (now correctly, from
 * parent's arena) by the time this runs; this function only ever
 * handles bookkeeping, never construction.
 *
 * Deliberately never casts `child` to T* anywhere. Many ontology leaf
 * types inherit Entity VIRTUALLY (Ephemeral_, ConnectionState_,
 * HtmlPage_, etc.), which makes base-to-derived conversions either
 * illegal (static_cast) or a genuine runtime operation (dynamic_cast) -
 * and neither is actually necessary, since every operation below can
 * be expressed either through Entity's own virtual interface
 * (getRID()), direct access to Entity's own private members (parent_
 * etc - legal since this IS a member of Entity), or through
 * RIDListHandle's type-erased invoke_insert(), which takes Entity* and
 * was captured back when T WAS known (inside RIDList<T>::handle(),
 * RIDList.h). T is needed in this function only to instantiate a
 * fresh RIDList<T*> the first time a given tag is seen - once that
 * handle exists (fresh or already-present), nothing below touches T
 * again.
 *
 * Because every addTag<T> call across the whole process is now
 * serialized through the loader's single ordering thread, this body
 * runs with an implicit mutual-exclusion guarantee against every OTHER
 * addTag<T> call, on any type, from any module - it does not need its
 * own locking for the module-level RIDList insertion below. m_tagMutex
 * is still taken for typed_children_, since THAT map is also touched
 * by hasTag()/call()/etc. from arbitrary threads outside this event
 * path.
 * -----------------------------------------------------------------------
 */
template<typename T>
    static RID addTagTrampoline(Entity* parent, Entity* child, const ETCS::Buffer& tag)
    {
        std::lock_guard<std::mutex> lock(parent->m_tagMutex);
 
        auto it = parent->typed_children_.find(tag);
        if (it == parent->typed_children_.end())
        {
            auto* list = parent->local_arena_->allocate<RIDList<T*>>(*parent->local_arena_);
            it = parent->typed_children_.emplace(tag, list->handle(tag.c_str())).first;
            /*
 * First time THIS tag type has ever been seen on this entity -
 * record it in insertion order. Deliberately NOT inside the
 * block below (which runs on every attach, not just the first
 * per type) - see typed_child_order_'s own comment for why
 * this is a type-level, not instance-level, record.
 */
            parent->typed_child_order_.push_back(tag);
        }
 
        RID rid = child->getRID(); // virtual call - no cast needed
        it->second.invoke_insert(rid, child);
 
        /*
 * Also register into T's own module-level RIDList - the same one
 * _make_T() populates for entities spawned via the loader factory
 * path (`spawn Module::T`), already exposed through
 * EventNode::ridMap[T::CONTRACT_TAG] regardless of whether any instance
 * exists yet (ETCS_TAG_DECLARE registers the handle unconditionally
 * at module load time). Without this, an addTag<T>-spawned child is
 * fully tracked by its parent but invisible to `list Module::T` - a
 * different kind of RID than one spawned top-level, which is
 * exactly the inconsistency this closes.
 */
        {
            /*
 * Contract name: RegisterRIDRegistry publishes this list under the
 * tag block's name, so a concrete-name lookup misses silently and
 * the child never appears in `list Module::Type`.
 */
            ETCS::Buffer module_key(T::CONTRACT_TAG);
            auto& moduleRidMap = ETCS::EventNode::getInstance().ridMap;
            auto mit = moduleRidMap.find(module_key);
            if (mit != moduleRidMap.end())
            {
                mit->second.invoke_insert(rid, child);
                child->module_registry_key_ = module_key;
                child->module_registry_rid_ = rid;
            }
        }

        // Same fan-in a top-level spawn gets from _make_<T> (ETCS_API.h):
        // an addTag<T>-created child fulfills exactly the same families and
        // has to be as findable through them.
        etcs_supertype_fanout(child);
 
        child->parent_     = parent;
        child->parent_rid_ = rid;
        /*
 * Passive edge, set at the same moment and in the same place as
 * parent_ itself -- these two are the same fact recorded twice, for
 * two different consumers (typed-child bookkeeping and signal
 * propagation), and anything that changes one must change the other.
 * reparentChildrenTo is the only other place either is ever written,
 * and it writes both together for exactly this reason.
 */
        child->ctx_.setProvider(&parent->ctx_);
 
        /*
 * -- Tag closure, both directions ----------------------------------
 * The one place either reference is created, so the one place both
 * dependencies can be recorded. Parent now reaches T; child reaches the
 * parent through getParent(), which is untyped and so could never have
 * been caught at its own use site -- ChessGame::reportOutcomeLocked
 * calling back into its node is exactly that.
 *
 * Symmetry also buys transitivity: everything under one parent carries
 * the parent's bit, so a child reaching a sibling through it still
 * intersects, with no transitive edges recorded.
 *
 * T::TAG_MASK guards BOTH ORs on its own. It is assigned only by this
 * module's ETCS_TAG_DECLARE, so non-empty proves the module declares T;
 * addTag<T> is called on `this`, so that is the parent's module; and
 * the child IS a T. One module, one bit space. A contract typedef
 * resolving elsewhere reads empty and is skipped -- correct, since that
 * pair is a loader-scope fact carried by GetModuleBit.
 *
 * On the loader's ordering thread (addTagImpl calls this), the only
 * writer.
 */
        const ETCS::TagMask child_bit = T::TAG_MASK;
        if (child_bit.any())
        {
            parent->noteAcquires(child_bit);
            child->noteAcquires(parent->myTagMask());
        }
        return rid;
    }
};
 
/*
 * ScopeTag method bodies - declared in Bundles.h, defined here now that
 * Entity is complete. Same out-of-line split Module's own
 * validateManifest/etc. and LifetimeOwner's own getRID/etc. already use.
 *
 * ctx is threaded through now (was a 2-arg (entity, label) ctor) -- see
 * Scope's own comment (Bundles.h) for why the whole point of this type is
 * to make each in-flight call's own SignalContext reachable from
 * ~Entity()'s own interrupt sweep, which needs an actual ctx to register,
 * not just a flag string.
 */
inline ETCS::ScopeTag::ScopeTag(ETCS::Entity* entity, const char* label,
                                 const ETCS::SignalContext& ctx,
                                 const ETCS::TagMask& extra)
    : e(entity), extra_mask(extra)
{
    /*
 * "active_scope_Listen" -- no guard address anymore. The address made
 * every instance's flag unique, which meant the flag could only ever
 * express the fine-grained target, and expressed it as an unreadable
 * string no shell user could type. The label alone is now a genuine flag
 * (a boolean fact: is a Listen running), shared by every concurrent
 * instance, and the per-instance targeting moved to a real interface that
 * can carry an index.
 */
    tag = ETCS::Buffer((std::string(kPrefix) + label).c_str());
    /*
 * Register BEFORE addTag, same reversal as before: registerScope is a
 * plain locked insert on THIS thread; addTag fires a blocking
 * TagModifyEvent through an ordering thread. Doing the cheap local one
 * first closes the window where anyActive() reports false for a scope
 * already committed to running -- exactly the window a destroy drain
 * would slip through.
 */
    Scope::Registration reg = e->registerScope(label, ctx);
    scope_ctx = reg.ctx;
    scope_id  = reg.id;
    /*
 * Only the first concurrent call of this label raises the shared flag.
 * A second Listen starting would otherwise re-add a flag already present
 * (emplace no-ops), and -- worse -- the first one to FINISH would remove
 * it while the other is still running, so the flag would stop meaning
 * what it says. first_of_label is computed inside registerScope's own
 * lock rather than by a separate check here, so two concurrent
 * registrations can't both decide they were first.
 *
 * Safe against tagModifyImpl's own scope fork: that fork only fires on
 * REMOVAL, and this addTag is an insert.
 */
    if (reg.first_of_label) e->addTag(tag, extra_mask);
}
 
/*
 * e can be nullptr here -- see the move ctor's own comment (Bundles.h):
 * a moved-FROM ScopeTag (the one left behind in DEFINE_STREAM_FUNC_
 * PRODUCE's own trampoline after transferring into the pool-enqueued
 * lambda) has e set to nullptr specifically so its own, about-to-run
 * destructor becomes a harmless no-op instead of a double-unregister/
 * double-removeTag against a guard that was never really "this one" to
 * begin with.
 */
inline ETCS::ScopeTag::~ScopeTag()
{
    if (!e) return;
    Scope::Removal rem = e->unregisterScope(scope_id);
    /*
 * Unregister first, THEN removeTag -- load-bearing ordering. removeTag
 * fires a TagModifyEvent whose handler calls interruptLabel(label) for
 * this exact flag; having already removed this entry means that call
 * finds zero live scopes, returns 0, and correctly falls through to
 * erasing the flag from flags_ rather than treating the removal as an
 * interrupt request and returning early with the flag still set.
 */
    if (rem.found && rem.last_of_label) e->removeTag(tag, extra_mask);
}
 
/*
 * Routed through Entity::anyScopeActive() now -- scope_ is the actual,
 * authoritative registry (what ~Entity()'s own interrupt sweep walks);
 * the "active_scope_*" flags are a REPL-visible mirror of it, not the
 * other way around, so this no longer needs to walk flags_ itself to
 * infer presence.
 */
inline bool ETCS::ScopeTag::anyActive(ETCS::Entity* entity)
{
    return entity->anyScopeActive();
}
 
// --- GenerateEnvironmentSignature ---
inline HASH_TYPE GenerateEnvironmentSignature(const ETCS::Buffer& uniqueName)
{
    picohash_ctx_t ctx;
    picohash_init_sha256(&ctx);
 
    picohash_update(&ctx, uniqueName.buf, uniqueName.written);
 
    auto& manifest = ETCS::Entity::getManifest();
    for (auto const& [key, val] : manifest) {
        picohash_update(&ctx, key, std::strlen(key));
        picohash_update(&ctx, val, std::strlen(val));
    }
 
    unsigned char digest[PICOHASH_SHA256_DIGEST_LENGTH];
    picohash_final(&ctx, digest);
 
    HASH_TYPE result;
    std::memcpy(&result, digest, sizeof(HASH_TYPE));
    return result;
}
 
/*
 * -----------------------------------------------------------------------
 * Root - the bootstrap host. Solves the chicken-and-egg problem in
 * attachModule's own bootstrap path (DynamicLoader.h): the global Module
 * instance needs SOME entity or Root to run attachModule against before
 * any real entity of the requested tag exists yet - so the very FIRST
 * module ever loaded at a given scope has nothing to attach through yet.
 * Root breaks that cycle: it's a bare, freely-constructible type,
 * requiring no module at all, existing purely to give attachModule
 * something valid to run against until a real entity comes along and
 * claims lifetime_owner for itself.
 *
 * Standalone -- does NOT derive from Entity. A Root's entire purpose is
 * to host a Module until it either hands the lifetime token off to
 * something real, or goes out of scope still holding it (in which case
 * ~Module()'s own Root-relinquish branch, DynamicLoader.h, searches
 * root_registry for a sibling Root before vacating -- see
 * changeModuleImpl's own comment there). None of that has anything to
 * do with Entity's own machinery: typed children (addTag<T>), tags/
 * flags_ (module dispatch), the entity-local sub-arena, or the
 * election/reparenting dance MemoryArena's own registerDtor<T> runs for
 * arena-resident entities. A Root never has typed children, is never
 * dispatched through any tag, and is never arena-resident (it lives
 * wherever its own caller constructed it -- typically the stack, per
 * ShellREPL.h's own nav_root and CommandExecutor.h's detached_root/
 * run_root/local_root, but nothing about Root itself requires that).
 *
 * Root previously inherited Entity purely so it could satisfy
 * attachModule's own `Entity&` parameter type -- a real cost for no
 * real benefit: every Root paid for an entity-local sub-arena
 * (local_arena_, allocated unconditionally off the GLOBAL arena in
 * Entity's own constructor) it never used, and because ~Entity()'s own
 * body never reclaimed that arena early and Root was never
 * arena-resident for anything else to reclaim it via either, every Root
 * ever constructed leaked exactly one 4KB MemoryArena until actual
 * process exit. Now that attachModule (and everything else that used to
 * require an Entity& specifically) takes LifetimeOwner instead --
 * tagged to hold either an Entity* or a Root* -- Root has no reason to
 * masquerade as an Entity at all, and simply doesn't have local_arena_
 * (or tags/flags_/typed_children_) to leak in the first place.
 *
 * Tag is deliberately lowercase ("root") - the ONE exception to the
 * TitleCase convention every other entity tag follows in this codebase.
 * That's intentional, structural marking: "root" can never collide with a
 * real dispatch tag (which are always TitleCase by convention). It is
 * also what generateRID<Root>() uses as its own Derived::TAG, seeding
 * Root's own RID sequence the identical, deterministic way every other
 * leaf type's RID sequence is seeded -- no clock, no address, no
 * randomness anywhere in that computation, same as before.
 * -----------------------------------------------------------------------
 */
class Root
{
public:
    static constexpr const char* TAG = "root";
 
    /*
 * First member, deliberately, for the same reason it's Entity's own
 * first member: immediately valid the moment any Root& is in scope.
 */
    Module module_;
 
    /*
 * Required, not defaulted -- every Root construction site must
 * supply the SignalContext actually governing its own lifetime
 * (main()'s own ctx via WIRE_CONTEXT(), a REPL loop's own sig
 * parameter, a detached executor's own local_sig, a MirrorBuffer's
 * own bound_ctx_, etc.). By value, like every other SignalContext
 * parameter in this codebase (Entity::call, bindContext, every
 * WorkFunc/StreamFunc) -- it's a small, cheap-to-copy bundle of
 * pointers into shared atomics, and the copy still resolves to the
 * exact same underlying flags as the original. See ~Root()'s own
 * comment for why this is load-bearing rather than just
 * documentation: a real, reproduced SIGSEGV traced to ~Root() having
 * no way to know a signal-driven shutdown was already in progress,
 * and attempting its own normal, graceful module vacate/unload dance
 * anyway -- racing dlclose() against a process that was already
 * tearing itself down around it.
 */
    SignalContext ctx_;
 
    explicit Root(SignalContext ctx) : module_("", *this), ctx_(ctx) {}
 
    ~Root() {
        /*
 * WHAT THIS TESTS, and what it used to test. The condition was
 * `ctx_.isInterrupted() || ctx_.isTerminated()` -- a signal on this
 * Root's chain, used as a proxy for "the process is tearing itself
 * down". That proxy held only while the ONLY thing that raised a
 * signal was the process stopping. It no longer does: a closure
 * ending raises the same authority (SignalContext::raiseClosure)
 * with the runtime very much alive, and a detached script's Root --
 * whose ctx_ IS that closure's chain -- then took this branch and
 * returned WITHOUT unregistering, leaving root_registry holding a
 * pointer into a thread stack that the closure drain had already
 * joined and unmapped. findRootCandidate handed that pointer to
 * requestUnloadImpl 100ms later and it wrote through it:
 *
 *     survivor->module_.parent = target;      DynamicLoader.h:1859
 *
 * -- a reproduced SIGSEGV, 3 runs in 3, from scripts/render_script.etcs.
 *
 * So test the thing the comment always MEANT. The dance is unsafe
 * exactly when the machinery it needs is gone: ChangeModuleEvent is
 * synchronous on the loader's ordering thread, and EventNode::alive()
 * is false precisely once that thread has been joined in ~EventNode.
 * Alive means the event completes; dead means there is nothing left
 * to complete it and the OS reclaims the mapping anyway. A raised
 * signal, by itself, says nothing about either.
 *
 * The race the old condition was really guarding -- ~Root's vacate
 * firing a RequestUnloadEvent whose recheck thread then dlclose'd
 * under still-running workers -- is closed at its own level now, by
 * PendingUnloadRegistry's join barrier (DynamicLoader.h): a recheck
 * cannot be started after the barrier, and every one started before
 * it is joined. Guarding it a second time here, with a condition
 * that also drops registry bookkeeping, cost more than it bought.
 *
 * If a signal-driven shutdown is already in progress, skip the
 * normal graceful vacate/unload dance entirely -- there is no
 * safe way to synchronously wait for an asynchronous module
 * unload's own worker threads to finish while the PROCESS
 * itself is concurrently mid-teardown; the OS reclaims
 * everything regardless the instant this process actually
 * exits. Attempting the ordinary path here (ChangeModuleEvent's
 * own synchronous vacate, which can itself go on to fire a
 * RequestUnloadEvent) is exactly what raced dlclose() against
 * still-running worker threads and produced a real, reproduced
 * SIGSEGV. PendingUnloadRegistry (DynamicLoader.h) closes the
 * equivalent race for an ORDINARY exit path (main() actually
 * returning, wait_for_environment_drain unblocking normally),
 * but a signal arriving mid-navigation doesn't reliably route
 * through that return path in time for the join to matter --
 * this stops the race from ever starting in the first place,
 * for this specific case, rather than trying to win it after
 * the fact.
 *
 * ctx_ is still carried and still load-bearing -- every entity
 * this Root hosts reaches it as signal authority -- it is just
 * no longer what decides this branch. See the note above the
 * condition for why.
 */
        if (!ETCS::EventNode::alive())
        {
            /*
 * This body returning early is NOT enough on its own --
 * module_ is a MEMBER, not something this body controls the
 * destruction of. C++ destroys members in reverse
 * declaration order regardless of what this body does, so
 * module_'s own ~Module() runs immediately after this
 * function returns either way, and ~Module() has its OWN,
 * completely independent path to the identical cascade (its
 * own "Root going out of scope -- relinquishing/
 * unregistering" branch, DynamicLoader.h, gated only on
 * `parent && hosting_entity.kind == LifetimeOwner::Kind::
 * Root`) -- a real, reproduced second instance of the same
 * SIGSEGV traced to exactly that: the check above alone
 * silenced ~Root()'s own attempt, but ~Module()'s fired
 * moments later regardless, via a code path this class
 * never touches directly. Nulling parent here makes that
 * branch's own condition false, so ~Module() safely skips
 * it -- without Module itself needing any SignalContext
 * awareness of its own. hosting_entity is deliberately left
 * alone: nothing else in ~Module() reads it once parent is
 * null, and leaving it intact costs nothing.
 */
            module_.parent = nullptr;
            return;
        }
 
        /*
 * A node must formally resign its topology role before it ceases to exist.
 * Relying on ~Module() to do this was an ownership inversion (Module is
 * destroyed AFTER this body runs, and Root's state is already conceptually gone).
 * We fire this unconditionally for any attached Root so root_registry
 * never accumulates dangling pointers, regardless of owner status.
 */
        if (module_.hosting_entity.kind == LifetimeOwner::Kind::Root) {
            ETCS::ChangeModuleEvent{"", this}();
        }
    }
 
    RID getRID() const { return m_rid; }
 
    /*
 * -----------------------------------------------------------------------
 * changeModule(targetModule) - swaps this Root's own module_ to a
 * DIFFERENT module by name, blocking until the loader's ordering
 * thread has fully processed the switch. If this Root happened to be
 * holding the lifetime token for whatever module it's currently
 * attached to, giving that up first searches for another live Root
 * already attached to the SAME module (see EventNode::LoaderStream's
 * own root_registry) before falling back to an ordinary vacate +
 * RequestUnloadEvent - exactly the hand-off an ordinary dying
 * arena-resident entity already gets via MemoryArena's own
 * findNextCandidateScope, extended here to cover Root, which never
 * appears in any arena's dtor chain and so could never be found by
 * that search at all.
 *
 * This Root only becomes the NEW module's own lifetime_owner if
 * attachModule's normal claim logic finds it vacant at the moment of
 * attach (first entity/Root ever to touch it, or first to reattach
 * after a previous owner vacated) - no special-casing here beyond
 * what attachModule already does for any other first attach.
 *
 * Safe to call with the module this Root is already attached to -
 * a deliberate no-op, matching attachModule's own convention.
 *
 * Defined out-of-line in DynamicLoader.h (needs ChangeModuleEvent and
 * getLoader() fully visible, same reason every other Event-firing
 * method on Entity/Root lives there rather than here).
 * -----------------------------------------------------------------------
 */
    void changeModule(const std::string& targetModule);
 
private:
    const uint64_t m_rid = ETCS::generateRID<Root>();
};
 
/*
 * -- LifetimeOwner method bodies ------------------------------------------------
 * Declared in Bundles.h (Root only needed to be forward-declared there
 * for a tagged pointer member); defined here, now that both Entity and
 * Root are fully visible. Same out-of-line pattern Module's own
 * validateManifest/registerLoader/getTagAddress/~Module already use.
 */
 
inline uint64_t ETCS::LifetimeOwner::getRID() const
{
    switch (kind)
    {
        case Kind::Entity: return as_entity->getRID();
        case Kind::Root:   return as_root->getRID();
        default:           return 0;
    }
}
 
inline ETCS::Module& ETCS::LifetimeOwner::module() const
{
    switch (kind)
    {
        case Kind::Entity: return as_entity->module_;
        case Kind::Root:   return as_root->module_;
        default:
            throw std::runtime_error(
                "LifetimeOwner::module(): kind is None -- nothing to dereference.");
    }
}
 
inline ETCS::Entity& ETCS::LifetimeOwner::asEntity() const
{
    if (kind != Kind::Entity)
        throw std::runtime_error("LifetimeOwner::asEntity(): does not hold an Entity.");
    return *as_entity;
}
 
inline ETCS::Root& ETCS::LifetimeOwner::asRoot() const
{
    if (kind != Kind::Root)
        throw std::runtime_error("LifetimeOwner::asRoot(): does not hold a Root.");
    return *as_root;
}
 
 
/*
 * spawn<T> family - the primary way to create a top-level (root, parent_
 * == nullptr) entity, replacing raw create-type events sent straight to
 * the loader. All three typed overloads share one shape: construct T
 * FIRST, on the calling thread, via whichever strategy was asked for -
 * then hand the already-built pointer to a LoadEvent (its `prebuilt`
 * field), which does ONLY the module-attachment orchestration
 * (attachModule: registry check, proxy, bootstrap, or Root-hosted
 * transfer). This mirrors addTagTrampoline<T>'s own reasoning exactly: a
 * bare function pointer can't capture arbitrary constructor arguments the
 * way a lambda could, so construction has to happen before the event
 * fires, not inside its handler.
 *   spawn<T>(args...)          - allocates from the GLOBAL (module-wide)
 *                                 singleton arena. The default - matches
 *                                 how root entities have always been
 *                                 allocated (Entity()'s own ctor already
 *                                 does this whenever s_pending_parent_arena_
 *                                 is null).
 *
 *   spawn<T>(arena, args...)  - uses arena's OWN typed allocate<T>()
 *                                 method, so the destructor IS properly
 *                                 registered in that arena's own dtor
 *                                 chain.
 *
 *
 * ETCS_MODULE_NAME and T::CONTRACT_TAG supply the module:tag key -- contract,
 * since loadImpl looks the tag half up in a contract-keyed catalog.
 * CONTRACT_TAG is static-init rather than constexpr, which costs nothing here:
 * the key is a runtime concat either way.
 * -----------------------------------------------------------------------
 */
template<typename T, typename... Args>
T* spawn(Args&&... args)
{
    static_assert(std::is_base_of<Entity, T>::value, "spawn<T>: T must derive from Entity");
    T* entity = ETCS::MemoryArena::getInstance().allocate<T>(std::forward<Args>(args)...);
    ETCS::LoadEvent evt((std::string(ETCS_MODULE_NAME) + ":" + T::CONTRACT_TAG).c_str());
    evt.prebuilt = entity;
    ETCS::Entity* result = evt();
    return result ? static_cast<T*>(result) : nullptr;
}
 
template<typename T, typename... Args>
T* spawn(ETCS::MemoryArena& arena, Args&&... args)
{
    static_assert(std::is_base_of<Entity, T>::value, "spawn<T>(arena): T must derive from Entity");
    /*
 * NOTE: entity->owning_arena_ will read as the GLOBAL singleton, not
 * `arena` -- Entity's ctor resolves it from s_pending_parent_arena_,
 * which this path deliberately leaves null (so the new entity's own
 * local_arena_ keeps coming off the global singleton, as it always
 * has). Harmless today: every spawn yields parent_ == nullptr, and
 * both readers of owning_arena_ (Entity::operator delete,
 * entityUnloadImpl/destroyImpl) take their module_arena branch for a
 * parentless entity and never consult it. If a future change ever
 * makes owning_arena_ load-bearing for a root-level entity, THIS is
 * the call site that has to be reconciled first.
 */
 
    T* entity = arena.allocate<T>(std::forward<Args>(args)...);
    ETCS::LoadEvent evt((std::string(ETCS_MODULE_NAME) + ":" + T::CONTRACT_TAG).c_str());
    evt.prebuilt = entity;
    ETCS::Entity* result = evt();
    return result ? static_cast<T*>(result) : nullptr;
}
 
 
inline ETCS::DestroyEvent::DestroyEvent(const char* key, ETCS::Entity* t, bool children)
        : Event(key), rid(t->getRID()), delete_children(children), target(t) {};
inline ETCS::DestroyEvent::DestroyEvent(const ETCS::Buffer& key, ETCS::Entity* t, bool children)
        : Event(key), rid(t->getRID()), delete_children(children), target(t) {};
#ifdef ETCS_LOADER
/*
 * The runtime-string counterpart - loader-only, for .etcs scripts working
 * in runtime string space with no compile-time T available at all.
 *
 * root_for_bootstrap is the one genuine case that needs an entity/Root
 * passed in explicitly rather than found by walking a parent chain:
 * there's no entity of ANY kind in existence yet at this call site
 * (that's the entire point - resolving the module has to happen before
 * Make() can even be called), so there's nothing to walk. The caller
 * (spawn_entity, CommandExecutor.h) already has one in scope via
 * ExecutionContext's own root_entity - either inherited from a parent
 * script or freshly stack-allocated for this one, per the "root
 * auto-instantiates on first use in whatever scope needs it" rule.
 * LifetimeOwner, not a bare Entity& -- see DLInEvent::bootstrap_root's
 * own comment (EventNode.h) for why this needs to hold either kind; in
 * current practice it is always a Root, since every caller of this
 * function threads it straight from ExecutionContext::root_entity.
 */
inline Entity* spawn(const std::string& module_name, const std::string& tag,
                      LifetimeOwner root_for_bootstrap)
{
    LoadEvent evt((module_name + ":" + tag).c_str());
    evt.root = root_for_bootstrap;
    return evt();
}
#endif

/*
 * -- etcs_supertype_fanout ------------------------------------------------
 * Inserts a fully-constructed entity into the aggregate RIDList of every
 * ontology family it fulfills. ETCS_SUPERTYPE_BASE (ETCS_API.h) publishes
 * one such list per family under the bare family name; this is what puts
 * anything IN them.
 *
 * Both this file's and DynamicLoader.h's comments have described this
 * function as existing for a while, and destroyImpl already carries the
 * matching fan-OUT -- but nothing ever fanned in, so every family aggregate
 * was permanently empty and that removal loop was dead. Found by trying to
 * use one: a spawned ImageSurface carries the Pixels type tag and its
 * interface pointer, and was absent from the "Pixels" list.
 *
 * Called post-construction only (_make_/_make_child_/addTagTrampoline).
 * It cannot run from ETCS_MAKE_INSTANCE's own ctor, which is where the
 * families are declared: at that point the object is still being built,
 * bases after this one have not registered their interface pointers yet,
 * and getRID() would be dispatching through a half-formed vtable.
 *
 * WHAT IS STORED is the interface pointer -- the Family_* subobject
 * address ETCS_MAKE_INSTANCE registered -- because a family aggregate is
 * RIDList<Family_*> and RIDList is genuinely typed at its own local
 * provider (RIDList.h). It goes in through insert_iface rather than the
 * plain insert slot: that one recovers T with getTrueType(), which is the
 * most-derived address and therefore the WRONG pointer for a base
 * subobject. Both slots convert inside handle(), the last place T is
 * known; the handle stays string-keyed and erased on purpose, and what
 * makes a key trustworthy across that boundary is module verification, not
 * anything recoverable from the pointer itself.
 */
// Declared in RIDList.h, where Entity is necessarily incomplete -- see its
// comment there. Defined here, where the class is complete: it is the one
// member call RIDList's handle lambdas need and cannot make themselves.
inline void* etcs_true_type(Entity* e) { return e ? e->getTrueType() : nullptr; }

inline void etcs_supertype_fanout(Entity* e)
{
    if (!e) return;
    std::vector<ETCS::Buffer> families;
    e->getInterfaceFamilies(families);
    if (families.empty()) return;

    auto& ridMap = ETCS::EventNode::getInstance().ridMap;
    const RID rid = e->getRID();
    for (const ETCS::Buffer& family : families)
    {
        auto it = ridMap.find(family);
        if (it == ridMap.end()) continue;   // a family this module does not publish
        // insert_iface, not insert: this list's T is a base subobject
        // (Pixels_*, Surface_*), so the pointer it must store is the
        // adjusted one ETCS_MAKE_INSTANCE registered -- getTrueType(),
        // which the plain insert slot uses, would be the wrong address.
        it->second.invoke_insert_iface(rid, e->getInterfacePointer(family));
    }
}

/*
 * A RID plus a family name in, a usable Base* out -- the uniform call
 * surface over a CRTP family. The list lookup finds the entity whoever
 * built it, and the interface pointer is the correctly-adjusted subobject
 * address that ETCS_MAKE_INSTANCE registered, so the caller can invoke the
 * family's contract without knowing the concrete type, which module made
 * it, or whether that type exposes the same operations as ETCS work
 * functions -- every implementor satisfies the C++ contract whether or not
 * it publishes one.
 *
 * Returns nullptr if the family is not published here, the RID is not in
 * it, or the entity somehow lacks the interface pointer.
 */
template <typename Base>
inline Base* resolve_in_family(const char* family, RID rid)
{
    if (rid == 0) return nullptr;
    const ETCS::Buffer key(family);
    auto& ridMap = ETCS::EventNode::getInstance().ridMap;
    auto it = ridMap.find(key);
    if (it == ridMap.end()) return nullptr;
    // Straight out of the list: a family aggregate stores the Base*
    // itself (RIDList.h), so there is no interface-pointer round trip and
    // no adjustment happening here -- this is the pointer that was stored.
    return static_cast<Base*>(it->second.invoke_get_iface(rid));
}

} // namespace ETCS

#endif
