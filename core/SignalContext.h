#ifndef SIGNALCONTEXT_H__
#define SIGNALCONTEXT_H__
#include "Buffer.h"
#include <csignal>
#include <atomic>

// Signal flags. std::atomic, not volatile: volatile gives no cross-thread
// ordering at all, so the old form worked on x86 only by TSO accident (and
// by the optimizer choosing not to hoist ordinary loads across the volatile
// read, which it was always free to do). Costs nothing on x86-64 -- acquire
// and release both lower to plain mov -- and is required on aarch64. No
// architecture conditional on purpose: the happens-before requirement is
// identical everywhere, and an #ifdef would make causal exhaustion a
// property of the host ISA rather than of the module.
//
// Lock-free atomics are permitted in signal handlers ([support.signal]);
// the static_assert makes that precondition checked rather than assumed.
//
// inline: namespace-scope definitions in a header. Per-DSO copies are
// intentional and inert -- AdoptRootSignalContext repoints a module's slot
// at the loader's instance, so a module's own copies are never read.

namespace ETCS { using SignalFlag = std::atomic<sig_atomic_t>; }
static_assert(ETCS::SignalFlag::is_always_lock_free,
              "signal flags must be lock-free to be async-signal-safe");

extern "C" void global_signal_handler(int sig);

inline ETCS::SignalFlag g_sig_int {0};
inline ETCS::SignalFlag g_sig_term{0};
inline ETCS::SignalFlag g_sig_usr1{0};

extern "C" inline void global_signal_handler(int sig)
{
    switch(sig) {
        case SIGINT:  g_sig_int .store(1, std::memory_order_release); break;
        case SIGTERM: g_sig_term.store(1, std::memory_order_release); break;
        case SIGUSR1: g_sig_usr1.store(1, std::memory_order_release); break;
    }
}

// Loader scope ONLY, once, in _core_init. Modules get authority via
// AdoptRootSignalContext instead -- see below.
//
// All three dispositions use sigaction with SA_RESTART deliberately cleared.
// SA_RESTART controls only whether a blocked syscall restarts after the
// handler returns -- it does not affect whether the handler runs or the
// flag is set. The previous form used std::signal (implicit SA_RESTART on
// glibc) out of a mistaken concern: that an EINTR return would let the
// outermost blocking call "consume" the interrupt before the owning scope
// saw it. But the flag is already set by the handler; EINTR only controls
// when the thread wakes, not which scope handles it. With SA_RESTART, the
// thread stays asleep and the flag goes unnoticed until the next voluntary
// yield -- too late for prompt teardown. Without it, EINTR wakes the frame,
// the flag walk finds the raised global flag, and the interrupt propagates
// to the correct scope as designed.
//
// The non-TTY stdin case that originally motivated SA_RESTART -- read(2)
// returning -1/EINTR on SIGINT instead of delivering the typed line -- is
// fixed by polling (repl_shell_get_char_unix, ShellREPL.h) and does not
// need global syscall restart.
#define WIRE_ROOT_SIGNAL_CONTEXT()                              \
    do {                                                        \
        ETCS::SignalContext& root = ETCS::RootSignalContext();  \
        root.interrupt = &g_sig_int;                            \
        root.terminate = &g_sig_term;                           \
        root.user1     = &g_sig_usr1;                           \
                                                                \
        struct sigaction sa {};                                  \
        sa.sa_handler = global_signal_handler;                  \
        sigemptyset(&sa.sa_mask);                               \
        sa.sa_flags = 0; /* no SA_RESTART */                    \
        sigaction(SIGINT,  &sa, nullptr);                       \
        sigaction(SIGTERM, &sa, nullptr);                       \
        sigaction(SIGUSR1, &sa, nullptr);                       \
    } while (0)


