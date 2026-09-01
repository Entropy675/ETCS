#ifndef DYNAMICLOADER_H__
#define DYNAMICLOADER_H__
#include <string>
#include <stdexcept>
#include <iostream>
#include <unordered_map>
#include <vector>
#include <memory>
#include <sstream>
#include <algorithm>
#include <iomanip>
#include <cstdlib>
#include <thread>
#include <mutex>
#include <chrono>
// shared_mutex removed - all state mutation serialized by LoaderStream consumer.
#include "../core_defs.h"
#include "EventNode.h"
#include "SignalContext.h"
/*
 * SECURITY CONSIDERED: compiler flags stub out these functions on the module
 * side to prevent the static-init lambda trick from side-loading DLLs before
 * the injection registry overwrite completes.
 * -- Static plumbing declared first -------------------------------------------
 * vDynamicMap and getLoader() must be visible before Module::registerLoader()
 * so they appear at the top of the file rather than the bottom.
 */
struct vDynamicMap { ETCS::EventNode* node = nullptr; };
static vDynamicMap dynamicLoader;
namespace ETCS {
    /*
 * Returns the root EventNode - in loader scope this is the singleton that
 * owns LoaderStream; in module scope it is wired via RegisterDynamicLoader.
 */
    static EventNode& getLoader() { return *dynamicLoader.node; }
}
namespace ETCS
{
using namespace ETCS;
/*
 * -- PendingUnloadRegistry ------------------------------------------------------
 * Tracks every RequestUnloadEvent-spawned 200ms-delay-then-recheck thread
 * joinably, rather than the raw std::thread(...).detach() this used to
 * be (see the Kind::RequestUnload case below). A detached thread has NO
 * handle anywhere at all, meaning nothing -- including, critically, the
 * process's own normal exit path -- could ever wait for it to finish.
 *
 * A real, reproduced SIGSEGV traced to exactly that gap: SIGINT during
 * interactive module navigation causes ~Root() to fire a SYNCHRONOUS
 * vacate (ChangeModuleEvent), which itself fires a non-blocking
 * RequestUnloadEvent; ~Root() returns the instant the vacate alone is
 * acknowledged, with no idea the asynchronous 200ms recheck it just
 * triggered hasn't even started yet. The REPL loop then exits (interrupt
 * flag still set), main() returns, and the process's own exit sequence
 * proceeds concurrently with -- and can easily outrun -- that
 * still-pending recheck's own eventual dlclose() on a module whose
 * worker threads may still be mid-flight, tearing code out from under a
 * thread that's still executing it.
 *
 * Joined from drive_main_loop_then_exit (ShellREPL.h), right alongside
 * shutdown_detached_executors() -- the process is never allowed to
 * actually exit while any recheck is still in progress. An empty
 * registry (nothing was ever mid-unload, the overwhelmingly common
 * case) joins nothing and costs nothing.
 */
struct PendingUnloadRegistry
{
    std::mutex               mutex_;
    std::vector<std::thread> threads_;
    /*
 * Set by join_all(): the join barrier has been crossed and no further
 * recheck may be started. Without this the registry has a second hole
 * the same shape as the detach() one it was built to close, just at
 * the other end of the process's life -- see spawn() below.
 */
    bool                     closed_ = false;

    /*
 * Starts a recheck ONLY while there is still someone left to join it.
 * Returns false once the barrier has passed, and the caller drops the
 * recheck.
 *
 * Reproduced, on pristine main, with any loader that owns a module
 * outliving its last entity: main() deletes its entities, calls
 * join_all(), prints, returns -- and THEN static destruction vacates
 * the module's lifetime_owner, firing one last RequestUnloadEvent.
 * The thread that fired for it was tracked into a registry nobody
 * would ever join again, so ~vector<std::thread> destroyed a joinable
 * thread and the process aborted with "terminate called without an
 * active exception" AFTER a completely successful run. Loaders were
 * papering over it by calling join_all() at exactly the right moment,
 * which is a convention, not a guarantee -- and the one ordering that
 * defeats it is the one nobody writes down.
 *
 * Dropping the recheck at that point loses nothing: its whole job is
 * to dlclose a module, and the process is already unmapping everything
 * it owns. The construction happens INSIDE the lock so the decision and
 * the spawn cannot straddle a concurrent join_all().
 */
    bool spawn(std::function<void()> body)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (closed_) return false;
        threads_.emplace_back(std::move(body));
        return true;
    }
    void track(std::thread t)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        threads_.push_back(std::move(t));
    }
    void join_all()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& t : threads_)
            if (t.joinable()) t.join();
        threads_.clear();
        closed_ = true;
    }
    /*
 * Belt and braces. With spawn()'s guard nothing can be added after
 * join_all(), so this loop is empty in every ordering the guard covers
 * -- it exists so that a future call site reaching for track() directly
 * still cannot end a run by destroying a joinable thread.
 */
    ~PendingUnloadRegistry()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& t : threads_)
            if (t.joinable()) t.join();
        threads_.clear();
    }
    static PendingUnloadRegistry& getInstance()
    {
        static PendingUnloadRegistry instance;
        return instance;
    }
};
/*
 * -- Module --------------------------------------------------------------------
 * Full definition now lives in Bundles.h (moved there so Entity can hold a
 * Module module_ member directly -- see that header's own comment for the
 * full reasoning). Only the methods needing EventNode/SignalContext/complete-
 * Entity are defined here, out-of-line, exactly where their bodies used to
 * live -- validateManifest, registerLoader, getTagAddress, and ~Module()
 * (new: needs the shared survivor-search logic below for its one special
 * case).
 */
// Thin wrapper over the shared, symmetric comparison (Bundles.h) -- this is
// the LOADER's own half; RegisterDynamicLoader below runs the same check on
// the module's side, against the loader's manifest.
bool ETCS::Module::validateManifest(Manifest* dllManifest)
{
    bool mismatch = ETCS::compareManifests(ETCS::Entity::getManifest(), dllManifest, name);
    ETCS_LOG("DynamicLoader:Module", name << (mismatch ? " FAILED integrity check." : " integrity verified."));
    return mismatch;
}
/*
 * Takes EventNode& st so registerLoader can absorb the module's ridMap
 * directly via st - called from resolveImpl which is inside the consumer,
 * so st == *stream.owner and no getInstance() call is needed.
 */
bool ETCS::Module::registerLoader(EventNode& st)
{
#ifdef ETCS_LOADER
    /*
 * Validate BEFORE calling into the module at all. discoverTags() only
 * needs a dlsym'd manifest-returning symbol -- it doesn't touch the
 * module's own EventNode/ThreadPool, so a mismatch is caught here with
 * nothing on the module side ever started. Throws ManifestMismatchException
 * on a HEADER:/ONTOLOGY: disagreement (attachModule handles that
 * distinctly from an ordinary load failure); tags is filled either way.
 *
 * This is the loader's own independent half of the check. The module ran
 * its own half already -- at dlopen()'s static-init time, before dlopen()
 * even returned to attachModule, so before registerLoader (this function)
 * was ever entered (see ETCS_MODULE_EXPORT_MAIN's static-init block, ETCS_API.h, and
 * ETCS_GetLoaderManifest just above). Neither side waits on the other's
 * result or on call ordering between them; the one thing both are
 * guaranteed to precede is RegisterDynamicLoader below actually completing
 * -- that return is the real sync point.
 */
    discoverTags(tags);

    using RegisterLoaderFunc = ETCS::EventNode* (*)(void*);
    void* funcPtr = getTagFunction("RegisterDynamicLoader");
    if (funcPtr)
    {
        RegisterLoaderFunc reg = reinterpret_cast<RegisterLoaderFunc>(funcPtr);
        ETCS_LOG("DynamicLoader:Module", "Found registry function @" << funcPtr << ", passing local loader...");
        // Pass the loader's EventNode (not DynamicLoader) to the module
        ETCS::EventNode* node = reg(static_cast<void*>(&getLoader()));
        if (node)
        {
            // Absorb module ridMap entries directly via st
            for (const auto& [originalKey, handle] : node->ridMap)
            {
                std::stringstream ss;
                ss << node->scope << ":" << originalKey;
                ETCS::Buffer combinedKey;
                ss >> combinedKey;
                st.ridMap[combinedKey] = handle;
                ETCS_LOG("EventNode:" << st.scope,
                    "Absorbed module " << node->scope << " RIDList: " << originalKey);
            }
        }
        else
        {
            ETCS_LOG("DynamicLoader:Module", "Module returned a null EventNode!");
            return false;
        }
    }
    else
    {
        ETCS_LOG("DynamicLoader:Module", "Module missing 'RegisterDynamicLoader' export!");
        return validBinary;
    }
    /*
 * -- Signal authority transfer ---------------------------------------
 * Hand the loader's real, OS-signal-backed root across the dlopen
 * boundary so the module adopts it rather than standing up a second,
 * disconnected instance the first time module-scope code touches
 * RootSignalContext()/WIRE_SIGNAL_CONTEXT. Optional export: a module
 * built without RegisterRootSignalContext simply keeps a local,
 * never-wired root - inert (no local, no parent authority) rather
 * than fighting the loader over std::signal() registration.
 */
    using RegisterSignalsFunc = void (*)(ETCS::SignalContext*);
    void* sigFuncPtr = getTagFunction("RegisterRootSignalContext");
    if (sigFuncPtr)
    {
        RegisterSignalsFunc regSig = reinterpret_cast<RegisterSignalsFunc>(sigFuncPtr);
        regSig(&ETCS::RootSignalContext());
        ETCS_LOG("DynamicLoader:Module", "Transferred root SignalContext authority to module: " << name);
    }
    else
    {
        ETCS_LOG("DynamicLoader:Module",
            "Module missing 'RegisterRootSignalContext' export -- "
            "module will not receive live global signal authority.");
    }
    ETCS_LOG("DynamicLoader:Module", "tags! " << tags.size());
    validBinary = true;
    return validBinary;
#endif
    (void)st;
    return false;
}
ETCS::ModuleBundle ETCS::Module::getTagAddress(const std::string& tag)
{
#ifdef ETCS_LOADER
    std::string hashFuncSymbol = tag + "_GetHash";
    std::string makeFuncSymbol = tag + "_Make";
    void* hashAddr = getTagFunction(hashFuncSymbol);
    void* makeAddr = getTagFunction(makeFuncSymbol);
    if (!makeAddr) throw std::runtime_error("Failed to find '" + makeFuncSymbol + "' in " + name);
    if (!hashAddr) throw std::runtime_error("Failed to find '" + hashFuncSymbol + "' in " + name);
    ETCS_LOG("DynamicLoader:ModuleBundle", "Raw "     << makeAddr << " from " << tag << "!");
    ETCS_LOG("DynamicLoader:ModuleBundle", "RawHash " << hashAddr << " from " << tag << "!");
    using MakeFuncResolver = MakeFunc (*)();
    using MakeFunc         = ETCS::Entity*(*)(ETCS::Buffer&);
    using HashFunc         = HASH_TYPE(*)();
    MakeFunc  actualMake = reinterpret_cast<MakeFuncResolver>(makeAddr)();
    HASH_TYPE actualHash = reinterpret_cast<HashFunc>(hashAddr)();
    ETCS_LOG("DynamicLoader:ModuleBundle", "Got " << tag << " from " << name << "!");
    ETCS::FlatMap<ETCS::Buffer, WorkBundle> actions;
    Manifest* actionsHashes = discoverActions(tag, actions);
    SignalContext moduleSignals = {
        name.c_str(), "Factory",
        &this->interrupt, &this->terminate, &this->hangup,
        &this->pause,     &this->resume,
        &this->user1,     &this->user2
    };
    /*
 * Wire this Module's local authority up to the process root - this
 * is what lets isInterrupted()/isTerminated() on the bundle (and
 * anything that inherits from it, e.g. an entity's forwarded ctx)
 * see real OS signals, not just this Module's own local flags.
 */
    moduleSignals.setParent(&ETCS::RootSignalContext());
    return {tag, this, actualHash, actualMake, actionsHashes, actions, moduleSignals, ETCS::Buffer()};
#else
    (void)tag;
    return {};
#endif
}
/*
 * Module lifetime, current design: every entity's own module_ is ALWAYS
 * just a forwarding proxy onto the one, permanent, loader-owned global
 * Module instance (parent set the moment attachModule ever touches it) --
 * no per-entity Module ever holds real content (library_handle,
 * module_arena, type_catalog) itself, and nothing ever transfers content
 * between Module instances (adoptOwnershipFrom is gone). What moves
 * instead is a single pointer on the global instance itself,
 * lifetime_owner, tracking which entity's (or Root's) own module_ token
 * is currently the elected one.
 *
 * The election/vacate DECISION and the actual promotion/vacate
 * BOOKKEEPING are now two separate things, living in two separate
 * places, for a structural reason: an arena-resident (non-stack) entity's
 * destructor only ever runs when something explicitly walks the arena's
 * own dtor records and calls it -- nothing does that "for free" the way
 * stack unwinding does for a stack-allocated Root. So the search itself
 * (which needs to run while the dying entity's siblings are still
 * intact, and needs MemoryArena-level access to that arena's own dtor
 * chain) lives in MemoryArena's own run_entity_delete callback
 * (registerDtor<T>, MemoryArena.h) for the Entity case, or in
 * EventNode::LoaderStream's own root_registry for the Root case (see
 * changeModuleImpl below) -- triggered explicitly, BEFORE this entity's
 * destructor (and hence ~Module()) ever runs at all. promoteOrVacate()
 * below just does the bookkeeping/event-firing that either search's own
 * result implies, uniformly, via LifetimeOwner.
 * promoteOrVacate - called by MemoryArena's own run_entity_delete
 * callback for a global-scope entity (survivor always Entity-kind, found
 * via that arena's own dtor chain), and by
 * EventNode::LoaderStream::changeModuleImpl for a Root giving up its
 * module (survivor always Root-kind, found via root_registry). A bare
 * Entity Root still converts implicitly to LifetimeOwner at both call
 * sites, so neither caller needed to change for this. A no-op if this
 * token was never the elected lifetime_owner in the first place -- most
 * global-scope entities dying are ordinary proxies, not the owner.
 */
