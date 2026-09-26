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

#if defined(__EMSCRIPTEN__)
    /*
     * Wait for `pred` by DRIVING an ordering loop, because in the browser
     * nothing else will until etcs_boot_runtime_threads arms the ordering
     * threads (EventStream::start takes none). The loop driven is `node`'s:
     * the loader's for everything that enqueues onto getLoader().stream, which
     * is the default; a module's own for the one event that orders locally
     * (TagModifyEvent), since driving the loader's loop while a ring nobody
     * services holds the event is a wait that cannot end.
     *
     * The loop admits one driver at a time, so a waiter that does not get it
     * stands down rather than spinning on the claim: the thread that does have
     * it is servicing this waiter's event too, and taking the CPU off it is the
     * one way to make the wait longer.
     *
     * A SHORT SLEEP, NOT yield(). sched_yield is a no-op in wasm, so a yield
     * spin never leaves the thread -- and leaving it is what makes the thread
     * cooperative. emscripten runs a thread's share of the runtime's own
     * proxied work on the way out of a futex wait (_emscripten_yield), which is
     * the only place a thread replays a dlopen or a new dlsym table entry; a
     * thread that only ever yields is invisible to that and can hold up any
     * other thread waiting for it. The sleep is short enough not to add latency
     * worth measuring and is the whole difference between a thread that
     * participates and one that merely burns.
     */
    template <typename Pred>
    inline void etcs_emscripten_spin(EventNode& node, Pred&& pred)
    {
        // A wait this long is not slow, it is stuck: every event this function
        // waits on is serviced by the loop it is driving, so a second of it
        // means the window will not clear on its own. The reorder snapshot names
        // which of the three ways it went wrong -- and without it the browser
        // path had no stall diagnostics at all, which is why a blocked admission
        // there read as "the script simply stopped".
        constexpr unsigned STUCK = 5000;   // x200us
        unsigned spins = 0;
        while (!pred())
        {
            // Through the node's own trampoline, NEVER node.stream.emscripten_poll()
            // -- see EventNode::drive_ordering for what a module's copy of that
            // template does to the loader's events.
            if (!node.drive_ordering || !node.drive_ordering())
                ::std::this_thread::sleep_for(::std::chrono::microseconds(200));
            if (++spins % STUCK == 0 && !node.stream.stallReportsExhausted())
                node.stream.reportReorderState("REORDER STALL (web waiter)");
        }
    }
    template <typename Pred>
    inline void etcs_emscripten_spin(Pred&& pred)
    {
        etcs_emscripten_spin(getLoader(), ::std::forward<Pred>(pred));
    }
#endif

#if defined(__EMSCRIPTEN__) && defined(ETCS_LOADER)
    // Defined below after LoaderStream::attachModule is complete.
    bool etcs_web_root_attach_module(Root& root, const ::std::string& module_name);
    // Module EventNodes deferred during preload; drained in etcs_boot_runtime_threads.
    inline ::std::vector<EventNode*>& emscripten_deferred_module_nodes()
    {
        static ::std::vector<EventNode*> nodes;
        return nodes;
    }