namespace ETCS 
{

// One level of signal authority plus a link to its parent.
//
// Previously seven local flags AND seven flattened *_parent slots. A single
// flat slot can only hold one ancestor, so setParent had to pick which --
// it picked the topmost, discarding every intermediate level. Result: a
// detached job's own flag was invisible to anything nested inside it, so
// `signal <id> interrupt` reached exactly one hop.
//
// LIFETIME: a SignalContext must outlive anything naming it as parent.
// Satisfied structurally at every tier, not by discipline:
//   root    function-local static, outlives everything.
//   run     stack, but CmdRun blocks on run_script -- the frame cannot
//           return while the child runs. That IS the guarantee.
//   detach  no such guarantee, so never parents to a stack scope.
//           DetachedExecutor is heap-owned until join_all, so
//           detach->detach is fine; detach from a run scope roots at
//           global instead (CmdDetach, CommandExecutor.h).
//   scope   Scope::Entry owns a SNAPSHOT of the caller's context and
//           parents to that -- DEFINE_STREAM_FUNC_PRODUCE moves its
//           ScopeTag into a pool lambda that outlives the trampoline
//           frame (Scope::registerContext, Bundles.h).
//
// ABI: sizeof changed, and this crosses the DSO boundary by value in
// WorkFunc/StreamFunc. Rebuild every module; the manifest hash enforces it.
struct SignalContext
{
    ETCS::Buffer tag    = "invalid_tag_signal_context";
    ETCS::Buffer parent = "invalid_parent_signal_context";

    // Local authority. Null means this level has none for that signal --
    // the chain is still walked.
    SignalFlag* interrupt = nullptr;
    SignalFlag* terminate = nullptr;
    SignalFlag* hangup    = nullptr;
    SignalFlag* pause     = nullptr;
    SignalFlag* resume    = nullptr;
    SignalFlag* user1     = nullptr;
    SignalFlag* user2     = nullptr;

    // ACTIVE edge -- the call chain. Who invoked the work this context
    // governs (global -> detach -> run -> scope). Dynamic, per-call,
    // rebuilt every time a context is forwarded.
    const SignalContext* up = nullptr;

    // PASSIVE edge -- the ownership chain. Which entity structurally owns
    // the entity this context belongs to. Static across an entity's life,
    // rewritten in exactly one place (Entity::reparentChildrenTo) when the
    // ownership graph itself changes.
    //
    // Two edges rather than one because they answer different questions and
    // a single slot can only hold one answer -- the same failure the flat
    // *_parent slots had before `up` replaced them, one level up. "My caller
    // was interrupted" and "the entity hosting me is being torn down" are
    // both reasons to stop, and neither implies the other: a detached job's
    // caller may be long gone while its host entity is fine, and a host can
    // be deleted mid-call while the caller is still happily blocked.
    //
    // LIFETIME, and this one is sharper than `up`'s: a provider normally
    // points at an Entity's own ctx_, which lives in that entity's OUTER
    // SHELL -- and reclaimEntity (MemoryArena.h) zeroes those bytes and
    // pushes them onto the exact-(size, alignment) free list, where the next
    // allocate<T> of the same concrete type takes them. A stale provider is
    // therefore not a dangling pointer that crashes; it is a valid pointer
    // to a DIFFERENT live entity's ctx_, silently rerouting signal authority
    // into an unrelated subtree. Every path that reclaims an entity must
    // repoint its children's providers first.
    const SignalContext* provider = nullptr;

    // Self-check only. Re-registering under an occupied key is the one way
    // a caller plausibly hands this its own address, and a self-link makes
    // every getter recurse forever. Longer cycles need two deliberate
    // setParent calls forming a loop; every construction site parents to
    // something strictly older, so they aren't reachable. (The getters
    // short-circuit on a raised flag, but the common case is a poll finding
    // nothing -- so unreachability is what protects the walk, not the flag
    // semantics.)
    void setParent(const SignalContext* parent_ptr)
    {
        if (parent_ptr == this) return;
        up = parent_ptr;
    }

    // Same self-check, same reasoning -- see setParent above. The cycle
    // argument extends cleanly: a provider always points at a strictly
    // OLDER entity (a child's provider is its parent, set where parent_
    // itself is set; a reparent only ever moves it further up the same
    // tree, never down). Mixed cycles across the two edges would need an
    // Entity's own ctx_.up to point at a descendant, which never happens --
    // Entity::call() sets up on a COPY it forwards, never on the entity's
    // own stored ctx_.
    void setProvider(const SignalContext* provider_ptr)
    {
        if (provider_ptr == this) return;
        provider = provider_ptr;
    }

