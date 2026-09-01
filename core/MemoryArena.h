#ifndef MEMORYARENA_H__
#define MEMORYARENA_H__

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <algorithm>
#include <memory>
#include <new>
#include <type_traits>
#include <cmath>
#include <vector>
#include <unordered_map>
#include <functional>

#include "Buffer.h"

// --- Platform Specific Includes ---
#if defined(_WIN32) || defined(_WIN64)
    #include <windows.h>
#else
    #include <sys/mman.h>
    #include <unistd.h>
    #include <sys/resource.h>
    #ifdef __linux__
        #include <linux/mman.h>
        #ifndef MAP_HUGE_2MB
            #define MAP_HUGE_2MB (21 << MAP_HUGE_SHIFT)
        #endif
    #endif
#endif

// Minimum chunk size hint. pickChunkSize() scales this up to huge page
// granularity automatically if huge pages are available, or keeps it at
// OS page granularity otherwise. Also used directly, with performance=false,
// as the chunk size for entity-local sub-arenas — see Entity()'s
// local_arena_ construction in Entity.h — so entity-local arenas use the
// same standard OS page size rather than a second, separately-chosen value.
#define DEFAULT_ARENA_START_PAGE 4096

// ETCS_MEMORY_ARENA_DEBUG — define this (e.g. -DETCS_MEMORY_ARENA_DEBUG on
// the compiler command line, or #define before including this header) to
// enable the child-page pool's own verbose per-slab/per-chunk/per-page
// tracing: every slab mint and unmap, every Chunk creation, every carved-
// page acquire and release. Added during a real investigation (a cross-
// DSO singleton-mismatch bug -- MemoryArena::getInstance() silently
// resolving to the wrong DSO's own instance depending on which compiled
// code happened to be calling it; see global_arena_'s own comment for the
// full story) and left available behind this flag rather than deleted
// once fixed -- the same CLASS of bug (a page minted or resolved against
// the wrong arena instance) could resurface after some future change,
// and this exact tracing is what actually found it: reasoning about raw
// addresses across a dozen log excerpts by hand, without it, cost real
// turns. Off by default -- at real per-connection churn rates this is far
// too verbose for anything but active investigation. The pool's own
// always-on diagnostics (the "no owning slab found" warning, the
// hugepage-fallback notice, and memoryTeardown()'s own per-arena
// teardown/release lines) are NOT gated by this flag -- those signal a
// real, meaningful condition on their own and are cheap enough (one line
// per arena teardown, not one per page) to stay on unconditionally.
#ifdef ETCS_MEMORY_ARENA_DEBUG
    #define ETCS_ARENA_DEBUG_LOG(scope, msg) ETCS_LOG(scope, msg)
#else
    #define ETCS_ARENA_DEBUG_LOG(scope, msg) do {} while (0)
#endif

namespace ETCS
{

// Forward declaration only. MemoryArena needs to hand back Entity* (as an
// opaque, comparable pointer) from its dtor-chain walk so loader-side code
// (DynamicLoader.h, which DOES have Entity.h fully visible) can search for
// module-scope candidates without MemoryArena.h needing to complete the
// Entity->MemoryArena->Entity circular include Entity.h already documents
// at its own top. MemoryArena.h never dereferences this pointer — only
// stores/returns/compares it — so an incomplete type is sufficient.
class Entity;

/*
 * NO DESTRUCTIBLE STATIC IN HERE, and that is a hard requirement rather than
 * a style preference -- this function is called from memoryTeardown(), which
 * is called from ~MemoryArena(), which runs during static destruction:
 * __cxa_finalize on dlclose for a module arena, __run_exit_handlers at
 * process exit for the loader's.
 *
 * It used to hold `static const std::vector<std::string> units`. A
 * function-local static is registered for destruction on its FIRST CALL, and
 * destroyed in reverse order of registration -- so `units` was registered
 * when something first logged a byte count, which is strictly LATER than the
 * arena's own construction, and therefore destroyed strictly EARLIER than the
 * arena. Every teardown log after that point read a freed vector and streamed
 * a freed std::string.
 *
 * Reproduced under ASAN on both exit paths, from the current dev branch:
 *
 *   heap-use-after-free ... READ of size 8
 *     formatBytesToString  MemoryArena.h:90
 *     MemoryArena::memoryTeardown  MemoryArena.h:1676
 *     ~MemoryArena  MemoryArena.h:903
 *     __cxa_finalize                     <- module unloaded mid-run
 *
 *   heap-use-after-free ... READ of size 2
 *     ... same three frames ...
 *     __run_exit_handlers                <- ordinary process exit
 *
 * Whether that fault is visible depends on whether the freed block still
 * happens to hold intact string data, which is why it presented as an
 * INTERMITTENT segfault at the end of a run rather than a reliable one, and
 * why which scripts hit it looked arbitrary: it is decided by the order in
 * which arenas happen to tear down relative to the first byte-count log.
 *
 * String literals have static storage duration and no destructor at all, so
 * there is nothing left to destroy early. constexpr makes that checkable
 * rather than merely true.
 *
 * inline, too: this is a header, and a non-inline free function here is one
 * multi-TU module away from a duplicate symbol.
 */
inline std::string formatBytesToString(uint64_t bytes)
{
    static constexpr const char* units[] = {"B", "KB", "MB", "GB", "TB"};
    static constexpr int unit_count = static_cast<int>(sizeof(units) / sizeof(units[0]));
    if (bytes == 0) return "0B";
    int magnitude = static_cast<int>(std::log2(bytes) / 10);
    if (magnitude >= unit_count) magnitude = unit_count - 1;
    if (magnitude < 0)           magnitude = 0;
    double value = static_cast<double>(bytes) / std::pow(1024, magnitude);
    std::ostringstream out;
    if (magnitude == 0) out << bytes << units[0];
    else out << std::fixed << std::setprecision(1) << value << units[magnitude];
    return out.str();
}

// --- OS Page Size Detection ---
inline long long getOSPageSize()
{
#if defined(_WIN32) || defined(_WIN64)
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return static_cast<long long>(si.dwPageSize);
#else
    long sz = sysconf(_SC_PAGESIZE);
    return (sz > 0) ? static_cast<long long>(sz) : 4096LL;
#endif
}

// ---------------------------------------------------------------------------
// PageOrigin — which of allocatePage's own three tiers actually served a
// given request. Used by the GLOBAL arena's own child-page pool to
// decide, for EACH slab it mints, whether that specific slab's own
// address can be trusted via arithmetic (ExplicitHuge only — the OS
// guarantees 2MB alignment for a genuine MAP_HUGETLB mapping) or must be
// tracked through an explicit per-page registry instead (TransparentHuge/
// Plain — ordinary anonymous mappings with no alignment guarantee at
// all, even though TransparentHuge may LATER be backed by huge pages via
// khugepaged, which happens after the fact and carries no
// address-alignment promise at allocation time). This decision is made
// fresh at EVERY mint, never assumed from a prior one — see
// MemoryArena::Slab::hugepage_aligned's own comment for why a single,
// process-wide snapshot isn't safe (vm.nr_hugepages is a finite,
// depletable reservation, not a fixed host capability).
// ---------------------------------------------------------------------------
enum class PageOrigin : uint8_t { ExplicitHuge, TransparentHuge, Plain };

// --- Page Allocation / Deallocation Helpers ---
// Returns page-aligned memory of exactly `size` bytes (must be a multiple of page size).
// Tries explicit huge pages first on Linux, falls back to madvise THP, then plain mmap.
//
// origin_out — optional; when non-null, records which tier actually
// served this request. Every EXISTING call site is unaffected (defaults
// to nullptr) — only MemoryArena's own slab-minting path passes a real
// pointer, to determine once, at first mint, whether address arithmetic
// is trustworthy for its own child-page pool.
inline void* allocatePage(long long size, bool tryHuge, PageOrigin* origin_out = nullptr)
{
    auto report = [&](PageOrigin o) { if (origin_out) *origin_out = o; };
#if defined(_WIN32) || defined(_WIN64)
    void* ptr = VirtualAlloc(nullptr, static_cast<SIZE_T>(size),
                             MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!ptr) throw std::bad_alloc();
    report(PageOrigin::Plain); // Windows large-page semantics not modeled here
    return ptr;
#else
    int flags = MAP_PRIVATE | MAP_ANONYMOUS;

    #ifdef __linux__
    if (tryHuge) {
        // Try explicit huge pages first (requires vm.nr_hugepages > 0)
        void* ptr = mmap(nullptr, static_cast<size_t>(size),
                         PROT_READ | PROT_WRITE,
                         flags | MAP_HUGETLB | MAP_HUGE_2MB, -1, 0);
        if (ptr != MAP_FAILED) { report(PageOrigin::ExplicitHuge); return ptr; }

        // Fall back to transparent huge pages via madvise (no reserved pool needed)
        ptr = mmap(nullptr, static_cast<size_t>(size),
                   PROT_READ | PROT_WRITE, flags, -1, 0);
        if (ptr != MAP_FAILED) {
            madvise(ptr, static_cast<size_t>(size), MADV_HUGEPAGE);
            report(PageOrigin::TransparentHuge);
            return ptr;
        }
    }
    #endif

    // Standard mmap fallback
    void* ptr = mmap(nullptr, static_cast<size_t>(size),
                     PROT_READ | PROT_WRITE, flags, -1, 0);
    if (ptr == MAP_FAILED) throw std::bad_alloc();
    report(PageOrigin::Plain);
    return ptr;
#endif
}

inline void freePage(void* ptr, long long size)
{
#if defined(_WIN32) || defined(_WIN64)
    (void)size;
    VirtualFree(ptr, 0, MEM_RELEASE);
#else
    munmap(ptr, static_cast<size_t>(size));
#endif
}

class MemoryArena 
{
private:
    // ---------------------------------------------------------------------
    // Slab — bookkeeping for one 2MB region minted by the GLOBAL arena's
    // own child-page pool and carved into kStandardPageSize pages. Only
    // ever exists on the true global singleton (parent_ == nullptr);
    // never touched by a child arena directly. Heap-allocated via plain
    // `new`, matching Chunk's own existing precedent (raw page-backing
    // metadata living outside any arena's own bump space, since it
    // describes memory that ISN'T that arena's own content).
    // ---------------------------------------------------------------------
    struct Slab
    {
        // 2MB slab / 4KB minimum realistic page size = 512 -- a fixed,
        // hardcoded upper bound rather than derived from MemoryArena's
        // own kHugeSlabSize/kStandardPageSize() (both declared later in
        // this same enclosing class): a nested class body is processed
        // immediately as encountered, unlike an outer class's own member
        // FUNCTION bodies, so it does NOT get deferred access to
        // declarations appearing later in the same class.
        // mintSlabLocked() asserts the real runtime page count never
        // exceeds this at first mint -- every real target of this
        // codebase uses a page size >= 4096B, so this bound holds in
        // practice; the assert exists to fail loudly rather than
        // silently overflow if that ever stops being true.
        static constexpr long long kMaxPages = 512;

