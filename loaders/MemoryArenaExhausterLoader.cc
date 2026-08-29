// MemoryArenaTesterLoader.cc
//
// A standalone, loader-side test harness for MemoryArena/Entity/
// ArenaAllocator, deliberately isolated from any real module. Every test
// here spawns entities directly off MemoryArena::getInstance() (the
// true global/root arena) or off a plain top-level entity's own
// local_arena_, and tears them down via MemoryArena::deleteEntity()
// called DIRECTLY -- bypassing DestroyEvent/the loader's own ordering
// thread entirely. That's a deliberate simplification, not an oversight:
// every bug chased this session (the cross-DSO getInstance() mismatch
// aside, which structurally can't reproduce in a single binary with no
// dlopen at all) lives inside MemoryArena's own allocation/reclaim code,
// not in the event-routing layer around it, so calling deleteEntity
// directly reaches the exact same code paths a production DestroyEvent
// would -- just without needing a second thread to originate the call.
//
// Build (adjust to match this codebase's own actual loader build rule --
// see etcs.cc's own Makefile target for the authoritative flags; this is
// a best-effort reconstruction, not verified against it):
//
//   g++ -std=c++17 -fvisibility=hidden -Wall -Wextra -O0 -g \
//       -DETCS_LOADER -DETCS_MODULE_NAME=\"MemoryArenaTester\" \
//       -I. -I.. -pipe -pthread -fuse-ld=gold -ldl \
//       MemoryArenaTesterLoader.cc -o MemoryArenaTesterLoader
//
// Run under LLDB:
//   lldb ./MemoryArenaTesterLoader
//   (lldb) run
//   (lldb) bt all          # if it crashes
//
// -O0 -g deliberately, not -O2: every production crash this session was
// diagnosed from an optimized build with "No symbol table info
// available" filling most frames. A debug build of THIS harness should
// give full symbols and locals at every frame if something still breaks
// here.
#undef ETCS_PRODUCTION_BUILD
#ifndef ETCS_MODULE_NAME
#define ETCS_MODULE_NAME "MemoryArenaTester"
#endif
#include "../ETCS.h"
#include <iostream>
#include <iomanip>
#include <thread>
#include <vector>
#include <atomic>
#include <unordered_map>

// ===========================================================================
// Minimal, self-contained test entity types.
//
// Deliberately NOT reusing any real module type (SocketConnectionState,
// HTTPParser, etc.) -- the whole point of this harness is to isolate
// MemoryArena/Entity/ArenaAllocator's own behavior from module-specific
// complexity (MirrorBuffer, real sockets, TLS, ScopeTag's own
// TagModifyEvent routing) that made the production crashes slow to pin
// down. Every type here is small enough to read in full at a glance.
// ===========================================================================

// Baseline "just an outer shell" case -- isolates the free-list's own
// basic recycle behavior (allocate<T>()'s own free_blocks_ check, and
// registerDtorLocked's own DestructorRecord recycle) from anything
// container-related. No ArenaAllocator-backed member at all.
class PlainLeaf : public ETCS::Entity
{
public:
    WIRE_TYPE_IDENTITY(PlainLeaf)
    char padding[64]; // arbitrary, just to give this a distinguishable size
};

// The specific shape that reproduces the reentrant free_blocks_
// mutation bug: an ArenaAllocator-backed container as a DIRECT member,
// bound to THIS entity's own getArena() -- exactly what Entity's own
// tags/flags_/typed_children_/interface_pointers_ already are, made
// explicit and independently exercisable here. fill() deliberately
// leaves every entry LIVE, never erased before the object is destroyed
// -- forcing ~unordered_map() to walk and deallocate every remaining
// node during ~Entity(), which is the exact path that crashed in
// production (reclaimEntity -> ~SocketConnectionState -> ~Entity ->
// ~interface_pointers_ -> ArenaAllocator::deallocate ->
// releaseToFreeList -> free_blocks_'s own operator[]).
class LeafWithMap : public ETCS::Entity
{
public:
    WIRE_TYPE_IDENTITY(LeafWithMap)