    bool isNull() const
    {
        return !interrupt && !terminate && !hangup && !pause
            && !resume    && !user1     && !user2  && !up && !provider;
    }

    // Acquire, not relaxed: every observer ACTS on these -- tears down a
    // scope, unwinds a job, aborts a read -- so the read must order what
    // follows it.
    static bool raised(const SignalFlag* flag)
    {
        return flag && flag->load(std::memory_order_acquire) != 0;
    }

    // Raised here, or anywhere above. Each level answers for its own flag
    // and delegates the rest. Tail position, so -O2 turns these into a
    // loop; real chains are 3-4 deep (global -> detach -> run -> scope).
    // One walk, seven thin wrappers -- deliberately, rather than seven
    // hand-written traversals. With two edges each getter would repeat the
    // identical three-way disjunction, and a second edge added later (or a
    // getter added later) is exactly the drift this codebase keeps writing
    // out of existence elsewhere: one getter silently missing the provider
    // check would produce a signal that reaches six of seven kinds, which
    // is far harder to notice than one that reaches none.
    //
    // The member pointer is a compile-time constant at every call site, so
    // -O2 still inlines and tail-calls this exactly as the old per-getter
    // form did.
    //
    // Branching cost: both edges are followed, so a context with two
    // distinct chains visits both. Not a blowup in practice -- an Entity's
    // own ctx_ carries a provider and no up, so the passive chain is linear;
    // only a scope context has both, and it branches once into two linear
    // walks of 3-4. A diamond (both chains reaching the same root) revisits
    // that root once, which is a redundant atomic load, not a correctness
    // problem.
    bool walk(SignalFlag* const SignalContext::* flag) const
    {
        if (raised(this->*flag))          return true;
        if (up       && up->walk(flag))       return true;
        if (provider && provider->walk(flag)) return true;
        return false;
    }

    bool isInterrupted() const { return walk(&SignalContext::interrupt); }
    bool isTerminated()  const { return walk(&SignalContext::terminate); }
    bool isHungup()      const { return walk(&SignalContext::hangup);    }
    bool isPaused()      const { return walk(&SignalContext::pause);     }
    bool isResumed()     const { return walk(&SignalContext::resume);    }
    bool isUser1()       const { return walk(&SignalContext::user1);     }
    bool isUser2()       const { return walk(&SignalContext::user2);     }

    // Diagnostic: "why did this interrupt not reach here" without a debugger.
    // Longest path to a root along EITHER edge. Was unambiguous with one
    // edge; now it answers "how far can a signal have to travel to reach
    // me", which is the question this was ever actually asked for.
    // providerDepth() separately reports the passive chain alone, since
    // "which entity generation am I" is a genuinely different diagnostic
    // than "how deep is the call stack".
    int chainDepth() const
    {
        int a = up       ? up->chainDepth()       : 0;
        int b = provider ? provider->chainDepth() : 0;
        return 1 + (a > b ? a : b);
    }

    int providerDepth() const { return 1 + (provider ? provider->providerDepth() : 0); }
};

// Indirection cell for "the one true root". Null means RootSignalContext()
// lazily allocates a local static -- correct in the LOADER binary.
//
// In a module .so that same path would create a second, disconnected
// instance (RTLD_LOCAL guarantees no symbol merging). AdoptRootSignalContext
// overwrites this slot with the loader's real instance so every
// RootSignalContext() call inside the module resolves there instead. That is
// also what makes the chain's root tier safe across the DSO boundary: a
// module context whose `up` reaches the root points at loader-static storage.
inline SignalContext*& RootSignalContextSlot()
{
    static SignalContext* slot = nullptr;
    return slot;
}

inline SignalContext& RootSignalContext()
{
    SignalContext*& slot = RootSignalContextSlot();
    if (!slot)
    {
        static SignalContext instance;
        slot = &instance;
    }
    return *slot;
}

// Module-scope entry point; loader side is RegisterRootSignalContext
// (DynamicLoader.h). Must run before anything in this module has called
// RootSignalContext() and locked in a local instance -- RegisterDynamicLoader
// is always the module's first entry point and this follows it immediately,
// so that ordering holds naturally.
inline void AdoptRootSignalContext(SignalContext* loader_root)
{
    RootSignalContextSlot() = loader_root;
}

}

#endif