        char*     base                  = nullptr;
        long long live_pages            = 0; // checked-out count; unmap-eligible at 0
        // Whether THIS slab's own mint actually landed a genuine
        // MAP_HUGETLB mapping (PageOrigin::ExplicitHuge), decided once,
        // at THIS slab's own mint time -- see resolveSlabLocked's own
        // comment for why this can no longer be a single process-wide
        // decision. vm.nr_hugepages is a finite, depletable reservation,
        // not a fixed host capability: the first several mints in a
        // process's life can genuinely get real huge pages while a
        // LATER one, once that reservation is exhausted, silently falls
        // through to an ordinary (non-2MB-aligned) mmap -- a single
        // latched-at-first-mint flag would keep trusting address
        // arithmetic for that later slab's pages regardless, since
        // nothing ever re-checked.
        bool      hugepage_aligned      = false;
        // This slab's OWN free-page stack -- fixed-size, embedded
        // directly in the struct rather than a separate heap container
        // (no std::vector of its own), since the true capacity is
        // already a known, bounded constant. Per-slab rather than one
        // flat pool-wide list specifically so a drain (unmapSlabLocked)
        // only ever touches the handful of entries that actually belong
        // to IT, not every free page across every slab this module's
        // pool currently holds.
        char*     free_slots[kMaxPages] = {};
        long long free_count            = 0;
    };

    struct Chunk 
    {
        char*     buffer    = nullptr;
        long long size      = 0;
        long long used      = 0;
        Chunk*    next      = nullptr;
        // true: this Chunk's backing bytes came from the GLOBAL arena's
        // own child-page pool (acquireChildPage) and must be returned
        // there (releaseChildPage), not munmap'd directly — see
        // MemoryArena::allocateNewChunk's own comment for the full
        // routing. false: this Chunk backs the TRUE GLOBAL arena's own
        // direct bump-allocation content (Entity::operator new and
        // similar), minted via a direct allocatePage call exactly as
        // every Chunk always was before this pool existed.
        bool      from_pool = false;

        // Diagnostic only -- see MemoryArena::scope_tag_'s own comment.
        // Copied (not referenced) at construction time: this Chunk can
        // outlive whatever value scope_tag_ happens to hold LATER (an
        // arena's own tag could, in principle, be reassigned after some
        // of its chunks already exist), and a dangling reference into
        // an arena that might itself be mid-teardown by the time this
        // Chunk's own destructor runs is exactly the wrong thing to add
        // here.
        std::string owner_scope_tag;

        // THE actual cross-DSO fix -- see MemoryArena::global_arena_'s
        // own comment for the full reasoning. Cached here, at
        // construction (same moment owner_scope_tag is captured, same
        // reasoning: guaranteed-correct DSO context NOW, not necessarily
        // later), rather than having ~Chunk() call MemoryArena::
        // getInstance() itself -- which could resolve to the WRONG DSO's
        // singleton if this Chunk's own destructor happens to run from
        // loader-compiled code (destroyImpl and friends) acting on a
        // module-owned entity. nullptr for a non-pooled (from_pool ==
        // false) Chunk, which never reaches releaseChildPage at all.
        MemoryArena* release_target = nullptr;

        Chunk(char* buf, long long sz, bool pooled, std::string tag, MemoryArena* target)
            : buffer(buf), size(sz), used(0), next(nullptr), from_pool(pooled),
              owner_scope_tag(std::move(tag)), release_target(target) {}

        ~Chunk();  // defined below MemoryArena's own class body -- needs
                   // releaseChildPage/freePage visible, and this is the
                   // outer class's own nested type, so the usual
                   // "declare here, define after the enclosing class is
                   // complete" split applies.

        Chunk(const Chunk&)            = delete;
        Chunk& operator=(const Chunk&) = delete;
    };

    // DestructorRecord — as_entity added. Non-null iff T (at the allocate<T>
    // call site) derives from Entity; lets a caller holding only a void*
    // record walk this chain looking for live Entities without needing to
    // know the concrete T of each entry (RIDList<T*> instances, sub-arenas,
    // etc. also live on this same chain and simply carry as_entity==nullptr).
    // The conversion captured here is always a safe derived-to-base upcast
    // (T is fully known at the allocate<T> call site), which is legal even
    // across virtual inheritance — unlike the reverse direction, which is
    // exactly why addTagTrampoline<T> (Entity.h) never casts the other way.
    struct DestructorRecord 
    {
        void*             ptr;
        void            (*dtor)(void*);
        DestructorRecord* prev;
        Entity*         (*as_entity)(void*) = nullptr;
        // The full "properly delete this entity" logic -- election
        // search + promote/vacate for a global-scope entity (evoking
        // both it and its own arena unconditionally afterward), or
        // reparenting its own children up a generation for a non-global
        // one (evoking just it, its own arena left intact). Resolved
        // per-T in registerDtor<T> below, since MemoryArena.h itself
        // only forward-declares Entity and can't call its members
        // directly -- see registerDtor<T>'s own comment for why this
        // works despite that.
        void            (*run_entity_delete)(void*, MemoryArena&, bool) = nullptr;
    };

    // -------------------------------------------------------------------
    // FreeBlockKey / free_blocks_ — a PER-ARENA (not global-pool) free
    // list, keyed by EXACT (size, alignment), individually recycling any
    // allocation's own backing bytes once its C++ lifetime has genuinely
    // ended -- closing the gap where a bump allocator can reclaim a
    // whole ARENA at once but never one prior allocation's bytes
    // individually. Deliberately separate from the global child-page
    // pool (acquireChildPage/releaseChildPage): that operates at fixed
    // 4KB granularity, sourced from a SHARED 2MB slab -- wasteful for a
    // small object, and it would mean giving back a WHOLE page for one
    // small allocation rather than recycling the exact-sized hole within
    // THIS arena's own already-mapped chunk space, where same-size
    // allocations can reuse each other's freed slots directly.
    //
    // Genuinely type-agnostic, by design -- nothing here or in
    // tryAcquireFromFreeList/releaseToFreeList below knows or cares
    // whether the caller is an Entity, a DestructorRecord, or an
    // ArenaAllocator-backed container's own internal node. That
    // agnosticism wasn't fully realized at first: this started as
    // reclaimEntity's own private mechanism (Entity outer-shell bytes
    // only), grew a second caller for DestructorRecord's own separate
    // allocation, and only THEN got these public entry points so
    // ArenaAllocator<T>::allocate/deallocate could reach it too --
    // meaning EVERY ArenaAllocator-backed container in this codebase
    // (tags/flags_/typed_children_/interface_pointers_ on Entity, every
    // RIDList<T>'s own node storage, anything else built on it) now
    // individually reclaims its own erased nodes, not just Entity outer
    // shells. The three original callers (allocate<T>, registerDtorLocked,
    // reclaimEntity) were rewritten to route through these same
    // primitives too, rather than leaving two parallel implementations
    // of the identical idea.
    // -------------------------------------------------------------------
    struct FreeBlockKey
    {
        long long size;
        long long alignment;
        bool operator==(const FreeBlockKey& o) const
        { return size == o.size && alignment == o.alignment; }
    };
    struct FreeBlockKeyHash
    {
        size_t operator()(const FreeBlockKey& k) const
        {
            return std::hash<long long>()(k.size) ^ (std::hash<long long>()(k.alignment) << 1);
        }
    };
    std::unordered_map<FreeBlockKey, std::vector<void*>, FreeBlockKeyHash> free_blocks_;

    // "Locked" pair -- assumes allocationMutex_ is ALREADY held by the
    // caller, same convention allocateRawLocked/registerDtorLocked
    // already use in this file. Internal callers that already hold the
    // lock (allocate<T>'s own free-list check) use these directly, to
    // avoid a double-lock deadlock (std::mutex isn't recursive) that
    // would result from calling the public, lock-taking versions below
    // instead. releaseToFreeListLocked does NOT zero -- zeroing doesn't
    // need the lock, so the public wrapper does it before ever taking
    // the lock; a caller that already holds the lock and wants zeroing
    // is expected to have already done its own (reclaimEntity does).
    void* tryAcquireFromFreeListLocked(long long size, long long alignment)
    {
        auto it = free_blocks_.find(FreeBlockKey{size, alignment});
        if (it == free_blocks_.end() || it->second.empty())
            return nullptr;
        void* mem = it->second.back();
        it->second.pop_back();
        return mem;
    }

    void releaseToFreeListLocked(void* ptr, long long size, long long alignment)
    {
        free_blocks_[FreeBlockKey{size, alignment}].push_back(ptr);
    }

public:
    // Public pair -- takes allocationMutex_ itself. This is what
    // ArenaAllocator<T>::allocate/deallocate (a genuinely separate
    // class, never holding this arena's own lock already) calls, and
    // what any other external, not-already-locked caller should use.
    void* tryAcquireFromFreeList(long long size, long long alignment)
    {
        std::lock_guard<std::mutex> lock(allocationMutex_);
        void* mem = tryAcquireFromFreeListLocked(size, alignment);
        if (mem)
            ETCS_ARENA_DEBUG_LOG("MemoryArena", "tryAcquireFromFreeList: [" << scope_tag_
                     << "] HIT size=" << size << " align=" << alignment << " mem=" << mem);
        else
            ETCS_ARENA_DEBUG_LOG("MemoryArena", "tryAcquireFromFreeList: [" << scope_tag_
                     << "] MISS size=" << size << " align=" << alignment);
        return mem;
    }

    // Zeroes before ever taking the lock -- same info-leak reasoning
    // reclaimEntity's own zeroing already relies on (a later, unrelated
    // allocation of the same size could otherwise observe this one's
    // stale contents), and safe regardless of caller: by the time any
    // allocator's own deallocate() runs, the container has already
    // destructed whatever lived here (standard allocator_traits contract
    // -- destroy() always precedes deallocate()), so this is writing
    // into memory whose C++ object lifetime has already ended, not into
    // a live object.
    void releaseToFreeList(void* ptr, long long size, long long alignment)
    {
        if (!ptr) return;
        std::memset(ptr, 0, static_cast<size_t>(size));
        std::lock_guard<std::mutex> lock(allocationMutex_);
        releaseToFreeListLocked(ptr, size, alignment);
        ETCS_ARENA_DEBUG_LOG("MemoryArena", "releaseToFreeList: [" << scope_tag_
                 << "] pushed size=" << size << " align=" << alignment << " mem=" << ptr);
    }

private:
    // ---------------------------------------------------------------------
    // Standard carve/dedication threshold. getOSPageSize(), not a
    // hardcoded literal -- superseding the old smallPageSize (2048)
    // constant, which predates this pool and was never actually reached
    // in practice anyway (DEFAULT_ARENA_START_PAGE == 4096 already
    // dominated it via chunkSize_'s own std::max). Any child request up
    // to and including this size is served from the pool's carved pages;
    // anything larger is treated as a dedicated blob -- see
    // allocateNewChunk's own comment.
    // ---------------------------------------------------------------------
    static long long kStandardPageSize()
    {
        static const long long sz = getOSPageSize(); // computed once -- sysconf is a real
                                                       // syscall, not worth repeating on
                                                       // every ctor/pool operation.
        return sz;
    }