    using MapAlloc = ETCS::ArenaAllocator<std::pair<const int, int>>;
    std::unordered_map<int, int, std::hash<int>, std::equal_to<int>, MapAlloc> data;

    LeafWithMap() : data(MapAlloc(&getArena())) {}

    void fill(int n)
    {
        for (int i = 0; i < n; ++i) data[i] = i * 2;
    }
};

// Parent type for the cascading-teardown tests below -- otherwise
// identical to PlainLeaf, named separately purely for readable test
// output ("Parent" vs "Child" in a crash's own stack, if it crashes).
class ParentLeaf : public ETCS::Entity
{
public:
    WIRE_TYPE_IDENTITY(ParentLeaf)
};

// ===========================================================================
// Test scaffolding -- deliberately minimal, no external test framework
// dependency. Each CHECK prints its own PASS/FAIL immediately, so a
// crash's own last-printed line tells you exactly which assertion it
// was mid-evaluating, even with no debugger attached at all.
// ===========================================================================
static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond, msg)                                                     \
    do {                                                                     \
        if (cond) { std::cout << "  [PASS] " << msg << "\n"; ++g_pass; }     \
        else      { std::cout << "  [FAIL] " << msg << " (" #cond ")\n"; ++g_fail; } \
    } while (0)

static void section(const char* name)
{
    std::cout << "\n=== " << name << " ===\n" << std::flush;
}

// Resolves the SAME arena production code resolves before ever calling
// deleteEntity (see EventNode::LoaderStream::destroyImpl, DynamicLoader.h:
// "target->getParent() ? &target->getParent()->getArena() : ..."). Every
// arena's own dtorHead_/DestructorRecord chain is LOCAL to that specific
// instance -- deleteEntity has to be called ON the arena that actually
// owns the target's own outer-shell bytes, not blindly on the root
// singleton every time. A non-top-level target's own record lives in
// ITS PARENT's arena (that's where addTag<T> bump-allocated it); a
// top-level target's own record lives directly in the root singleton.
// Calling deleteEntity on the wrong arena doesn't crash -- it silently
// finds nothing and no-ops, which is its own trap: an earlier version
// of this harness did exactly that for a non-top-level target and the
// failure looked like a REPARENTING bug (child->getParent() staying
// wrong) rather than what it actually was (parent was never touched at
// all).
static ETCS::MemoryArena& resolveOwningArena(ETCS::Entity* target)
{
    return target->getParent()
        ? target->getParent()->getArena()
        : ETCS::MemoryArena::getInstance();
}

// ===========================================================================
// Test 1 -- basic outer-shell free-list recycling.
//
// Spawn a PlainLeaf, note its own address, delete it, spawn a SECOND
// PlainLeaf. If the free-list is working, allocate<T>()'s own check
// should hand back the EXACT SAME address -- a direct, programmatic way
// to confirm reuse without needing to parse -DETCS_MEMORY_ARENA_DEBUG
// log output.
// ===========================================================================
static void test_basic_recycle()
{
    section("Test 1: basic outer-shell free-list recycling");

    auto& arena = ETCS::MemoryArena::getInstance();

    PlainLeaf* a = arena.allocate<PlainLeaf>();
    void* addr_a = static_cast<void*>(a);
    CHECK(a != nullptr, "first PlainLeaf constructed");

    arena.deleteEntity(a, /*delete_children=*/false);
    std::cout << "  deleted first PlainLeaf at " << addr_a << "\n";

    PlainLeaf* b = arena.allocate<PlainLeaf>();
    void* addr_b = static_cast<void*>(b);
    std::cout << "  second PlainLeaf constructed at " << addr_b << "\n";

    CHECK(addr_a == addr_b, "second PlainLeaf reused the first's exact address (free-list HIT)");

    arena.deleteEntity(b, false);
}

// ===========================================================================
// Test 2 -- repeated cycles stay bounded, not growing per-cycle.
//
// Runs many allocate/delete cycles of the SAME type and confirms the
// address keeps coming back to the same small set of values rather than
// drifting upward forever (which would indicate the free-list ISN'T
// actually being hit past the first recycle, e.g. if reclaimEntity's own
// zeroing or push were subtly broken for later cycles specifically).
// ===========================================================================
static void test_repeated_cycles()
{
    section("Test 2: repeated allocate/delete cycles stay bounded");

    auto& arena = ETCS::MemoryArena::getInstance();
    constexpr int kCycles = 200;

    void* first_addr = nullptr;
    int distinct_addresses = 0;
    std::vector<void*> seen;

    for (int i = 0; i < kCycles; ++i)
    {
        PlainLeaf* p = arena.allocate<PlainLeaf>();
        void* addr = static_cast<void*>(p);
        if (i == 0) first_addr = addr;

        bool seen_before = false;
        for (void* s : seen) if (s == addr) { seen_before = true; break; }
        if (!seen_before) { seen.push_back(addr); ++distinct_addresses; }

        arena.deleteEntity(p, false);
    }

    std::cout << "  " << kCycles << " cycles touched " << distinct_addresses
              << " distinct address(es)\n";
    // A handful of distinct addresses is fine (free-list is a stack, not
    // guaranteed to hand back the SAME slot every time if anything else
    // interleaves) -- what matters is this number staying small and
    // bounded, not growing with kCycles, which would mean the free-list
    // isn't actually being hit past the first few cycles.
    CHECK(distinct_addresses <= 4, "distinct addresses stayed bounded across all cycles (free-list is being hit consistently)");
}

// ===========================================================================
// Test 3 -- THE reentrancy reproduction.
//
// This is the specific scenario the production SIGFPE traced back to:
// an entity with an ArenaAllocator-backed container holding LIVE
// entries at the moment of its own destruction, torn down via
// deleteEntity(delete_children=true) -- reclaimEntity -> ~LeafWithMap
// -> ~Entity -> ~data (the unordered_map) -> N x
// ArenaAllocator::deallocate -> releaseToFreeList -> free_blocks_'s own
// operator[]. If the lock-scope fix in memoryTeardown()/reset() (or any
// SIBLING path that still holds allocationMutex_ across a destructor
// call) is incomplete, this is where it should crash or hang, with full
// debug symbols and a small, isolated call stack -- not fifty threads of
// unrelated production noise.
// ===========================================================================
static void test_reentrancy_reproduction()
{
    section("Test 3: live-map-at-teardown reentrancy reproduction");

    auto& arena = ETCS::MemoryArena::getInstance();
    constexpr int kEntries = 64;
    constexpr int kCycles  = 20;

    for (int cycle = 0; cycle < kCycles; ++cycle)
    {
        LeafWithMap* leaf = arena.allocate<LeafWithMap>();
        leaf->fill(kEntries);
        CHECK(static_cast<long long>(leaf->data.size()) == kEntries,
              "map has all entries live immediately before teardown (cycle " + std::to_string(cycle) + ")");

        // The line that matters. If this returns normally, the whole
        // reclaimEntity -> ~Entity -> ~unordered_map -> N x
        // ArenaAllocator::deallocate -> releaseToFreeList chain ran
        // without a same-thread mutex re-lock or a corrupted
        // free_blocks_ bucket table.
        arena.deleteEntity(leaf, false);
    }

    std::cout << "  completed " << kCycles << " live-map teardown cycles ("
              << kEntries << " entries each) without crashing\n";
    CHECK(true, "all live-map teardown cycles completed");
}

// ===========================================================================
// Test 4 -- cascading parent/child teardown (delete_children=true).
//
// addTag<T> a LeafWithMap (with live entries) onto a ParentLeaf, then
// delete the PARENT with delete_children=true -- exercising the
// global-scope branch of registerDtor<T>'s own run_entity_delete lambda
// (findNextCandidateScope/promoteOrVacate, then reclaimEntity on the
// parent itself), with a child that ALSO has the live-map-at-teardown
// shape nested one level down.
// ===========================================================================
static void test_cascading_teardown()
{
    section("Test 4: cascading parent/child teardown (delete_children=true)");

    auto& arena = ETCS::MemoryArena::getInstance();

    ParentLeaf* parent = arena.allocate<ParentLeaf>();
    CHECK(parent != nullptr, "parent constructed");

    LeafWithMap* child = parent->addTag<LeafWithMap>();
    CHECK(child != nullptr, "child attached via addTag<T>");
    child->fill(32);
    CHECK(static_cast<long long>(child->data.size()) == 32, "child's own map has live entries");
    CHECK(child->getParent() == parent, "child's own getParent() correctly points at parent");

    arena.deleteEntity(parent, /*delete_children=*/true);
    std::cout << "  parent + child (with live map) torn down via delete_children=true\n";
    CHECK(true, "cascading teardown completed without crashing");
}

// ===========================================================================
// Test 5 -- reparent case (delete_children=false, the default).
//
// A top-level entity can NEVER exercise this path -- confirmed the hard
// way, by an earlier version of this test that made `parent` itself
// top-level and crashed here. Tracing why is worth keeping as the
// test's own documentation: registerDtor<T>'s own three-way branch
// checks e->getParent() == nullptr FIRST, unconditionally, before ever
// looking at delete_children at all -- a top-level entity always takes
// that branch, which ALWAYS calls evokeDestructor(own_arena),
// unconditionally tearing down its own content arena regardless of what
// delete_children was passed. Coyote time (the reparent branch, which
// deliberately leaves the dying entity's own content arena alive) is
// only reachable when e->getParent() != nullptr -- there has to be a
// real grandparent to reparent children UP TO for "preserve the
// content arena so reparented children stay valid" to mean anything at
// all. This is documented, intentional behavior (see
// reparentChildrenTo's own comment, Entity.h: "the global-scope case
// never calls this at all"), not a bug -- but worth naming explicitly:
// deleteEntity(top_level_entity, false), expecting coyote-time
// protection for ITS OWN children, hits exactly this same crash in
// production too, not just in a test.
//
// So this test needs a genuine THREE-level hierarchy: a top-level
// grandparent, a MIDDLE parent attached via addTag<T> (giving it a
// real, non-null getParent()), and the child attached to THAT. Deleting
// the middle parent with delete_children=false is what actually
// reaches the reparent branch -- the child should survive, reparented
// up to the grandparent, with its own content arena (living inside the
// now-dead middle parent's own local_arena_, which coyote time keeps
// mapped) still genuinely valid.
// ===========================================================================
static void test_reparent_case()
{
    section("Test 5: reparent case (delete_children=false, three-level hierarchy)");

    auto& arena = ETCS::MemoryArena::getInstance();

    ParentLeaf* grandparent = arena.allocate<ParentLeaf>();
    ParentLeaf* parent      = grandparent->addTag<ParentLeaf>();
    LeafWithMap* child      = parent->addTag<LeafWithMap>();
    child->fill(16);
    ETCS::RID child_rid = child->getRID();
    CHECK(parent->getParent() == grandparent, "middle parent's own getParent() correctly points at grandparent (non-null)");

    resolveOwningArena(parent).deleteEntity(parent, /*delete_children=*/false);
    std::cout << "  middle parent deleted with delete_children=false\n";

    // Child should have survived -- its own outer shell was never
    // touched, only reparented, and the now-dead middle parent's own
    // content arena (where child's own outer shell AND local_arena_
    // both physically live) was deliberately left mapped (coyote time),
    // specifically because THIS branch -- unlike the top-level case --
    // had a real grandparent to reparent up to.
    CHECK(child->getParent() == grandparent,
          "child's own parent_ correctly redirected up a generation (to grandparent, not nullptr)");
    CHECK(static_cast<long long>(child->data.size()) == 16,
          "child's own map is still fully readable after middle parent's teardown (coyote time held)");
    CHECK(child->getRID() == child_rid, "child's own RID unchanged across reparenting");

    // Manually clean up -- deleting the grandparent with
    // delete_children=true reaches everything transitively: the
    // now-orphaned child (still reachable through grandparent's own
    // typed_children_, since reparentChildrenTo already redirected it
    // there), and the middle parent's own still-alive content arena
    // that child's own bytes live inside.
    arena.deleteEntity(grandparent, /*delete_children=*/true);
}

// ===========================================================================
// Test 6 -- reset() specifically.
//
// reset() has the identical dtor-walk-under-lock shape memoryTeardown()
// had -- same fix, same reason to verify independently. Spawns several
// LeafWithMap instances (live entries) directly into a CHILD arena
// (not the global singleton itself, since reset() on the true root
// would tear down this whole test process's own state), then calls
// reset() on that child arena and confirms it returns normally.
// ===========================================================================
static void test_reset_reentrancy()
{
    section("Test 6: reset() reentrancy (same shape as memoryTeardown())");

    auto& arena = ETCS::MemoryArena::getInstance();

    // A throwaway top-level entity purely to host a REAL, resettable
    // child arena (getArena()) that isn't the global singleton itself.
    ParentLeaf* host = arena.allocate<ParentLeaf>();
    for (int i = 0; i < 10; ++i)
    {
        LeafWithMap* child = host->addTag<LeafWithMap>();
        child->fill(20);
    }

    std::cout << "  calling reset() on host's own local_arena_ (10 prior live-map children)...\n";
    host->getArena().reset();
    std::cout << "  reset() returned normally\n";
    CHECK(true, "reset() completed without crashing despite live-map children");

    arena.deleteEntity(host, true);
}

// ===========================================================================
// Test 7 -- concurrent, multi-threaded churn on the SAME arena.
//
// The production bug involved genuine cross-thread concurrency (a pool
// worker thread's own callback touching an entity while the loader's
// ordering thread concurrently tore it down) -- this stress-tests
// MemoryArena's own locking discipline directly: several threads,
// simultaneously, repeatedly allocating and deleting entities of the
// SAME type off the SAME global arena, for a fixed duration. Any
// remaining race in allocationMutex_'s own coverage (a path that reads/
// writes free_blocks_, dtorHead_, or head_ without holding it) has a
// real chance to surface here as a crash, hang, or corrupted state --
// though a clean run is evidence of absence, not proof; races are
// probabilistic; treat this as a screening net, not a formal guarantee.
// ===========================================================================
static void test_concurrent_churn()
{
    section("Test 7: concurrent multi-threaded churn on the same arena");

    auto& arena = ETCS::MemoryArena::getInstance();
    constexpr int kThreads          = 8;
    constexpr int kCyclesPerThread  = 500;
    std::atomic<int> crashes_or_errors{0}; // can't catch a SIGSEGV/SIGFPE via
                                            // try/catch -- this only counts
                                            // C++ exceptions, if any get
                                            // thrown; an actual crash still
                                            // takes down the whole process,
                                            // which is itself the signal.

    auto worker = [&](int thread_id)
    {
        try
        {
            for (int i = 0; i < kCyclesPerThread; ++i)
            {
                LeafWithMap* leaf = arena.allocate<LeafWithMap>();
                leaf->fill(8);
                arena.deleteEntity(leaf, false);
            }
        }
        catch (const std::exception& e)
        {
            std::cerr << "  [thread " << thread_id << "] exception: " << e.what() << "\n";
            ++crashes_or_errors;
        }
    };

    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) threads.emplace_back(worker, t);
    for (auto& t : threads) t.join();

    std::cout << "  " << kThreads << " threads x " << kCyclesPerThread
              << " cycles each completed\n";
    CHECK(crashes_or_errors.load() == 0, "no exceptions across all concurrent threads");
    CHECK(true, "process still alive after concurrent churn (no crash/hang)");
}

int main()
{
    std::cout << "MemoryArenaTesterLoader -- direct MemoryArena/Entity/"
                 "ArenaAllocator test harness\n";
    std::cout << "Build with -DETCS_MEMORY_ARENA_DEBUG for full allocation "
                 "tracing alongside these results.\n";

    // Same boilerplate etcs.cc itself uses -- brings up the loader's own
    // signal context and (via ETCS.h's own include chain) the ordering
    // thread infrastructure addTag<T> depends on for tests 4-6.
    shell_startup();
    WIRE_CONTEXT();
    (void)ctx;

    test_basic_recycle();
    test_repeated_cycles();
    test_reentrancy_reproduction();
    test_cascading_teardown();
    test_reparent_case();
    test_reset_reentrancy();
    test_concurrent_churn();

    std::cout << "\n=== Summary: " << g_pass << " passed, " << g_fail << " failed ===\n";
    return g_fail == 0 ? 0 : 1;
}