void ETCS::Module::promoteOrVacate(LifetimeOwner survivor)
{
    if (!is_lifetime_owner) return;
    Module* global = parent;
    if (!global) return;
    // Guard against promoting something attached to a different module.
    if (survivor && survivor.module().parent != nullptr
                 && survivor.module().parent != global)
    {
        survivor = LifetimeOwner();
    }
    /*
 * Deliberately does NOT search root_registry here anymore, even
 * though it used to. This function is compiled once per translation
 * unit that includes DynamicLoader.h -- an ordinary member
 * definition, not an event handler -- and gets INVOKED from
 * registerDtor<T>'s own captured lambda (MemoryArena.h), which is
 * itself compiled into whichever module first instantiated
 * allocate<T> for the dying entity's own type. A Root-search block
 * gated behind #ifdef ETCS_LOADER right here meant the LOADER's own
 * compiled copy had it while every MODULE's own separately-compiled
 * copy did not (ETCS_LOADER is never defined in a module build) --
 * and the call always resolves to whichever binary's copy the dying
 * entity's own T was compiled in, silently skipping the search for
 * every entity that dies inside its own hosting module, which is the
 * ordinary case. Moved to requestUnloadImpl instead (DynamicLoader.h)
 * -- a function that only ever exists inside the loader binary in
 * the first place, so there is no which-copy ambiguity left to have.
 * The vacated module already waits 100ms there before actually
 * unloading, checking whether anything reclaimed lifetime_owner in
 * the meantime -- checking root_registry there too is the same kind
 * of check, just covering the population this function can no longer
 * safely search itself.
 */
    if (survivor)
    {
        survivor.module().parent            = global;
        survivor.module().is_lifetime_owner  = true;
        global->lifetime_owner              = survivor;
        ETCS_LOG("DynamicLoader:Module", "Module '" << global->name
            << "' lifetime_owner promoted to entity RID:" << survivor.getRID());
        return;
    }
    global->lifetime_owner = nullptr;
    ETCS::RequestUnloadEvent{global->name, global}();
    ETCS_LOG("DynamicLoader:Module", "Module '" << global->name
        << "' lifetime_owner vacated -- RequestUnloadEvent fired.");
}
/*
 * ~Module() - pure dlclose/cleanupModule cleanup now, no election call at
 * all: that's fully decided, synchronously, before this destructor ever
 * runs (see promoteOrVacate's own comment above). Reached only by the
 * ONE, PERMANENT GLOBAL instance, and only at actual process shutdown
 * (the loader's own MemoryArena::getInstance() tearing down) --
 * per-entity tokens never have library_handle set, so this branch is
 * structurally unreachable for them. If the module was still loaded at
 * process exit (nobody ever triggered an unload), clean it up directly
 * here rather than through RequestUnloadEvent's own async delay
 * machinery -- there's no reason to wait 100ms when the process is
 * exiting anyway.
 * ~Module() - two genuinely separate cases now, not one:
 *
 * 1. The ONE, PERMANENT GLOBAL instance, at actual process shutdown (the
 *    loader's own MemoryArena::getInstance() tearing down) -- identified
 *    by library_handle being set, which no per-entity/per-Root token ever
 *    has. If the module was still loaded at process exit, clean it up
 *    directly here rather than through RequestUnloadEvent's own async
 *    delay -- there's no reason to wait 100ms when the process is
 *    exiting anyway.
 *
 * 2. A per-Root token that is STILL the elected lifetime_owner at the
 *    moment it destructs, with nothing ever having been spawned from its
 *    module to hand the token off to (see attachModule's own explicit
 *    hand-off logic, which is what normally moves the token away from
 *    Root before this could ever happen). This case is specific to
 *    Root: unlike an arena-resident entity, whose destructor only ever
 *    runs via MemoryArena::deleteEntity's own explicit trigger, Root's
 *    destructor runs "for free" via ordinary scope exit (stack unwinding
 *    for a stack-allocated Root, or ordinary member-destruction order if
 *    a Root were ever heap-allocated) -- there is no equivalent
 *    arena-level trigger for it at all. So THIS destructor, running at
 *    exactly the right, C++-guaranteed moment for Root's own module_
 *    member, is the one correct place to relinquish the token -- but NOT
 *    unconditionally: see changeModuleImpl's own comment for why this
 *    now searches root_registry for a sibling Root ALREADY attached to
 *    the same module before ever vacating outright. A module with TWO
 *    live Roots would otherwise incorrectly vacate the instant either
 *    one destructed, even while the other is still actively using it.
 *
 * Only a Root's own module_ member can ever reach branch 2 now (never an
 * Entity's) -- an Entity that was ever the elected lifetime_owner always
 * gets decided via MemoryArena's own run_entity_delete callback calling
 * promoteOrVacate BEFORE ~Entity() (and hence this destructor) ever
 * runs, so by the time any Entity-hosted Module reaches its own
 * destructor, is_lifetime_owner has already been resolved one way or the
 * other. hosting_entity.asRoot() below asserts this invariant rather
 * than silently guessing.
 */
/*
 * unmapLibrary - see its declaration comment (Bundles.h) for the four steps
 * and why their order is load-bearing. Defined here because steps 1-3 need
 * EventNode complete.
 *
 * This is the whole sequence ~Module and requestUnloadImpl used to open-code
 * separately (each carrying a "same as the other one" comment), and that
 * attachModule's own failure paths did NOT: its generic catch closed the
 * handle bare, skipping both the _Cleanup that stops the module's ordering
 * thread (a hang at dlclose's static-dtor join) and the ridMap purge (rows
 * absorbed by registerLoader left pointing into unmapped memory).
 */
void ETCS::Module::unmapLibrary(ETCS::EventNode* node)
{
#ifdef ETCS_LOADER
    if (!library_handle) return;
    ETCS_LOG("DynamicLoader:Module", "Unmapping module: [" << name << "::" << library_handle << "]");

    /*
 * 1. Raise this Module's OWN authority before anything else touches the
 *    library. Every entity ever spawned from it has a passive edge
 *    terminating at one of this module's ModuleBundle ctxs (a root-level
 *    entity's provider is set in attachModule; an addTag<T> child's walks
 *    up to one), so this reaches them -- and ONLY them, which is the point.
 *    A module unload is not a process-wide event: Ctrl+C raises the global
 *    flags and stops everything, while this must stop exactly the work
 *    whose CODE is about to be unmapped.
 *
 *    These flags live on the Module itself, which outlives the close (it is
 *    loader-arena allocated; only the LIBRARY is unmapped), so a work
 *    function reading its ctx one last time during teardown reads a live
 *    flag rather than freed memory. Release, matching
 *    global_signal_handler's store and paired with SignalContext::raised's
 *    acquire.
 */
    interrupt.store(1, std::memory_order_release);
    terminate.store(1, std::memory_order_release);

    // 2. registerLoader absorbs a module's ridMap under "<module>:<tag>"
    //    keys, each handle wrapping a RIDList in the MODULE's image.
    //    Per-entity removal empties those lists without unlinking the rows.
    if (node)
    {
        const std::string prefix = name + ":";
        size_t purged = 0;
        for (auto it = node->ridMap.begin(); it != node->ridMap.end(); )
        {
            const std::string key = it->first.toString();
            if (key.compare(0, prefix.size(), prefix) == 0) { it = node->ridMap.erase(it); ++purged; }
            else ++it;
        }
        if (purged)
            ETCS_LOG("DynamicLoader:Module", "Purged " << purged << " ridMap row(s) for '"
                     << name << "' -- their RIDLists are about to be unmapped.");
    }

    // 3-4. Cleanup then close. Nulled so a second close is impossible.
    cleanupModule();
    ETCS_LOG("DynamicLoader:Module", "Unloading library: " << getFilename() << " ...");
#ifdef _WIN32
    FreeLibrary(library_handle);
#else
    dlclose(library_handle);
#endif
    library_handle = nullptr;
#else
    (void)node;
#endif
}

ETCS::Module::~Module()
{
#ifdef ETCS_LOADER
    if (hasLibrary())
    {
        /*
 * Process-exit path: nobody triggered an explicit unload, so the global
 * flags may or may not already be raised (an ordinary main() return never
 * sets them; only a signal or shutdown_detached_executors does).
 * unmapLibrary raises this module's own either way, which is what makes
 * the entities it spawned observe the stop in the return-normally case.
 */
        unmapLibrary(&ETCS::EventNode::getInstance());
    }
    else if (parent && hosting_entity.kind == LifetimeOwner::Kind::Root)
    {
        /*
 * Was gated on is_lifetime_owner alone -- wrong now that
 * attachModule registers every attaching Root, not just the
 * owner. Any Root-hosted token needs unregistering here, owner
 * or not, or root_registry accumulates dangling Root*s.
 * promoteOrVacate itself no-ops if this token was never owner.
 */
        ETCS_LOG("DynamicLoader:Module", "Module '" << parent->name
            << "' Root going out of scope -- relinquishing/unregistering.");
        ETCS::ChangeModuleEvent{"", &hosting_entity.asRoot()}();
    }
#endif
}
/*
 * -- MirrorBuffer wrap/unwrap method bodies ------------------------------------
 * Declared in MirrorBuffer.h; defined here because they need Entity
 * complete (getTypedChildren/getTypedChild/hasTag/getInterfacePointer/
 * getArena) and, for resolveWrapChain's unwrap branch, LoadEvent's own
 * operator()() body (defined further down in this same file, in the
 * "Event operator() definitions" section) -- neither is reachable from
 * MirrorBuffer.h itself, which is parsed WHILE Entity.h is still
 * mid-definition. Same out-of-line split Module's own
 * validateManifest/registerLoader/getTagAddress/~Module above already
 * use, for the identical reason.
 * buildWrapManifest - the WRAP-side, entity-owning half of chain
 * resolution. Walks owner's live addTag'd children in attach order,
 * keeps the ones tagged "Wrapper" whose Scope() includes this pair's own
 * strategy, and records each survivor's (module, tag) identity into
 * wrap_manifest_ -- the wire form, since a live pointer means nothing on
 * the far side of a genuine process boundary. Called once, on the
 * producer object, from inside makePair; the resulting array is then
 * copied verbatim onto the consumer object by makePair itself (see
 * MirrorBuffer.h), never recomputed there.
 *
 * Looks up "Wrapper" -- the SAME key ETCS_MAKE_INSTANCE already
 * registers for every family, generically, via
 * registerInterfacePointer(#Name, static_cast<void*>(static_cast<Name##_*>(this))).
 * No separate registration or hand-written constructor needed on
 * Wrapper_'s side: the stored pointer (a Wrapper_* address) is safe to
 * reinterpret as IWireWrapper* directly PROVIDED IWireWrapper is
 * declared as Wrapper_'s FIRST, non-virtual base -- under the Itanium
 * C++ ABI (this project's actual target), the first non-virtual base
 * subobject sits at offset 0, so a Wrapper_* and an IWireWrapper*
 * pointing at the same object are bit-identical. This is a real,
 * load-bearing dependency on Wrapper_'s own declared base order, not a
 * style choice -- getting it wrong produces a silently mis-adjusted
 * pointer, not a compile error.
 */
void ETCS::MirrorBuffer::buildWrapManifest(ETCS::Entity* owner)
{
    wrap_manifest_len_ = 0;
    if (!owner) return;
    std::vector<std::pair<ETCS::Buffer, RID>> children;
    owner->getTypedChildren(children);
    for (auto& [tag, rid] : children)
    {
        if (wrap_manifest_len_ >= MAX_WRAP_STAGES) break;
        Entity* child = owner->getTypedChild(tag, rid);
        if (!child || !child->hasTag(ETCS::Buffer("Wrapper"))) continue;
        void* raw = child->getInterfacePointer(ETCS::Buffer("Wrapper"));
        if (!raw) continue;
        IWireWrapper* w = static_cast<IWireWrapper*>(raw);
        if (!scopeApplies(w->Scope(), currentScopeBit())) continue;
        wrap_manifest_[wrap_manifest_len_].module = child->getSourceModule();
        wrap_manifest_[wrap_manifest_len_].tag    = child->getSourceTag();
        ++wrap_manifest_len_;
    }
    /*
 * LMAX-strategy responsibility, additionally: allocate the scratch
 * pool exactly once here, alongside the manifest walk that just
 * decided whether any wrapper actually applies to THIS pair. See
 * wrap_scratch_pool_'s own comment (MirrorBuffer.h) for why sizing
 * it to lmax_page_->slot_count_ specifically is what makes slot
 * reuse safe with no separate synchronization.
 */
    if (active_ == ActiveStrategy::LMAX && wrap_manifest_len_ > 0)
    {
        assert(lmax_page_ &&
            "buildWrapManifest: LMAX strategy with no ring allocated yet -- "
            "makePair should have set lmax_page_ before this call.");
        long long count = lmax_page_->slot_count_;
        void* mem = owner->getArena().allocateRaw(
            static_cast<long long>(sizeof(MBuffer)) * count, alignof(MBuffer));
        wrap_scratch_pool_ = static_cast<MBuffer*>(mem);
        for (long long i = 0; i < count; ++i)
            new (&wrap_scratch_pool_[i]) MBuffer();
    }
}
/*
 * resolveWrapChain - populates wrap_chain_ (the LIVE, resolved
 * IWireWrapper* stages writeRaw/readRaw actually call) from
 * wrap_manifest_, which by the time this runs has already been
 * deserialized off the wire inside unpack(). Branches on is_producer_ --
 * see each branch's own comment for why the wrap and unwrap sides
 * resolve their chains through genuinely different mechanisms.
 */
void ETCS::MirrorBuffer::resolveWrapChain(ETCS::Entity* handler)
{
    wrap_chain_len_ = 0;
    unwrap_failed_  = false;
    if (is_producer_)
    {
        /*
 * Wrap side: handler is already known, in-process -- re-walk its
 * OWN typed children directly, exactly like buildWrapManifest
 * just did on the ORIGINAL producer object makePair touched
 * (this is a freshly-reconstructed object from unpack(), a
 * different instance entirely). Deterministic given the same
 * handler + the same active_, so this reproduces
 * buildWrapManifest's own filtered result exactly -- the
 * deserialized wrap_manifest_ is read here purely for wire
 * uniformity (both sides always carry it), not actually
 * consulted for resolution on this side.
 */
        if (!handler) return;
        std::vector<std::pair<ETCS::Buffer, RID>> children;
        handler->getTypedChildren(children);
        for (auto& [tag, rid] : children)
        {
            if (wrap_chain_len_ >= MAX_WRAP_STAGES) break;
            Entity* child = handler->getTypedChild(tag, rid);
            if (!child || !child->hasTag(ETCS::Buffer("Wrapper"))) continue;
            void* raw = child->getInterfacePointer(ETCS::Buffer("Wrapper"));
            if (!raw) continue;
            IWireWrapper* w = static_cast<IWireWrapper*>(raw);
            if (!scopeApplies(w->Scope(), currentScopeBit())) continue;
            wrap_chain_[wrap_chain_len_++] = w;
        }
        return;
    }
    /*
 * Unwrap side: wrap_manifest_ IS the capability manifest -- fire one
 * LoadEvent per entry, IN ORDER, against a single, reused stack-local
 * Root. Safe to reuse across every stage: each LoadEvent's own
 * attachModule call hands the lifetime token off to whatever it
 * spawns, so this Root never actually ends up holding a module's
 * token between stages -- there is no drifting-Root-holding-a-
 * lifetime-token problem to worry about here.
 *
 * Any stage failing to load is the graceful capability-negotiation
 * failure this whole manifest mechanism exists for: the far side
 * asked for a wrapper this side cannot construct (module not
 * loadable, or the resolved entity doesn't actually declare itself
 * Wrapper_-family). unwrap_failed_ surfaces through unpack()'s own
 * bool return, which the DEFINE_STREAM_FUNC_PRODUCE/_CONSUME
 * trampolines (ETCS_API.h) check before ever invoking the
 * developer's own body.
 *
 * Entities bootstrapped here are recorded into ephemeral_entities_
 * regardless of whether a LATER stage in this same loop goes on to
 * fail -- ~MirrorBuffer() tears down whatever's there unconditionally,
 * so a partial chain from a failed negotiation is still cleaned up
 * correctly with no special-casing needed here.
 */
    ETCS::Root boot_root(bound_ctx_);
    for (size_t i = 0; i < wrap_manifest_len_; ++i)
    {
        if (wrap_chain_len_ >= MAX_WRAP_STAGES) break;
        std::string key = wrap_manifest_[i].module.toString() + ":"
                         + wrap_manifest_[i].tag.toString();
        ETCS::LoadEvent evt(key.c_str());
        evt.root = ETCS::LifetimeOwner(&boot_root);
        ETCS::Entity* e = evt();
        if (!e)
        {
            ETCS_LOG("MirrorBuffer", "resolveWrapChain: failed to bootstrap "
                     "required wrapper '" << key << "' -- refusing to unwrap.");
            unwrap_failed_ = true;
            return;
        }
        void* raw = e->getInterfacePointer(ETCS::Buffer("Wrapper"));
        if (!raw)
        {
            ETCS_LOG("MirrorBuffer", "resolveWrapChain: '" << key
                     << "' loaded but does not declare itself Wrapper_-family "
                        "(no interface pointer under the \"Wrapper\" key) -- "
                        "refusing to unwrap.");
            ephemeral_entities_[ephemeral_count_++] = e; // still owned, still torn down
            unwrap_failed_ = true;
            return;
        }
        ephemeral_entities_[ephemeral_count_++] = e;
        wrap_chain_[wrap_chain_len_++] = static_cast<IWireWrapper*>(raw);
    }
}
/*
 * ~MirrorBuffer() - tears down every entity THIS instance bootstrapped
 * via LoadEvent during unpack()'s unwrap branch. Never populated on the
 * wrap side (which walks entities it doesn't own), and never populated
 * at all for the overwhelming majority of MirrorBuffer instances (no
 * wrap chain attached), so this loop is a no-op in the ordinary case.
 *
 * delete_children = true on the DestroyEvent: a wrapper entity could in
 * principle have addTag'd its own children (a stateful TLS wrapper
 * holding some helper), and those need to go with it -- there's no
 * separate mechanism that would otherwise reach them.
 */