    // 2MB -- matches the MAP_HUGE_2MB flag allocatePage's own explicit-
    // huge-page branch already hardcodes. Not derived from /proc/meminfo's
    // own Hugepagesize value: that branch is already committed to 2MB
    // specifically regardless of what the system default happens to be.
    static constexpr long long kHugeSlabSize = 2LL * 1024 * 1024;

    // ---------------------------------------------------------------------
    // parent_ — non-null iff this arena was constructed via SOME OTHER
    // arena's own allocate<MemoryArena>() call (i.e. this is an entity-
    // local sub-arena, not the true global singleton). Set exactly once,
    // in the constructor's own init list, by allocate<T>'s special-cased
    // MemoryArena branch below -- BEFORE this constructor's own body runs
    // its first allocateNewChunk call, which needs to already see this
    // set correctly to route its very first chunk request to the pool
    // rather than mmap'ing directly.
    //
    // Deliberately NEVER walked for anything beyond "is this arena the
    // global root, yes or no" -- allocateNewChunk always sources pooled
    // backing bytes from global_arena_ (see that field's own comment for
    // why THAT, not a fresh MemoryArena::getInstance() call, is the
    // correct routing target), never by chaining through parent_ however
    // many addTag<T> levels deep this specific arena's own immediate
    // parent actually sits at. A multi-hop relay through however many
    // levels would mean per-level contention and per-level bookkeeping
    // with no benefit over one single, shared point of contention -- the
    // same reasoning ThreadPool::getInstance() already commits to
    // elsewhere in this codebase. The pointer is still stored (rather
    // than a bare bool) since it's occasionally useful for diagnostics/
    // identity, but no allocation-path logic anywhere reads anything
    // from *parent_ itself -- only whether it's null.
    // ---------------------------------------------------------------------
    MemoryArena* parent_ = nullptr;

    // -----------------------------------------------------------------------
    // global_arena_ — THE actual fix for a real, confirmed bug: MemoryArena::
    // getInstance()'s function-local static is a PER-DSO singleton (same
    // -fvisibility=hidden isolation ThreadPool::getInstance()/EventNode::
    // getInstance() already document elsewhere) -- but acquireChildPage/
    // releaseChildPage are ORDINARY, non-virtual methods, so calling
    // MemoryArena::getInstance() from allocateNewChunk/~Chunk() resolves to
    // WHICHEVER DSO's OWN COMPILED COPY happens to be executing at that
    // exact call site -- NOT whichever DSO the arena instance itself
    // logically belongs to. Code that crosses the loader/module boundary
    // (destroyImpl, addTagTrampoline<T> -- both genuinely loader-compiled,
    // even when acting on a module's own entity) can therefore mint a page
    // from the LOADER's own singleton while every surrounding allocation
    // for the same object correctly used the MODULE's -- two entirely
    // separate pools, silently, for the same logical arena.
    //
    // The fix: never re-resolve MemoryArena::getInstance() from a call site
    // that might be running as the wrong DSO's code. Resolve it EXACTLY
    // ONCE, at construction time -- which is guaranteed correct, since
    // entity/arena construction only ever happens via the OWNING module's
    // own addTag<T>/allocate<T> flow -- and cache the resulting pointer
    // here. allocateNewChunk and Chunk's own release path both route
    // through THIS pointer thereafter, never a fresh getInstance() call.
    // Once we hold a real MemoryArena* to the correct underlying object,
    // WHICH DSO's compiled copy of acquireChildPage/releaseChildPage
    // actually executes stops mattering at all -- poolMutex_/page_registry_/
    // etc. are ordinary instance data, identical in every DSO's copy of the
    // class layout, not per-DSO state; the only thing that was ever wrong
    // was getInstance() picking the wrong OBJECT, never the method logic.
    //
    // nullptr for the true global root itself (never needs to route
    // anywhere) and for every DEDICATED/blob chunk (bypasses the pool
    // entirely, see acquireChildPage's own comment) -- only meaningful for
    // a non-root arena's own carved-page requests.
    // -----------------------------------------------------------------------
    MemoryArena* global_arena_ = nullptr;

    // Purely diagnostic -- never read by any allocation-path logic, only
    // by memoryTeardown()'s own log output. "root" default matches
    // Root's own established lowercase-tag convention (the ONE thing
    // that can never collide with a real dispatch tag, which are always
    // TitleCase) -- an arena that never gets explicitly tagged (the true
    // global singleton, or an orphaned plain-composed-Entity-member's
    // own sub-arena -- see Entity::addTag<T>'s own setScopeTag call for
    // the one place this DOES get set, and PicoHTTPParser's own accum_
    // comment for a concrete example of an arena that currently never
    // reaches that call at all) shows up here as "root" rather than
    // silently unlabeled, so a teardown log full of same-sized releases
    // can actually be told apart by WHICH scope they belonged to.
    std::string  scope_tag_ = "root";

    mutable std::mutex  allocationMutex_;
    Chunk*              head_         = nullptr;
    Chunk*              current_      = nullptr;
    Chunk*              tail_         = nullptr;  // O(1) append
    long long           pageSize_;                // OS base page size
    long long           hugePageSize_;            // 0 if unavailable
    long long           chunkSize_;               // chunk granularity
    bool                useHugePages_;
    DestructorRecord*   dtorHead_     = nullptr;
    bool                isTeardown_   = false;

    // ---------------------------------------------------------------------
    // Child-page pool — meaningful ONLY on the true global singleton
    // (parent_ == nullptr). Every non-global arena's own allocateNewChunk
    // routes here via its own cached global_arena_ (resolved once, at
    // construction -- see that field's own comment for why a fresh
    // MemoryArena::getInstance() call at allocateNewChunk's own call site
    // is NOT safe to rely on), never maintains any of this bookkeeping
    // itself.
    //
    // A SEPARATE mutex from allocationMutex_ above, deliberately: that
    // one guards THIS instance's own Chunk bump-list (used for the
    // global arena's OWN direct bump-allocation content -- Entity::
    // operator new and similar); this one guards ONLY the pool's slab/
    // free-list/registry bookkeeping. Keeping them distinct means a
    // child's own allocateNewChunk can safely call into the pool while
    // its OWN allocationMutex_ is still held (exactly as it always has
    // been -- allocateRawLocked already only ever calls allocateNewChunk
    // from inside an allocationMutex_-locked section) without any risk
    // of a lock-ordering cycle: pool-mutex-guarded code here never
    // reaches back into any child's own allocationMutex_ at all.
    // ---------------------------------------------------------------------
    std::mutex                        poolMutex_;
    std::vector<Slab*>                available_slabs_;  // slabs with free_count > 0 --
                                                            // popped from directly on acquire,
                                                            // NOT a flat page-level list; see
                                                            // Slab::free_slots' own comment
    std::unordered_map<char*, Slab*>  slabs_by_base_;     // every still-live slab, keyed by base
    std::unordered_map<char*, Slab*>  page_registry_;     // per-PAGE owner lookup -- populated
                                                            // for EVERY page of every slab,
                                                            // unconditionally; the sole
                                                            // resolution path now -- see
                                                            // resolveSlabLocked's own comment
    // Log-suppression only now -- see hugepage_aligned's own comment for
    // why the actual trust decision moved onto each Slab individually.
    // Set the first time ANY mint in this process's life falls back off
    // ExplicitHuge, so sustained depletion (every later mint also
    // falling back) doesn't spam the log once for every single slab.
    bool  hugepage_fallback_warned_ = false;

    // --- Private Helpers (must be called under lock) ---

    long long alignUp(long long size, long long alignment) const
    {
        return (size + alignment - 1) & (~(alignment - 1));
    }

    long long alignToChunk(long long minSize) const
    {
        return alignUp(minSize, chunkSize_);
    }

    // ---------------------------------------------------------------------
    // mintSlabLocked / resolveSlabLocked / unmapSlabLocked — the pool's
    // own internals. Called only while poolMutex_ is already held (see
    // acquireChildPage/releaseChildPage below); only ever meaningful on
    // the true global singleton.
    // ---------------------------------------------------------------------

    // Mints one fresh 2MB slab, carves it into kStandardPageSize() pages
    // into ITS OWN free_slots (never a pool-wide list), and marks it
    // available. Records THIS mint's own actual origin on the slab
    // itself (see hugepage_aligned's own comment) -- purely informational
    // now; every page gets registered into page_registry_ regardless
    // (see resolveSlabLocked's own comment for why).
    void mintSlabLocked()
    {
        PageOrigin origin{};
        void* raw = allocatePage(kHugeSlabSize, /*tryHuge=*/true, &origin);
        char* base = static_cast<char*>(raw);

        Slab* slab = new Slab{};
        slab->base            = base;
        // Purely informational now (TLB/performance signal) -- no longer
        // read by any resolution-path logic. See resolveSlabLocked's own
        // comment for why address arithmetic was dropped entirely rather
        // than kept as a per-slab-gated fast path: the two-tier design
        // meant a page's resolvability depended on this flag being
        // correct, on top of the arithmetic itself being correct -- two
        // things that had to independently agree, rather than one single
        // code path with nothing to disagree with itself.
        slab->hugepage_aligned = (origin == PageOrigin::ExplicitHuge);

        if (!slab->hugepage_aligned && !hugepage_fallback_warned_)
        {
            hugepage_fallback_warned_ = true;
            ETCS_LOG("MemoryArena",
                "True huge pages unavailable for at least one child-page-pool slab "
                "(fell back to " << (origin == PageOrigin::TransparentHuge ? "THP-madvise" : "plain mmap")
                << ") -- likely vm.nr_hugepages exhaustion. Purely a TLB/performance "
                   "signal now -- page-ownership tracking is uniform regardless (see "
                   "resolveSlabLocked's own comment), so this has no correctness impact "
                   "at all, not even the reduced one it used to have.");
        }

        long long page_size  = kStandardPageSize();
        long long page_count = kHugeSlabSize / page_size;
        assert(page_count <= Slab::kMaxPages &&
            "MemoryArena: OS page size smaller than this pool's assumed realistic "
            "minimum (4096B) -- Slab::free_slots[]'s fixed capacity would overflow.");

        for (long long i = 0; i < page_count; ++i)
        {
            char* page = base + i * page_size;
            slab->free_slots[slab->free_count++] = page;
            page_registry_[page] = slab; // unconditional now -- see resolveSlabLocked
        }

        slabs_by_base_[base] = slab;
        available_slabs_.push_back(slab);

        ETCS_ARENA_DEBUG_LOG("MemoryArena", "mintSlabLocked: new slab base=" << static_cast<void*>(base)
                 << " hugepage_aligned=" << slab->hugepage_aligned
                 << " total_live_slabs=" << slabs_by_base_.size());
    }