#endif

    /*
 * EVERY LOADED MODULE'S OWN LOG DESTINATION, in one place the shell can reach.
 *
 * ETCS::log_to_file is an inline variable, so there is one per DSO -- which is
 * the grain that makes sense, since getModuleLog() already writes to
 * logs/<ModuleName>.log and a module's destination is a module's own business.
 * The cost of that grain is that a single switch has to visit each of them,
 * and this map is how it finds them: filled at registerLoader, emptied at
 * unload, keyed by the module's own scope name.
 */
    inline ::std::unordered_map<::std::string, EventNode*>& module_log_nodes()
    {
        static ::std::unordered_map<::std::string, EventNode*> nodes;
        return nodes;
    }

    // Everything at once -- `log all file` / `log all term`. The loader's own
    // flag first, then every module's, through the trampoline each registered.
    inline void set_log_destination(bool to_file)
    {
        ETCS::set_log_to_file(to_file);
        for (auto& [name, node] : module_log_nodes())
            if (node && node->set_log_to_file) node->set_log_to_file(to_file);
    }

    /*
 * ONE MODULE'S, which is what the per-binary flag was always for.
 *
 * Setting them together was never a policy, it was the only reachable
 * operation: the shell had one verb and the verb visited the whole map. But a
 * frame loop drowning a prompt is ONE module's lines, and moving all of them
 * to files to get rid of it takes away every other module's output too --
 * which is the trade the person at the prompt was trying not to make.
 *
 * Returns false for a scope nothing has registered, rather than silently doing
 * nothing: "RenderProvider is not loaded" and "RenderProvider now logs to a
 * file" are different answers and the shell prints them differently.
 */
    inline bool set_module_log_destination(const ::std::string& scope, bool to_file)
    {
        auto it = module_log_nodes().find(scope);
        if (it == module_log_nodes().end() || !it->second || !it->second->set_log_to_file)
            return false;
        it->second->set_log_to_file(to_file);
        return true;
    }

    // Same shape for the read. `found` distinguishes "not loaded" from "on the
    // terminal", which a bare bool cannot.
    inline bool module_log_destination_is_file(const ::std::string& scope, bool& found)
    {
        auto it = module_log_nodes().find(scope);
        found = (it != module_log_nodes().end() && it->second && it->second->get_log_to_file);
        return found ? it->second->get_log_to_file() : false;
    }

    // What THE LOADER is doing -- its own flag and nothing else's. It used to
    // be able to say "and every module's too", because the only setter moved
    // them together; with the targeted one above that is no longer true, and a
    // reader that assumed it would report the loader's answer for a module
    // that had been set the other way.
    inline bool log_destination_is_file() { return ETCS::get_log_to_file(); }

    // Every registered scope, for `log status` -- which now has to enumerate
    // rather than generalise, for the reason just above.
    inline ::std::vector<::std::string> module_log_scopes()
    {
        ::std::vector<::std::string> out;
        out.reserve(module_log_nodes().size());
        for (auto& [name, node] : module_log_nodes())
            if (node && node->get_log_to_file) out.push_back(name);
        ::std::sort(out.begin(), out.end());
        return out;
    }
}
namespace ETCS
{
using namespace ETCS;
/*
 * -- PendingUnloadRegistry ------------------------------------------------------
 * Tracks every RequestUnloadEvent-spawned delay-then-recheck thread
 * joinably, rather than the raw ::std::thread(...).detach() this used to
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
 * Joined from drive_main_loop_then_exit (CommandExecutor.h), right alongside
 * shutdown_detached_executors() -- the process is never allowed to
 * actually exit while any recheck is still in progress. An empty
 * registry (nothing was ever mid-unload, the overwhelmingly common
 * case) joins nothing and costs nothing.
 */
struct PendingUnloadRegistry
{
    ::std::mutex               mutex_;
    ::std::vector<::std::thread> threads_;
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
 * would ever join again, so ~vector<::std::thread> destroyed a joinable
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
    bool spawn(::std::function<void()> body)
    {
        ::std::lock_guard<::std::mutex> lock(mutex_);
        if (closed_) return false;
        threads_.emplace_back(::std::move(body));
        return true;
    }
    void track(::std::thread t)
    {
        ::std::lock_guard<::std::mutex> lock(mutex_);
        threads_.push_back(::std::move(t));
    }
    void join_all()
    {
        ::std::lock_guard<::std::mutex> lock(mutex_);
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
        ::std::lock_guard<::std::mutex> lock(mutex_);
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
        if (node && node != &st && node != &getLoader())
        {
            // Native distinct module EventNode
            ETCS::module_log_nodes()[node->scope] = node;
#if defined(__EMSCRIPTEN__) && defined(ETCS_LOADER)
            ETCS::emscripten_deferred_module_nodes().push_back(node);
#endif
            node->set_log_to_file(ETCS::get_log_to_file());
            /*
 * MIRROR, DO NOT MERGE. Every name this module publishes gets a row in the
 * loader's ridMirror tagged with this module's name -- not a write into
 * st.ridMap, which is the loader's OWN lists and has room for one handle per
 * name. Nine providers publish "Deletable"; a write would have kept the last
 * and lost eight, which is exactly what it did.
 *
 * NO KEY REWRITING. The name goes across as the module spelled it. The
 * distinction between providers is the row, so there is nothing to encode into
 * a string and nothing for a reader to parse back out.
 */
            {
                const ETCS::Buffer mod_key(name.c_str());
                size_t mirrored = 0;
                for (const auto& [originalKey, handle] : node->ridMap)
                {
                    st.RegisterMirror(originalKey, mod_key, handle);
                    ++mirrored;
                }
                ETCS_LOG("EventNode:" << st.scope, "Mirrored " << mirrored
                    << " RIDList(s) from module '" << name << "'");
            }
        }
        else if (node == &st || node == &getLoader())
        {
            /*
 * The module's EventNode::getInstance() answered with the LOADER's node, so
 * the module is binding header-inline statics to this image instead of its
 * own -- every per-DSO invariant in core/ (arena ownership, RID seeds, the
 * ordering stream this node fronts) is void. A build without
 * -fvisibility=hidden does exactly this under dylink. Refused here, where
 * the cause is nameable, rather than run in a model nothing else expects.
 */
            ETCS_LOG("DynamicLoader:Module",
                "Module '" << name << "' shares the loader's EventNode -- it was "
                "built without -fvisibility=hidden. Refusing to register it.");
            return false;
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
 * than fighting the loader over ::std::signal() registration.
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
ETCS::ModuleBundle ETCS::Module::getTagAddress(const ::std::string& tag)
{
#ifdef ETCS_LOADER
    ::std::string hashFuncSymbol      = tag + "_GetHash";
    ::std::string makeFuncSymbol      = tag + "_Make";
    ::std::string makeChildFuncSymbol = tag + "_MakeChild";
    void* hashAddr      = getTagFunction(hashFuncSymbol);
    void* makeAddr      = getTagFunction(makeFuncSymbol);
    void* makeChildAddr = getTagFunction(makeChildFuncSymbol);
    if (!makeAddr) throw ::std::runtime_error("Failed to find '" + makeFuncSymbol + "' in " + name);
    if (!hashAddr) throw ::std::runtime_error("Failed to find '" + hashFuncSymbol + "' in " + name);
    ETCS_LOG("DynamicLoader:ModuleBundle", "Raw "     << makeAddr << " from " << tag << "!");
    ETCS_LOG("DynamicLoader:ModuleBundle", "RawHash " << hashAddr << " from " << tag << "!");
    using MakeFuncResolver = MakeFunc (*)();
    using MakeFunc         = ETCS::Entity*(*)(ETCS::Buffer&);
    using HashFunc         = HASH_TYPE(*)();
    MakeFunc  actualMake = reinterpret_cast<MakeFuncResolver>(makeAddr)();
    HASH_TYPE actualHash = reinterpret_cast<HashFunc>(hashAddr)();
    /*
     * Absence is not fatal here, unlike _Make. A tag whose block predates
     * receiver-scoped spawn still has a perfectly good top-level factory; only
     * `parent.spawn` is unavailable to it, and make_typed_child names the tag
     * when that happens rather than the whole module failing to load.
     */
    using MakeChildResolver = MakeChildFunc (*)();
    MakeChildFunc actualMakeChild = makeChildAddr
        ? reinterpret_cast<MakeChildResolver>(makeChildAddr)()
        : nullptr;
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
    return {tag, this, actualHash, actualMake, actualMakeChild,
            actionsHashes, actions, moduleSignals, ETCS::Buffer()};
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
    interrupt.store(1, ::std::memory_order_release);
    terminate.store(1, ::std::memory_order_release);

    /*
 * 2. Drop the loader-mirror rows THIS module published.
 *
 * BY MODULE, which is the only key that is this module's to drop. Dropping by
 * NAME erased whichever provider happened to own the row -- and since the row
 * was shared, unloading one module took another's still-live list down with
 * it, or left a handle into memory about to be unmapped, depending on load
 * order. The mirror carries the publisher on the row precisely so this step
 * can be exact.
 *
 * Before the close, not after: a row must not stay reachable once the code
 * behind its RIDList is gone.
 */
    if (node)
    {
        const size_t purged = node->DropMirror(ETCS::Buffer(name.c_str()));
        if (purged)
            ETCS_LOG("DynamicLoader:Module", "Purged " << purged << " mirror row(s) for '"
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
    ::std::vector<::std::pair<ETCS::Buffer, RID>> children;
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
    wrap_owner_rid_ = owner->getRID();
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
size_t ETCS::MirrorBuffer::chainOf(ETCS::Entity* owner, WireScope scope, IWireWrapper** out, size_t max)
{
    size_t n = 0;
    if (!owner) return 0;
    ::std::vector<::std::pair<ETCS::Buffer, RID>> children;
    owner->getTypedChildren(children);
    for (auto& [tag, rid] : children)
    {
        if (n >= max) break;
        Entity* child = owner->getTypedChild(tag, rid);
        if (!child || !child->hasTag(ETCS::Buffer("Wrapper"))) continue;
        void* raw = child->getInterfacePointer(ETCS::Buffer("Wrapper"));
        if (!raw) continue;
        IWireWrapper* w = static_cast<IWireWrapper*>(raw);
        if (!scopeApplies(w->Scope(), scope)) continue;
        out[n++] = w;
    }
    return n;
}
inline bool ETCS::MirrorBuffer::localFrame(ETCS::Entity* e)
{
    return e && (e->hasTag(ETCS::Buffer("Database")) || e->hasTag(ETCS::Buffer("Local")));
}
::std::string ETCS::MirrorBuffer::manifestOf(ETCS::Entity* owner, WireScope scope)
{
    ::std::string out;
    if (!owner) return out;
    ::std::vector<::std::pair<ETCS::Buffer, RID>> children;
    owner->getTypedChildren(children);
    for (auto& [tag, rid] : children)
    {
        Entity* child = owner->getTypedChild(tag, rid);
        if (!child || !child->hasTag(ETCS::Buffer("Wrapper"))) continue;
        void* raw = child->getInterfacePointer(ETCS::Buffer("Wrapper"));
        if (!raw || !scopeApplies(static_cast<IWireWrapper*>(raw)->Scope(), scope)) continue;
        const ETCS::Buffer t = child->getSourceTag();
        out += child->getSourceModule().toString() + ":" + t.toString() + "#"
             + ::std::to_string(child->tagHash(t)) + ";";
    }
    return out;
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
        // The chain's owner, when makePair named one other than the handler
        // (wrap_owner_rid_'s comment): a remote pair's authority layer.
        Entity* owner = handler;
        if (wrap_owner_rid_ && (!handler || handler->getRID() != wrap_owner_rid_))
            if (Entity* o = ETCS::etcs_resolve_rid_anywhere(&ETCS::getLoader(), wrap_owner_rid_))
                owner = o;
        if (!owner) return;
        ::std::vector<::std::pair<ETCS::Buffer, RID>> children;
        owner->getTypedChildren(children);
        for (auto& [tag, rid] : children)
        {
            if (wrap_chain_len_ >= MAX_WRAP_STAGES) break;
            Entity* child = owner->getTypedChild(tag, rid);
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
    /*
 * A SOCKET PAIR UNWRAPS WITH ITS OWNER'S OWN STAGES. Such a pair is one half
 * of a stream whose other half is in another runtime (Entity::produceOnto /
 * consumeFrom), and its owner is the entity whose authority layer the frames
 * must pass here -- the far node's own, or the surface that mirrors it. A
 * stage there may hold what a fresh one cannot (a key, NetworkProvider's
 * Seal), and it is THAT stage the far side's frames must satisfy. Nothing
 * else on this side shares the instances: the local pair's producer object
 * is never used. The owner must still carry exactly the manifest's stages,
 * or the pair is refused like any unfulfilled one.
 */
    if (active_ == ActiveStrategy::Socket && wrap_owner_rid_)
    {
        Entity* owner = ETCS::etcs_resolve_rid_anywhere(&ETCS::getLoader(), wrap_owner_rid_);
        IWireWrapper* live[MAX_WRAP_STAGES] = {};
        const size_t n = owner ? chainOf(owner, WireScope::Socket, live, MAX_WRAP_STAGES) : 0;
        bool same = owner && n == wrap_manifest_len_;
        if (same)
        {
            ::std::vector<::std::pair<ETCS::Buffer, RID>> children;
            owner->getTypedChildren(children);
            size_t k = 0;
            for (auto& [tag, rid] : children)
            {
                Entity* child = owner->getTypedChild(tag, rid);
                if (!child || !child->hasTag(ETCS::Buffer("Wrapper"))) continue;
                void* raw = child->getInterfacePointer(ETCS::Buffer("Wrapper"));
                if (!raw || !scopeApplies(static_cast<IWireWrapper*>(raw)->Scope(), WireScope::Socket)) continue;
                if (k >= wrap_manifest_len_
                    || child->getSourceModule().toString() != wrap_manifest_[k].module.toString()
                    || child->getSourceTag().toString()    != wrap_manifest_[k].tag.toString())
                { same = false; break; }
                ++k;
            }
        }
        if (!same)
        {
            ETCS_LOG("MirrorBuffer", "resolveWrapChain: this side's authority layer does not carry "
                     "the pair's wrappers -- refusing to unwrap.");
            unwrap_failed_ = true;
            return;
        }
        for (size_t i = 0; i < n; ++i) wrap_chain_[wrap_chain_len_++] = live[i];
        return;
    }
    ETCS::Root boot_root(bound_ctx_);
    for (size_t i = 0; i < wrap_manifest_len_; ++i)
    {
        if (wrap_chain_len_ >= MAX_WRAP_STAGES) break;
        ::std::string key = wrap_manifest_[i].module.toString() + ":"
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
        ::std::string key = e->getSourceModule().toString() + ":"
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
    ::std::vector<ETCS::Buffer> tags;
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
bool ETCS::WorkBundle::operator()(ETCS_RID_SIZE rid, const ETCS::Buffer& conjugate_key,
                                  ETCS::Buffer& tagbuff, ETCS::SignalContext ctx)
{
    /*
 * THE RESOLUTION IS THE LIVENESS CHECK -- see WorkBundle's own comment.
 * Either the RID names something the loader still holds, in which case the
 * pointer is good for the whole of this frame, or it does not and there is
 * nothing to dispatch to. No flag, no second question, one early exit.
 */
    ETCS::Entity* child = ETCS::etcs_resolve_by_key(conjugate_key, rid);
    if (!child)
    {
        ETCS_LOG("WorkBundle::operator()", "RID:" << rid << " (" << conjugate_key
            << ") no longer resolves -- the entity was reclaimed before "
            << module_tag << "." << work_tag << " could dispatch. Refusing.");
        return false;
    }
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
bool ETCS::WorkBundle::operator()(ETCS_RID_SIZE rid, const ETCS::Buffer& conjugate_key,
                                  ETCS::MBuffer& tagbuff, ETCS::SignalContext ctx)
{
    // Same resolution-is-the-liveness-check as the Buffer overload above.
    ETCS::Entity* child = ETCS::etcs_resolve_by_key(conjugate_key, rid);
    if (!child)
    {
        ETCS_LOG("WorkBundle::operator()", "RID:" << rid << " (" << conjugate_key
            << ") no longer resolves -- the entity was reclaimed before "
            << module_tag << "." << work_tag << " could dispatch. Refusing.");
        return false;
    }
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
 * AN ACTION THAT WAS ASKED FOR AND NEVER REACHED ONE.
 *
 * The one site that holds both halves: a call was attempted, and it did not
 * arrive. Downstream they are already indistinguishable -- a work function that
 * ran and wrote nothing leaves exactly the buffer a missing one does, and the
 * trampoline that would have recorded the call is the thing that never ran. So
 * this is also where an attempt gets recorded when attempts become entries.
 *
 * IT NAMES WHAT THE TAG DOES PROVIDE, because that list is in hand right here
 * and it separates the three causes without a second run: a typo reads as a
 * near miss in an otherwise right list; an action with a DEFINE_WORK_FUNC but
 * no entry in its ETCS_TAG_BLOCK reads as an absence from a list that is
 * otherwise complete -- the one direction the build cannot catch, since the
 * trampoline is defined, has external linkage and is simply never referenced;
 * and a call aimed at the wrong type reads as somebody else's surface entirely.
 * The old line named only what was asked for, which answers none of the three.
 *
 * ONE FUNCTION FOR BOTH DISPATCH PATHS, so a second recording site cannot drift
 * from the first.
 */
inline void etcs_report_unreached_action(const ETCS::Buffer& tag,
                                         const ETCS::Buffer& work,
                                         const ETCS::FlatMap<ETCS::Buffer, ETCS::WorkBundle>& actions,
                                         ETCS_RID_SIZE rid, bool stream)
{
    ::std::string provided;
    for (uint32_t i = 0; i < actions.size; ++i)
    {
        if (!provided.empty()) provided += ", ";
        provided += actions.data[i].first.toString();
        if (actions.data[i].second.isStream) provided += " (stream)";
    }
    if (provided.empty())
        provided = "nothing -- its catalog is empty, so no tag block reached discoverActions";

    ETCS_LOG("ModuleBundle::operator()", "Tag: " << tag << " does not provide requested "
             << (stream ? "stream action" : "action") << ": " << work << "  (RID:" << rid << ")"
             << "\n    " << tag << " provides: " << provided
             << "\n    If " << tag << "::" << work << " has a DEFINE_WORK_FUNC, it is missing "
                "from that tag's ETCS_TAG_BLOCK -- which compiles, links, and fails only here.");
}
/*
 * bool, not void: false means the action was not found in this tag's table, or
 * its WorkBundle refused to dispatch -- never that the action ran and produced
 * no output, which is ordinary. A caller inspecting only the buffer afterward
 * cannot tell "wrote nothing" from "never ran", which is exactly how a missing
 * action came to look like a successful response echoing the request back.
 */
bool ETCS::ModuleBundle::operator()(ETCS_RID_SIZE rid, const ETCS::Buffer& conjugate_key,
                                    const ETCS::Buffer& work,
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
        pass = it->second(rid, conjugate_key, data, ctx);
#ifdef ETCS_LOG_FUNCTION_EVOCATION_PATH
        ETCS_LOG("ModuleBundle::operator()", "EXIT -- WorkBundle::operator()(...) returned normally for " << work);
#endif
    }
    else
        etcs_report_unreached_action(this->tag, work, actions, rid, false);
    return pass;
}
bool ETCS::ModuleBundle::operator()(ETCS_RID_SIZE rid, const ETCS::Buffer& conjugate_key,
                                    const ETCS::Buffer& work,
                                    ETCS::MBuffer& data, ETCS::SignalContext ctx)
{
    auto it = actions.find(work);
    bool pass = false;
    if (it != actions.end()) pass = it->second(rid, conjugate_key, data, ctx);
    else etcs_report_unreached_action(this->tag, work, actions, rid, true);
    return pass;
}
/*
 * THE TARGET OF A FLAG CHANGE, FOUND AGAIN WHERE THE CHANGE IS APPLIED.
 *
 * The emitter names the entity -- its RID and the key its type's list is
 * published under -- and the ordering thread resolves that name here, at the
 * moment the change lands. The resolution IS the liveness check, as it is for a
 * work function (WorkBundle::operator()): an entity retired while the event sat
 * in the ring is simply not found, and the change is refused rather than written
 * into memory the arena may already have handed to somebody else.
 *
 * Keyed on TYPE, as every ordinary RID lookup is -- per-provider-type is the
 * uniqueness the generator guarantees (Entity.h, resolve_in_family).
 *
 * Both handlers call this -- the loader's and a module's -- so there is one
 * place that decides what "still there" means for a flag.
 */
/*
 * WHETHER A TYPE CAN BE ASKED ABOUT AT ALL: some list, in the loader's own map
 * or mirrored from a module, published under this key. A type with none -- a
 * test's local leaf, an internal type added without a tag block -- is outside
 * the registry, and "not found" would say nothing about whether it is alive.
 */
inline bool etcs_type_published(const ETCS::Buffer& key)
{
    if (key.written == 0) return false;
    ETCS::EventNode* owner = ETCS::etcs_loader_event_node();
    if (!owner) return false;
    const ETCS::Buffer bare = ETCS::etcs_bare_family_key(key);
    if (owner->ridMap.find(bare) != owner->ridMap.end()) return true;
    auto m = owner->ridMirror.find(bare);
    return m != owner->ridMirror.end() && !m->second.empty();
}

inline ETCS::Entity* etcs_tagmodify_target(const ETCS::DLInEvent& evt)
{
    // Unlisted: nothing to check against, so the emitter's word stands.
    if (!etcs_type_published(evt.tagmodify_type)) return evt.tagmodify_target;

    ETCS::Entity* live = ETCS::etcs_resolve_by_key(evt.tagmodify_type, evt.tagmodify_rid);
    if (!live)
        ETCS_LOG("TagModify", "'" << evt.conjugate_key << "' "
                 << (evt.tagmodify_is_remove ? "off" : "on") << " " << evt.tagmodify_type
                 << " RID:" << evt.tagmodify_rid << " -- refused: its type's list no longer "
                    "holds it, so it was retired while the change was queued.");
    return live;
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
            ::std::string module_name = evt.conjugate_key.toString();
            bool ok = evt.resolve_target
                   && resolveImpl(module_name, evt.resolve_target);
            sendAckIfNeeded(evt);
            evt.release_value = ok ? 1 : 0;
            return {ETCS::DispatchKind::Inline, &evt};
        }
        case DLInEvent::Kind::Destroy:
        {
            ::std::string key_str = evt.conjugate_key.toString();
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
            evt.rid_out->store(r, ::std::memory_order_relaxed);
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
            ETCS::Entity* live = etcs_tagmodify_target(evt);
            evt.release_value = (live && evt.tagmodify_impl(live, evt.conjugate_key,
                                                             evt.tagmodify_is_remove)) ? 1 : 0;
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
 * with no idea the asynchronous recheck it just triggered
 * hasn't even started yet. The REPL loop then
 * exits (interrupt flag still set) and main() returns,
 * letting the process's own exit sequence proceed
 * concurrently with -- and easily outrun -- that
 * still-pending recheck's own eventual dlclose() on a
 * module whose worker threads may still be mid-flight.
 * Tracking this thread (and joining every tracked entry
 * from drive_main_loop_then_exit, CommandExecutor.h, right
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
                    ::std::this_thread::sleep_for(::std::chrono::milliseconds(100));
                    /*
 * Stack-allocated, unlike the old heap-allocated
 * recheck_evt -- safe now specifically because this
 * thread blocks on `done` below before its own frame
 * ever pops, exactly the same stack-lifetime contract
 * every OTHER synchronous DLInEvent-based call in this
 * codebase (TagModifyEvent, ChangeModuleEvent, etc.)
 * already relies on.
 */
                    ::std::atomic<bool> done{false};
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
                    #if defined(__EMSCRIPTEN__)
    etcs_emscripten_spin([&]{ return done.load(::std::memory_order_acquire); });
#else
    while (!done.load(::std::memory_order_acquire));
#endif
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
    const ::std::string& module_name, Module* mod)
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
 * claimLifetime - the election, for any module root attaching to a loaded
 * module: a global-scope entity or Root (attachModule), or a child made
 * under another module's entity (addTagImpl) -- which is a root of its own
 * module's arena and so holds its module exactly as a global one would.
 */
void ETCS::EventNode::LoaderStream::claimLifetime(ETCS::Module* global_mod, ETCS::LifetimeOwner entity)
{
    /*
 * Claim the lifetime-owner slot iff it's currently vacant -- either
 * this is the very first entity/Root ever to touch this module
 * (right after its bootstrap), or the previous owner's own
 * ~Module() already vacated it (a RequestUnloadEvent either already
 * fired or about to be) and this attach is what saves it from
 * actually unloading: RequestUnloadEvent's own delayed recheck will
 * see lifetime_owner non-vacant again and do nothing.
 */
    if (!global_mod->lifetime_owner)
    {
        entity.module().is_lifetime_owner = true;
        global_mod->lifetime_owner        = entity;
        ETCS_LOG("DynamicLoader", "Module '" << global_mod->name
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
        ETCS_LOG("DynamicLoader", "Module '" << global_mod->name
                 << "' lifetime_owner handed off from bootstrap Root to entity RID:"
                 << entity.getRID());
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
    const ::std::string& module_name, ETCS::LifetimeOwner entity,
    const ::std::string& spawn_tag)
{
    /*
 * Step 1. ONE MODULE PER ENTITY, FOR ITS WHOLE LIFETIME -- an enforced
 * invariant, not an assumption the rest of this function relies on.
 *
 * Dropping a different-module request rather than erroring is the
 * deliberate part: rebinding would orphan whatever module_ already pointed
 * at. The correct way to target a different module is a fresh entity/Root
 * -- Root is reconstructed on the stack whenever one is needed, so this is
 * never a real constraint -- or Root::changeModule() to migrate in place.
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
    // Step 2. The one permanent loader-owned instance for this name. No
    // per-entity Module ever holds real content, so nothing is ever
    // transferred between entities -- they are all proxies onto this.

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
 * CAUGHT HERE BECAUSE NO CALLER CAN CATCH IT. Everything in this
 * bootstrap -- dlopen, registerLoader/discoverTags,
 * catalogTypes/discoverActions -- throws ::std::runtime_error for
 * ordinary reasons: a typo'd module name, a missing export.
 *
 * This runs on the loader's ONE ordering thread, which services
 * attachModule for the rest of the process. An escaping exception
 * unwinds to the top of THAT thread, finds no handler, and calls
 * ::std::terminate -- killing every future Load/Resolve/Destroy/
 * AddTag/ChangeModule. A caller's try/catch around its blocking
 * evt() cannot help: it enqueues and spins on an atomic, and the
 * throw is on a different thread. That is exactly what crashed the
 * navigator on `Root> exot`.
 *
 * NOT the class of failure RegisterDynamicLoader's abort() guards:
 * that one covers a module that loaded and then violated a
 * structural invariant, where continuing breaks determinism. "Does
 * not exist, or is missing an export" is the ordinary failure
 * attachModule's bool return already represents, and every caller
 * already checks it.
 */
        library_handle_t handle = nullptr;
        try
        {
#ifdef _WIN32
            handle = LoadLibraryA(global_mod->getFilename().c_str());
            if (!handle)
                throw ::std::runtime_error("Failed to load DLL (error "
                    + ::std::to_string(GetLastError()) + ") for " + module_name);
#else
            handle = dlopen(global_mod->getFilename().c_str(), RTLD_LAZY | RTLD_LOCAL);
            if (!handle)
                throw ::std::runtime_error(::std::string("Failed to load SO: ") + dlerror());
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
 * unmapLibrary rather than a bare close -- see the catalogTypes catch
 * below for the hazard. Nothing reaches THIS catch with module threads
 * running today, but the full teardown no-ops cleanly either way.
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
            /*
 * Said twice, and the second one is the only one a browser shows: abort()
 * in wasm reaches the page as a bare "unreachable", and the terminal that
 * std::cerr and ETCS_LOG feed is an in-page widget that a failure during
 * module load kills before it renders. This is the path that fires first
 * under emscripten -- the module's own half is deferred to
 * RegisterDynamicLoader, which a mismatch means this function never reaches
 * (ETCS_MODULE_EXPORT_MAIN, ETCS_API.h).
 */
            const ::std::string fatal =
                "FATAL: '" + module_name + "' -- " + mex.what()
                + " -- loader and module were not built for the same epoch. "
                  "Shutting down. (Rebuild both halves together: "
                  "`ace wasm make all && ace wasm make loader etcs`.)";
            ::std::cerr << fatal << ::std::endl;
            ETCS_WEB_CONSOLE_ERROR(fatal.c_str());
            ::std::abort();
        }
        catch (const ::std::exception& ex)
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
 
    // Step 3.
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
 
    claimLifetime(global_mod, entity);

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
 * CommandExecutor.h's resolve_module/spawn_entity and its navigator)
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
    ETCS::Root* root, const ::std::string& target_module)
{
    if (root->module_.parent != nullptr)
    {
        const ::std::string current_module = root->module_.parent->name;
 
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
 * (module-root election-and-evoke, or child reparent-and-evoke). Always a genuine Entity* -- Root
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
 *   CHILD (parent_ != nullptr): the arena its record is in -- its
 *   parent's, or for a module root (a child made under another module's
 *   entity) its own module's root arena. getOwningArena() is both.
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
 * Runs 100ms after promoteOrVacate() found no survivor and vacated
 * target->lifetime_owner; the delay runs on a PendingUnloadRegistry thread
 * rather than blocking this ordering thread.
 *
 * Re-verifies lifetime_owner is STILL vacant before doing anything
 * irreversible: if an attachModule call claimed it during the window (a
 * fresh spawn from this same module), this is correctly a no-op.
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
 * was never the owner -- e.g. the navigator's own nav_root, still open
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
 
    const ::std::string module_name = target->name;
    ETCS_LOG("DynamicLoader", "Module '" << module_name
        << "' RequestUnload recheck: still vacant after the delay -- "
           "unloading now.");
    // Loader-side bookkeeping first: nothing new can find this module while
    // its mapping is being torn down. (The signal-raise that used to sit here
    // is unmapLibrary's step 1 now -- see its comment for why it is
    // module-scoped rather than process-wide.)
    module_registry.erase(module_name);
    // Before the library goes: the trampoline it holds lives in that DSO.
    ETCS::module_log_nodes().erase(module_name);
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
 * enqueues a Kind::Ack onto reply_to->stream and returns -- see the body
 * below for why it no longer waits, and why the event is heap-allocated.
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
bool ETCS::EventNode::LoaderStream::resolveImpl(const ::std::string& module_name, ETCS::LifetimeOwner entity)
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
    const ::std::string& conjugate_key, ETCS::LifetimeOwner bootstrap_root)
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
bool ETCS::EventNode::LoaderStream::destroyImpl(const ::std::string& conjugate_key, ETCS::RID rid,
                                                 bool delete_children)
{
    // RID-aware: a family name has one list PER PROVIDER, and the RID is what
    // says which. Asking by name alone could only answer for one of them.
    ETCS::RIDListHandle* handle =
        ETCS::etcs_ridmap_row(owner, ETCS::Buffer(conjugate_key.c_str()), rid);
    if (!handle)
    {
        ETCS_LOG("DynamicLoader", "destroyImpl: RID " << rid
                 << " is in no RIDList registered for: " << conjugate_key);
        return false;
    }
 
    ETCS::Entity* target = handle->invoke_get(rid);

    /*
 * THE GATE CLOSES BEFORE THE LISTS DO, and that order is the whole of it.
 *
 * Retiring first means no new hold is granted from here on, so no new walk
 * can start; the removals below then mean no new RESOLUTION can start either.
 * Together those shut the front door. The wait further down shuts the back
 * one: whatever was already walking gets to finish before the memory it is
 * walking goes away.
 *
 * The other order -- lists first, gate second -- leaves exactly the window
 * this exists to close, because a resolve that has already returned is not
 * stopped by removing anything.
 */
    if (target) target->beginRetire();
    /*
 * Fan in -- the inverse of the fanout that published this entity, replacing the
 * loop that used to be here.
 *
 * Why the removal matters, kept from what stood here: without it those lists
 * grow without bound in the module's root arena (measured at ~94MB over six
 * hours of one polling page), and every entry past the first is a dangling
 * Entity*, so anything iterating an aggregate walks freed memory.
 *
 * Why it is a call now: what was here walked getTags() and built "Module:Tag"
 * keys, while fanout inserts under the bare names getInterfaceFamilies()
 * returns. The two never named the same key, so the aggregates this was written
 * to clean were never cleaned by it. etcs_supertype_fanin reads the same
 * accessor the insert does and covers both scopes. Position unchanged: before
 * awaitQuiesced, while target still resolves.
 */
    if (target) ETCS::etcs_supertype_fanin(target);
    // Presence was established by the invoke_contains check above, and fan-in
    // removed it from everything -- so "did this remove a live entity" is
    // "was there one to remove".
    const bool removed = (target != nullptr);
    ETCS_LOG("DynamicLoader", "destroyImpl: fanned RID " << rid << " out of "
        << conjugate_key << " -> " << (removed ? "ok" : "no target"));

    if (removed && target)
    {
        /*
 * The last thing between the removals and the free: whatever was already
 * walking this entity when the gate closed.
 *
 * Bounded, and a timeout degrades to the OLD behaviour rather than to a
 * hang -- free anyway and say so, naming the entity. A walk still holding
 * after two seconds has broken the one rule a held region carries (see
 * ETCS_ASSERT_NO_LIFETIME_HOLD), and this is where that becomes a line in
 * the log instead of a parked ordering thread.
 */
        if (!target->awaitQuiesced(2000))
            ETCS_LOG("DynamicLoader", "destroyImpl: RID " << rid << " (" << conjugate_key
                     << ") still has " << target->lifetimeHolds() << " lifetime hold(s) 2s "
                     "after retiring -- freeing anyway. Something is holding a resolve "
                     "across a wait.");

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
    ::std::string child_type_tag = tag.toString();
 
    auto owner_it = type_owner_index.find(child_type_tag);
    if (owner_it != type_owner_index.end())
    {
        const ::std::string& mod_name = owner_it->second;
        auto reg_it = module_registry.find(mod_name);
        Module* mod = (reg_it != module_registry.end()) ? reg_it->second : nullptr;
 
        if (mod == nullptr)
        {
            /*
 * Vacant: bootstrap the module. A module root (a child made under another
 * module's entity -- Entity::crossesModule) bootstraps it itself and claims
 * the token, as any first attach does. A same-module child's module is its
 * parent's and already loaded, so reaching here without one means the
 * parent chain was never attached: bootstrap against its ultimate ancestor
 * first, which then holds the token.
 */
            if (!child->isModuleRoot()) attachModule(mod_name, parent->getRootAncestor(), "");
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
 * Populate child's own module_ as a proxy onto the already-loaded global
 * instance -- a plain pointer assignment, not the full attachModule().
 *
 * A MODULE ROOT then stands for the token like any global-scope entity: it
 * is a root of its own module's arena (Entity::module_root_), and dying it
 * hands the token to a sibling there or gives it up (registerDtor<T>'s
 * module-root branch, MemoryArena.h). An ordinary child never claims: its
 * module is held by the root above it, and its own death reparents or
 * cascades without an election.
 */
                child->module_.parent = mod;
                if (child->isModuleRoot()) claimLifetime(mod, child);
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
 
    /*
 * A CHILD ENTERING IS A SUBTREE CHANGE, the mirror of etcs_retire_entity's
 * mark for one leaving. This is the funnel: every addTag<T>, module-side or
 * loader-side, arrives here through AddTagEvent, so marking once here covers
 * every creation rather than relying on each leaf type's Create body
 * happening to set a flag.
 *
 * After the trampoline, deliberately -- it is what inserts into
 * typed_children_ and sets parent_, and an observer woken any earlier could
 * walk a tree the child is not in yet. It also holds parent->m_tagMutex for
 * its whole body, and MarkObserved takes its own lock and walks parents.
 */
    const ETCS::RID rid = trampoline(parent, child, tag);
    ETCS::Entity::markStateChange(parent, rid);
    return rid;
}
 
bool ETCS::EventNode::LoaderStream::isTypedActionStream(
    const ::std::string& origin, const ::std::string& conjugate_key)
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
        ETCS::Entity* live = etcs_tagmodify_target(evt);
        evt.release_value = (live && evt.tagmodify_impl(live, evt.conjugate_key,
                                                         evt.tagmodify_is_remove)) ? 1 : 0;
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
 
    // Fallback path only: Load/Resolve/Destroy/AddTag/EntityUnload enqueue
    // straight onto getLoader().stream themselves (EntityUnloadEvent explains
    // why). This forward remains for anything else still enqueuing onto
    // EventNode::getInstance().stream from the module side.
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
#if defined(__EMSCRIPTEN__)
inline void etcs_boot_runtime_threads()
{
    ETCS_LOG("ETCS", "[trace] etcs_boot: enter");
    g_etcs_runtime_threads_started.store(true, ::std::memory_order_release);
    ETCS::MemoryArena::getInstance();
    dynamicLoader.node = &ETCS::EventNode::getInstance();

    // Ordering threads BEFORE ThreadPool workers
    auto& loader_node = ETCS::EventNode::getInstance();
    ETCS_LOG("ETCS", "[trace] etcs_boot: arming loader ordering thread scope="
             << loader_node.scope);
    loader_node.stream.arm_emscripten_ordering_thread();
    ETCS_LOG("ETCS", "emscripten: loader ordering thread armed");

    /*
 * Each module's node, through its own trampoline (EventNode::arm_runtime):
 * this starts the module's ordering thread AND its ThreadPool workers, both
 * of which are that image's own objects. ETCS::emscripten_deferred_module_nodes
 * is the list registerLoader filled -- ETCS:: spelled out, because an
 * unqualified name here can bind a global-scope definition and drain an empty
 * list.
 */
    for (ETCS::EventNode* mod_node : ETCS::emscripten_deferred_module_nodes())
    {
        if (!mod_node || !mod_node->arm_runtime) continue;
        ETCS_LOG("ETCS", "[trace] etcs_boot: arming module runtime scope="
                 << (mod_node->scope ? mod_node->scope : "(null)")
                 << " node=" << (void*)mod_node);
        mod_node->arm_runtime();
        ETCS_LOG("ETCS", "emscripten: module ordering thread and workers armed scope="
                 << mod_node->scope);
    }
    ETCS::emscripten_deferred_module_nodes().clear();

    /*
 * NOW THE WORKERS, AND THIS IS THE POINT OF THE DEFERRAL RATHER THAN A
 * RELAXATION OF IT.
 *
 * What is unsafe in the browser is spawning a thread BEFORE the modules are
 * loaded -- a Worker started while the main module is still inside
 * loadDynamicLibrary finds wasmMemory undefined, which is what
 * ETCS_MODULE_STATIC_REACH_LOADER and this whole deferred boot exist to avoid.
 * By the time this function runs, preload_web_modules has finished every dlopen
 * (loaders/etcs.cc calls them in that order, deliberately), so the condition the
 * deferral was protecting against no longer holds and there is nothing left to
 * wait for.
 *
 * LEAVING THEM DEFERRED FOREVER WAS NOT A SAFE DEFAULT, it was a silent one. A
 * stream edge -- `producer() -> consumer()` -- enqueues onto this pool
 * (ETCS_MODULE_EXPORT_STREAM, ETCS_API.h), so with no workers the producer is
 * queued and never runs: the edge is stated, nothing refuses it, and no event
 * ever arrives. A window's input pump IS such an edge
 * (window_events.etcs: ProduceEvents -> ConsumeEvents), so on the web it was
 * accepted and dead. Arming here is what makes a detached pump actually pump.
 */
    ETCS_LOG("ETCS", "[trace] etcs_boot: arming ThreadPool workers");
    ETCS::ThreadPool::getInstance().arm_emscripten_workers();
    ETCS_LOG("ETCS", "emscripten: ThreadPool workers armed (modules are all loaded, "
             "so a Worker can no longer start inside a dlopen)");
    ETCS_LOG("ETCS", "[trace] etcs_boot: leave");
}
#endif

inline const bool _core_init = []() {
    WIRE_ROOT_SIGNAL_CONTEXT();
    /*
 * Single producer - LoaderStream consumer is the sole writer of ridMap,
 * active_bundles, and active_modules. Replaces the old shared_mutex.
 */
    ETCS::MemoryArena::getInstance();
    dynamicLoader.node = &ETCS::EventNode::getInstance();
    /*
 * THE POOL IS NOT TOUCHED HERE WHEN IT IS SHARED, and that is what keeps the
 * adoption in RegisterDynamicLoader from orphaning anything: this runs during
 * a module's static init, BEFORE the loader has had a chance to hand its pool
 * over, so constructing one here would leave a second pool that adoption then
 * makes unreachable. Skipping it means the first getInstance() in this image
 * happens after adoption and answers with the loader's.
 *
 * Unconditional when pools are per-image, where the eager construction is the
 * point -- it is what makes the pool exist before anything enqueues onto it.
 */
#if !ETCS_SHARED_THREAD_POOL
#  if defined(__EMSCRIPTEN__)
    // May construct pool / call start(); both defer std::thread until boot.
    (void)ETCS::ThreadPool::getInstance();
#  else
    ETCS::ThreadPool::getInstance();
#  endif
#endif
    ETCS::EventNode::getInstance().stream.start(
        ETCS::MemoryArena::getInstance(), 1);
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

// The process's one provenance slot (core/Provenance.h), found by modules the
// same way as the manifest above.
extern "C"
#ifdef _WIN32
__declspec(dllexport)
#else
__attribute__((visibility("default")))
#endif
void* ETCS_GetProvenance()
{
    return static_cast<void*>(&ETCS::provenance_local());
}
#endif // ETCS_LOADER

/*
 * RegisterDynamicLoader - the module-side entry point the loader calls
 * immediately after dlopen, handing this module a pointer to the loader's
 * own EventNode. Wrapped in try/catch because this runs across the dlopen
 * boundary (extern "C"): an exception unwinding through it, with
 * -fvisibility=hidden hiding the RTTI a handler on the far side would need,
 * is what "terminate called without an active exception" looks like.
 *
 * Both catch blocks CRASH deliberately. Logging and returning nullptr would let
 * Module::registerLoader fail the load "gracefully", and graceful failure here
 * is the wrong instinct: a module that fails to load for a structural reason
 * (the OS not having finished tearing down a just-unloaded instance of this
 * exact library, or a genuine allocation failure) has violated the guarantee
 * that ETCS scripts execute deterministically, and returning nullptr converts
 * that into a soft "module not available" some later, unrelated line behaves
 * differently around. Stopping loudly, naming the condition, is also what
 * enforces the invariant that a root-hosted Module is not created and destroyed
 * in a tight cycle -- that races the OS's asynchronous post-dlclose teardown,
 * and the crash makes it impossible to do silently rather than merely slow.
 *
 * No manifest check here: it runs in ETCS_MODULE's static-init lambda
 * (ETCS_API.h), which dlsym(RTLD_DEFAULT)s ETCS_GetLoaderManifest the instant
 * dlopen maps this library, so by the time control reaches this function each
 * side has already verified the other independently (the loader's half is
 * Module::registerLoader's validateManifest, before it calls this).
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
 * No signal wiring here: re-registering ::std::signal() per module would
 * silently steal OS signal disposition from the loader (process-global, last
 * dlopen wins). The loader hands this module its real root SignalContext* via
 * RegisterRootSignalContext() below, called from Module::registerLoader()
 * immediately after this function returns.
 */
        ETCS::MemoryArena::getInstance(); // this may actaully be the real cleanup order
        ETCS_LOG("DynamicLoader", "Passed MemoryArena! ");
#if ETCS_SHARED_THREAD_POOL
        /*
 * ADOPT THE LOADER'S POOL, before this image's own getInstance() is asked for
 * anything. See ThreadPool::adopt_shared: one pool for the whole runtime,
 * because on the web a pool is Workers and a Worker is every open side module
 * instantiated again.
 *
 * UNDER ETCS_SHARED_THREAD_POOL, which is on for web builds and off for native
 * ones -- see that macro (core/ETCS_API.h) for why the two substrates want
 * opposite defaults, and for what has to stay true of a module for the native
 * setting to be safe to flip.
 */
        if (dynamicLoader.node && dynamicLoader.node->get_pool)
        {
            ETCS::ThreadPool::adopt_shared(dynamicLoader.node->get_pool());
            ETCS_LOG("DynamicLoader", "adopted the loader's ThreadPool -- one pool "
                     "for the runtime, so this image arms no Workers of its own");
        }
        else
            ETCS_LOG("DynamicLoader", "loader node offers no pool trampoline -- "
                     "this image will use its own ThreadPool (older loader)");
#endif
        ETCS::ThreadPool::getInstance();
        ETCS_LOG("DynamicLoader", "Passed ThreadPool! ");
#if defined(__EMSCRIPTEN__)
        /*
         * The registrations static-init queued instead of running (see
         * ETCS_MODULE_STATIC_REACH_LOADER): this is the first point the loader
         * is on the other end of a call into this module, and the count is
         * logged because a skipped flush looks exactly like a completed one.
         */
        const size_t flushed = ETCS::etcs_flush_deferred_rid_registrars();
        ETCS_LOG("DynamicLoader",
            "emscripten: flushed " << flushed << " deferred RIDList registrar(s) "
            "(per-type AND per-family)");
#endif
        /*
         * THIS module's own stream, on every platform. In the browser start()
         * allocates the rings and returns in sync-poll mode; the ordering thread
         * comes later, from etcs_boot_runtime_threads through arm_runtime, once
         * every dlopen is done. It cannot be skipped there: TagModifyEvent orders
         * locally, onto this stream, and an addTag from a work function reaches
         * a ring that was never allocated -- a spin that never returns, with
         * nothing logged.
         */
        ETCS::EventNode::getInstance().stream.start(
            ETCS::MemoryArena::getInstance(), 1);
        ETCS_LOG("DynamicLoader", "Passed EventNode stream start! ");
        return &ETCS::EventNode::getInstance();
    }
    catch (const ETCS::EventStreamZombieException&)
    {
        ::std::cerr << "Reloaded DLL " << ETCS_MODULE_NAME
                   << " too many times within a very short period: "
                      "OS generated zombie DLL (zombie thread in EventStream "
                      "detected)" << ::std::endl;
        ::std::abort();
    }
    catch (const ::std::exception&)
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
        ::std::cerr << "DLL cannot be loaded: Out of memory" << ::std::endl;
        ::std::abort();
    }
    catch (...)
    {
        ::std::cerr << "DLL cannot be loaded: unknown fatal error during "
                      "RegisterDynamicLoader." << ::std::endl;
        ::std::abort();
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
        ::std::this_thread::sleep_for(::std::chrono::milliseconds(10));
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
    ETCS_ASSERT_NO_LIFETIME_HOLD("LoadEvent");
    DLInEvent evt{};
    evt.kind = DLInEvent::Kind::Load;
    evt.conjugate_key = conjugate_key;
    evt.entity_out = &result;
    evt.prebuilt_entity = prebuilt;
    evt.bootstrap_root = root;
#if !defined(ETCS_LOADER)
    /*
 * Module-side caller: reply_to lets the loader ack back onto THIS
 * module's own stream after it finishes. Every platform, the browser
 * included: the module's node and stream are its own there too, and the
 * ack is what orders the completion on the module's ordering thread.
 */
    evt.reply_to = &ETCS::EventNode::getInstance();
    evt.origin_extra_mask = ETCS::ActivePairModuleMask();
#endif
    // Straight to the loader's own stream, never this side's own possibly
    // torn-down local one -- see EntityUnloadEvent below for the hang that
    // routing memory-altering events through a dead local stream caused.
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
#if defined(__EMSCRIPTEN__)
    etcs_emscripten_spin([&]{
        e = result.load(::std::memory_order_acquire);
        return e != nullptr;
    });
#else
    while (!(e = result.load(::std::memory_order_acquire)));
#endif
    return e == reinterpret_cast<ETCS::Entity*>(UINTPTR_MAX) ? nullptr : e;
}
 
inline bool ETCS::ResolveEvent::operator()()
{
    ETCS_ASSERT_NOT_ORDERING_THREAD("ResolveEvent");
    ETCS_ASSERT_NO_LIFETIME_HOLD("ResolveEvent");
    DLInEvent evt{};
    evt.kind           = DLInEvent::Kind::Resolve;
    evt.conjugate_key  = conjugate_key;
    evt.resolve_target = target;
    evt.resolve_ok     = &ok;
#if !defined(ETCS_LOADER)
    evt.reply_to = &ETCS::EventNode::getInstance();
    evt.origin_extra_mask = ETCS::ActivePairModuleMask();
#endif
    if (!getLoader().stream.enqueue(DLInEventPtr{&evt}))
        return false;
    int8_t r;
#if defined(__EMSCRIPTEN__)
    etcs_emscripten_spin([&]{
        r = ok.load(::std::memory_order_acquire);
        return r >= 0;
    });
#else
    while ((r = ok.load(::std::memory_order_acquire)) < 0);
#endif
    return r != 0;
}
 
inline bool ETCS::DestroyEvent::operator()()
{
    ETCS_ASSERT_NOT_ORDERING_THREAD("DestroyEvent");
    ETCS_ASSERT_NO_LIFETIME_HOLD("DestroyEvent");
    /*
 * THE expected drain point. `target` is optional only because the older
 * two-and-three-arg constructors predate it; when supplied, no caller has
 * to remember to signal-and-wait by hand, which is the whole point.
 */
    if (!ETCS::drainEntityScopes(target, "DestroyEvent")) return false;
    // Read while the target is whole: a child leaving is its parent's change.
    ETCS::Entity* const leaving_from = target ? target->getParent() : nullptr;
    const ETCS::RID     leaving      = target ? target->getRID() : rid;
    DLInEvent evt{};
    evt.kind = DLInEvent::Kind::Destroy;
    evt.conjugate_key = conjugate_key;
    evt.rid = rid;
    evt.destroy_children = delete_children;
    evt.tri_out = &result;
#if !defined(ETCS_LOADER)
    evt.reply_to = &ETCS::EventNode::getInstance();
    evt.origin_extra_mask = ETCS::ActivePairModuleMask();
#endif
    if (!getLoader().stream.enqueue(DLInEventPtr{&evt}))
        return false;
    int8_t r;
#if defined(__EMSCRIPTEN__)
    etcs_emscripten_spin([&]{
        r = result.load(::std::memory_order_acquire);
        return r >= 0;
    });
#else
    while ((r = result.load(::std::memory_order_acquire)) < 0);
#endif
    if (r != 0 && leaving_from) ETCS::Entity::recordEffect(leaving_from, ETCS::Entity::childKey(leaving), false);
    if (r != 0) ETCS::forget_script_name(leaving);
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
    ETCS_ASSERT_NO_LIFETIME_HOLD("AddTagEvent");
    DLInEvent evt{};
    evt.kind              = DLInEvent::Kind::AddTag;
    evt.conjugate_key     = conjugate_key;
    evt.addtag_parent     = parent;
    evt.addtag_child      = child;
    evt.addtag_trampoline = trampoline;
    evt.rid_out           = &result;
    evt.ready_out         = &ready;
#if !defined(ETCS_LOADER)
    evt.reply_to = &ETCS::EventNode::getInstance();
    evt.origin_extra_mask = ETCS::ActivePairModuleMask();
#endif
    if (!getLoader().stream.enqueue(DLInEventPtr{&evt}))
        return 0;
#if defined(__EMSCRIPTEN__)
    etcs_emscripten_spin([&]{ return ready.load(::std::memory_order_acquire); });
#else
    while (!ready.load(::std::memory_order_acquire));
#endif
    // A child made inside an action is part of that action's effect on its
    // parent (Entity::recordEffect, core/Provenance.h).
    const ETCS::RID made = result.load(::std::memory_order_relaxed);
    if (made && parent) ETCS::Entity::recordEffect(parent, ETCS::Entity::childKey(made), true);
    return made;
}
 
/*
 * EntityUnloadEvent - blocking, same spin pattern as AddTagEvent. Fired
 * unconditionally from removeTag's own entity-relation deletion
 * (tagModifyImpl, Entity.h) -- its only remaining caller now that
 * ~Entity() no longer triggers anything itself (see entityUnloadImpl's
 * own comment for why). By the time this returns, entityUnloadImpl has
 * determined target's correct parent arena and MemoryArena::deleteEntity
 * has fully run: election-and-evoke (module root) or reparent-and-evoke
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
    ETCS_ASSERT_NO_LIFETIME_HOLD("EntityUnloadEvent");
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
#if !defined(ETCS_LOADER)
    evt.reply_to = &ETCS::EventNode::getInstance();
    evt.origin_extra_mask = ETCS::ActivePairModuleMask();
#endif
    if (!getLoader().stream.enqueue(DLInEventPtr{&evt}))
        return;
#if defined(__EMSCRIPTEN__)
    etcs_emscripten_spin([&]{ return done.load(::std::memory_order_acquire); });
#else
    while (!done.load(::std::memory_order_acquire));
#endif
}
 
/*
 * Root::changeModule() - see its own declaration comment, Entity.h.
 * Just builds and fires a ChangeModuleEvent -- all the actual logic
 * (survivor search via root_registry, promoteOrVacate, the reattach)
 * lives in changeModuleImpl above, on the loader's own ordering thread.
 */
inline void ETCS::Root::changeModule(const ::std::string& targetModule)
{
    ETCS::ChangeModuleEvent evt(targetModule, this);
    evt();
}

#if defined(__EMSCRIPTEN__) && defined(ETCS_LOADER)
inline bool ETCS::etcs_web_root_attach_module(ETCS::Root& root, const ::std::string& module_name)
{
    return ETCS::getLoader().stream.attachModule(module_name, &root, "");
}
#endif

 
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
    ETCS_ASSERT_NO_LIFETIME_HOLD("ChangeModuleEvent");
    DLInEvent evt{};
    evt.kind              = DLInEvent::Kind::ChangeModule;
    evt.conjugate_key      = conjugate_key;
    evt.changemodule_root  = root;
    evt.changemodule_done  = &done;
#if !defined(ETCS_LOADER)
    evt.reply_to = &ETCS::EventNode::getInstance();
    evt.origin_extra_mask = ETCS::ActivePairModuleMask();
#endif
    if (!getLoader().stream.enqueue(DLInEventPtr{&evt}))
        return;
#if defined(__EMSCRIPTEN__)
    etcs_emscripten_spin([&]{ return done.load(::std::memory_order_acquire); });
#else
    while (!done.load(::std::memory_order_acquire));
#endif
}
 
/*
 * TagModifyEvent - blocking, same spin pattern as every other event here.
 * Fired from Entity::addTag(Buffer flag)/removeTag(Buffer tag). Ordered
 * LOCALLY (see ModuleProxy::on_event's fork) - never crosses to the
 * loader itself, regardless of which scope fires it.
 */
inline bool ETCS::TagModifyEvent::operator()()
{
    ETCS_ASSERT_NOT_ORDERING_THREAD("TagModifyEvent");
    ETCS_ASSERT_NO_LIFETIME_HOLD("TagModifyEvent");
    DLInEvent evt{};
    evt.kind                = DLInEvent::Kind::TagModify;
    evt.conjugate_key       = conjugate_key;
    evt.tagmodify_rid       = target->getRID();
    evt.tagmodify_type      = target->myConjugateKey();
    evt.tagmodify_target    = target;
    evt.tagmodify_is_remove = is_remove;
    evt.tagmodify_impl      = impl;
    evt.tagmodify_done      = &done;
    evt.tagmodify_changed   = &changed;
    /*
 * ACQUIRED HERE, from the thread this event is being emitted on, exactly as
 * origin_extra_mask is at every other site in this file:
 *
 *     own bit          identity, unconditional -- two ops on one entity
 *                      serialize whatever either of them reaches
 *   | causal edge      what the WORK FUNCTION running on this thread has
 *                      been observed to touch (CausalScope, Bundles.h)
 *   | extra            the one thing a caller supplies: ScopeTag's stream
 *                      pair, so a flag orders against both halves
 *
 * falling back to the type's whole TAG_CLOSURE while no frame has settled.
 * Resolved at the EMIT site and never by the handler -- see
 * DLInEvent::tagmodify_mask's own comment (EventNode.h) for why that lookup
 * was outright wrong on the loader's side.
 */
    evt.tagmodify_mask      = ETCS::CausalEdgeMask(target->myTagMask(),
                                                   target->myTagClosure())
                            | extra_mask;
    /*
 * Fail shut. An empty mask means the emitting type has no contract identity
 * at all, and a type the ordering system has never heard of is the last
 * thing to grant independence from it -- same reasoning as DispatchResult.
 */
    if (!evt.tagmodify_mask.any())
        evt.tagmodify_mask  = ETCS::TagMask::all();
    /*
 * A REFUSED ENQUEUE ANSWERS "not mine", and that is the right answer: the
 * stream is already cleaning up, so there is no ordering left to join and
 * nothing should be acting on a claim it thinks it won.
 */
    if (!ETCS::EventNode::getInstance().stream.enqueue(DLInEventPtr{&evt}))
        return false;
#if defined(__EMSCRIPTEN__)
    // This node's loop, not the loader's: the event is on THIS stream.
    etcs_emscripten_spin(ETCS::EventNode::getInstance(),
                         [&]{ return done.load(::std::memory_order_acquire); });
#else
    while (!done.load(::std::memory_order_acquire));
#endif
    return changed.load(::std::memory_order_acquire);
}
 
/*
 * PairMaskEvent - blocking, same spin pattern as everything here. Straight onto
 * getLoader().stream for the same reason the five memory-altering kinds go
 * direct (EntityUnloadEvent).
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
    ETCS_ASSERT_NO_LIFETIME_HOLD("PairMaskEvent");
    DLInEvent evt{};
    evt.kind           = DLInEvent::Kind::PairMask;
    evt.conjugate_key  = conjugate_key;
    evt.pairmask_tag_b = tag_b;
    evt.pairmask_out   = &result;
    evt.pairmask_done  = &done;
#if !defined(ETCS_LOADER)
    evt.reply_to = &ETCS::EventNode::getInstance();
    evt.origin_extra_mask = ETCS::ActivePairModuleMask();
#endif
    if (!getLoader().stream.enqueue(DLInEventPtr{&evt}))
        return;
#if defined(__EMSCRIPTEN__)
    etcs_emscripten_spin([&]{ return done.load(::std::memory_order_acquire); });
#else
    while (!done.load(::std::memory_order_acquire));
#endif
}
 
#ifndef ETCS_LOADER
inline ETCS::Entity* ETCS::CreateEvent::operator()()
{
    ETCS_ASSERT_NOT_ORDERING_THREAD("CreateEvent");
    ETCS_ASSERT_NO_LIFETIME_HOLD("CreateEvent");
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
#if defined(__EMSCRIPTEN__)
    etcs_emscripten_spin([&]{
        e = result.load(::std::memory_order_acquire);
        return e != nullptr;
    });
#else
    while (!(e = result.load(::std::memory_order_acquire)));
#endif
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
 
// The loader's EventNode -- the one node that holds every module's lists under
// their origin-affixed keys, and therefore the only thing that can resolve a
// COMPOSED entity for a module that did not create it (core/Entity.h's
// resolve_in_family). Declared there and defined here because that is the
// direction the include graph allows. Null in a build with no loader, where
// nothing has been composed and nothing should resolve.
namespace ETCS {
inline EventNode* etcs_loader_event_node() { return ::dynamicLoader.node; }
}

#endif
 