ETCS::MirrorBuffer::~MirrorBuffer()
{
    for (size_t i = 0; i < ephemeral_count_; ++i)
    {
        Entity* e = ephemeral_entities_[i];
        if (!e) continue;
        std::string key = e->getSourceModule().toString() + ":"
                         + e->getSourceTag().toString();
        ETCS::DestroyEvent{key.c_str(), e, true}();
    }
}
// -- ModuleBundle / WorkBundle operator() bodies -------------------------------
ETCS::Entity* ETCS::ModuleBundle::operator()()
{
#ifdef ETCS_LOADER
    if (!tag || tag[0] == '\0' || makeFunc == nullptr || owner == nullptr)
    {
        ETCS_LOG("DynamicLoader:ModuleBundle",
            "Cannot initialize empty module: " << tag << " invalid module construction!");
        return nullptr;
    }
    ETCS_LOG("DynamicLoader:ModuleBundle", tag << " Bundle attempting to spawn type... ");
    tagbuff.writeString(tag);
    ETCS::Entity* result = makeFunc(tagbuff);
    /*
 * Purely diagnostic -- see MemoryArena::scope_tag_'s own comment.
 * This is a top-level spawn, not an addTag<T> child, so it's tagged
 * here rather than inside Entity::addTag<T> (which only ever sees
 * ITS OWN children, never a root-level entity like this one). `tag`
 * is already a real runtime string at this point -- no compile-time
 * T::TAG needed the way addTag<T>'s own call has available.
 */
    result->getArena().setScopeTag(tag.toString());
    /*
 * Wire the entity's module identity - set on the ordering thread,
 * before the entity is ever returned to a caller. Plain strings only
 * (no cached Module* here at all anymore) - see Entity::source_module_'s
 * own comment for why a cached pointer would go dangling across a
 * later module-scope election that this entity has no part in.
 */
    result->setModuleSource(ETCS::Buffer(tag), ETCS::Buffer(owner->name.c_str()));
    /*
 * By reference into the catalog - the one true, persistent copy -
 * never *this, which may be a caller-held temporary.
 */
    ETCS::ModuleBundle& catalog_bundle = owner->catalog()[tag.toString()];
    result->addTag(catalog_bundle);
    /*
 * Passive edge, root-level terminus. A top-level spawn has no parent
 * entity, so its provider is the spawning bundle's own ctx -- which
 * already carries this Module's local authority (interrupt/terminate/
 * hangup/pause/resume/user1/user2, wired in Module::getTagAddress) with
 * `up` pointing at RootSignalContext(), so global signals reach this
 * entity through it without the passive edge itself needing to continue
 * any further.
 *
 * catalog_bundle, NOT *this: the same reasoning addTag directly above
 * already depends on. `this` may be a caller-held temporary whose ctx
 * dies with it, while the catalog entry is the one true, persistent copy
 * -- allocated once from the module's own arena and never moved or
 * recreated across a module-scope hand-off (see Entity::TagEntry::bundle's
 * own comment). Taking the reference once and using it for both calls
 * makes that shared requirement explicit rather than repeating the
 * subscript and hoping both land on the same object.
 */
    result->getContext().setProvider(&catalog_bundle.ctx);
    ETCS_LOG("DynamicLoader:ModuleBundle",
        "Instance " << result->myTag() << " [HID: " << result->getID() << ", RID: " << result->getRID() << "] Tags:");
    std::vector<ETCS::Buffer> tags;
    result->getTags(tags);
    for (ETCS::Buffer i : tags)
        ETCS_LOG("DynamicLoader:ModuleBundle", "    - " << i);
    ETCS_LOG("DynamicLoader:ModuleBundle", tag << " Bundle spawned type: "
        << tagbuff.toString() << " (HID: " << result->getID() << ", RID: " << result->getRID() << ")");
    return result;
#else
    return nullptr;
#endif
}
bool ETCS::WorkBundle::operator()(ETCS::Entity* child, ETCS::Buffer& tagbuff, ETCS::SignalContext ctx)
{
#ifdef ETCS_LOG_FUNCTION_EVOCATION_PATH
    ETCS_LOG("WorkBundle::operator()", "ENTER module_tag=" << module_tag << " work_tag=" << work_tag
        << " child=" << (void*)child << " workFunc=" << (void*)workFunc);
#endif
    if (!child)
    {
        ETCS_LOG("WorkBundle::operator()", "!!! null child for " << module_tag
            << "." << work_tag << " -- refusing to dispatch.");
        return false;
    }
    if (!child->hasTag(module_tag.toString()))
    {
        ETCS_LOG("WorkBundle::operator()", "\U0001F641 - you passed the work function " << module_tag
            << "." << work_tag << " an invalid child tag.");
        return false;
    }
    if (!workFunc)
    {
        ETCS_LOG("WorkBundle::operator()", "!!! workFunc is NULL for " << module_tag << "." << work_tag
            << " -- would have crashed on the call below.");
        return false;
    }
#ifdef ETCS_LOG_FUNCTION_EVOCATION_PATH
    ETCS_LOG("WorkBundle::operator()", "[" << module_tag << "." << this->work_tag
        << "::" << this->hash << "] Entity " << child->myTag()
        << " takes action " << this->work_tag << " with data: " << tagbuff.toString());
    ETCS_LOG("WorkBundle::operator()", "about to invoke raw workFunc pointer " << (void*)workFunc << "...");
#endif
    reinterpret_cast<ETCS::WorkFunc>(const_cast<void*>(workFunc))(child, tagbuff, ctx);
#ifdef ETCS_LOG_FUNCTION_EVOCATION_PATH
    ETCS_LOG("WorkBundle::operator()", "EXIT -- workFunc(...) returned normally for "
        << module_tag << "." << work_tag);
#endif
    /*
 * true means DISPATCHED, not "produced output". A work function writing
 * nothing into tagbuff is an ordinary outcome (Delete has no result), and
 * is not something this layer has any business judging.
 */
    return true;
}
/*
 * Stream counterpart. Same ungating, same reasoning -- and this one needed the
 * logs more than its sibling did: its two guards were bare `return`s with no
 * message even inside the loader, so a stream dispatch failing here was
 * invisible in both scopes rather than only one.
 */
bool ETCS::WorkBundle::operator()(ETCS::Entity* child, ETCS::MBuffer& tagbuff, ETCS::SignalContext ctx)
{
#ifdef ETCS_LOG_FUNCTION_EVOCATION_PATH
    ETCS_LOG("WorkBundle::operator()", "ENTER (stream) module_tag=" << module_tag
        << " work_tag=" << work_tag << " child=" << (void*)child
        << " workFunc=" << (void*)workFunc);
#endif
    if (!child)
    {
        ETCS_LOG("WorkBundle::operator()", "!!! null child for stream " << module_tag
            << "." << work_tag << " -- refusing to dispatch.");
        return false;
    }
    if (!child->hasTag(module_tag.toString()))
    {
        ETCS_LOG("WorkBundle::operator()", "\U0001F641 - you passed the stream function " << module_tag
            << "." << work_tag << " an invalid child tag.");
        return false;
    }
    if (!workFunc)
    {
        ETCS_LOG("WorkBundle::operator()", "!!! workFunc is NULL for stream " << module_tag
            << "." << work_tag << " -- would have crashed on the call below.");
        return false;
    }
    reinterpret_cast<ETCS::StreamFunc>(const_cast<void*>(workFunc))(child, tagbuff, ctx);
#ifdef ETCS_LOG_FUNCTION_EVOCATION_PATH
    ETCS_LOG("WorkBundle::operator()", "EXIT -- streamFunc(...) returned normally for "
        << module_tag << "." << work_tag);
#endif
    return true;
}
/*
 * bool, not void: false means the action was not found in this tag's table, or
 * its WorkBundle refused to dispatch -- never that the action ran and produced
 * no output, which is ordinary. A caller inspecting only the buffer afterward
 * cannot tell "wrote nothing" from "never ran", which is exactly how a missing
 * action came to look like a successful response echoing the request back.
 */
bool ETCS::ModuleBundle::operator()(ETCS::Entity* child, const ETCS::Buffer& work,
                                    ETCS::Buffer& data, ETCS::SignalContext ctx)
{
#ifdef ETCS_LOG_FUNCTION_EVOCATION_PATH
    ETCS_LOG("ModuleBundle::operator()", "ENTER tag=" << this->tag << " work=" << work
        << " child=" << (void*)child);
#endif
    auto it = actions.find(work);
#ifdef ETCS_LOG_FUNCTION_EVOCATION_PATH
    ETCS_LOG("ModuleBundle::operator()", "[" << this->tag << "::" << this->hash << "] Entity "
        << child->myTag() << " (RID: " << child->getRID() << ")"
        << " [" << this->tag << "] attempting action: " << work
        << " -- found=" << (it != actions.end()));
#endif
    bool pass = false;
    if (it != actions.end())
    {
#ifdef ETCS_LOG_FUNCTION_EVOCATION_PATH
        ETCS_LOG("ModuleBundle::operator()", "about to invoke WorkBundle::operator() for " << work << "...");
#endif
        pass = it->second(child, data, ctx);
#ifdef ETCS_LOG_FUNCTION_EVOCATION_PATH
        ETCS_LOG("ModuleBundle::operator()", "EXIT -- WorkBundle::operator()(...) returned normally for " << work);
#endif
    }
    else
        ETCS_LOG("ModuleBundle::operator()", "Tag: " << this->tag
            << " does not provide requested action: " << work);
    return pass;
}
bool ETCS::ModuleBundle::operator()(ETCS::Entity* child, const ETCS::Buffer& work,
                                    ETCS::MBuffer& data, ETCS::SignalContext ctx)
{
    auto it = actions.find(work);
    bool pass = false;
    if (it != actions.end()) pass = it->second(child, data, ctx);
    else ETCS_LOG("ModuleBundle::operator()", "Tag: " << this->tag
             << " does not provide requested action (stream): " << work);
    return pass;
}
/*
 * -- LoaderStream method bodies ------------------------------------------------
 * Declared in EventNode.h, defined here where Module is fully defined.
 * All EventNode state access goes through owner pointer - no getInstance().
 */
#ifdef ETCS_LOADER
/*
 * mask_for - the ordering mask, from the event alone, BEFORE the handler runs.
 * That ordering is the restructure: a mask returned by the handler can only
 * order the handler's RESULT, which is why the buffer used to be inert.
 * Computed here, it decides whether the handler may start.
 *
 * Everything below is answerable pre-dispatch, and not by luck -- carrying
 * tagmodify_mask and origin_extra_mask ON the event, earlier in this epoch, is
 * what made it so.
 *
 * LOADER SCOPE THROUGHOUT: every bit is a module bit from GetModuleBit. Tag
 * bits never appear here; OriginScopeMask is the conversion.
 */
ETCS::TagMask ETCS::EventNode::LoaderStream::mask_for(
    DLState&, const DLInEventPtr& ref)
{
    const DLInEvent& evt = *ref.ptr;
    const ETCS::TagMask origin = OriginScopeMask(evt);
    switch (evt.kind)
    {
        case DLInEvent::Kind::Resolve:
            /*
 * conjugate_key IS the module name here -- no parse, unlike
 * Load/Destroy's "module:tag".
 */
            return GetModuleBit(evt.conjugate_key.toString()) | origin;
        case DLInEvent::Kind::Destroy:
            return GetModuleBit(
                parseConjugateOriginKey(evt.conjugate_key.toString()).first) | origin;
        case DLInEvent::Kind::AddTag:
        {
            /*
 * Both modules: the child's owner, whose catalog and ridMap this
 * touches, and the parent's, whose typed_children_ it touches. The
 * same lookup addTagImpl does, on a string already in hand.
 */
            ETCS::TagMask m;
            auto owner_it = type_owner_index.find(evt.conjugate_key.toString());
            if (owner_it != type_owner_index.end())
                m |= GetModuleBit(owner_it->second);
            if (evt.addtag_parent)
                m |= GetModuleBit(evt.addtag_parent->getSourceModule().toString());
            return m | origin;
        }
        case DLInEvent::Kind::TagModify:
            /*
 * NOT evt.tagmodify_mask: those are TAG bits in the emitting
 * module's space, and OR-ing them with module bits would alias two
 * unrelated indices into one word. A TagModify only reaches this
 * stream from loader-compiled code, whose TAG_MASK is never
 * assigned (no ETCS_TAG_DECLARE in a loader build) and so already
 * fail-shut to all() -- the same answer, now on purpose.
 */
            return ETCS::TagMask::all();
        /*
 * Load, EntityUnload, ChangeModule, RequestUnload: memory topology
 * changes -- a module going vacant-to-anchored, an arena reclaimed, a
 * hand-off touching two modules. Never commutable with anything.
 *
 * PairMask looks read-only but GetModuleBit MUTATES module_bit_index on
 * first use, so it takes the barrier too. Ack is unreachable here (see
 * its own case in on_event).
 */
        default:
            return ETCS::TagMask::all();
    }
}