    // Resolves a checked-out (or being-returned) page's owning Slab* --
    // ALWAYS via the explicit per-page registry, unconditionally
    // populated for every page of every slab at mint time (see
    // mintSlabLocked, above). No address-arithmetic fallback anymore --
    // that two-tier design (registry for non-aligned slabs, arithmetic
    // for aligned ones) meant correctness depended on the per-slab
    // hugepage_aligned flag being right, on top of the arithmetic itself
    // being right: two things that had to independently agree with each
    // other, rather than one single, uniform accounting surface with
    // nothing left to disagree. One extra hash-map entry per page at
    // mint time (this pool's own churn is nowhere near the LMAX ring/
    // GapReorderBuffer hot path this codebase actually optimizes for) is
    // a small, bounded cost for removing an entire class of resolution
    // failure outright, rather than trying to prove the two-tier version
    // correct after the fact.
    Slab* resolveSlabLocked(char* page_addr)
    {
        auto it = page_registry_.find(page_addr);
        return (it != page_registry_.end()) ? it->second : nullptr;
    }

    // Unmaps a slab whose live_pages just hit 0 -- every one of its
    // pages is necessarily sitting in its OWN free_slots at this point
    // (that's what live_pages==0 means: free_count == kMaxPages), so it
    // must currently be present in available_slabs_ too; removed here
    // via a linear search bounded by TOTAL SLAB COUNT, not total free
    // page count -- the whole reason free-page tracking moved onto each
    // Slab individually rather than staying one flat pool-wide list.
    void unmapSlabLocked(Slab* slab)
    {
        ETCS_ARENA_DEBUG_LOG("MemoryArena", "unmapSlabLocked: releasing slab base="
                 << static_cast<void*>(slab->base) << " back to the OS");

        auto it = std::find(available_slabs_.begin(), available_slabs_.end(), slab);
        if (it != available_slabs_.end())
        {
            *it = available_slabs_.back();
            available_slabs_.pop_back();
        }

        long long page_size  = kStandardPageSize();
        long long page_count = kHugeSlabSize / page_size;
        for (long long i = 0; i < page_count; ++i)
            page_registry_.erase(slab->base + i * page_size); // unconditional, matches mintSlabLocked
        slabs_by_base_.erase(slab->base);

        freePage(slab->base, kHugeSlabSize);
        delete slab;
    }

    void allocateNewChunk(long long minSize)
    {
        long long sz = alignToChunk(minSize);

        char* buf;
        bool  pooled;
        if (parent_)
        {
            // Not the true global root -- source backing bytes from the
            // GLOBAL arena's own child-page pool. Routes through
            // global_arena_ (cached at construction), NEVER a fresh
            // MemoryArena::getInstance() call here -- see global_arena_'s
            // own comment for why re-resolving getInstance() from this
            // call site is exactly the bug that was silently minting
            // pages from the wrong DSO's singleton.
            buf    = global_arena_->acquireChildPage(sz, useHugePages_);
            pooled = true;
        }
        else
        {
            buf    = static_cast<char*>(allocatePage(sz, useHugePages_));
            pooled = false;
        }

        Chunk* newChunk = new Chunk(buf, sz, pooled, scope_tag_, global_arena_);

        ETCS_ARENA_DEBUG_LOG("MemoryArena", "allocateNewChunk: [" << scope_tag_ << "] requested minSize="
                 << minSize << " sz=" << sz << " buf=" << static_cast<void*>(buf)
                 << " pooled=" << pooled);

        if (!head_)
        {
            head_    = newChunk;
            current_ = newChunk;
            tail_    = newChunk;
        }
        else
        {
            tail_->next = newChunk;
            tail_       = newChunk;
            current_    = newChunk;
        }
    }

    void* allocateRawLocked(long long size, long long alignment)
    {
        if (isTeardown_) throw std::runtime_error("MemoryArena: Allocation after teardown.");
        if (!current_) allocateNewChunk(size);

        long long cp       = reinterpret_cast<long long>(current_->buffer + current_->used);
        long long ap       = alignUp(cp, alignment);
        long long offset   = ap - reinterpret_cast<long long>(current_->buffer);
        long long required = offset + size;

        if (required > current_->size)
        {
            allocateNewChunk(size);
            long long np = reinterpret_cast<long long>(current_->buffer + current_->used);
            long long na = alignUp(np, alignment);
            offset   = na - reinterpret_cast<long long>(current_->buffer);
            required = offset + size;

            if (required > current_->size) throw std::bad_alloc();
        }

        void* memory    = current_->buffer + offset;
        current_->used  = offset + size;
        return memory;
    }

    void registerDtorLocked(void* ptr, void (*dtor)(void*), Entity* (*as_entity)(void*) = nullptr,
                            void (*run_entity_delete)(void*, MemoryArena&, bool) = nullptr)
    {
        // Checks this arena's own free list first, via the same
        // tryAcquireFromFreeListLocked primitive allocate<T>()/
        // ArenaAllocator<T>::allocate/reclaimEntity's own pushes all
        // share -- DestructorRecord isn't templated on T, so every
        // record ever allocated shares the exact same (size, alignment)
        // key regardless of which entity type it originally described.
        // Populated by reclaimEntity's own reclaim of the record itself
        // (below in this file), not just the outer-shell bytes it was
        // already recycling -- see that method's own comment for why
        // the record's own backing bytes needed this too, not just the
        // entity's. Already holds allocationMutex_ (registerDtor<T>'s
        // own caller took it), so the "Locked" variant, not the public,
        // lock-taking tryAcquireFromFreeList.
        long long recSize  = static_cast<long long>(sizeof(DestructorRecord));
        long long recAlign = static_cast<long long>(alignof(DestructorRecord));
        void* recMem = tryAcquireFromFreeListLocked(recSize, recAlign);
        if (recMem)
        {
            ETCS_ARENA_DEBUG_LOG("MemoryArena", "registerDtorLocked: [" << scope_tag_
                     << "] free-list HIT for DestructorRecord mem=" << recMem);
        }
        else
        {
            recMem = allocateRawLocked(recSize, recAlign);
            ETCS_ARENA_DEBUG_LOG("MemoryArena", "registerDtorLocked: [" << scope_tag_
                     << "] free-list MISS for DestructorRecord -- bump-allocating fresh, mem=" << recMem);
        }
        dtorHead_ = new (recMem) DestructorRecord{ptr, dtor, dtorHead_, as_entity, run_entity_delete};
    }

    // Chunk granularity: huge page size if available, else OS page size,
    // rounded up to fit minSize. pickChunkSize scales a small minSize up
    // to huge page granularity automatically when huge pages are detected.
    static long long pickChunkSize(long long hugePageSize, long long pageSize, long long minSize)
    {
        long long base = (hugePageSize > 0) ? hugePageSize : pageSize;
        long long n    = (minSize + base - 1) / base;
        return base * std::max(n, 1LL);
    }

public:
    // performance=true:  use 2MB huge pages when available (default)
    // performance=false: use standard OS page size (for sub-arenas that don't need huge pages)
    // parent: internal only -- see parent_'s own comment. Never pass this
    // explicitly at an ordinary call site; allocate<T>'s own special case
    // for T == MemoryArena is the only thing that ever supplies it, and
    // every existing call site (Entity()'s own init list,
    // MemoryArena::getInstance()'s own function-local static) keeps
    // working completely unchanged, since it defaults to nullptr.
    // global_arena: internal only, same rules as parent -- see
    // global_arena_'s own comment for why this HAS to be threaded in as a
    // constructor argument (set before this ctor's own body runs its
    // first allocateNewChunk call, which already needs to route through
    // it correctly) rather than assigned afterward.
    explicit MemoryArena(long long initialSize = DEFAULT_ARENA_START_PAGE, bool performance = true,
                          MemoryArena* parent = nullptr, MemoryArena* global_arena = nullptr)
        : parent_(parent), global_arena_(global_arena)
    {
        pageSize_ = getOSPageSize();

        // Detect huge page size for diagnostics (even if we won't use it)
        hugePageSize_ = 0LL;
#ifdef __linux__
        if (FILE* f = fopen("/proc/meminfo", "r"))
        {
            char line[256];
            while (fgets(line, sizeof(line), f))
            {
                if (strncmp(line, "Hugepagesize:", 13) == 0)
                {
                    unsigned long long sz = 0;
                    if (sscanf(line + 13, "%llu", &sz) == 1)
                        hugePageSize_ = static_cast<long long>(sz * 1024ULL);
                    break;
                }
            }
            fclose(f);
        }
#endif

        useHugePages_ = performance && (hugePageSize_ > 0);

        if (performance)
        {
            // Use huge page granularity when available, OS page size otherwise
            chunkSize_ = pickChunkSize(hugePageSize_, pageSize_, initialSize);
        }
        else
        {
            // Standard OS page size granularity for non-performance sub-arenas
            long long standard = kStandardPageSize();
            chunkSize_ = alignUp(std::max(initialSize, standard), standard);
        }

        allocateNewChunk(chunkSize_); // parent_ is already set above -- correctly routes
                                       // to the pool here if this is a child arena.
        // Only the global root publishes liveness -- see s_alive.
        if (!parent_) s_alive.store(true, std::memory_order_release);
    }

    // Liveness of THIS DSO's global root arena, readable AFTER it has been
    // destroyed. See EventNode::s_alive for the hazard.
    //
    // Gated on parent_ == nullptr in BOTH directions, unlike EventNode's and
    // ThreadPool's: this type is genuinely multi-instance (every entity owns
    // a local sub-arena), so an unguarded store would let any child arena's
    // ordinary destruction report the global root as dead while it is still
    // fully alive -- turning a guard into a silent skip of real cleanup.
    inline static std::atomic<bool> s_alive{false};
    static bool alive() { return s_alive.load(std::memory_order_acquire); }

    static MemoryArena& getInstance()
    {
#ifdef ETCS_DEBUG_STATIC_GET_INSTANCE_ORDER
        ETCS_LOG("MemoryArena", "getInstance()");
#endif
        static MemoryArena instance;  // uses default: performance=true, parent=nullptr -- the
                                       // one and only true global root for this DSO.
        return instance;
    }
    ~MemoryArena()
    {
        if (!parent_) s_alive.store(false, std::memory_order_release);
        memoryTeardown();
    }

    MemoryArena(const MemoryArena&)            = delete;
    MemoryArena& operator=(const MemoryArena&) = delete;

    // See scope_tag_'s own comment -- purely a debug label, set
    // explicitly by whichever caller happens to know a meaningful
    // identity at the point an arena is created (Entity::addTag<T>, for
    // its own children; ModuleBundle::operator()(), for a top-level
    // spawn). Never set automatically or inferred -- an arena that
    // nothing ever calls this on stays "root", which is itself useful
    // diagnostic information (it means nothing along this specific
    // creation path knew, or bothered, to identify it).
    void               setScopeTag(const std::string& tag) { scope_tag_ = tag; }
    const std::string& getScopeTag() const                 { return scope_tag_; }

    // --- Public API ---

    void* allocateRaw(long long size, long long alignment = alignof(std::max_align_t))
    {
        std::lock_guard<std::mutex> lock(allocationMutex_);
        return allocateRawLocked(size, alignment);
    }

    // -------------------------------------------------------------------
    // acquireChildPage / releaseChildPage — the child-page pool's public
    // surface. Only ever called on MemoryArena::getInstance() (from
    // allocateNewChunk, above) -- never on a non-global instance, and
    // never by anything outside this file.
    //
    // want_dedicated (acquire) / the size comparison (release) decide
    // carved-page vs. dedicated-blob:
    //   - requested_size > kStandardPageSize(), OR want_dedicated true
    //     (a performance=true child, e.g. a module wanting huge-page
    //     throughput for its own entities): DEDICATED. Minted fresh via
    //     a direct allocatePage call sized to fit, returned directly to
    //     the OS the instant it's released. Nothing pooled, nothing
    //     shared -- by construction this mapping only ever serves that
    //     one caller. This is the same treatment a single object bigger
    //     than one page already got informally (see memoryTeardown's own
    //     long-standing isLarge diagnostic) -- generalized here into an
    //     explicit, uniform rule rather than an incidental side effect.
    //   - otherwise: CARVED. Popped from whichever slab in
    //     available_slabs_ currently has room, minting a fresh 2MB slab
    //     first if none does. Slices of a REAL 2MB huge page when the
    //     host actually has them available (mintSlabLocked always
    //     requests tryHuge=true for the shared slab itself, regardless
    //     of what any individual carved-page requester's own
    //     useHugePages_ says) -- so even an ordinary performance=false
    //     child gets whatever TLB benefit the underlying huge page
    //     offers, for free, without having asked for it directly.
    // -------------------------------------------------------------------
    char* acquireChildPage(long long requested_size, bool want_dedicated)
    {
        if (want_dedicated || requested_size > kStandardPageSize())
        {
            void* p = allocatePage(requested_size, /*tryHuge=*/want_dedicated);
            return static_cast<char*>(p);
        }

        std::lock_guard<std::mutex> lock(poolMutex_);
        if (available_slabs_.empty())
            mintSlabLocked();

        Slab* slab = available_slabs_.back();
        char* page = slab->free_slots[--slab->free_count];
        slab->live_pages++;

        ETCS_ARENA_DEBUG_LOG("MemoryArena", "acquireChildPage: handed out page=" << static_cast<void*>(page)
                 << " from slab base=" << static_cast<void*>(slab->base)
                 << " (now live_pages=" << slab->live_pages << ", free_count=" << slab->free_count << ")");

        if (slab->free_count == 0)
            available_slabs_.pop_back();

        return page;
    }

    // Zeroes before this memory is EVER handed to a different borrower --
    // see the class-level pool comment above for why this can't be
    // deferred to acquisition time instead (a fresh mmap is already
    // kernel-zeroed for free; a RECYCLED page or slab is not, unless
    // something explicitly wipes it, and this is that something).
    // caller_tag — purely diagnostic, threaded from the releasing Chunk's
    // OWN owner_scope_tag (see that field's own comment) so a resolution
    // failure below can say WHICH arena's chunk this was, not just the
    // pool's own generic complaint. Defaults to "" for the few call
    // sites that don't have one (none currently -- every real call
    // already goes through Chunk::~Chunk(), which always has one).
    void releaseChildPage(char* ptr, long long size, const std::string& caller_tag = "")
    {
        std::memset(ptr, 0, static_cast<size_t>(size));

        if (size > kStandardPageSize())
        {
            // Dedicated blob, released directly -- see acquireChildPage's
            // own comment.
            freePage(ptr, size);
            return;
        }

        std::lock_guard<std::mutex> lock(poolMutex_);
        Slab* owner = resolveSlabLocked(ptr);
        if (!owner)
        {
            ETCS_LOG("MemoryArena", "releaseChildPage: no owning slab found for "
                     << static_cast<void*>(ptr) << " (releasing arena's own scope: \""
                     << caller_tag << "\") -- dropping without returning to pool.");
            return;
        }

        ETCS_ARENA_DEBUG_LOG("MemoryArena", "releaseChildPage: resolved page=" << static_cast<void*>(ptr)
                 << " to slab base=" << static_cast<void*>(owner->base)
                 << " (currently live_pages=" << owner->live_pages << ")");

        // Re-enters available_slabs_ only on the 0->1 transition -- it's
        // already present for every free_count > 1 case, and pushing it
        // again there would leave a stale duplicate entry once it later
        // drains back to empty and gets removed by only ONE of the two.
        bool was_empty = (owner->free_count == 0);

        owner->free_slots[owner->free_count++] = ptr;
        owner->live_pages--;

        if (was_empty)
            available_slabs_.push_back(owner);

        if (owner->live_pages == 0)
            unmapSlabLocked(owner);
    }

    // -------------------------------------------------------------------
    // allocate<T> — reserves memory and constructs T in place, then
    // registers its destructor via registerDtor<T> (below).
    //
    // This used to build its own, separate, INLINE DestructorRecord right
    // here (object + record combined into one bump-allocated block, the
    // record as a "footer" immediately after the object) rather than
    // calling registerDtor<T> -- and that inline copy only ever set
    // ptr/dtor/prev/as_entity, never run_entity_delete. Since this
    // function is the ONLY path any real entity in this codebase is ever
    // constructed through (_make_##Name, addTag<T>, spawn<T> all route
    // here, never anywhere else), that meant deleteEntity's own callback
    // lookup (see its own comment below) silently found nothing to call
    // for every entity that ever existed: no promoteOrVacate, no
    // reparenting, no actual C++ destructor invocation -- the entity just
    // sat inert in the arena until memoryTeardown()'s own unconditional,
    // whole-chain walk eventually reached it, however much later that
    // happened to be (often not until process exit).
    //
    // registerDtor<T>'s own doc comment (below) already describes this
    // exact calling relationship -- "only ever happens from a call site
    // (MemoryArena::allocate<T>...)" -- strongly suggesting this function
    // used to call it directly before some earlier refactor inlined a
    // second, incomplete copy here instead and left that comment stale.
    // Routing through registerDtor<T> again, rather than writing a THIRD
    // copy of the same run_entity_delete lambda inline, is what makes
    // that specific class of drift structurally impossible going
    // forward: there is exactly one place this logic is written now, not
    // two (or three) that can silently diverge again.
    //
    // The one behavioral difference from the old inline version: object
    // and DestructorRecord are no longer combined into a single bump
    // allocation -- registerDtor<T> does its own, separate bump
    // allocation for the record via registerDtorLocked. Nothing in this
    // codebase reads a DestructorRecord's address from outside
    // dtorHead_'s own chain traversal, so nothing depends on that
    // adjacency; the cost is one additional bump-pointer reservation per
    // non-trivially-destructible object, negligible next to this arena's
    // actual throughput requirements (the LMAX ring and GapReorderBuffer
    // carry this system's real performance budget, not entity
    // construction, which is a comparatively rare, cold-ish path: module
    // bootstrap, addTag<T> children, spawns).
    //
    // IMPORTANT: the lock is still held ONLY around the bump-pointer
    // reservation for T itself -- NOT across T's constructor, and NOT
    // across registerDtor<T>'s own separate critical section either. If
    // any lock spanned construction, any T whose constructor itself
    // allocates from this same arena (directly, or transitively through
    // a member that does) would self-deadlock on allocationMutex_, since
    // std::mutex is non-recursive. Entity is exactly such a T: its
    // constructor allocates an entity-local MemoryArena out of this same
    // global instance. registerDtor<T> is called here only AFTER T's
    // constructor has already fully run (unlocked) -- the same invariant
    // this function always had, just expressed across two functions
    // instead of duplicated inline.
    //
    // as_entity/run_entity_delete capture: if constexpr
    // (std::is_base_of<Entity, T>) inside registerDtor<T> -- this only
    // type-checks correctly at each ACTUAL call site's instantiation
    // point (two-phase template lookup), by which point every real
    // caller (always compiled after Entity.h is fully included — every
    // module .h starts with core_defs.h/ETCS_API.h, which resolves
    // Entity.h before any concrete leaf type is ever declared) already
    // has Entity complete. MemoryArena.h itself only needs the forward
    // declaration above.
    //
    // T == MemoryArena special case: threads `this` (whichever arena
    // .allocate<MemoryArena>() was actually called ON) into the new
    // arena's own third constructor argument, invisible to every real
    // call site -- see parent_'s own comment for why this has to happen
    // as an extra constructor argument (set before the new arena's own
    // ctor body runs its first allocateNewChunk call) rather than a
    // post-construction assignment.
    // -------------------------------------------------------------------
    template<typename T, typename... Args>
    T* allocate(Args&&... args)
    {
        if constexpr (!std::is_trivially_destructible_v<T>)
        {
            void* mem;
            {
                std::lock_guard<std::mutex> lock(allocationMutex_);
                // Check this arena's own free list first, via the SAME
                // primitive ArenaAllocator<T>/registerDtorLocked/
                // reclaimEntity all now share (tryAcquireFromFreeListLocked,
                // above) -- a previously reclaimed, exact-size-and-alignment
                // block is exactly as valid as fresh bump-allocated space,
                // and reusing it is what actually closes the "outer-shell
                // bytes never come back" gap under repeated addTag<T> churn
                // of the same concrete type. Falls through to ordinary bump
                // allocation on a miss -- unchanged from before. The
                // "Locked" variant, not tryAcquireFromFreeList itself:
                // allocationMutex_ is already held right here, and that
                // public entry point takes the same lock itself, which
                // would deadlock.
                mem = tryAcquireFromFreeListLocked(
                    static_cast<long long>(sizeof(T)), static_cast<long long>(alignof(T)));
                if (mem)
                {
                    ETCS_ARENA_DEBUG_LOG("MemoryArena", "allocate<T>: [" << scope_tag_
                             << "] free-list HIT size=" << sizeof(T) << " align=" << alignof(T)
                             << " mem=" << mem);
                }
                else
                {
                    mem = allocateRawLocked(
                        static_cast<long long>(sizeof(T)),
                        static_cast<long long>(alignof(T))
                    );
                    ETCS_ARENA_DEBUG_LOG("MemoryArena", "allocate<T>: [" << scope_tag_
                             << "] free-list MISS size=" << sizeof(T) << " align=" << alignof(T)
                             << " -- bump-allocating fresh, mem=" << mem);
                }
            }

            // Constructor runs UNLOCKED. Safe for it to call back into
            // this arena (allocateRaw / allocate<U> / registerDtor) —
            // those each take and release the lock independently.
            T* obj;
            if constexpr (std::is_same_v<T, MemoryArena>)
                // MemoryArena::getInstance() evaluated HERE, not later --
                // this exact point is guaranteed to be executing as the
                // OWNING module's own compiled code (construction only
                // ever happens via that module's own addTag<T>/allocate<T>
                // flow), so this resolves to the correct DSO's singleton.
                // Threaded through as global_arena_ and cached on the new
                // instance -- see that field's own comment for why this
                // one-time resolution, done here, is what closes the
                // cross-DSO pool-mismatch bug outright.
                obj = new (mem) T(std::forward<Args>(args)..., this, &MemoryArena::getInstance());
            else
                obj = new (mem) T(std::forward<Args>(args)...);

            registerDtor<T>(obj);

            return obj;
        }
        else
        {
            void* mem;
            {
                std::lock_guard<std::mutex> lock(allocationMutex_);
                mem = allocateRawLocked(
                    static_cast<long long>(sizeof(T)),
                    static_cast<long long>(alignof(T))
                );
            }
            return new (mem) T(std::forward<Args>(args)...);
        }
    }