ETCS::DispatchResult ETCS::EventNode::LoaderStream::on_event(
    DLState&, const DLInEventPtr& ref, uint64_t)
{
    /*
 * RAII guard, not a plain set/reset pair - on_event has multiple
 * return points (including early returns inside the switch below,
 * and thrown exceptions from things like registerLoader), and a
 * plain reset-at-the-bottom would miss those, leaving the flag
 * incorrectly stuck true for the rest of this thread's lifetime.
 */
    struct OrderingThreadGuard {
        OrderingThreadGuard()  { EventNode::on_ordering_thread = true; }
        ~OrderingThreadGuard() { EventNode::on_ordering_thread = false; }
    } _ordering_thread_guard;
    /*
 * NO HANDLER BELOW RELEASES ITS CALLER. Every completion store moved to
 * on_emit (EventNode.h), called when this event's slot commits -- because
 * releasing a blocked caller IS the commit, and ordering it is the job.
 *
 * That retires a bug class this file was full of: a completion store woke
 * the caller, the caller returned, its stack frame (where evt lives) was
 * reused, and anything read from evt afterwards -- sendAckIfNeeded's
 * evt.reply_to, most famously -- was garbage. One reproduced SIGSEGV,
 * latent in every other case. It cannot recur: the flag is not set until
 * this function has returned, so evt is alive throughout. sendAckIfNeeded's
 * position is now taste, not safety.
 */
    DLInEvent& evt = *ref.ptr;
    switch (evt.kind)
    {
        case DLInEvent::Kind::Load:
        {
            ETCS::Entity* e = nullptr;
            if (evt.prebuilt_entity)
            {
                /*
 * spawn<T>/spawn<T>(arena) path: T was
 * already constructed on the calling thread (the same
 * reason addTagTrampoline<T> never constructs - a bare
 * function pointer can't capture arbitrary constructor
 * args). conjugate_key still carries "module:tag" in the
 * usual form; only the attachModule step is needed here.
 * attachModule now returns success/failure rather than a
 * reference -- evt.prebuilt_entity is already the correct
 * entity to hand back on success, no different than what
 * it was handed in as.
 */
                auto [module_name, tag] = parseConjugateOriginKey(evt.conjugate_key.toString());
                if (attachModule(module_name, evt.prebuilt_entity, tag))
                    e = evt.prebuilt_entity;
            }
            else
            {
                e = loadImpl(evt.conjugate_key.toString(), evt.bootstrap_root);
            }
            /*
 * nullptr is the in-progress sentinel in the calling convention,
 * so use UINTPTR_MAX to signal failure through the spin loop.
 */
            sendAckIfNeeded(evt);
            /*
 * Held for on_emit, not stored. UINTPTR_MAX is LoadEvent's
 * not-null failure sentinel -- nullptr means in-progress.
 */
            evt.release_value = reinterpret_cast<uint64_t>(
                e ? e : reinterpret_cast<ETCS::Entity*>(UINTPTR_MAX));
            return {ETCS::DispatchKind::Inline, &evt};
        }
        case DLInEvent::Kind::Resolve:
        {
            /*
 * conjugate_key for Resolve IS the module name directly - see
 * ResolveEvent's own construction (ResolveEvent{mod_name...}),
 * unlike Load/Destroy's "module:tag" form. No parsing needed.
 */
            std::string module_name = evt.conjugate_key.toString();
            bool ok = evt.resolve_target
                   && resolveImpl(module_name, evt.resolve_target);
            sendAckIfNeeded(evt);
            evt.release_value = ok ? 1 : 0;
            return {ETCS::DispatchKind::Inline, &evt};
        }
        case DLInEvent::Kind::Destroy:
        {
            std::string key_str = evt.conjugate_key.toString();
            bool removed = destroyImpl(key_str, evt.rid, evt.destroy_children);
            sendAckIfNeeded(evt);
            evt.release_value = removed ? 1 : 0;
            return {ETCS::DispatchKind::Inline, &evt};
        }
        case DLInEvent::Kind::AddTag:
        {
            // The mask this event was admitted under is in mask_for above.
            RID r = addTagImpl(evt.addtag_parent, evt.addtag_child,
                                evt.conjugate_key, evt.addtag_trampoline);
            sendAckIfNeeded(evt);
            /*
 * rid_out relaxed, here; ready_out is the release and moved to
 * on_emit. The caller's acquire-load of it publishes this write
 * along with everything else the handler did.
 */
            evt.rid_out->store(r, std::memory_order_relaxed);
            return {ETCS::DispatchKind::Inline, &evt};
        }
        case DLInEvent::Kind::EntityUnload:
        {
            /*
 * Both the root case (reached when ~Entity() couldn't call
 * entityUnloadImpl directly -- wasn't already on this
 * ordering thread) and the explicit child-target case (e.g.
 * removeTag's entity-relation deletion) now converge on the
 * same call: entityUnloadImpl determines target's correct
 * parent arena and delegates to MemoryArena::deleteEntity,
 * which is the ONLY thing that actually runs target's own
 * destructor at all -- nothing destructs an arena-resident
 * entity except through this explicit path, so target is
 * always still fully alive at this point; there's no longer
 * an "already destructed" case to distinguish here.
 */
            entityUnloadImpl(evt.unload_target, evt.unload_delete_children);
            sendAckIfNeeded(evt);
            return {ETCS::DispatchKind::Inline, &evt};
        }
        case DLInEvent::Kind::ChangeModule:
        {
            changeModuleImpl(evt.changemodule_root, evt.conjugate_key.toString());
            sendAckIfNeeded(evt);
            return {ETCS::DispatchKind::Inline, &evt};
        }
        case DLInEvent::Kind::TagModify:
        {
            /*
 * No forward-or-local fork needed here - the loader IS the
 * top of the chain already (see ModuleProxy::on_event's own
 * comment for why the fork lives there instead).
 *
 * Masked all() by mask_for, not evt.tagmodify_mask -- see that
 * case for why tag bits stay out of a loader-scope mask.
 *
 * The read-before-store discipline this function used to follow is
 * gone with it: nothing here stores a completion flag, on_emit does
 * once the slot may commit, and the event stays alive until then
 * for the very reason the discipline existed -- a caller cannot pop
 * its frame while spinning on a flag nobody has set.
 */
            evt.tagmodify_impl(evt.tagmodify_target, evt.conjugate_key, evt.tagmodify_is_remove);
            return {ETCS::DispatchKind::Inline, &evt};
        }
        case DLInEvent::Kind::PairMask:
        {
            /*
 * The one scope conversion a module cannot do for itself: two
 * contract tags in, a MODULE-scope mask out. type_owner_index is
 * loader state, so only this side can answer.
 *
 * Fails shut on either tag being unknown -- a pair naming a type no
 * loaded module claims is the last thing to grant independence.
 * Both halves in one module collapse to one bit, which answers the
 * same-module case for free.
 *
 * No sendAckIfNeeded: acks order a module's own stream after the
 * loader alters memory for it, and the only mutation here is
 * module_bit_index's first-use assignment, on this thread.
 */
            ETCS::TagMask m;
            auto it_a = type_owner_index.find(evt.conjugate_key.toString());
            auto it_b = type_owner_index.find(evt.pairmask_tag_b.toString());
            if (it_a == type_owner_index.end() || it_b == type_owner_index.end())
            {
                ETCS_LOG("LoaderStream", "PairMask: unresolved tag ("
                    << evt.conjugate_key << ", " << evt.pairmask_tag_b
                    << ") -- failing shut to all().");
                m = ETCS::TagMask::all();
            }
            else
            {
                m = GetModuleBit(it_a->second) | GetModuleBit(it_b->second);
            }
            if (evt.pairmask_out) *evt.pairmask_out = m;
            return {ETCS::DispatchKind::Inline, &evt};
        }
        case DLInEvent::Kind::RequestUnload:
        {
            ETCS::Module* target = evt.request_unload_target;
            if (!evt.request_unload_recheck)
            {
                /*
 * Initial, non-blocking fire -- unchanged in spirit: never
 * block THIS ordering thread for the 100ms delay. What
 * changed is .detach() -> tracked via
 * PendingUnloadRegistry (this file), and the spawned
 * thread now WAITS for its own recheck to genuinely
 * finish before returning, via a stack-local done flag
 * (request_unload_done) rather than firing the recheck
 * and forgetting about it entirely.
 *
 * This closes a real, reproduced SIGSEGV: a detached
 * thread has no handle ANYWHERE, so nothing -- including
 * the process's own normal exit path -- could ever wait
 * for it. SIGINT during interactive navigation causes
 * ~Root() to fire a SYNCHRONOUS vacate (ChangeModuleEvent)
 * that itself fires THIS non-blocking RequestUnloadEvent;
 * ~Root() returns the instant the vacate is acknowledged,
 * with no idea the asynchronous 200ms recheck it just
 * triggered hasn't even started yet. The REPL loop then
 * exits (interrupt flag still set) and main() returns,
 * letting the process's own exit sequence proceed
 * concurrently with -- and easily outrun -- that
 * still-pending recheck's own eventual dlclose() on a
 * module whose worker threads may still be mid-flight.
 * Tracking this thread (and joining every tracked entry
 * from drive_main_loop_then_exit, ShellREPL.h, right
 * alongside shutdown_detached_executors()) closes that
 * window: the process is never allowed to actually exit
 * while any recheck is still in progress. An empty
 * registry -- nothing was ever mid-unload, the
 * overwhelmingly common case -- joins nothing and costs
 * nothing.
 */
                // spawn(), not track(): refused once join_all() has run,
                // which is what keeps a last-gasp vacate during static
                // destruction from leaving a joinable thread behind. See
                // PendingUnloadRegistry::spawn's own comment.
                const bool recheck_started = PendingUnloadRegistry::getInstance().spawn([target]()
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    /*
 * Stack-allocated, unlike the old heap-allocated
 * recheck_evt -- safe now specifically because this
 * thread blocks on `done` below before its own frame
 * ever pops, exactly the same stack-lifetime contract
 * every OTHER synchronous DLInEvent-based call in this
 * codebase (TagModifyEvent, ChangeModuleEvent, etc.)
 * already relies on.
 */
                    std::atomic<bool> done{false};
                    DLInEvent recheck_evt{};
                    recheck_evt.kind                   = DLInEvent::Kind::RequestUnload;
                    recheck_evt.request_unload_target  = target;
                    recheck_evt.request_unload_recheck = true;
                    recheck_evt.request_unload_done    = &done;
                    /*
 * enqueue() refuses silently (returns false) if this
 * stream is already cleaning up at that point -- in
 * that case there is genuinely nothing left to wait
 * for, so just return; the loader is on its own way
 * out regardless.
 */
                    if (getLoader().stream.enqueue(DLInEventPtr{&recheck_evt}))
                        while (!done.load(std::memory_order_acquire));
                });
                if (!recheck_started)
                    ETCS_LOG("DynamicLoader:Module", "Module '" << target
                             << "' unload recheck skipped -- the process is past its join "
                                "barrier, so the mapping goes with the exit.");
                /*
 * This event (the FIRST fire) is still heap-allocated by
 * RequestUnloadEvent::operator()() -- delete it here,
 * exactly as before. Only the RECHECK event's own
 * allocation strategy changed (heap -> stack), not this
 * one's.
 * No completion: heap-allocated by
 * RequestUnloadEvent::operator()() with nothing waiting on it.
 * The RECHECK fire below has the blocked thread behind it.
 */
                delete &evt;
                return {ETCS::DispatchKind::Inline, nullptr};
            }
            /*
 * Recheck fire -- evt is STACK-allocated by the waiting
 * thread spawned above (recheck_evt, held live by that
 * thread's own while(!done) spin), not heap-allocated the
 * way the first fire's own event is. NEVER delete this one:
 * its owning thread's own stack frame is what releases it,
 * once the store below wakes that thread's own spin and lets
 * it return. Every read of `evt` happens BEFORE the store,
 * never after -- the same ordering discipline this session's
 * TagModifyEvent fix (Entity.h) already established, for
 * exactly the same reason: signalling completion first and
 * reading the event's own fields afterward risks reading
 * memory the other side has already reclaimed.
 */
            requestUnloadImpl(target);
            return {ETCS::DispatchKind::Inline, &evt};
        }
        case DLInEvent::Kind::Ack:
        {
            /*
 * Structurally unreachable here: an Ack is only ever
 * constructed by sendAckIfNeeded, targeting reply_to->stream
 * -- a MODULE's own ModuleProxy, never this LoaderStream
 * itself (the loader never sends itself an ack). Handled
 * explicitly, rather than left to the switch's own default
 * fallthrough, so reaching this branch at all is loud and
 * diagnosable -- it would mean something enqueued a
 * Kind::Ack event directly onto the loader's own stream,
 * which is always a bug elsewhere, not a case this function
 * itself needs to do anything for.
 */
            ETCS_LOG("DynamicLoader", "on_event: Kind::Ack reached the "
                     "LOADER's own stream -- this should be structurally "
                     "impossible; an Ack always targets a module's own "
                     "ModuleProxy stream instead. Ignoring.");
            return {ETCS::DispatchKind::Inline, nullptr};
        }
    }
    return {ETCS::DispatchKind::Inline, nullptr};
}
/*
 * registerTypeOwnership - registers every tag in mod->type_catalog as
 * owned by module_name. Logs - does NOT silently overwrite - if a tag
 * name is already claimed by a DIFFERENT module, surfacing a genuine
 * naming collision loudly rather than picking one arbitrarily.
 */
void ETCS::EventNode::LoaderStream::registerTypeOwnership(
    const std::string& module_name, Module* mod)
{
    for (const auto& [tag_str, bundle] : mod->catalog())
    {
        auto it = type_owner_index.find(tag_str);
        if (it != type_owner_index.end() && it->second != module_name)
        {
            ETCS_LOG("DynamicLoader", "!!! WARNING !!! Type '" << tag_str
                     << "' is exported by both '" << it->second << "' and '"
                     << module_name << "' -- keeping the first owner ('"
                     << it->second << "'). addTag<T> resolution for this "
                     << "type will always resolve to that module; the "
                     << "other module's copy is unreachable via addTag<T>.");
            continue;
        }
        type_owner_index[tag_str] = module_name;
    }
}
/*
 * attachModule - THE single entry point for giving any entity or Root a
 * Module reference for module_name. Current design:
 *
 *   1. If entity.module() is already valid (parent set), this is a no-op
 *      success if it's the SAME module_name (a script referencing one
 *      module across many lines is the common case), or a dropped
 *      request if it's a DIFFERENT one (rebinding would silently orphan
 *      whatever it already pointed at -- see the guard's own comment
 *      below for why dropping, not erroring, is deliberate).
 *
 *   2. Look up (or bootstrap, if vacant) the ONE, PERMANENT, loader-
 *      owned global Module instance for this name -- allocated once,
 *      from the loader's own arena, and never moved or recreated. This
 *      is where dlopen/registerLoader/module_arena/type_catalog actually
 *      happen, exactly once per module name for the whole process.
 *
 *   3. entity.module().parent = global_mod, unconditionally -- every
 *      attach is now structurally just a forwarding proxy.
 *
 *   4. If global_mod->lifetime_owner is vacant, THIS entity/Root claims
 *      it: is_lifetime_owner = true, lifetime_owner = entity. Otherwise
 *      entity is just an ordinary proxy, same as any other live
 *      reference to an already-anchored module.
 *
 * spawn_tag == "" skips entity's own dispatch wiring entirely (used when
 * resolveImpl just wants the module attached, not any specific type
 * spawned from it, and ALWAYS the case when entity holds a Root, since
 * Root has no dispatch surface at all -- Root::changeModule() and every
 * bootstrap call site pass "" unconditionally). Ordering-thread only.
 *
 * Returns success/failure rather than a reference now: the only caller
 * that ever propagated the old ETCS::Entity* return value as something
 * further used was loadImpl's own vacant-branch tail, which already
 * holds the exact same pointer it passed in as `entity` -- there was
 * never any information in the return value a caller didn't already
 * have. Returning bool removes an awkward ambiguity a two-kind `entity`
 * parameter would otherwise create for the return type too.
 */