    template<typename T>
    T* allocateArray(size_t count)
    {
        std::lock_guard<std::mutex> lock(allocationMutex_);
        long long sizeBytes = static_cast<long long>(count * sizeof(T));
        void*     mem       = allocateRawLocked(sizeBytes, alignof(T));
        return static_cast<T*>(mem);
    }

    // registerDtor<T> — for T deriving from Entity, ALSO resolves
    // run_entity_delete, written in terms of dependent T so two-phase
    // lookup defers checking it until Entity/Module are complete at the
    // actual call site (allocate<T>, via addTag<T>/ETCS_TAG_DECLARE or
    // DynamicLoader.h).
    //
    // Three cases, decided each time deleteEntity actually runs:
    //   - Global (getParent() == nullptr): search for a sibling,
    //     promoteOrVacate, evoke e AND its own arena unconditionally --
    //     already a full subtree delete regardless of delete_children,
    //     since only root-level entities are ever lifetime_owner-eligible.
    //   - Non-global, delete_children == true: evoke e, then evoke e's
    //     own arena too -- its ~MemoryArena() walks e's own dtor chain in
    //     turn, cascading the subtree via ordinary nested teardown. Never
    //     paired with reparenting -- that would destroy entities the
    //     RIDList rewrite just told the grandparent it now owns.
    //   - Non-global, delete_children == false (default): reparent e's
    //     children up to e's own parent, evoke only e -- e's own arena
    //     stays intact ("coyote time" -- see reparentChildrenTo, Entity.h).
    template<typename T>
    void registerDtor(T* ptr)
    {
        if constexpr (!std::is_trivially_destructible_v<T>)
        {
            std::lock_guard<std::mutex> lock(allocationMutex_);
            if (!isTeardown_)
            {
                if constexpr (std::is_base_of<Entity, T>::value)
                    registerDtorLocked(ptr, [](void* p) { static_cast<T*>(p)->~T(); },
                        [](void* p) -> Entity* { return static_cast<Entity*>(static_cast<T*>(p)); },
                        [](void* p, MemoryArena& parentArena, bool delete_children)
                        {
                            T* e = static_cast<T*>(p);
                            if (e->getParent() == nullptr)
                            {
                                Entity* survivor = parentArena.findNextCandidateScope(
                                    [](Entity*) { return true; }, e);
                                e->module_.promoteOrVacate(survivor);
                                // Captured BEFORE reclaimEntity, not after --
                                // reclaimEntity's own memset zeroes e's
                                // WHOLE outer-shell memory once ~T() has
                                // run, including e->local_arena_ itself, so
                                // reading e->getArena() AFTER that call
                                // reads zeroed garbage instead of the real
                                // pointer (this is exactly what crashed
                                // immediately -- the original evokeDestructor
                                // (e) never touched the bytes after running
                                // ~T(), only reclaimEntity's own zeroing
                                // does, and only this ordering avoids it).
                                MemoryArena* own_arena = &e->getArena();
                                // Children first -- see
                                // destroyChildEntitiesFirst. Before
                                // reclaimEntity, because that is what runs
                                // this entity's own ~T().
                                own_arena->destroyChildEntitiesFirst();
                                // reclaimEntity, not evokeDestructor -- see
                                // that method's own comment. Safe here for
                                // the same reason it's safe below: whatever
                                // referenced e (its parent's own
                                // typed_children_ entry, or the module
                                // registry for a global-scope entity) has
                                // already been resolved/removed by this
                                // point, so its own outer-shell bytes are
                                // free to recycle, not just its content arena.
                                parentArena.reclaimEntity(e, sizeof(T), alignof(T));
                                // reclaimArena, not evokeDestructor: the arena
                                // object itself was allocate<MemoryArena>'d out
                                // of parentArena, so its own outer bytes belong
                                // back on parentArena's free list exactly as the
                                // entity's do. evokeDestructor runs the dtor and
                                // unlinks the record but returns nothing --
                                // leaking sizeof(MemoryArena) per deleted child,
                                // which under connection churn is unbounded
                                // growth in a parent that never tears down.
                                parentArena.reclaimArena(own_arena);
                            }
                            else if (delete_children)
                            {
                                MemoryArena* own_arena = &e->getArena(); // see comment above
                                // Children first -- see
                                // destroyChildEntitiesFirst. This is the
                                // cascade case, so every descendant is
                                // destroyed here, depth-first, and only then
                                // does reclaimEntity run this entity's ~T().
                                own_arena->destroyChildEntitiesFirst();
                                parentArena.reclaimEntity(e, sizeof(T), alignof(T));
                                // reclaimArena, not evokeDestructor: the arena
                                // object itself was allocate<MemoryArena>'d out
                                // of parentArena, so its own outer bytes belong
                                // back on parentArena's free list exactly as the
                                // entity's do. evokeDestructor runs the dtor and
                                // unlinks the record but returns nothing --
                                // leaking sizeof(MemoryArena) per deleted child,
                                // which under connection churn is unbounded
                                // growth in a parent that never tears down.
                                parentArena.reclaimArena(own_arena);
                            }
                            else
                            {
                                // reparentChildrenTo redirects every child's own
                                // addressing past e, so nothing dangling depends on
                                // e's own address surviving and its outer shell is
                                // as reclaimable here as in the cascade case above.
                                // Capture own_arena BEFORE reclaimEntity zeroes that
                                // shell -- local_arena_ lives in those bytes (same
                                // reason the two branches above capture it early).
                                MemoryArena* own_arena = &e->getArena();

                                // "Coyote time" -- keeping this entity's own arena
                                // alive after the entity itself is gone -- exists
                                // for exactly ONE reason: reparented children whose
                                // storage still physically lives in it. An entity
                                // with no typed children has nothing to preserve it
                                // for, and preserving it anyway leaks the arena
                                // object plus its DestructorRecord into the PARENT's
                                // arena, permanently, once per deletion. Measured at
                                // 2112 bytes per connection against a
                                // SocketConnectionState (children=0, always, since a
                                // connection never addTag<T>s anything) -- invisible
                                // per request, tens of MB over a day of polling.
                                //
                                // Checked BEFORE the reparent, not after. The
                                // question is "does any entity still have STORAGE
                                // inside this arena", and reparenting does not move
                                // storage: reparentChildrenTo rewrites parent_ and
                                // mints a fresh RIDList in newParent's arena, but
                                // every child's own shell and local_arena_ stay
                                // physically here -- which is the entire reason that
                                // method's own comment says this arena "does survive
                                // as coyote time".
                                //
                                // Asking AFTER inverted it. A SUCCESSFUL migration
                                // empties typed_children_, so the arena hosting those
                                // now-migrated children was reclaimed out from under
                                // them (their shells destructed with it -- a live
                                // grandparent left holding RIDs into freed memory).
                                // A migration that FAILED left its children behind
                                // and kept the arena -- alive for the one case that
                                // no longer needed reachable children. Exactly
                                // backwards, and reachable from any
                                // deleteEntity(middle, false) on a three-level tree.
                                //
                                // Before the reparent, "had children at all" answers
                                // it for both kinds at once: migrated and orphaned
                                // children are hosted here alike. The children == 0
                                // case this check exists for (SocketConnectionState,
                                // which never addTag<T>s anything) is untouched --
                                // it has nothing hosted here either way.
                                std::vector<std::pair<ETCS::Buffer, ETCS_RID_SIZE>> hosted;
                                e->getTypedChildren(hosted);
                                e->reparentChildrenTo(e->getParent());
                                parentArena.reclaimEntity(e, sizeof(T), alignof(T));
                                if (hosted.empty())
                                    parentArena.reclaimArena(own_arena);
                            }
                        });
                else
                    registerDtorLocked(ptr, [](void* p) { static_cast<T*>(p)->~T(); });
            }
        }
    }
 
    // Entity*-aware overload: compares via the stored as_entity
    // conversion, never the raw record pointer. Required for correctness
    // under virtual inheritance — several ontology leaf types inherit
    // Entity virtually, meaning a T* and its Entity* base-subobject
    // address are not guaranteed to coincide, so comparing an Entity* the
    // caller holds against a raw void* record pointer built from T* can
    // silently fail to find the very record it's looking for. Overload
    // resolution routes any Entity*-typed argument here automatically;
    // the plain void* overloads below remain correct for their actual
    // callers (Module*, MemoryArena*), which are never polymorphic.
    DestructorRecord* unlinkRecord(Entity* target)
    {
        std::lock_guard<std::mutex> lock(allocationMutex_);
        if (isTeardown_) return nullptr;
        DestructorRecord* prevRec = nullptr;
        DestructorRecord* cur = dtorHead_;
        while (cur)
        {
            if (cur->as_entity && cur->as_entity(cur->ptr) == target)
            {
                if (prevRec) prevRec->prev = cur->prev;
                else         dtorHead_    = cur->prev;
                return cur;
            }
            prevRec = cur;
            cur = cur->prev;
        }
        return nullptr;
    }
 
    DestructorRecord* unlinkRecord(void* target)
    {
        std::lock_guard<std::mutex> lock(allocationMutex_);
        if (isTeardown_) return nullptr;
        DestructorRecord* prevRec = nullptr;
        DestructorRecord* cur = dtorHead_;
        while (cur)
        {
            if (cur->ptr == target)
            {
                if (prevRec) prevRec->prev = cur->prev;
                else         dtorHead_    = cur->prev;
                return cur;
            }
            prevRec = cur;
            cur = cur->prev;
        }
        return nullptr;
    }
 
public:
    bool evokeDestructor(Entity* target)
    {
        DestructorRecord* rec = unlinkRecord(target);
        if (!rec) return false;
        rec->dtor(rec->ptr);
        return true;
    }
 