bool ETCS::EventNode::LoaderStream::attachModule(
    const std::string& module_name, ETCS::LifetimeOwner entity,
    const std::string& spawn_tag)
{
    /*
 * An entity's or Root's module_ starts vacant (constructed that way
 * by Entity's/Root's own ctor) and is meant to be bound to exactly
 * one module for its whole lifetime -- this is what makes "an entity
 * can only ever host or proxy one module" an enforced invariant
 * rather than an assumption the rest of this function silently
 * relies on. Re-resolving the SAME module against an already-bound
 * entity is a harmless no-op (the common case: a script referencing
 * one module across many lines, each one independently calling
 * resolveImpl) and succeeds immediately without touching anything
 * further below. Requesting a DIFFERENT module against an
 * already-bound entity is what actually gets dropped: rebinding it
 * would silently orphan whatever it already pointed at. Dropping
 * that request outright, rather than erroring, is deliberate: the
 * correct way to target a different module is a fresh entity/Root
 * (Root gets reconstructed on the stack each time a new one is
 * needed specifically so this is always available, never a real
 * constraint in practice) -- or, for a Root that specifically needs
 * to migrate in place, Root::changeModule().
 */
    bool already_valid = entity.module().parent != nullptr;
    if (already_valid && entity.module().parent->name == module_name)
        return true;
    if (already_valid)
    {
        ETCS_LOG("DynamicLoader", "attachModule: entity RID:" << entity.getRID()
            << " already has a valid module bound (" << entity.module().parent->name
            << ") -- dropping this request for '" << module_name << "'.");
        return false;
    }
    /*
 * Look up (or bootstrap) the ONE, PERMANENT, loader-owned global
 * Module instance for this name -- never an entity's own member.
 * Every entity's own module_ is now ALWAYS just a forwarding proxy
 * onto this single object, for as long as the module is loaded at
 * all; there is no more "transfer" of real content between entities,
 * since no per-entity Module ever holds any real content to move.
 */
    auto reg_it = module_registry.find(module_name);
    Module* global_mod = (reg_it != module_registry.end()) ? reg_it->second : nullptr;
    if (!global_mod)
    {
        /*
 * BOOTSTRAP - allocate the permanent global instance itself, once,
 * from the loader's own arena (MemoryArena::getInstance() here
 * correctly resolves to the LOADER's own arena, since this
 * function only ever runs on the loader's own ordering thread).
 */
        global_mod = MemoryArena::getInstance().allocate<Module>(module_name);
        /*
 * Everything in this bootstrap sequence -- dlopen/LoadLibrary,
 * registerLoader (which calls discoverTags), catalogTypes (which
 * calls discoverActions via getTagAddress) -- can throw
 * std::runtime_error for perfectly ORDINARY, expected reasons: a
 * typo'd module name with no matching .so/.dll, a module file
 * that exists but is missing an expected export, etc.
 *
 * This function runs EXCLUSIVELY on the loader's own single
 * ordering thread (LoaderStream's consumer), which services
 * attachModule for the ENTIRE remaining lifetime of the process.
 * Letting an exception escape this function past on_event does
 * not just fail this one request -- it terminates the ordering
 * thread outright (an uncaught exception unwinds to the top of
 * THAT thread's own call stack, finds no handler, and calls
 * std::terminate() -- "Aborted"), permanently breaking every
 * future Load/Resolve/Destroy/AddTag/ChangeModule call for the
 * rest of the process. Critically, this can NEVER be caught by
 * any caller's own try/catch around its own blocking evt() call
 * (ResolveEvent::operator()(), LoadEvent::operator()(), etc.) --
 * those just enqueue onto this same ordering thread and spin on
 * an atomic; the throw happens on a DIFFERENT thread than the
 * one spinning, and a try/catch can only ever catch an exception
 * thrown on its own thread. This is exactly what crashed the
 * REPL on a mistyped module name (Root> exot): ShellREPL.h's own
 * try/catch around ResolveEvent{...}() was never capable of
 * catching this, structurally, no matter how it was written.
 *
 * Deliberately NOT the same class of failure
 * RegisterDynamicLoader's own abort()-on-exception guards against
 * (see that function's own comment, this file) -- that one
 * covers a module that ALREADY dlopen'd successfully turning out
 * to violate a structural invariant (ABI/manifest mismatch, a
 * zombie DLL, genuine OOM), where continuing would silently
 * violate the determinism guarantee this whole system is built
 * on. "The requested module doesn't exist, or is missing an
 * export" is the ordinary, expected failure attachModule's own
 * bool return type already exists to represent -- every single
 * caller in this codebase (resolveImpl, loadImpl,
 * CommandExecutor.h's resolve_module/spawn_entity, ShellREPL.h)
 * already checks that bool and prints a friendly message. The
 * throw here was simply unreachable from any of them; converting
 * it to the same bool contract everything else already expects
 * is what actually makes graceful handling possible -- no change
 * needed on any caller's side at all.
 */
        library_handle_t handle = nullptr;
        try
        {
#ifdef _WIN32
            handle = LoadLibraryA(global_mod->getFilename().c_str());
            if (!handle)
                throw std::runtime_error("Failed to load DLL (error "
                    + std::to_string(GetLastError()) + ") for " + module_name);
#else
            handle = dlopen(global_mod->getFilename().c_str(), RTLD_LAZY | RTLD_LOCAL);
            if (!handle)
                throw std::runtime_error(std::string("Failed to load SO: ") + dlerror());
#endif
            global_mod->adoptLibrary(handle);
            if (!global_mod->registerLoader(*owner)) return false;
            auto arena_it = module_arena_registry.find(module_name);
            if (arena_it != module_arena_registry.end())
            {
                global_mod->module_arena = arena_it->second;
            }
            else
            {
                void* arenaAddr = global_mod->getTagFunction(module_name + "_GetArena");
                if (!arenaAddr)
                {
                    ETCS_LOG("DynamicLoader", "attachModule: module '" << module_name
                             << "' missing '_GetArena' export -- module lifetime "
                             << "features unavailable for it.");
                }
                else
                {
                    using GetArenaFunc = MemoryArena* (*)();
                    global_mod->module_arena = reinterpret_cast<GetArenaFunc>(arenaAddr)();
                    module_arena_registry[module_name] = global_mod->module_arena;
                }
            }
 
            auto cat_reg_it = type_catalog_registry.find(module_name);
            if (cat_reg_it != type_catalog_registry.end())
            {
                global_mod->type_catalog = cat_reg_it->second;
            }
            else
            {
                global_mod->catalogTypes();
                if (global_mod->type_catalog)
                    type_catalog_registry[module_name] = global_mod->type_catalog;
            }
            registerTypeOwnership(module_name, global_mod);
 
            module_registry[module_name] = global_mod;
            ETCS_LOG("DynamicLoader", "Module '" << module_name
                     << "' bootstrapped (global, permanent instance).");
        }
        catch (const ETCS::ManifestMismatchException& mex)
        {
            /*
 * A HEADER:/ONTOLOGY: disagreement, not an ordinary load failure -- see
 * this function's own comment above (the paragraph distinguishing this
 * from "module doesn't exist"). No caller gets a graceful bool for this
 * one; determinism is already violated the moment two builds that
 * disagree on the contract both keep running.
 *
 * unmapLibrary rather than a bare close: discoverTags() runs inside
 * registerLoader() BEFORE RegisterDynamicLoader (see that reordering),
 * so nothing reaches this catch with module threads actually running
 * today -- but the full teardown stays correct if that ever changes,
 * and its steps no-op cleanly when nothing was started.
 */
            global_mod->unmapLibrary(owner);
            /*
 * TODO(recovery): before giving up, re-fetch whichever of {this loader
 * binary, this module} is older from anticurrententropy.com and retry once.
 * This is a SECURITY boundary, not just a determinism one -- the fetch must
 * be over TLS with the cert chain signed by the ACE root key on both the
 * binary and its source, not the plain LetsEncrypt cert the site uses
 * today (that still needs to be issued/wired up on the site side). An
 * unverified replacement binary is strictly worse than aborting. Not
 * wired in yet -- pending both that signing infrastructure and the
 * release-serving protocol itself -- so every mismatch goes straight to
 * shutdown rather than fetching-and-trusting something unverifiable, or
 * silently continuing on a build that already can't be trusted.
 */
            std::cerr << "FATAL: '" << module_name << "' -- " << mex.what()
                      << " -- loader and module were not built for the same "
                         "epoch. Shutting down." << std::endl;
            std::abort();
        }
        catch (const std::exception& ex)
        {
            /*
 * global_mod itself is simply abandoned here: it was never
 * published into module_registry, so nothing else will ever
 * find or reuse this half-initialized instance -- a later
 * attempt to load the same module_name bootstraps a fresh
 * global_mod from scratch, as if this attempt never
 * happened. Its own memory is arena-allocated and reclaimed
 * the same way any other unused arena allocation already is,
 * at process teardown.
 *
 * The one thing that DOES need explicit cleanup: if dlopen/
 * LoadLibrary itself succeeded before a LATER step threw
 * (registerLoader's own discoverTags, or catalogTypes' own
 * discoverActions), `handle` is a real, open library handle
 * that must be torn down here, and the handle nulled, or
 * ~Module() eventually running on this abandoned instance at
 * process teardown would close it AGAIN.
 *
 * unmapLibrary, not a bare close: reaching this catch from
 * catalogTypes/discoverActions means registerLoader ALREADY
 * completed, so the module's ordering thread is running and its
 * ridMap rows are already absorbed. Closing bare stranded the
 * thread (dlclose then hangs joining it in ~EventStream) and left
 * those rows pointing into unmapped memory. A missing _Make or
 * _List export on a declared tag is enough to get here.
 */
            global_mod->unmapLibrary(owner);
            ETCS_LOG("DynamicLoader", "attachModule: failed to load module '"
                     << module_name << "' -- " << ex.what());
            return false;
        }
    }
 
    // Every attach, bootstrap or not, is now structurally just a proxy.
    entity.module().parent = global_mod;
 
    /*
 * Register any Root attaching here as a hand-off candidate for this
 * module, regardless of path (bootstrap, ordinary resolve, or
 * Root::changeModule()) -- previously only changeModuleImpl did this,
 * leaving ordinarily-attached Roots invisible to findRootCandidate.
 * Only reached once per (root, module): already_valid above returns
 * early on every later attach for the same pair.
 */
    if (entity.kind == LifetimeOwner::Kind::Root)
        registerRoot(module_name, &entity.asRoot());
 
    /*
 * Claim the lifetime-owner slot iff it's currently vacant -- either
 * this is the very first entity/Root ever to touch this module
 * (right after the bootstrap above), or the previous owner's own
 * ~Module() already vacated it (a RequestUnloadEvent either already
 * fired or about to be) and this attach is what saves it from
 * actually unloading: RequestUnloadEvent's own delayed recheck will
 * see lifetime_owner non-vacant again and do nothing.
 */
    if (!global_mod->lifetime_owner)
    {
        entity.module().is_lifetime_owner = true;
        global_mod->lifetime_owner        = entity;
        ETCS_LOG("DynamicLoader", "Module '" << module_name
                 << "' lifetime_owner claimed by entity RID:" << entity.getRID());
    }
    else if (global_mod->lifetime_owner.kind == LifetimeOwner::Kind::Root
             && entity.kind == LifetimeOwner::Kind::Entity)
    {
        /*
 * Explicit hand-off, not a search result: the current owner
 * being Root-kind means it was never spawned through the
 * ontology dispatch system at all -- i.e. it's a bootstrap
 * entity, not a real, dispatchable type. This is the direct,
 * structural version of a check that used to be inferred from a
 * side-channel string field (whether the current owner's own
 * source tag was empty) -- now that Root and Entity are
 * genuinely distinct types rather than one polymorphically
 * masquerading as the other, "is the current owner a bootstrap
 * Root" is something LifetimeOwner::kind can just say directly,
 * with no heuristic involved.
 *
 * Root always lives either on the stack or wherever its own
 * caller constructed it (never in any module's own arena), while
 * every real entity spawned from this module lives in the
 * MODULE's own arena -- two entirely disjoint homes. That means
 * a bootstrap Root owner can NEVER be discovered as a sibling by
 * the arena-level search in MemoryArena's own run_entity_delete
 * callback, and, symmetrically, nothing in the module's own
 * arena could ever be found from root_registry's side either.
 * Leaving the token with the Root until its own destruction
 * would mean promoteOrVacate has nothing to search and no
 * sibling to find, vacating the module while THIS entity (which
 * genuinely was spawned/dispatched) is still alive and depending
 * on it. So the transfer happens here instead, unconditionally,
 * the moment any real, dispatched entity ever attaches while a
 * Root still holds the token -- this only ever fires once,
 * since after the first real entity takes over, lifetime_owner's
 * own kind is no longer Root and this branch can never match
 * again.
 */
        global_mod->lifetime_owner.module().is_lifetime_owner = false;
        entity.module().is_lifetime_owner = true;
        global_mod->lifetime_owner        = entity;
        ETCS_LOG("DynamicLoader", "Module '" << module_name
                 << "' lifetime_owner handed off from bootstrap Root to entity RID:"
                 << entity.getRID());
    }
 
    if (!spawn_tag.empty())
    {
        /*
 * Only ever reached when entity holds a genuine Entity -- every
 * Root-taking call site (changeModuleImpl, the bootstrap paths
 * in loadImpl/addTagImpl) always passes spawn_tag == "".
 * asEntity() asserts that invariant rather than silently
 * assuming it.
 */
        Entity& real = entity.asEntity();
        real.setModuleSource(ETCS::Buffer(spawn_tag.c_str()), ETCS::Buffer(module_name.c_str()));
        auto& catalog = real.module_.catalog();
        auto cat_it = catalog.find(spawn_tag);
        if (cat_it != catalog.end())
        {
            real.addTag(cat_it->second);
            /*
 * Passive-edge terminus for a root-level entity, set HERE rather
 * than at each spawn site, because this is the one point every
 * root-level entity provably passes through: loadImpl's anchored
 * branch (after ModuleBundle::operator()()), loadImpl's bootstrap
 * branch (after a raw _Make, which bypasses ModuleBundle
 * entirely), and spawn<T>'s prebuilt path via
 * on_event's Kind::Load. Wiring it at each of those instead would
 * be three places to keep in agreement, and the bootstrap branch
 * is exactly the one an earlier pass missed.
 *
 * Idempotent by construction: a second attach for the same pair
 * returns early at the already_valid check far above, so this
 * only ever runs once per entity, and setProvider is a plain
 * assignment regardless.
 *
 * cat_it->second is the catalog's own persistent entry -- the
 * same object addTag just took a reference to, for the same
 * reason (see Entity::TagEntry::bundle's own comment: allocated
 * once from the module's own arena, never moved or recreated
 * across a module-scope hand-off). Its ctx carries this Module's
 * local authority with `up` wired to RootSignalContext()
 * (Module::getTagAddress), so the passive edge terminating here
 * still reaches the global flags -- SignalContext::walk crosses
 * onto the active edge at this node without either side needing
 * to arrange it.
 */
            real.getContext().setProvider(&cat_it->second.ctx);
        }
        else
        {
            /*
 * Now a hard failure, not a note. Every entity must be reachable
 * by a signal; an entity with no provider and no dispatch table
 * is one that will sit through an interrupt indefinitely, and
 * this branch is the only way a root-level entity can end up
 * that way. attachModule's own bool contract already exists for
 * exactly this -- every caller (resolveImpl, loadImpl,
 * CommandExecutor.h's resolve_module/spawn_entity, ShellREPL.h)
 * checks it and reports gracefully, and both loadImpl call sites
 * return nullptr on false, so the unreachable entity is never
 * handed back to anyone.
 */
            ETCS_LOG("DynamicLoader", "attachModule: '" << spawn_tag
                     << "' not in " << module_name << "'s catalog after load "
                     << "-- refusing to return an entity with no dispatch "
                     << "table and no signal provider.");
            return false;
        }
    }
 
    return true;
}
 
/*
 * changeModuleImpl - Root::changeModule()'s own ordering-thread handler,
 * also reused (with an empty target_module) by ~Module()'s own
 * Root-destruction branch to relinquish a module without reattaching
 * anywhere.
 *
 * target_module == "" means relinquish-only: give up whatever module
 * `root` currently holds, with the same survivor search and
 * promoteOrVacate() call as the reattach case below, just skipping the
 * final attachModule/registerRoot step.
 *
 * The survivor search here is root_registry's own reason to exist (see
 * EventNode.h): an ordinary arena-resident entity dying while holding a
 * module's lifetime token gets a sibling search over its own arena's
 * dtor chain for free (registerDtor<T>'s run_entity_delete callback).
 * Root is typically stack-allocated and so never appears in any arena's
 * dtor chain - without this parallel, explicit registry, a module with
 * two live Roots attached to it would incorrectly vacate the instant
 * EITHER ONE gave it up or destructed, even while the other is still
 * actively using it.
 */
void ETCS::EventNode::LoaderStream::changeModuleImpl(
    ETCS::Root* root, const std::string& target_module)
{
    if (root->module_.parent != nullptr)
    {
        const std::string current_module = root->module_.parent->name;
 
        if (current_module == target_module)
            return; // already there
 
        /*
 * promoteOrVacate no-ops if this Root's token isn't the elected
 * owner, so this is safe to call unconditionally.
 */
        Root* survivor = findRootCandidate(current_module, root);
        root->module_.promoteOrVacate(survivor); // Root* -> LifetimeOwner
        unregisterRoot(current_module, root);
 
        /*
 * Fully vacate -- lets the reattach below proceed, and leaves
 * root in the same state a fresh, never-attached Root starts in.
 */
        root->module_.parent            = nullptr;
        root->module_.is_lifetime_owner = false;
    }
 
    if (target_module.empty())
        return; // relinquish-only
 
    /*
 * attachModule registers root itself on success now -- no separate
 * call needed here.
 */
    attachModule(target_module, root, ""); // Root* -> LifetimeOwner
}
 
/*
 * entityUnloadImpl - THE Kind::EntityUnload handler. Determines target's
 * correct parent arena and delegates to MemoryArena::deleteEntity, which
 * is the ONLY thing that actually runs target's own destructor at all
 * now (via its own run_entity_delete callback -- see registerDtor<T>'s
 * own comment, MemoryArena.h). No root-vs-child branching needed here
 * anymore: deleteEntity's own callback handles both cases internally
 * (global-scope election-and-evoke, or non-global reparent-and-evoke),
 * keyed off target->getParent() itself, which is exactly what determines
 * which arena is "correct" here too. Always a genuine Entity* -- Root
 * never reaches this at all (see EntityUnloadEvent's own comment,
 * EventNode.h).
 *
 *   GLOBAL SCOPE (parent_ == nullptr): target's own parent arena is the
 *   MODULE's own global arena -- module_.parent->module_arena, resolved
 *   once at bootstrap and safe to read from any thread/side, unlike
 *   calling MemoryArena::getInstance() directly from loader-compiled
 *   code (this function's own compilation context), which would resolve
 *   to the LOADER's own arena instead of whichever module target
 *   actually lives in -- the same cross-DSO hazard this session traced
 *   and fixed elsewhere for event routing.
 *
 *   CHILD (parent_ != nullptr): target's own parent arena is simply
 *   target->getParent()->getArena() -- no module involvement needed at
 *   all, since only root-level entities are ever module-lifetime-
 *   relevant.
 */
void ETCS::EventNode::LoaderStream::entityUnloadImpl(ETCS::Entity* target, bool delete_children)
{
    MemoryArena* parentArena = target->getParent()
        ? &target->getOwningArena()
        : (target->module_.parent ? target->module_.parent->module_arena : nullptr);
 
    if (!parentArena) return;
    parentArena->deleteEntity(target, delete_children);
}
 
/*
 * requestUnloadImpl - THE Kind::RequestUnload delayed-recheck handler.
 * Runs 100ms after promoteOrVacate() found no survivor at all and vacated
 * target->lifetime_owner (see RequestUnloadEvent's own comment for why
 * that delay happens via a detached thread rather than blocking this
 * ordering thread). Re-verifies lifetime_owner is STILL vacant before
 * doing anything irreversible: if some attachModule call claimed it in
 * the meantime (a fresh spawn from this same module, during the 100ms
 * window), this is correctly a no-op -- the module stays loaded, nothing
 * here contradicts that later claim.
 *
 * The actual unload, when it does proceed: erase every registry entry
 * that pointed at this instance (module_registry, module_arena_registry,
 * type_catalog_registry -- all three, since target's own arena and type
 * catalog are about to be genuinely unmapped along with the library
 * itself), then cleanupModule()/dlclose(), mirroring exactly what
 * Module's own destructor used to do directly before this whole
 * mechanism existed.
 */
void ETCS::EventNode::LoaderStream::requestUnloadImpl(ETCS::Module* target)
{
    if (target->lifetime_owner)
    {
        ETCS_LOG("DynamicLoader", "Module '" << target->name
            << "' RequestUnload recheck: lifetime_owner reclaimed during "
               "the delay -- staying loaded.");
        return;
    }
 
    /*
 * Second resolution moment, now covering Roots too: root_registry
 * holds EVERY Root that ever attached to this module (attachModule's
 * own registerRoot call, unconditional -- not only whichever one
 * happened to claim ownership), so an ordinary proxying Root that
 * was never the owner -- e.g. ShellREPL's own nav_root, still open
 * on some stack -- is exactly as valid a rescue candidate here as
 * one that was previously promoted and later gave the token back.
 * Safe here, unambiguously, because this function only ever exists
 * inside the loader binary (see promoteOrVacate's own comment,
 * DynamicLoader.h, for why that's the property that actually
 * matters).
 */
    if (Root* survivor = findRootCandidate(target->name, nullptr))
    {
        survivor->module_.parent            = target;
        survivor->module_.is_lifetime_owner = true;
        target->lifetime_owner              = survivor;
        ETCS_LOG("DynamicLoader", "Module '" << target->name
            << "' RequestUnload recheck: promoting already-registered Root "
               "RID:" << survivor->getRID() << " -- staying loaded.");
        return;
    }
 
    const std::string module_name = target->name;
    ETCS_LOG("DynamicLoader", "Module '" << module_name
        << "' RequestUnload recheck: still vacant after the delay -- "
           "unloading now.");
    // Loader-side bookkeeping first: nothing new can find this module while
    // its mapping is being torn down. (The signal-raise that used to sit here
    // is unmapLibrary's step 1 now -- see its comment for why it is
    // module-scoped rather than process-wide.)
    module_registry.erase(module_name);
    module_arena_registry.erase(module_name);
    type_catalog_registry.erase(module_name);

    /*
 * The ridMap purge, _Cleanup and close that used to be open-coded here are
 * now unmapLibrary's steps 2-4 (Bundles.h) -- same sequence, one copy, so
 * attachModule's failure paths get it too. The registry erases above stay
 * here: they are the LOADER's bookkeeping, not the module's mapping.
 */
    target->unmapLibrary(owner);
}
 
/*
 * sendAckIfNeeded - see its own declaration comment (EventNode.h). A
 * no-op if evt.reply_to is null (loader-originated call). Otherwise
 * enqueues a lightweight Kind::Ack DLInEvent onto reply_to->stream
 * (the ORIGINATING module's own ordering thread) and blocks on it --
 * stack-allocated here, safe because this function doesn't return until
 * the wait is over, exactly the same pattern every blocking event in
 * this file already relies on. If reply_to's own stream refuses the
 * enqueue (already cleaning up -- e.g. that module is mid-teardown right
 * now), there is nothing left alive to ever set ack_done, so this
 * returns immediately rather than hanging.
 */
void ETCS::EventNode::LoaderStream::sendAckIfNeeded(DLInEvent& evt)
{
    if (!evt.reply_to) return;
    /*
 * Heap-allocated, deliberately -- the identical reasoning
 * RequestUnloadEvent::operator()() already documents: enqueue() copies only
 * the 8-byte POINTER, never the event's own contents, and nothing blocks
 * here any more, so a stack-allocated DLInEvent would already be dead by
 * the time the consuming ordering thread ever read it. ModuleProxy::
 * on_event's own Kind::Ack case is this allocation's terminus.
 *
 * NO LONGER BLOCKS on an ack_done flag. That wait was a synchronous,
 * cross-stream round-trip performed from INSIDE on_event, and it deadlocked
 * outright: this ordering thread sat here waiting for an Ack the module's
 * own ModuleProxy could not reach, because ModuleProxy was itself parked
 * waiting for an earlier sequence that this very in-flight event was still
 * holding open -- two ordering threads each waiting on the other, with the
 * originating caller spinning on its own completion flag behind them (a
 * real, reproduced hang: 1.5M progressiveYield spins into the watchdog).
 * The ack is a sync point for the RECEIVING side; the loader never needed
 * its completion, so it no longer waits for one.
 */
    DLInEvent* ack_evt = new DLInEvent();
    ack_evt->kind = DLInEvent::Kind::Ack;
    if (!evt.reply_to->stream.enqueue(DLInEventPtr{ack_evt}))
        delete ack_evt; /*
 * refused (that module is already cleaning up) --
 * nothing will ever consume this, so free it here
 */
}
 
/*
 * resolveImpl - attaches a module to the given entity's or Root's own
 * .module_ slot, with no spawn_tag: proxy onto the live anchor if one
 * exists, or attachModule's bootstrap path if vacant. A thin wrapper now
 * that both "resolve" and "spawn" ultimately go through the same, single
 * attach path - there is no separate transient-browse mechanism anymore
 * (bindModule, formerly here, is gone). A resolve-only call (no type
 * being spawned from the module) is exactly attachModule with an empty
 * spawn_tag: attachModule's own tail (entity.setModuleSource/addTag) is
 * gated on spawn_tag being non-empty, so passing "" here correctly
 * attaches the module without instantiating any specific type from it.
 */
bool ETCS::EventNode::LoaderStream::resolveImpl(const std::string& module_name, ETCS::LifetimeOwner entity)
{
    return attachModule(module_name, entity, "");
}
 
 
/*
 * loadImpl - registry-first. Anchored -> spawn through the live anchor's
 * catalog (ModuleBundle::operator()(), which constructs the entity and
 * wires its dispatch table), THEN attachModule with an empty spawn_tag
 * to populate the new entity's own module_ (proxy onto the existing
 * global instance, claiming lifetime_owner if it happens to be vacant at
 * that moment -- see attachModule's own comment). Vacant -> bootstrap
 * against bootstrap_root first (the chicken-and-egg solution: the module
 * needs to be dlopen'd before Make() can even be resolved, and
 * attachModule's bootstrap path needs SOME entity/Root to run against
 * before any real entity of the requested tag exists). bootstrap_root
 * can be either kind -- it's the caller's job to supply one (see
 * LoadEvent::root / spawn_entity, CommandExecutor.h, which in current
 * practice always supplies a Root; addTagImpl's own vacant branch
 * supplies a genuine Entity via getRootAncestor() instead). Once the
 * module's global instance exists, the real entity is Make()'d and
 * attachModule'd exactly the same way the already-anchored branch above
 * does, with the same lifetime_owner claim logic applying uniformly
 * either way.
 */
ETCS::Entity* ETCS::EventNode::LoaderStream::loadImpl(
    const std::string& conjugate_key, ETCS::LifetimeOwner bootstrap_root)
{
    auto [module_name, tag_type] = parseConjugateOriginKey(conjugate_key);
 
    auto reg_it = module_registry.find(module_name);
    if (reg_it != module_registry.end() && reg_it->second != nullptr)
    {
        Module* mod = reg_it->second;
        auto& catalog = mod->catalog();
        auto cat_it = catalog.find(tag_type);
        if (cat_it == catalog.end())
        {
            ETCS_LOG("DynamicLoader", "loadImpl: '" << tag_type
                     << "' not found in " << module_name << "'s type_catalog.");
            return nullptr;
        }
        ETCS::Entity* result = cat_it->second();   // existing spawn path
        if (!result) return nullptr;
        /*
 * ModuleBundle::operator()() above already constructed the
 * entity and wired its dispatch table (setModuleSource/addTag) --
 * it does NOT touch the entity's own module_ member at all (that
 * predates Module becoming an Entity member). Without this call,
 * result->module_ stays completely vacant -- no library_handle,
 * no module_arena, nothing -- which is exactly what a stream
 * dispatch reading module_arena/signal wiring out of it would
 * segfault on. attachModule with an empty spawn_tag here only
 * populates module_ (proxy onto the existing anchor, or triggers
 * the transfer if the current owner is a Root-hosted bootstrap
 * still pending one) -- it never re-adds the tag, since that
 * already happened.
 */
        if (!attachModule(module_name, result, "")) return nullptr;
        return result;
    }
 
    /*
 * Vacant - bootstrap against the caller-supplied entity/Root
 * (spawn_tag "" skips dispatch wiring entirely; bootstrap_root isn't
 * actually of type tag_type, it's just hosting the module until a
 * real entity attaches).
 */
    if (!bootstrap_root)
    {
        ETCS_LOG("DynamicLoader", "loadImpl: '" << module_name
                 << "' is vacant and no bootstrap_root was supplied -- "
                 << "cannot load. Caller must provide an entity/Root to "
                 << "bootstrap against (see LoadEvent::root).");
        return nullptr;
    }
    if (!attachModule(module_name, bootstrap_root, "")) return nullptr;
 
    auto reg_it2 = module_registry.find(module_name);
    if (reg_it2 == module_registry.end() || reg_it2->second == nullptr) return nullptr;
    Module* mod = reg_it2->second;
 
    void* makeAddr = mod->getTagFunction(tag_type + "_Make");
    if (!makeAddr)
    {
        ETCS_LOG("DynamicLoader", "loadImpl: no '" << tag_type
                 << "_Make' in " << module_name);
        return nullptr;
    }
    using MakeFuncResolver = MakeFunc (*)();
    MakeFunc make = reinterpret_cast<MakeFuncResolver>(makeAddr)();
    ETCS::Buffer tagbuff;
    tagbuff.writeString(tag_type.c_str());
    ETCS::Entity* entity = make(tagbuff);
    if (!entity)
    {
        ETCS_LOG("DynamicLoader", "loadImpl: Make for '"
                 << tag_type << "' returned null.");
        return nullptr;
    }
 
    /*
 * Populates entity's own module_ (proxy onto the global instance --
 * bootstrap_root's own module_ may or may not have claimed
 * lifetime_owner already; either way this entity ends up correctly
 * attached, claiming lifetime_owner itself only if it's still
 * vacant). attachModule returns success/failure now, so the entity
 * pointer this function already holds is what gets returned, not
 * whatever attachModule itself would have handed back.
 */
    if (!attachModule(module_name, entity, tag_type)) return nullptr;
    return entity;
}
 
/*
 * destroyImpl - entity-granularity. conjugate_key is the same "module:tag"
 * form registerLoader's absorption already uses, e.g. "WindowProvider:Window".
 *
 * Looks up the RIDListHandle the loader already holds for that type (placed
 * there by registerLoader when the module was first resolved), fetches the
 * live Entity* BEFORE removing it from tracking, then delegates to
 * MemoryArena::deleteEntity on its correct parent arena -- the same
 * determination entityUnloadImpl uses (root: module_.parent->module_arena;
 * child: getParent()->getArena()). This is the actual fix for a real gap:
 * an earlier version of this function only ever removed the RID from the
 * module's own RIDList tracking, and NOTHING then triggered the entity's
 * own destructor at all -- the underlying object just sat in memory,
 * fully intact, until the whole arena eventually tore down on its own,
 * however much later that happened to be (sometimes not until process
 * exit). deleteEntity is what actually runs it now.
 */
bool ETCS::EventNode::LoaderStream::destroyImpl(const std::string& conjugate_key, ETCS::RID rid,
                                                 bool delete_children)
{
    ETCS::Buffer key(conjugate_key);
    auto it = owner->ridMap.find(key);
    if (it == owner->ridMap.end())
    {
        ETCS_LOG("DynamicLoader", "destroyImpl: no RIDList registered for: " << conjugate_key);
        return false;
    }
 
    if (!it->second.invoke_contains(rid))
    {
        ETCS_LOG("DynamicLoader", "destroyImpl: RID " << rid << " not found in " << conjugate_key);
        return false;
    }
 
    ETCS::Entity* target = it->second.invoke_get(rid);
    /*
 * Fan OUT of every aggregate this entity was fanned INTO. An entity is
 * inserted into its own per-tag RIDList (the conjugate_key one below) AND
 * into one aggregate list per supertype family it declares -- Deletable,
 * Ephemeral, ConnectionState, and so on (ETCS_SUPERTYPE_BASE publishes
 * those under the bare family name; etcs_supertype_fanout inserts on
 * construction). Only the per-tag removal existed, so every aggregate kept
 * a permanent node holding a pointer into an arena that is about to be
 * reclaimed.
 *
 * Two consequences, and the second is worse than the leak: those lists grew
 * without bound in the MODULE's root arena (measured at ~94MB over six
 * hours of one polling page), and every entry past the first was a dangling
 * Entity*, so anything iterating an aggregate walked freed memory.
 *
 * The node bytes DO come back once erased -- ArenaAllocator::deallocate
 * pushes to the arena's free list and every node is identically sized, so
 * the next insert reuses them. Reclamation was never missing; the removal
 * was. Done BEFORE the per-tag remove so `target` is still resolvable.
 */
    if (target)
    {
        /*
 * conjugate_key is "Module:Tag" -- split here rather than adding a
 * parameter, since on_event's own parse (Kind::Destroy) is a different
 * scope and this is the only other place that needs the module half.
 */
        const size_t colon = conjugate_key.find(':');
        const std::string module_name =
            (colon == std::string::npos) ? conjugate_key : conjugate_key.substr(0, colon);
        std::vector<ETCS::Buffer> type_tags;
        target->getTags(type_tags);
        for (const ETCS::Buffer& t : type_tags)
        {
            ETCS::Buffer agg_key(module_name + ":" + t.toString());
            if (agg_key == key) continue;              // the per-tag list, handled below
            auto agg = owner->ridMap.find(agg_key);
            if (agg == owner->ridMap.end()) continue;  // not an aggregate this module publishes
            if (agg->second.invoke_remove(rid))
                ETCS_LOG("DynamicLoader", "destroyImpl: removed RID " << rid
                         << " from aggregate " << agg_key.toString());
        }
    }
    bool removed = it->second.invoke_remove(rid);
    ETCS_LOG("DynamicLoader", "destroyImpl: removed RID " << rid << " from " << conjugate_key
        << " -> " << (removed ? "ok" : "failed"));
 
    if (removed && target)
    {
        MemoryArena* parentArena = target->getParent()
            ? &target->getOwningArena()
            : (target->module_.parent ? target->module_.parent->module_arena : nullptr);
 
        if (parentArena) parentArena->deleteEntity(target, delete_children);
    }
 
    return removed;
}
 