    // reclaimEntity — same unlink-then-destruct sequence as
    // evokeDestructor(Entity*) above, PLUS returns both the entity's own
    // outer-shell bytes AND its DestructorRecord's own backing bytes to
    // this arena's free list (releaseToFreeList, above -- the same
    // public entry point ArenaAllocator<T>::deallocate uses), for reuse
    // by a future allocate<T>()/registerDtorLocked call of the identical
    // size+alignment. size/alignment are passed explicitly (sizeof(T)/
    // alignof(T), known at the registerDtor<T> call site, the only
    // caller of this method) rather than stored on DestructorRecord
    // itself, since nothing else needs them. Zeroing (so a later,
    // unrelated same-size allocation never observes this one's stale
    // contents) happens inside releaseToFreeList itself, not here.
    bool reclaimEntity(Entity* target, long long size, long long alignment)
    {
        DestructorRecord* rec = unlinkRecord(target);
        if (!rec)
        {
            //ETCS_LOG("MemoryArena", "reclaimEntity: NO RECORD for " << (void*)target
            //         << " size=" << size << " -- nothing reclaimed.");
            return false;
        }
        //ETCS_LOG("MemoryArena", "reclaimEntity: reclaiming " << (void*)target
        //         << " size=" << size << " align=" << alignment);
        void* raw = rec->ptr;
        rec->dtor(raw);
 
        // rec itself -- unlinkRecord only removed it from dtorHead_'s own
        // linked list; that's bookkeeping, not reclaim. registerDtorLocked
        // bump-allocates one of these separately for EVERY addTag<T>/
        // allocate<T> call (falling through to the SAME free list on a
        // miss -- see its own comment), so without this, sustained churn
        // of the SAME concrete type still permanently grows the arena by
        // sizeof(DestructorRecord) per instance -- slower than the
        // outer-shell leak this whole mechanism was built to close (many
        // records fit per page), but just as unbounded over a
        // long-enough run. Every field of rec has already been read by
        // this point (dtor above was the last one), so it's safe to
        // reclaim here. DestructorRecord isn't templated on T, so this
        // key never varies across records.
        //
        // Both pushes go through the PUBLIC releaseToFreeList, not the
        // Locked variant -- unlinkRecord (above) already took and
        // released allocationMutex_ on its own, so it isn't held here.
        // Neither raw nor rec needs an explicit zero/value-init first:
        // releaseToFreeList's own memset takes void*, so the compiler
        // never sees DestructorRecord's own static type at that memset
        // call site regardless of what's actually being passed in --
        // -Wclass-memaccess only fires when a properly-typed pointer is
        // passed to memset directly, which this no longer does.
        releaseToFreeList(raw, size, alignment);
        releaseToFreeList(rec,
            static_cast<long long>(sizeof(DestructorRecord)),
            static_cast<long long>(alignof(DestructorRecord)));
        return true;
    }
    
    // The MemoryArena counterpart to reclaimEntity. Same unlink-destruct-
    // recycle sequence, with sizeof/alignof known statically here rather than
    // passed in -- there is only one type this can ever be.
    //
    // Deliberately NOT folded into evokeDestructor(void*): that overload is
    // also used for Module, whose bytes have a different owner, and for
    // targets whose size the caller may not know. This is the one case where
    // the type is fixed and the owning arena is the one being called on.
    bool reclaimArena(MemoryArena* target)
    {
        DestructorRecord* rec = unlinkRecord(static_cast<void*>(target));
        if (!rec) return false;
        void* raw = rec->ptr;
        rec->dtor(raw);
        releaseToFreeList(raw, static_cast<long long>(sizeof(MemoryArena)),
                                static_cast<long long>(alignof(MemoryArena)));
        releaseToFreeList(rec,
            static_cast<long long>(sizeof(DestructorRecord)),
            static_cast<long long>(alignof(DestructorRecord)));
        return true;
    }
    
 
    // deleteEntity — THE entry point for properly deleting an arena-
    // resident entity. delete_children only changes anything for a
    // non-global target: true cascades the whole subtree; false
    // (default) reparents children instead, preserving prior behavior.
    // Meaningless for a global-scope target -- already a full cascade
    // either way (see registerDtor<T>'s own comment).
    //
    // Looks up target's own run_entity_delete callback WITHOUT unlinking
    // its record -- the callback itself unlinks+destructs via its own
    // evokeDestructor call, once it's finished any election/reparenting
    // work that needs target still fully intact.
    /*
 * destroyChildEntitiesFirst — destroy every ENTITY this arena holds, oldest
 * first, before anything destroys the entity that OWNS the arena.
 *
 * WHY THIS EXISTS. A subtree teardown used to run the parent's ~T() and only
 * then walk the arena, so the observed order for a three-level tree was
 *
 *     parent  child_b  child_a  grandchild
 *
 * -- exactly inverted. A child's destructor legitimately reaches its parent
 * (to unregister, to hand back a token, to log what it belonged to), and
 * every one of those reads a destroyed object. It survived because the
 * children in this codebase mostly do not look up; the first one that does
 * would have been a use-after-free with no obvious cause.
 *
 * Only ENTITY records are touched. The parent's own container allocations
 * live in this same arena and its ~T() still needs them, so they are left
 * exactly where they are -- this is the one reason the fix is not simply
 * "reclaim the arena first", which would pull the parent's own members out
 * from under its destructor.
 *
 * Oldest first, and deterministic. dtorHead_ is a LIFO stack, so taking the
 * head would destroy siblings newest-first -- an order nothing chose and
 * nothing can rely on. Walking to the tail destroys them in the order they
 * were attached, which is what "first in, first out" says and what a reader
 * of the script that spawned them expects.
 *
 * Depth is free: each callback is the same cascade, so a child destroys ITS
 * children before itself by the same path, all the way down.
 *
 * Terminates because each callback reclaims the entity it was given, which
 * unlinks that record from this chain -- so the search that follows cannot
 * return it again. The lock is released across the callback for the same
 * reason deleteEntity releases it: the cascade re-enters this arena.
 */
    void destroyChildEntitiesFirst()
    {
        while (true)
        {
            void (*callback)(void*, MemoryArena&, bool) = nullptr;
            void* rawPtr = nullptr;
            {
                std::lock_guard<std::mutex> lock(allocationMutex_);
                for (DestructorRecord* rec = dtorHead_; rec; rec = rec->prev)
                {
                    if (rec->as_entity && rec->run_entity_delete)
                    {
                        callback = rec->run_entity_delete;   // keep walking:
                        rawPtr   = rec->ptr;                 // the tail is the
                    }                                        // oldest record
                }
            }
            if (!callback) return;
            callback(rawPtr, *this, true);
        }
    }

    void deleteEntity(Entity* target, bool delete_children = true)
    {
        void (*callback)(void*, MemoryArena&, bool) = nullptr;
        void* rawPtr = nullptr;
        {
            std::lock_guard<std::mutex> lock(allocationMutex_);
            DestructorRecord* rec = dtorHead_;
            while (rec)
            {
                if (rec->as_entity && rec->as_entity(rec->ptr) == target)
                {
                    callback = rec->run_entity_delete;
                    rawPtr   = rec->ptr;
                    break;
                }
                rec = rec->prev;
            }
        }
        if (callback) callback(rawPtr, *this, delete_children);
    }
 
    bool forget(Entity* target)
    {
        return unlinkRecord(target) != nullptr;
    }
 
    // -------------------------------------------------------------------
    // evokeDestructor(void*) — explicitly runs AND unlinks a single
    // record ahead of the arena's own full teardown, identified by its
    // raw allocated pointer. Correct ONLY for non-polymorphic,
    // single-inheritance callers (Module, MemoryArena itself) — see the
    // Entity* overload above for anything Entity-derived. Safe from ANY
    // thread (own mutex, independent of whichever thread happens to be
    // destroying something).
    // -------------------------------------------------------------------
    bool evokeDestructor(void* target)
    {
        DestructorRecord* rec = unlinkRecord(target);
        if (!rec) return false; // not found — already evoked/forgotten, or never existed here
        rec->dtor(rec->ptr);    // unlocked — matches allocate<T>'s own philosophy
        return true;
    }
 
    // forget(void*) — unlinks a record WITHOUT calling its destructor.
    // Use this when the destructor has ALREADY run through some other
    // path (most commonly: an ordinary `delete child;` on an Entity —
    // operator delete unconditionally forgets that entity's own outer
    // record from its parent's arena for exactly this reason). Correct
    // ONLY for non-polymorphic targets — see the Entity* overload above.
    bool forget(void* target)
    {
        return unlinkRecord(target) != nullptr;
    }
 
    // -------------------------------------------------------------------
    // findNextCandidateScope — walks the (still-live, i.e. not-yet-evoked)
    // dtor chain looking for an Entity satisfying `pred`, skipping any
    // record that isn't Entity-tagged (as_entity == nullptr — sub-arenas,
    // RIDLists, etc.) and skipping `exclude` if given.
    //
    // Comparison against `exclude` and the predicate both operate on the
    // CONVERTED Entity* (via as_entity), never on the raw record pointer —
    // required for correctness under virtual inheritance, where a T* and
    // its Entity* base-subobject address are not guaranteed to coincide.
    //
    // Deliberately generic: MemoryArena has no notion of "global scope" or
    // "which module" — the predicate carries all of that domain logic, so
    // this stays usable for any future "find a live entity meeting X"
    // need without MemoryArena.h ever needing more of Entity than its name.
    // -------------------------------------------------------------------
    template<typename Pred>
    Entity* findNextCandidateScope(Pred&& pred, Entity* exclude = nullptr)
    {
        std::lock_guard<std::mutex> lock(allocationMutex_);
        for (DestructorRecord* rec = dtorHead_; rec; rec = rec->prev)
        {
            if (!rec->as_entity) continue;
            Entity* e = rec->as_entity(rec->ptr);
            if (e == exclude) continue;
            if (pred(e)) return e;
        }
        return nullptr;
    }
 
    void cleanupTypedEntities()
    {
        std::lock_guard<std::mutex> lock(allocationMutex_);
        if (isTeardown_) return;
 
        DestructorRecord* rec = dtorHead_;
        while (rec) { rec->dtor(rec->ptr); rec = rec->prev; }
        dtorHead_ = nullptr;
    }
 
    void reset()
    {
        DestructorRecord* rec;
        {
            std::lock_guard<std::mutex> lock(allocationMutex_);
            if (isTeardown_) return;
            rec = dtorHead_;
            dtorHead_ = nullptr;
        }
 
        // Runs UNLOCKED -- see allocate<T>()'s own "constructor runs
        // unlocked" comment for the identical reasoning, now applying
        // symmetrically to destruction: a destructor (an unordered_map's
        // own ~unordered_map(), for instance -- walking its own nodes
        // and calling ArenaAllocator<T>::deallocate() on each) can
        // legitimately need to call back into THIS SAME ARENA's own
        // allocation machinery, which itself takes allocationMutex_.
        // Holding that lock across this whole loop (as this function
        // used to) meant such a destructor would re-lock a non-
        // recursive std::mutex from the SAME thread -- undefined
        // behavior, not a guaranteed clean deadlock -- and is exactly
        // what a real, reproduced SIGFPE inside std::unordered_map's own
        // operator[] traced back to (free_blocks_'s own bucket state
        // corrupted by two logically-concurrent, unsynchronized mutations
        // of the same map). This was invisible before ArenaAllocator's
        // own deallocate() actually did anything -- the reentrant call
        // was always POSSIBLE, just inert.
        while (rec) { rec->dtor(rec->ptr); rec = rec->prev; }
 
        std::lock_guard<std::mutex> lock(allocationMutex_);
        // Every chunk goes back to used=0 below -- any block this arena's
        // own free_blocks_ was tracking as an in-chunk "hole" is now
        // meaningless (the whole chunk is free again via the bump
        // pointer itself), and its backing bytes are about to be
        // MADV_DONTNEED'd/decommitted -- stale entries here would be
        // handed out as "valid" memory by a later allocate<T>() otherwise.
        free_blocks_.clear();
 
        for (Chunk* c = head_; c; c = c->next)
        {
            c->used = 0;
#if defined(_WIN32) || defined(_WIN64)
            VirtualAlloc(c->buffer, static_cast<SIZE_T>(c->size), MEM_RESET, PAGE_READWRITE);
#else
            madvise(c->buffer, static_cast<size_t>(c->size), MADV_DONTNEED);
#endif
        }
 
        current_ = head_;
    }
 