/*
 * addTagImpl - gives the child a real dispatch table before the trampoline
 * runs its typed_children_/module-registry bookkeeping, mirroring exactly
 * what ModuleBundle::operator()() already does for a top-level spawn
 * (result->addTag(tag, *this)) -- addTag<T>-created children were never
 * getting this. Without it, an addTag<T>-created entity's own `tags` map
 * stayed permanently empty: hasTag()/call()/getAllActions() all read that
 * map, so every addTag<T>-created entity (e.g. a per-connection
 * ConnectionState spawned by HTTPParser::Listen, per its own doc comment)
 * was fully visible and correctly counted via `list`/the module-level
 * RIDList registry the trampoline DOES populate, but completely uncallable
 * via .etcs or the REPL action menu.
 *
 * Resolution goes through type_owner_index (bare tag -> owning module
 * name), NOT through the parent's own module -- an earlier version of this
 * fix assumed the child shares its parent's module, which is wrong for any
 * cross-module addTag<T> relationship. The type itself is the correct key
 * into this machinery, exactly as it already is everywhere else in this
 * file (loadImpl, isTypedActionStream).
 * addTagImpl - gives the child a real dispatch table AND full module
 * identity (source/source_tag/source_module_) before the trampoline runs,
 * so an addTag<T> child is a first-class module citizen from birth:
 * dispatchable, menu-visible, staleness-checkable, identical to a
 * top-level spawn. This is what closes the original SocketConnectionState
 * REPL-invisibility bug - an addTag<T>-created connection is now callable
 * via .etcs and the REPL action menu, not merely list-visible.
 *
 * The TYPE is the resolution key (type_owner_index: bare tag -> owning
 * module), never the parent's module - cross-module addTag<T> is valid.
 * On a vacant registry we call attachModule DIRECTLY: we are already
 * on the ordering thread, so an event round-trip to ourselves would
 * deadlock (contrast Entity::operator delete's root case, which fires
 * EntityUnloadEvent precisely because it runs off-thread).
 *
 * Uses `tag` (== T::TAG, set once at the addTag<T> call site - see
 * Entity.h) directly for the type_owner_index/catalog lookup, NOT
 * child->myTag(). These can differ in principle - myTag() is the
 * concrete implementation identity (e.g. "GLFWWindow"), while `tag` is
 * the contract identity the dispatch system is actually keyed by (e.g.
 * "Window") - even though for typical addTag<T> children (no cross-
 * platform alias indirection) they happen to coincide. Using `tag`
 * directly is correct regardless of whether they coincide, and it's
 * already sitting right here as a parameter - no re-derivation needed.
 *
 * parent->getRootAncestor() below is always a genuine Entity* (walks
 * Entity's own parent_ chain, which Root was never part of), so the
 * bootstrap attachModule call here always passes a real Entity, never a
 * Root -- unlike loadImpl's own vacant branch, which in current practice
 * always bootstraps against a Root instead.
 */
ETCS::RID ETCS::EventNode::LoaderStream::addTagImpl(
    ETCS::Entity* parent, ETCS::Entity* child,
    const ETCS::Buffer& tag, ETCS::AddTagEvent::Trampoline trampoline)
{
    std::string child_type_tag = tag.toString();
 
    auto owner_it = type_owner_index.find(child_type_tag);
    if (owner_it != type_owner_index.end())
    {
        const std::string& mod_name = owner_it->second;
        auto reg_it = module_registry.find(mod_name);
        Module* mod = (reg_it != module_registry.end()) ? reg_it->second : nullptr;
 
        if (mod == nullptr)
        {
            /*
 * Vacant: bootstrap the module against parent's own ultimate
 * ancestor first (spawn_tag "" skips dispatch wiring - that
 * ancestor isn't of this type). No separate "root" concept
 * needed: whatever getRootAncestor() reaches becomes the
 * global instance's first lifetime_owner claimant, exactly
 * like any other first attach. child itself then gets its
 * own module_ populated as an ordinary proxy via the second
 * attachModule call below - a typed child is never itself
 * eligible to claim lifetime_owner (see attachModule's own
 * comment on why only root-level entities are election-
 * visible at all).
 */
            attachModule(mod_name, parent->getRootAncestor(), "");
            attachModule(mod_name, child, child_type_tag);
        }
        else
        {
            auto& catalog = mod->catalog();
            auto cat_it = catalog.find(child_type_tag);
            if (cat_it != catalog.end())
            {
                child->setModuleSource(ETCS::Buffer(child_type_tag.c_str()), ETCS::Buffer(mod_name.c_str()));
                child->addTag(cat_it->second);
                /*
 * Populate child's own module_ as an ordinary proxy onto
 * the already-loaded global instance -- a plain pointer
 * assignment, deliberately NOT the full attachModule()
 * (which also runs lifetime_owner claim logic with no
 * way to suppress it for a typed child specifically). A
 * typed/addTag<T> child must never become lifetime_owner:
 * registerDtor<T>'s own non-global branch (MemoryArena.h)
 * reparents-or-cascades a child on destruction and NEVER
 * calls promoteOrVacate -- if a child ever held
 * lifetime_owner, destroying it would leave
 * global_mod->lifetime_owner permanently dangling, with
 * nothing left to ever clear it.
 */
                child->module_.parent = mod;
            }
            else
                ETCS_LOG("DynamicLoader", "addTagImpl: '" << child_type_tag
                         << "' owned by '" << mod_name
                         << "' but missing from its type_catalog.");
        }
    }
    else
    {
        ETCS_LOG("DynamicLoader", "addTagImpl: no loaded module provides type '"
                 << child_type_tag << "' -- child will have no callable actions.");
    }
 
    return trampoline(parent, child, tag);
}
 
bool ETCS::EventNode::LoaderStream::isTypedActionStream(
    const std::string& origin, const std::string& conjugate_key)
{
    auto [type, action] = ETCS::Entity::parseConjugateActionKey(conjugate_key);
    auto it = module_registry.find(origin);
    Module* mod = (it != module_registry.end()) ? it->second : nullptr;
    if (!mod) { return false; }
    auto& catalog = mod->catalog();
    auto cat_it = catalog.find(type.toString());
    bool stream = cat_it != catalog.end()
               && cat_it->second.isActionStream(action);
    ETCS_LOG("DynamicLoader", "isTypedActionStream: " << stream
        << " for: " << origin << "." << conjugate_key);
    return stream;
}
 
#endif // ETCS_LOADER
 
 
/*
 * -- ModuleProxy::on_event body ------------------------------------------------
 * Module scope only. Three cases now: TagModify is handled LOCALLY
 * (never forwarded - see TagModifyEvent's own comment for why); Ack is
 * the loader's own sync-back after finishing a memory-altering event for
 * this module (see DLInEvent::reply_to's own comment) - just signals
 * done, no actual work; anything else falls through to a forwarding path
 * that's now effectively dead code for the five memory-altering event
 * kinds (Load/Resolve/Destroy/AddTag/EntityUnload), since each of THEIR
 * own operator()() methods enqueues directly onto getLoader().stream now,
 * never through this module's own stream at all - see each one's own
 * comment for why routing through a possibly-already-stopped local
 * stream first was the actual cause of a real hang this session traced
 * and fixed. The fallback remains only as a safety net for anything else
 * that might still target EventNode::getInstance().stream directly.
 */
#ifndef ETCS_LOADER
 
ETCS::DispatchResult ETCS::EventNode::ModuleProxy::on_event(
    DLState&, const DLInEventPtr& ref, uint64_t)
{
    DLInEvent& evt = *ref.ptr;
 
    if (evt.kind == DLInEvent::Kind::TagModify)
    {
        /*
 * The ONE place a genuine TAG-scope mask governs admission: this
 * stream is the emitting module's own, so evt.tagmodify_mask means here
 * what it meant at the call site. mask_for returns it directly
 * (EventNode.h), so by now the slot is already admitted under it.
 *
 * Also the path ScopeTag's addTag/removeTag take -- module-compiled, so
 * EventNode::getInstance() resolves to THIS ModuleProxy -- which is
 * what lets a stream pair's tag mask reach a buffer that can read it.
 */
        evt.tagmodify_impl(evt.tagmodify_target, evt.conjugate_key, evt.tagmodify_is_remove);
        return {ETCS::DispatchKind::Inline, &evt};
    }
 
    if (evt.kind == DLInEvent::Kind::Ack)
    {
        /*
 * The loader's own sync-back after finishing one of Load/Resolve/
 * Destroy/AddTag/EntityUnload for this module (see DLInEvent::
 * reply_to's own comment, EventNode.h, and sendAckIfNeeded's own
 * comment, DynamicLoader.h). Nothing to do but signal done - its
 * entire purpose is being ordered relative to whatever this
 * module's own ordering thread processes next, not any actual
 * work.
 */
        ETCS_LOG("ModuleProxy", "Ack received from loader.");
        /*
 * Heap-allocated by sendAckIfNeeded; this is its terminus. Nothing
 * waits on an ack any more (see that function's own comment), so there
 * is no completion flag to store -- being ordered on THIS module's own
 * ordering thread was always the ack's entire purpose.
 */
        delete &evt;
        return {ETCS::DispatchKind::Inline, nullptr};
    }
 
    /*
 * Fallback path only -- Load/Resolve/Destroy/AddTag/EntityUnload no
 * longer route through here at all (they enqueue directly onto
 * getLoader().stream from their own operator()(), regardless of
 * which side fires them -- see each one's own comment for why routing
 * through this module's own, possibly already-stopped stream first
 * was the actual cause of a real hang this session traced and fixed).
 * This forward remains only for whatever else might still enqueue
 * onto EventNode::getInstance().stream directly on the module side.
 */
    getLoader().stream.enqueue(ref);
    /*
 * No completion HERE: the event is the loader's now, and its on_emit
 * releases the caller when the slot commits over there. Releasing on this
 * side too would hand the caller back before the work it waits on has even
 * been admitted.
 *
 * The slot was still admitted under all() (mask_for, EventNode.h) -- a
 * hand-off's effects land in a scope this stream cannot describe, and
 * unknown must not read as independent.
 */
    return {ETCS::DispatchKind::Inline, nullptr};
}
 
#endif
 
 
} // namespace ETCS
 
 
 
// -- Initialization ------------------------------------------------------------
 
#ifdef ETCS_LOADER
inline const bool _core_init = []() {
    WIRE_ROOT_SIGNAL_CONTEXT();
    /*
 * Single producer - LoaderStream consumer is the sole writer of ridMap,
 * active_bundles, and active_modules. Replaces the old shared_mutex.
 */
    ETCS::MemoryArena::getInstance();
    ETCS::ThreadPool::getInstance();
    ETCS::EventNode::getInstance().stream.start(
        ETCS::MemoryArena::getInstance(), 1);
    dynamicLoader.node = &ETCS::EventNode::getInstance();
    return true;
}();

/*
 * ETCS_GetLoaderManifest - the loader's half of the manifest check, exported
 * with default visibility specifically so a dlopen'd module's own
 * static-init can dlsym(RTLD_DEFAULT, ...) it back independently, without
 * the loader calling into the module first. See ETCS_MODULE_EXPORT_MAIN's static-init
 * block (ETCS_API.h) for the module's half -- the two no longer wait on
 * each other; each runs at its own natural moment (this one already ran,
 * as part of the loader's own process start, long before any dlopen;
 * the module's runs the instant dlopen() maps it). Deliberately not
 * declared with ETCS_API: that macro is empty in loader builds (it exists
 * for a module's own DLL exports), and this needs default visibility
 * regardless of -fvisibility=hidden.
 */
extern "C"
#ifdef _WIN32
__declspec(dllexport)
#else
__attribute__((visibility("default")))
#endif
void* ETCS_GetLoaderManifest()
{
    return static_cast<void*>(&ETCS::Entity::getManifest());
}
#endif // ETCS_LOADER

/*
 * RegisterDynamicLoader - the module-side entry point the loader calls
 * immediately after dlopen, handing this module a pointer to the loader's
 * own EventNode. Wrapped in try/catch specifically because this runs
 * across the dlopen boundary (extern "C") - an uncaught exception trying
 * to unwind through that boundary is exactly what "terminate called
 * without an active exception" looks like when the unwinder can't resolve
 * a handler on the other side (see this session's own notes on
 * -fvisibility=hidden hiding RTTI across it).
 *
 * Both catch blocks below CRASH deliberately, rather than logging and
 * returning nullptr the way an earlier version of this did. That earlier
 * behavior let Module::registerLoader detect the null EventNode* and fail
 * the load "gracefully" - but graceful failure here is the wrong instinct:
 * ETCS scripts are meant to execute deterministically, and a module that
 * fails to load for a structural reason (the OS not having finished
 * tearing down a just-unloaded instance of this exact library, or a
 * genuine allocation failure) leaves that guarantee violated the moment
 * execution is allowed to continue past it. Silently returning nullptr
 * converts a hard invariant violation into a soft "module not available"
 * that some later, unrelated line might behave differently around,
 * exactly the kind of thing determinism can't tolerate. Better to stop
 * the whole process here, loudly, with a message that names the actual
 * condition - which also surfaces what amounts to a third invariant this
 * session settled on: a root-hosted Module cannot be created and destroyed
 * repeatedly in a tight cycle; doing so risks racing the OS's own
 * asynchronous post-dlclose teardown, and the crash here is what makes
 * that impossible to do silently rather than merely slow.
 *
 * The manifest check is no longer done here. It used to run as this
 * function's first statement, fed a loader manifest pointer passed in as a
 * second argument -- moved to a static-init lambda in ETCS_MODULE
 * (ETCS_API.h) instead, which dlsym(RTLD_DEFAULT)s ETCS_GetLoaderManifest
 * back on its own the instant dlopen() maps this library. That runs before
 * this function is ever called (dlopen's static-init completes before
 * dlopen() itself returns to the loader), so by the time control reaches
 * here the module has already independently verified the loader, and
 * Module::registerLoader has already independently verified the module
 * (its own discoverTags()/validateManifest() call, before it ever gets to
 * calling this function) -- neither side waited on the other's result, only
 * on dlopen() being the one thing both must have already happened by.
 */
extern "C" ETCS_API ETCS::EventNode* RegisterDynamicLoader(void* ptr)
{
    try
    {
        // Module receives the loader's root EventNode and wires its ModuleProxy to it
        ETCS_LOG("DynamicLoader", "Parsing input EventNode pointer... " << ptr);
        dynamicLoader.node = static_cast<ETCS::EventNode*>(ptr);
        ETCS_LOG("DynamicLoader", "Passed EventNode! " << ptr);
        /*
 * No signal wiring here. Re-registering std::signal() per module used to
 * silently steal OS signal disposition from the loader (process-global,
 * last dlopen wins). The loader instead hands this module its real root
 * SignalContext* via RegisterRootSignalContext() below, called from
 * Module::registerLoader() immediately after this function returns.
 */
        ETCS::MemoryArena::getInstance(); // this may actaully be the real cleanup order
        ETCS_LOG("DynamicLoader", "Passed MemoryArena! ");
        ETCS::ThreadPool::getInstance();
        ETCS_LOG("DynamicLoader", "Passed ThreadPool! ");
        ETCS::EventNode::getInstance().stream.start(
            ETCS::MemoryArena::getInstance(), 1);
        ETCS_LOG("DynamicLoader", "Passed EventNode stream start! ");
        return &ETCS::EventNode::getInstance();
    }
    catch (const ETCS::EventStreamZombieException&)
    {
        std::cerr << "Reloaded DLL " << ETCS_MODULE_NAME
                   << " too many times within a very short period: "
                      "OS generated zombie DLL (zombie thread in EventStream "
                      "detected)" << std::endl;
        std::abort();
    }
    catch (const std::exception&)
    {
        /*
 * Any other exception here - allocation failures inside
 * allocateTunedRing being the expected case - gets treated as
 * out-of-memory rather than distinguished further. Determinism
 * requires stopping regardless of the precise cause; the message
 * is deliberately generic where the zombie case's is specific,
 * since this branch genuinely doesn't know which allocation
 * failed or why.
 */
        std::cerr << "DLL cannot be loaded: Out of memory" << std::endl;
        std::abort();
    }
    catch (...)
    {
        std::cerr << "DLL cannot be loaded: unknown fatal error during "
                      "RegisterDynamicLoader." << std::endl;
        std::abort();
    }
}
 
/*
 * Module-scope only - loader-side counterpart lives in Module::registerLoader()
 * above. Adopts the loader's real, OS-signal-backed root as this module's own
 * RootSignalContext(), unifying global authority across the dlopen boundary.
 */
#ifndef ETCS_LOADER
extern "C" ETCS_API void RegisterRootSignalContext(ETCS::SignalContext* loader_root)
{
    ETCS::AdoptRootSignalContext(loader_root);
}
#endif
 
bool ETCS::drainEntityScopes(ETCS::Entity* target, const char* who)
{
    if (!target) return true;
    if (target->isDestructed()) return true;
    /*
 * Reentrancy guard, same condition ~Entity() already uses. Reached from a
 * module's own cleanupTypedEntities() bulk sweep, we ARE the thread that
 * would have to service the ScopeTag's TagModifyEvent -- waiting here
 * deadlocks outright. The whole module is going down in that case, so
 * proceed, but say so loudly: this is the one path where the guarantee
 * genuinely cannot be provided.
 */
    if (ETCS::EventNode::on_ordering_thread)
    {
        if (ETCS::ScopeTag::anyActive(target))
            ETCS_LOG(who, "WARNING: destroying RID:" << target->getRID()
                     << " with live scopes -- already on an ordering thread, "
                        "cannot drain without deadlocking.");
        return true;
    }
    target->interruptAllScopes();
    int retries = 0;
    while (ETCS::ScopeTag::anyActive(target))
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        if (++retries > 500) // 5s
        {
            ETCS_LOG(who, "ERROR: RID:" << target->getRID()
                     << " still has active scopes after 5s -- REFUSING to destroy. "
                        "A scope is ignoring its own SignalContext.");
            return false;
        }
    }
    /*
 * Deliberately no ctx.isInterrupted() escape in that loop: an interrupt
 * arriving mid-teardown is not a reason to return early and free memory
 * another thread is still reading.
 */
    return true;
}
/*
 * -- Event operator() definitions ---------------------------------------------
 * Defined here - getLoader() and stream are fully resolved.
 * In loader scope: enqueues directly into LoaderStream.
 * In module scope: enqueues into ModuleProxy which re-enqueues into LoaderStream.
 * Either way the loader's consumer is the only fulfiller.
 */
 
inline ETCS::Entity* ETCS::LoadEvent::operator()()
{
    ETCS_ASSERT_NOT_ORDERING_THREAD("LoadEvent");
    DLInEvent evt{};
    evt.kind = DLInEvent::Kind::Load;
    evt.conjugate_key = conjugate_key;
    evt.entity_out = &result;
    evt.prebuilt_entity = prebuilt;
    evt.bootstrap_root = root;
#ifndef ETCS_LOADER
    /*
 * Module-side caller: reply_to lets the loader ack back onto THIS
 * module's own stream after it finishes -- see reply_to's own
 * comment (EventNode.h) for the full ordering-guarantee reasoning.
 */
    evt.reply_to = &ETCS::EventNode::getInstance();
    evt.origin_extra_mask = ETCS::ActivePairModuleMask();
#endif
    /*
 * Straight to the loader's own stream, regardless of which side
 * fires this -- never through this side's own (potentially already
 * torn down) local stream. See EntityUnloadEvent's own comment
 * below, and this session's own notes, for why routing memory-
 * altering events through a possibly-dead local stream first was the
 * actual cause of the hang this fixes.
 */
    if (!getLoader().stream.enqueue(DLInEventPtr{&evt}))
    {
        /*
 * Refused (loader's own stream is cleaning up -- process
 * shutdown). Nothing will ever service this; bail out rather
 * than spin on a flag nothing will set.
 */
        return nullptr;
    }
    ETCS::Entity* e;
    while (!(e = result.load(std::memory_order_acquire)));
    return e == reinterpret_cast<ETCS::Entity*>(UINTPTR_MAX) ? nullptr : e;
}
 
inline bool ETCS::ResolveEvent::operator()()
{
    ETCS_ASSERT_NOT_ORDERING_THREAD("ResolveEvent");
    DLInEvent evt{};
    evt.kind           = DLInEvent::Kind::Resolve;
    evt.conjugate_key  = conjugate_key;
    evt.resolve_target = target;
    evt.resolve_ok     = &ok;
#ifndef ETCS_LOADER
    evt.reply_to = &ETCS::EventNode::getInstance();
    evt.origin_extra_mask = ETCS::ActivePairModuleMask();
#endif
    if (!getLoader().stream.enqueue(DLInEventPtr{&evt}))
        return false;
    int8_t r;
    while ((r = ok.load(std::memory_order_acquire)) < 0);
    return r != 0;
}
 
inline bool ETCS::DestroyEvent::operator()()
{
    ETCS_ASSERT_NOT_ORDERING_THREAD("DestroyEvent");
    /*
 * THE expected drain point. `target` is optional only because the older
 * two-and-three-arg constructors predate it; when supplied, no caller has
 * to remember to signal-and-wait by hand, which is the whole point.
 */
    if (!ETCS::drainEntityScopes(target, "DestroyEvent")) return false;
    DLInEvent evt{};
    evt.kind = DLInEvent::Kind::Destroy;
    evt.conjugate_key = conjugate_key;
    evt.rid = rid;
    evt.destroy_children = delete_children;
    evt.tri_out = &result;
#ifndef ETCS_LOADER
    evt.reply_to = &ETCS::EventNode::getInstance();
    evt.origin_extra_mask = ETCS::ActivePairModuleMask();
#endif
    if (!getLoader().stream.enqueue(DLInEventPtr{&evt}))
        return false;
    int8_t r;
    while ((r = result.load(std::memory_order_acquire)) < 0);
    return r != 0;
}
 
/*
 * AddTagEvent::operator()() - builds a DLInEvent carrying the type-erased
 * trampoline captured at the addTag<T> call site (Entity.h), enqueues it,
 * and blocks until the loader's ordering thread has run it. Uses an
 * explicit `ready` completion flag rather than a reserved result value
 * (contrast LoadEvent's UINTPTR_MAX / ResolveEvent's 0x1 sentinel trick) -
 * RID 0 is not structurally guaranteed impossible the way a null pointer
 * is, so this follows DestroyEvent's more cautious pattern instead.
 */
inline ETCS::RID ETCS::AddTagEvent::operator()()
{
    ETCS_ASSERT_NOT_ORDERING_THREAD("AddTagEvent");
    DLInEvent evt{};
    evt.kind              = DLInEvent::Kind::AddTag;
    evt.conjugate_key     = conjugate_key;
    evt.addtag_parent     = parent;
    evt.addtag_child      = child;
    evt.addtag_trampoline = trampoline;
    evt.rid_out           = &result;
    evt.ready_out         = &ready;
#ifndef ETCS_LOADER
    evt.reply_to = &ETCS::EventNode::getInstance();
    evt.origin_extra_mask = ETCS::ActivePairModuleMask();
#endif
    if (!getLoader().stream.enqueue(DLInEventPtr{&evt}))
        return 0;
    while (!ready.load(std::memory_order_acquire));
    return result.load(std::memory_order_relaxed);
}
 
/*
 * EntityUnloadEvent - blocking, same spin pattern as AddTagEvent. Fired
 * unconditionally from removeTag's own entity-relation deletion
 * (tagModifyImpl, Entity.h) -- its only remaining caller now that
 * ~Entity() no longer triggers anything itself (see entityUnloadImpl's
 * own comment for why). By the time this returns, entityUnloadImpl has
 * determined target's correct parent arena and MemoryArena::deleteEntity
 * has fully run: election-and-evoke (global scope) or reparent-and-evoke
 * (child), all decided synchronously, before target's own destructor
 * even starts, on the loader's own ordering thread.
 *
 * Targets getLoader().stream directly, never this side's own local
 * stream -- this is THE event whose old behavior (routing through
 * EventNode::getInstance(), which resolves to a MODULE's own, possibly
 * already-stopped ModuleProxy when fired from module-side code) caused
 * the original hang this session traced: an entity destructing during a
 * module's own atexit sweep would enqueue onto that same module's own,
 * by-then-dead ordering thread, and spin forever waiting for a done flag
 * nothing was left alive to set.
 */
inline void ETCS::EntityUnloadEvent::operator()()
{
    ETCS_ASSERT_NOT_ORDERING_THREAD("EntityUnloadEvent");
    /*
 * Belt-and-braces, NOT the expected path -- and the two paths into this
 * event differ in what they can promise:
 *
 *   Root case (Entity::operator delete): the destructor has ALREADY run
 *   by the time we get here, so isDestructed() is true and this is a
 *   no-op. Nothing can be salvaged at that point. A long-lived stream
 *   body still touching such an entity is the same class of UB as one
 *   holding a reference to a stack frame that has already returned --
 *   the guarantee has to come from the caller's own structure, not here.
 *
 *   Child-target case: may arrive either side of the destructor, so the
 *   drain does real work when it arrives first.
 *
 * DestroyEvent is the path where this is actually load-bearing.
 */
    if (target && !target->isDestructed())
    {
        if (!ETCS::drainEntityScopes(target, "EntityUnloadEvent")) return;
    }
    DLInEvent evt{};
    evt.kind                    = DLInEvent::Kind::EntityUnload;
    evt.unload_target           = target;
    evt.unload_delete_children  = delete_children;
    evt.unload_done             = &done;
#ifndef ETCS_LOADER
    evt.reply_to = &ETCS::EventNode::getInstance();
    evt.origin_extra_mask = ETCS::ActivePairModuleMask();
#endif
    if (!getLoader().stream.enqueue(DLInEventPtr{&evt}))
        return;
    while (!done.load(std::memory_order_acquire));
}
 
/*
 * Root::changeModule() - see its own declaration comment, Entity.h.
 * Just builds and fires a ChangeModuleEvent -- all the actual logic
 * (survivor search via root_registry, promoteOrVacate, the reattach)
 * lives in changeModuleImpl above, on the loader's own ordering thread.
 */
inline void ETCS::Root::changeModule(const std::string& targetModule)
{
    ETCS::ChangeModuleEvent evt(targetModule, this);
    evt();
}
 
/*
 * ChangeModuleEvent::operator()() - blocking, same spin pattern as every
 * other event here. reply_to wiring mirrors EntityUnloadEvent's own
 * (module-side callers ack back onto their own stream) since a Root can
 * be constructed inside a module's own compiled work function just as
 * easily as loader-side code.
 */
inline void ETCS::ChangeModuleEvent::operator()()
{
    ETCS_ASSERT_NOT_ORDERING_THREAD("ChangeModuleEvent");
    DLInEvent evt{};
    evt.kind              = DLInEvent::Kind::ChangeModule;
    evt.conjugate_key      = conjugate_key;
    evt.changemodule_root  = root;
    evt.changemodule_done  = &done;
#ifndef ETCS_LOADER
    evt.reply_to = &ETCS::EventNode::getInstance();
    evt.origin_extra_mask = ETCS::ActivePairModuleMask();
#endif
    if (!getLoader().stream.enqueue(DLInEventPtr{&evt}))
        return;
    while (!done.load(std::memory_order_acquire));
}
 
/*
 * TagModifyEvent - blocking, same spin pattern as every other event here.
 * Fired from Entity::addTag(Buffer flag)/removeTag(Buffer tag). Ordered
 * LOCALLY (see ModuleProxy::on_event's fork) - never crosses to the
 * loader itself, regardless of which scope fires it.
 */
inline void ETCS::TagModifyEvent::operator()()
{
    ETCS_ASSERT_NOT_ORDERING_THREAD("TagModifyEvent");
    DLInEvent evt{};
    evt.kind                = DLInEvent::Kind::TagModify;
    evt.conjugate_key       = conjugate_key;
    evt.tagmodify_target    = target;
    evt.tagmodify_is_remove = is_remove;
    evt.tagmodify_impl      = impl;
    evt.tagmodify_done      = &done;
    /*
 * Stamped at construction from the emitting type's own static TAG_MASK
 * (Entity::myTagMask), so the handler never has to resolve it -- see
 * DLInEvent::tagmodify_mask's own comment (EventNode.h) for why that
 * lookup was outright wrong on the loader's side.
 */
    evt.tagmodify_mask      = type_mask;
    if (!ETCS::EventNode::getInstance().stream.enqueue(DLInEventPtr{&evt}))
        return;
    while (!done.load(std::memory_order_acquire));
}
 
/*
 * PairMaskEvent - blocking, same spin pattern as everything here. Straight onto
 * getLoader().stream for the same reason the five memory-altering kinds go
 * direct: the local stream may already be stopped, and routing through it first
 * was a real hang.
 *
 * The loader holds type_owner_index, so this is the one question a module has
 * to ask it. Callers memoize (Entity::resolvePairModuleMask), so it fires once
 * per distinct tag pair.
 *
 * A refused enqueue -- the loader shutting down -- leaves result EMPTY rather
 * than all(), the one place empty is right: there is no ordering left to join.
 */
inline void ETCS::PairMaskEvent::operator()()
{
    ETCS_ASSERT_NOT_ORDERING_THREAD("PairMaskEvent");
    DLInEvent evt{};
    evt.kind           = DLInEvent::Kind::PairMask;
    evt.conjugate_key  = conjugate_key;
    evt.pairmask_tag_b = tag_b;
    evt.pairmask_out   = &result;
    evt.pairmask_done  = &done;
#ifndef ETCS_LOADER
    evt.reply_to = &ETCS::EventNode::getInstance();
    evt.origin_extra_mask = ETCS::ActivePairModuleMask();
#endif
    if (!getLoader().stream.enqueue(DLInEventPtr{&evt}))
        return;
    while (!done.load(std::memory_order_acquire));
}
 
#ifndef ETCS_LOADER
inline ETCS::Entity* ETCS::CreateEvent::operator()()
{
    ETCS_ASSERT_NOT_ORDERING_THREAD("CreateEvent");
    /*
 * Folds into a Load event at the proxy boundary - conjugate_key carries
 * full addressing so the loader fulfills it directly. Always module-
 * side code (this whole type is #ifndef ETCS_LOADER-gated), so
 * reply_to is set unconditionally, same reasoning as every other
 * memory-altering event's own module-side branch.
 */
    DLInEvent evt{};
    evt.kind = DLInEvent::Kind::Load;
    evt.conjugate_key = conjugate_key;
    evt.entity_out = &result;
    evt.reply_to = &ETCS::EventNode::getInstance();
    evt.origin_extra_mask = ETCS::ActivePairModuleMask();
    if (!getLoader().stream.enqueue(DLInEventPtr{&evt}))
        return nullptr;
    ETCS::Entity* e;
    while (!(e = result.load(std::memory_order_acquire)));
    return e == reinterpret_cast<ETCS::Entity*>(UINTPTR_MAX) ? nullptr : e;
}
#endif
 
/*
 * RequestUnloadEvent - the non-blocking counterpart to the old, blocking
 * unload path (see its own declaration comment, EventNode.h, for the
 * full reasoning). Enqueues DIRECTLY onto getLoader().stream regardless
 * of which side (loader or module) fires it -- the exact same reasoning
 * as EntityUnloadEvent's own fix earlier this session: EventNode::
 * getInstance() resolves to THIS side's own, potentially-already-torn-
 * down local instance when called from module-side code, never what's
 * actually needed here (the loader's own LoaderStream, which is where
 * Kind::RequestUnload's handling actually lives).
 *
 * Heap-allocates its own DLInEvent, deliberately: every OTHER event in
 * this file keeps its DLInEvent alive by blocking (spinning on an atomic
 * the ordering thread eventually writes) -- enqueue() only copies the
 * 8-byte POINTER into the ring, never the event's own contents, so
 * whatever it points at must stay valid until actually read. Nothing
 * blocks here at all, so a stack-allocated DLInEvent would already be
 * destroyed (the enclosing function long since returned) by the time the
 * ordering thread gets to it -- a genuine use-after-free. on_event's own
 * Kind::RequestUnload case is responsible for deleting this heap
 * allocation once fully consumed, for both the initial fire and the
 * delayed recheck it spawns.
 */
inline void ETCS::RequestUnloadEvent::operator()()
{
    DLInEvent* evt = new DLInEvent();
    evt->kind                    = DLInEvent::Kind::RequestUnload;
    evt->conjugate_key           = conjugate_key;
    evt->request_unload_target   = target;
    evt->request_unload_recheck  = false;
    getLoader().stream.enqueue(DLInEventPtr{evt});
    /*
 * Deliberately no wait here at all -- see this event's own
 * declaration comment for why that's the entire point.
 */
}
 
#endif
 