    void memoryTeardown()
    {
        const int columns = 5;
 
        const std::string RED_START = "\033[1;31m";
        const std::string RESET     = "\033[0m";
 
        ETCS_LOG("MemoryArena", "[" << scope_tag_ << "] teardown attempt, already clean: " << isTeardown_);
 
        DestructorRecord* rec;
        {
            std::lock_guard<std::mutex> lock(allocationMutex_);
            if (isTeardown_) return;
            isTeardown_ = true;
            rec = dtorHead_;
            dtorHead_ = nullptr;
        }
 
        // Runs UNLOCKED -- see reset()'s own identical comment (above)
        // for the full reasoning: a destructor legitimately calling back
        // into this same arena's own allocation machinery (which itself
        // takes allocationMutex_) would otherwise re-lock a non-recursive
        // std::mutex from the same thread -- undefined behavior, and
        // exactly what a real, reproduced SIGFPE inside std::
        // unordered_map's own operator[] traced back to.
        while (rec) { rec->dtor(rec->ptr); rec = rec->prev; }
 
        std::lock_guard<std::mutex> lock(allocationMutex_);
        free_blocks_.clear(); // every chunk backing these addresses is about
                               // to be freed below regardless -- cleared here
                               // for explicitness, matching this method's own
                               // global-sweep discipline further down.
 
        long long total       = 0;
        long long actualBytes = 0;
        for (Chunk* c = head_; c; c = c->next) { total++; actualBytes += c->size; }
        ETCS_LOG("MemoryArena", "[" << scope_tag_ << "] ── releasing " << total << " pages (" << formatBytesToString(actualBytes) << ") ──");
 
        // Dedicated (direct mmap, this arena's exclusive) vs pooled
        // (carved from a shared 2MB slab) totals, and per-slab occupancy
        // for every DISTINCT slab this arena's own chunks draw from --
        // answers "how full is the shared pool region this arena is
        // using" at a glance, rather than needing to cross-reference the
        // raw address grid below by hand. Queried via each pooled
        // chunk's own cached release_target (see that field's own
        // comment for why it's the correct, cross-DSO-safe target to
        // ask, not necessarily `this`) -- one lock round-trip per
        // DISTINCT slab touched, not per page, so this stays cheap even
        // for an arena holding many chunks from the same slab.
        {
            long long dedicated_count = 0, dedicated_bytes = 0;
            long long pooled_count    = 0, pooled_bytes    = 0;
            std::unordered_map<char*, long long> slab_live_pages; // slab base -> live_pages, deduped
 
            for (Chunk* c = head_; c; c = c->next)
            {
                if (c->from_pool)
                {
                    pooled_count++;
                    pooled_bytes += c->size;
                    if (c->release_target)
                    {
                        PageUsageInfo info = c->release_target->queryPageUsage(c->buffer);
                        if (info.found)
                            slab_live_pages[info.slab_base] = info.live_pages;
                    }
                }
                else
                {
                    dedicated_count++;
                    dedicated_bytes += c->size;
                }
            }
 
            ETCS_LOG("MemoryArena", "[" << scope_tag_ << "] usage: "
                     << dedicated_count << " dedicated (" << formatBytesToString(static_cast<uint64_t>(dedicated_bytes))
                     << ", direct mmap), " << pooled_count << " carved ("
                     << formatBytesToString(static_cast<uint64_t>(pooled_bytes)) << ")");
 
            for (auto& [base, live] : slab_live_pages)
            {
                ETCS_LOG("MemoryArena", "[" << scope_tag_ << "]   slab " << static_cast<void*>(base)
                         << ": " << live << "/" << Slab::kMaxPages << " pages live ("
                         << formatBytesToString(static_cast<uint64_t>(live * kStandardPageSize())) << "/2.0MB)");
            }
        }
 
        auto flushRow = [&](std::ostringstream& row, long long rowStart, long long rowEnd)
        {
            row << "  ["
                << std::setw(3) << std::setfill('0') << rowStart
                << "-"
                << std::setw(3) << std::setfill('0') << rowEnd
                << "]" << std::setfill(' ');
            ETCS_LOG("MemoryArena", row.str());
            row.str("");
            row.clear();
        };
 
        Chunk*            chunk = head_;
        long long         idx   = 0;
        std::ostringstream row;
 
        while (chunk)
        {
            if (idx % columns != 0) row << " ";
 
            bool      isLarge = (chunk->size > chunkSize_);
            uintptr_t addr    = reinterpret_cast<uintptr_t>(chunk->buffer);
 
            if (isLarge) { row << RED_START; addr |= 1; }
 
            row << std::hex << std::setw(12) << std::setfill('0')
                << addr
                << std::dec << std::setfill(' ');
 
            if (isLarge) row << RESET;
 
            Chunk* next = chunk->next;
            delete chunk; // ~Chunk() routes to releaseChildPage or freePage as appropriate
            chunk = next;
            ++idx;
 
            if (idx % columns == 0)
            {
                long long rowEnd = idx - 1;
                flushRow(row, rowEnd - (columns - 1), rowEnd);
            }
        }
 
        if (idx % columns != 0)
        {
            long long filled  = idx % columns;
            long long missing = columns - filled;
            for (long long i = 0; i < missing; ++i)
                row << " " << std::setw(12) << std::setfill(' ') << "";
            flushRow(row, idx - filled, idx - 1);
        }
 
        ETCS_LOG("MemoryArena", "── " << total << " pages freed ──");
        head_    = nullptr;
        current_ = nullptr;
        tail_    = nullptr;
 
        // Global-only: force-free any child-pool slabs still outstanding
        // at process-exit time -- a child arena that never explicitly
        // tore down before the whole process went away (e.g. a detached
        // script's own entity, still technically "alive" when main()
        // returns) would otherwise leak its slabs past this arena's own
        // teardown. Unconditional -- ignores live_pages entirely, since
        // by this point the whole arena system is going away regardless
        // of whether individual children "properly" returned their pages
        // first. Takes poolMutex_ explicitly, matching every other touch
        // point on these same structures -- the "nothing else can be
        // racing this" reasoning only holds because this path is
        // reachable exclusively at genuine process exit (see this
        // class's own getInstance() comment), which is true today but
        // subtle enough that a future change could violate it silently;
        // locking here costs nothing (this runs once, ever) and removes
        // the dependency on that reasoning staying correct forever.
        if (!parent_)
        {
            std::lock_guard<std::mutex> pool_lock(poolMutex_);
            for (auto& [base, slab] : slabs_by_base_)
            {
                freePage(slab->base, kHugeSlabSize);
                delete slab;
            }
            slabs_by_base_.clear();
            page_registry_.clear();
            available_slabs_.clear();
        }
    }
 
    // --- Stats ---
    long long getCapacity() const
    {
        std::lock_guard<std::mutex> lock(allocationMutex_);
        long long total = 0;
        for (Chunk* c = head_; c; c = c->next) total += c->size;
        return total;
    }
 
    long long getUsage() const
    {
        std::lock_guard<std::mutex> lock(allocationMutex_);
        long long total = 0;
        for (Chunk* c = head_; c; c = c->next) total += c->used;
        return total;
    }
 
    long long getDtorRecordCount() const
    {
        std::lock_guard<std::mutex> lock(allocationMutex_);
        long long count = 0;
        for (DestructorRecord* r = dtorHead_; r; r = r->prev) count++;
        return count;
    }
 
    // PageUsageInfo / queryPageUsage — diagnostic-only lookup of a
    // carved page's own owning slab occupancy, used by memoryTeardown()'s
    // own usage-summary line (below). Deliberately takes poolMutex_
    // itself, independently: this is meant to be called ON WHICHEVER
    // arena a given Chunk's own release_target points at, which is
    // frequently a DIFFERENT MemoryArena instance than the one whose
    // memoryTeardown() is actually running (see Chunk::release_target's
    // own comment) -- allocationMutex_ (held by the CALLING arena's own
    // memoryTeardown()) says nothing about THIS instance's own pool
    // state, so this needs its own, separate lock.
    struct PageUsageInfo
    {
        bool      found      = false;
        long long live_pages = 0;
        long long max_pages  = 0;
        char*     slab_base  = nullptr;
    };
    PageUsageInfo queryPageUsage(char* page_addr)
    {
        std::lock_guard<std::mutex> lock(poolMutex_);
        Slab* owner = resolveSlabLocked(page_addr);
        if (!owner) return PageUsageInfo{};
        return PageUsageInfo{true, owner->live_pages, Slab::kMaxPages, owner->base};
    }
 
    // --- Diagnostics ---
    long long getPageSize()      const { return pageSize_; }
    long long getHugePageSize()  const { return hugePageSize_; }
    long long getChunkSize()     const { return chunkSize_; }
    bool      isUsingHugePages() const { return useHugePages_; }
    bool      isTearingDown()    const { return isTeardown_; }
};
 
// Chunk::~Chunk() — defined here, after MemoryArena's own class body is
// complete: needs releaseChildPage/freePage callable, neither of which
// exist yet at the point Chunk itself is declared (nested inside the
// class it needs to call back into). Same "declare in place, define
// once the enclosing type is whole" split this file already uses for
// Chunk's forward-declared-but-deferred destructor.
inline MemoryArena::Chunk::~Chunk()
{
    if (!buffer) return;
    if (from_pool)
        // release_target, NEVER MemoryArena::getInstance() called fresh
        // here -- this destructor can run from loader-compiled code
        // (destroyImpl and friends, acting on a module-owned entity's
        // arena) just as easily as the owning module's own code, and
        // getInstance() would silently resolve to whichever DSO happens
        // to be executing AT THIS POINT, not whichever DSO actually
        // owns this Chunk's own pool. release_target is a real pointer
        // to the correct underlying object, cached at construction time
        // when the DSO context was guaranteed correct -- see
        // global_arena_'s own comment for the full reasoning.
        release_target->releaseChildPage(buffer, size, owner_scope_tag);
    else
        freePage(buffer, size);
}
 
} // namespace ETCS
 
#endif
 

