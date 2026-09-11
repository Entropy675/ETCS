#include "../ETCS.h"
#include "../core/SharedPage.h"
#include "../core/LMAXSequentialSharedPage.h"
#include "../core/EventStream.h"
#include <thread>
#include <vector>
#include <atomic>
#include <cassert>
#include <iostream>
#include <chrono>
#include <iomanip>
#include <pthread.h>
#include <string>
#include <sstream>
#include <cmath>
#include <algorithm>
#include <random>
#include <numeric>
#include <mutex>
#include <condition_variable>
#include <unordered_map>
#include <functional>
#include <deque>
#include <dlfcn.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstdio>
#include <filesystem>
// ── helpers ───────────────────────────────────────────────────────────────────
static int s_passed = 0;
static int s_failed = 0;
#define TEST(name, expr) \
    do { \
        if (expr) { \
            std::cout << "  [PASS] " name "\n"; \
            ++s_passed; \
        } else { \
            std::cout << "  [FAIL] " name "\n"; \
            ++s_failed; \
        } \
    } while(0)
static void section(const char* name)
{
    std::cout << "\n=== " << name << " ===\n";
}
static int get_core_count()
{
    int n = static_cast<int>(std::thread::hardware_concurrency());
    return n > 0 ? n : 1;
}
static void pin_thread(int core_id)
{
    if (core_id < 0) return;
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core_id, &cpuset);
    pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
}
std::string formatWithCommas(long long n)
{
    std::string s = std::to_string(n);
    int pos = static_cast<int>(s.length()) - 3;
    int limit = (n < 0) ? 1 : 0;
    while (pos > limit) { s.insert(pos, ","); pos -= 3; }
    return s;
}
std::string formatDuration(double ns)
{
    if (ns <= 0) return "0ns";
    std::ostringstream o; o << std::fixed << std::setprecision(1);
    if      (ns >= 1e9) o << (ns / 1e9) << "s";
    else if (ns >= 1e6) o << (ns / 1e6) << "ms";
    else                o << ns          << "ns";
    return o.str();
}
std::string formatBytes(uint64_t bytes)
{
    static const std::vector<std::string> u = {"B","KB","MB","GB","TB"};
    if (!bytes) return "0B";
    int m = static_cast<int>(std::log2(bytes) / 10);
    if (m >= (int)u.size()) m = u.size() - 1;
    double v = static_cast<double>(bytes) / std::pow(1024, m);
    std::ostringstream o;
    if (!m) o << bytes << u[0];
    else    o << std::fixed << std::setprecision(1) << v << u[m];
    return o.str();
}
static const std::string RESET      = "\033[0m";
static const std::string FG_GREEN   = "\033[1;32m";
static const std::string FG_CYAN    = "\033[1;36m";
static const std::string FG_RED     = "\033[31m";
static const std::string FG_YELLOW  = "\033[1;33m";
static const std::string FG_MAGENTA = "\033[1;35m";
static const std::string FG_DIM     = "\033[2m";
// ── Test EventStream Types ────────────────────────────────────────────────────
// FIX: Aligned the state struct to prevent false sharing cache invalidations
struct alignas(64) TestState
{
    int inline_count     = 0;
    int async_count      = 0;
    int drop_count       = 0;
    int completion_count = 0;
};
enum class TestEventKind : int { Inline = 0, Async = 1, Drop = 2 };
// tag_bit, NOT a TagMask. Two reasons, and the first is a hard constraint:
//
//   1. SIZE. enqueue() memcpys the whole InEvent through one LBuffer slot
//      (MAX_LMAX_BUFFER_SIZE). A TagMask is TAG_WORDS * 8 bytes -- 32 at
//      ETCS_MAX_MODULE_TAGS 256 -- which does not fit, and would silently
//      overrun exactly the way ChessInEventPtr's own comment describes. Real
//      streams keep the event small and resolve the mask elsewhere; so does
//      this one.
//   2. SHAPE. The mask now comes from mask_for(state, event), which runs
//      BEFORE the handler, so the event only has to carry enough to derive it.
//      Passing a ready-made mask would be testing a path that no longer exists.
//
// -1 means the empty mask: independent of everything. Deliberately reachable --
// see test_eventstream_independent_tags.
struct TestInEvent
{
    TestEventKind kind    = TestEventKind::Inline;
    int           tag_bit = -1;
    int           data    = 0;
    /*
     * THE CLOSURE, and the reason a single tag_bit was never the real shape.
     *
     * A type's mask is its own bit OR'd with the bits of every type in the
     * same module it reaches -- TAG_CLOSURE in ETCS_API.h's
     * WIRE_TYPE_IDENTITY, accumulated at the typed acquisition sites. So a
     * real event's mask is an EDGE SET over the module's own bit space, not
     * one bit and not a spray of random ones.
     *
     * A uint32_t rather than a TagMask, and the width is not a guess:
     * MAX_LMAX_BUFFER_SIZE is 16 bytes and the three fields above spend 12,
     * so this is exactly the room left. It is still four times what is
     * needed -- the widest module in the tree declares 8 tags
     * (RenderProvider, NetworkProvider) -- and a module that outgrew 32 bits
     * would have outgrown far more than this test.
     */
    uint32_t      closure = 0;
};
static_assert(sizeof(TestInEvent) <= MAX_LMAX_BUFFER_SIZE,
              "TestInEvent must fit one LBuffer slot -- enqueue() memcpys it whole");
struct TestWorkResult : ETCS::WorkResult
{
    int result_data = 0;
};
// ── Lockless SPMC ring for uint64_t work items ────────────────────────────────
// Single producer (ordering thread via on_event), N consumers (workers).
// CAP >> GAP_DEPTH means the ring never wraps in practice.
struct alignas(64) SPMCSeqRing
{
    static constexpr uint64_t CAP  = 512;
    static constexpr uint64_t MASK = CAP - 1;
    alignas(64) std::atomic<uint64_t> write_{0};  // producer advances
    alignas(64) std::atomic<uint64_t> claim_{0};  // consumers CAS
    uint64_t buf_[CAP]{};
    // Single producer only. Capacity guaranteed by GAP_DEPTH backpressure.
    void push(uint64_t val) noexcept
    {
        uint64_t w = write_.load(std::memory_order_relaxed);
        buf_[w & MASK] = val;
        // release: buf_ write visible before write_ increment
        write_.store(w + 1, std::memory_order_release);
    }
    // Multiple consumers: CAS on claim_ for exclusive ownership of a slot.
    bool pop(uint64_t& val) noexcept
    {
        uint64_t c, w;
        do {
            c = claim_.load(std::memory_order_relaxed);
            // acquire: synchronises with producer's release on write_,
            // making buf_[c] visible before we read it
            w = write_.load(std::memory_order_acquire);
            if (c >= w) return false;
        } while (!claim_.compare_exchange_weak(
                     c, c + 1,
                     std::memory_order_acq_rel,
                     std::memory_order_relaxed));
        val = buf_[c & MASK];
        return true;
    }
};
struct TestEventStream
    : ETCS::EventStream<TestEventStream, TestState, TestInEvent>
{
    // ── lockless emit tracking ─────────────────────────────────────────────────
    //
    // on_emit runs exclusively on the ordering thread, so emit_idx_ needs no
    // synchronisation. emit_count_ is the publication fence: readers spin on it
    // with acquire; the ordering thread publishes with release.
    //
    // Happens-before chain:
    //   ordering thread:  emitted_buf_[emit_idx_++] = seq
    //                     emit_count_.fetch_add(release)
    //   test thread:      emit_count_.load(acquire)  → sees all buf_ writes up to N
    //                     emitted_buf_[0..N-1]       → safe to read
    static constexpr size_t MAX_TRACKED = 1 << 24; // 16M slots, 128 MB
    std::vector<uint64_t>           emitted_buf_;
    size_t                          emit_idx_   = 0;   // ordering-thread-private
    alignas(64) std::atomic<size_t> emit_count_{0};    // publication fence
    // Incremented at the TOP of on_event, so it counts ADMISSIONS, not arrivals.
    // Under the admission barrier a blocked event is parked in its slot with its
    // handler unrun, so this lagging behind the enqueue count is the observable
    // signature of the whole mechanism -- see test_eventstream_admission.
    std::atomic<size_t> events_ingested{0};
    std::atomic<size_t> completions_done{0};
    // ── lockless worker pool ───────────────────────────────────────────────────
    std::vector<std::thread> workers_;
    std::atomic<bool>        workers_running_{false};
    SPMCSeqRing              wq_;
    // ── constructor ────────────────────────────────────────────────────────────
    // Tracked capacity is a parameter because the capability section runs ~40
    // short tests back to back, and MAX_TRACKED is 128MB of vector each time.
    explicit TestEventStream(size_t tracked = MAX_TRACKED)
    {
        emitted_buf_.resize(tracked);
    }
    // ── CRTP contract ──────────────────────────────────────────────────────────
    //
    // mask_for runs on the raw event BEFORE on_event, and its answer decides
    // whether on_event may run at all. Hiding EventStream's own all() default,
    // which is what an un-overriding stream inherits.
    ETCS::TagMask mask_for(TestState&, const TestInEvent& evt)
    {
        if (evt.tag_bit < 0 && evt.closure == 0) return ETCS::TagMask{};
        ETCS::TagMask m;
        if (evt.tag_bit >= 0) m = ETCS::TagMask::bit(static_cast<size_t>(evt.tag_bit));
        // The closure half. OR'd rather than replacing, because a type always
        // collides with itself first -- two operations on one type must
        // serialize, which is the guarantee the mask exists to provide.
        for (size_t b = 0; b < 32; ++b)
            if (evt.closure & (uint32_t(1) << b)) m |= ETCS::TagMask::bit(b);
        return m;
    }
    ETCS::DispatchResult on_event(TestState& st,
                                  const TestInEvent& evt,
                                  uint64_t seq)
    {
        events_ingested.fetch_add(1, std::memory_order_release);
        switch (evt.kind)
        {
            case TestEventKind::Inline:
                st.inline_count++;
                return ETCS::DispatchResult{ ETCS::DispatchKind::Inline, nullptr };
            case TestEventKind::Async:
                st.async_count++;
                if (workers_running_.load(std::memory_order_acquire))
                    wq_.push(seq); // lockless; no mutex, no notify
                return ETCS::DispatchResult{ ETCS::DispatchKind::Async, nullptr };
            case TestEventKind::Drop:
                st.drop_count++;
                return ETCS::DispatchResult{ ETCS::DispatchKind::Drop, nullptr };
        }
        return ETCS::DispatchResult{};
    }
    void on_completion(TestState& st, ETCS::WorkResult*, uint64_t)
    {
        st.completion_count++;
        completions_done.fetch_add(1, std::memory_order_release);
    }
    void on_emit(TestState&, ETCS::GapSlot& slot)
    {
        // ordering-thread-private write: no atomic needed on the index or buf_
        if (emit_idx_ < emitted_buf_.size())
            emitted_buf_[emit_idx_++] = slot.sequence;
        // publish: release fence makes buf_ write visible to spinning test thread
        emit_count_.fetch_add(1, std::memory_order_release);
        // FIX: Removed the global allocator 'delete' call here.
        // The slot.result memory is now owned by the thread-local vectors in worker_loop.
    }
    // ── worker pool ────────────────────────────────────────────────────────────
    void start_workers(int count)
    {
        workers_running_.store(true, std::memory_order_release);
        for (int i = 0; i < count; ++i)
            workers_.emplace_back(&TestEventStream::worker_loop, this);
    }
    void worker_loop()
    {
        // FIX: Thread-local ring buffer for WorkResults to bypass global heap.
        // 8192 is more than enough to cover the GAP_DEPTH in-flight items per thread.
        std::vector<TestWorkResult> local_results(8192);
        size_t res_idx = 0;

        int retry = 0;
        while (workers_running_.load(std::memory_order_acquire))
        {
            uint64_t seq;
            if (wq_.pop(seq))
            {
                retry = 0;

                // Grab next pre-allocated object from the local ring
                auto* res        = &local_results[res_idx++ % local_results.size()];
                res->origin_seq  = seq;
                res->result_data = static_cast<int>(seq);
                post_completion(res);
            }
            else
            {
                // spin → yield → 1µs sleep; workers park cheaply when idle
                ETCS::LMAXSequentialSharedPage::progressiveYield(retry);
            }
        }
    }
    void stop_workers()
    {
        workers_running_.store(false, std::memory_order_release);
        for (auto& t : workers_)
            if (t.joinable()) t.join();
        workers_.clear();
    }
    // ── wait for emits ─────────────────────────────────────────────────────────
    //
    // Spin on the atomic publication fence instead of sleeping on a condvar.
    // progressiveYield keeps the CPU busy in the common case (events are fast)
    // and backs off gracefully under timeout conditions.
    std::vector<uint64_t> wait_for_emits(size_t count, int timeout_ms = 2000)
    {
        auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(timeout_ms);
        int retry = 0;
        while (emit_count_.load(std::memory_order_acquire) < count)
        {
            if (std::chrono::steady_clock::now() >= deadline)
                break;
            ETCS::LMAXSequentialSharedPage::progressiveYield(retry);
        }
        size_t n = std::min(
            emit_count_.load(std::memory_order_acquire),
            std::min(emit_idx_, MAX_TRACKED)
        );
        return std::vector<uint64_t>(emitted_buf_.data(), emitted_buf_.data() + n);
    }
    // Spin until `n` events have been ADMITTED, or the deadline passes. Returns
    // the count actually reached, so a test can assert it did NOT get there.
    size_t wait_for_ingest(size_t n, int timeout_ms = 300)
    {
        auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(timeout_ms);
        int retry = 0;
        while (events_ingested.load(std::memory_order_acquire) < n &&
               std::chrono::steady_clock::now() < deadline)
            ETCS::LMAXSequentialSharedPage::progressiveYield(retry);
        return events_ingested.load(std::memory_order_acquire);
    }
};
// ── unit tests ────────────────────────────────────────────────────────────────
static void test_eventstream_inline_only(ETCS::MemoryArena& arena)
{
    section("EventStream: Inline Only");
    TestEventStream stream;
    WIRE_CONTEXT();
    stream.start(arena, 1);
    constexpr int N = 10;
    for (int i = 0; i < N; ++i)
    {
        TestInEvent evt{TestEventKind::Inline, -1, i};
        stream.enqueue(evt);
    }
    auto emitted = stream.wait_for_emits(N);
    stream.stop();
    TEST("all inline events emitted", emitted.size() == N);
    bool ordered = true;
    for (size_t i = 0; i < emitted.size(); ++i)
    {
        if (emitted[i] != i) { ordered = false; break; }
    }
    TEST("inline events emitted in strict sequence order", ordered);
    TEST("state inline_count matches", stream.state().inline_count == N);
}
static void test_eventstream_drop_only(ETCS::MemoryArena& arena)
{
    section("EventStream: Drop Only");
    TestEventStream stream;
    WIRE_CONTEXT();
    stream.start(arena, 1);
    constexpr int N = 10;
    for (int i = 0; i < N; ++i)
    {
        TestInEvent evt{TestEventKind::Drop, -1, i};
        stream.enqueue(evt);
    }
    // Give the ordering thread time to process
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    stream.stop();
    TEST("drop events ingested", stream.events_ingested.load() == N);
    TEST("drop events not emitted", stream.emit_count_.load(std::memory_order_acquire) == 0);
    TEST("state drop_count matches", stream.state().drop_count == N);
}
// ── the admission barrier ─────────────────────────────────────────────────────
//
// The one test that distinguishes the current design from the previous one.
// Before, blocked_by_ was computed AFTER on_event had already run, so a
// dependent event's handler executed immediately and only its EMISSION was held
// back. Now acquire() runs first and a blocked event is parked with its handler
// unrun. events_ingested is incremented at the top of on_event, so it is a
// direct probe of that.
static void test_eventstream_admission(ETCS::MemoryArena& arena)
{
    section("EventStream: Admission Barrier (blocked handler does not run)");
    TestEventStream stream;
    WIRE_CONTEXT();
    stream.start(arena, 1);
    // No workers: seq 0 stays Running until we complete it by hand.
    TestInEvent blocker{TestEventKind::Async,  0, 0};   // bit 0
    TestInEvent blocked{TestEventKind::Inline, 0, 1};   // bit 0 -- same type
    stream.enqueue(blocker);
    stream.enqueue(blocked);
    size_t ingested = stream.wait_for_ingest(2, 300);
    TEST("blocker admitted", ingested >= 1);
    TEST("dependent handler has NOT run while its blocker is outstanding",
         ingested == 1);
    TEST("and nothing has been emitted", stream.emit_count_.load() == 0);
    // Release it. service() must launch the parked slot, see it complete
    // synchronously, and commit both -- in one call, via the fixpoint.
    auto* res = new TestWorkResult();
    res->origin_seq = 0;
    stream.post_completion(res);
    auto emitted = stream.wait_for_emits(2);
    stream.stop();
    TEST("dependent handler runs once its blocker commits",
         stream.events_ingested.load() == 2);
    TEST("both emitted, in dependency order",
         emitted.size() == 2 && emitted[0] == 0 && emitted[1] == 1);
    delete res;
}
// A Drop hands its slot straight back (GapReorderBuffer::abandon) rather than
// holding it to commit, so it must not block a later event sharing its mask.
// Admission still applies to the Drop itself -- Drop means "no completion to
// order", not "unordered".
static void test_eventstream_drop_abandons(ETCS::MemoryArena& arena)
{
    section("EventStream: Drop Abandons Its Slot");
    TestEventStream stream;
    WIRE_CONTEXT();
    stream.start(arena, 1);
    constexpr int N = 8;
    for (int i = 0; i < N; ++i)
    {
        // All on bit 3: if a Drop held its slot, each would block the next.
        TestInEvent evt{i % 2 ? TestEventKind::Inline : TestEventKind::Drop, 3, i};
        stream.enqueue(evt);
    }
    auto emitted = stream.wait_for_emits(N / 2);
    stream.stop();
    TEST("every event ran despite sharing one tag bit",
         stream.events_ingested.load() == N);
    TEST("only the inline half emitted", emitted.size() == static_cast<size_t>(N / 2));
    bool ordered = true;
    for (size_t i = 0; i < emitted.size(); ++i)
        if (emitted[i] != (i * 2 + 1)) { ordered = false; break; }
    TEST("emitted the odd sequences, in order", ordered);
}
static void test_eventstream_async_ordering(ETCS::MemoryArena& arena)
{
    section("EventStream: Async Ordering (Dependent)");
    TestEventStream stream;
    WIRE_CONTEXT();
    stream.start(arena, 1);
    stream.start_workers(1);
    // Event 0: Async (bit 0)
    // Event 1: Inline (bit 0 - depends on 0)
    // Event 1 is not admitted until 0 commits
    TestInEvent evt0{TestEventKind::Async,  0, 0};
    TestInEvent evt1{TestEventKind::Inline, 0, 1};
    stream.enqueue(evt0);
    stream.enqueue(evt1);
    auto emitted = stream.wait_for_emits(2);
    stream.stop_workers();
    stream.stop();
    TEST("both events emitted", emitted.size() == 2);
    if (emitted.size() == 2)
    {
        TEST("async evt0 emitted before dependent inline evt1",
             emitted[0] == 0 && emitted[1] == 1);
    }
}
static void test_eventstream_independent_tags(ETCS::MemoryArena& arena)
{
    section("EventStream: Independent Tags (Out-of-Order Commit)");
    TestEventStream stream;
    WIRE_CONTEXT();
    stream.start(arena, 1);
    // We will manually drive completions to force out-of-order commits
    // Event 0: Async (bit 0)
    // Event 1: Async (bit 1) - Independent of 0
    // Event 2: Inline (empty mask) - Independent of all
    //
    // The empty mask on evt2 is load-bearing and worth naming: intersects()
    // requires BOTH sides non-empty, so an empty mask never collides with
    // anything, in either direction. That is the fail-open hole every default
    // in the system is all() to avoid -- used deliberately here.
    TestInEvent evt0{TestEventKind::Async,   0, 0};
    TestInEvent evt1{TestEventKind::Async,   1, 1};
    TestInEvent evt2{TestEventKind::Inline, -1, 2};
    stream.enqueue(evt0);
    stream.enqueue(evt1);
    stream.enqueue(evt2);
    // evt2 is independent and inline, it should emit immediately
    // evt0 and evt1 are pending. We complete evt1 first.
    // Since evt1 is independent of evt0, it should emit before evt0.
    // Wait for evt2 to emit first so we know the ordering thread has processed ingests
    auto emitted = stream.wait_for_emits(1);
    TEST("independent inline evt2 emits first", emitted.size() == 1 && emitted[0] == 2);
    // Manually complete evt1 (seq=1)
    auto* res1 = new TestWorkResult();
    res1->origin_seq = 1;
    res1->result_data = 100;
    stream.post_completion(res1);
    auto emitted2 = stream.wait_for_emits(2);
    TEST("independent async evt1 emits before evt0 is complete",
         emitted2.size() == 2 && emitted2[1] == 1);
    // Complete evt0 (seq=0)
    auto* res0 = new TestWorkResult();
    res0->origin_seq = 0;
    res0->result_data = 50;
    stream.post_completion(res0);
    auto emitted3 = stream.wait_for_emits(3);
    stream.stop();
    TEST("all 3 events emitted", emitted3.size() == 3);
    TEST("evt0 emitted last", emitted3.size() == 3 && emitted3[2] == 0);
    delete res0;
    delete res1;
}
static void test_eventstream_gap_depth_backpressure(ETCS::MemoryArena& arena)
{
    section("EventStream: Gap Depth Backpressure");
    TestEventStream stream;
    WIRE_CONTEXT();
    stream.start(arena, 1);
    // We do NOT start workers. Any async event will stall indefinitely.
    // We push GAP_DEPTH + 10 events. The ordering thread will ingest GAP_DEPTH
    // and then stall. The remaining 10 will sit in the input ring.
    //
    // Every one of the first GAP_DEPTH gets a DISTINCT bit, so admission never
    // blocks and the window fills -- isolating slot exhaustion from mask
    // blocking, which are two different reasons to stall and would otherwise be
    // indistinguishable here.
    constexpr int PUSH_COUNT = ETCS::GAP_DEPTH + 10;
    static_assert(ETCS::GAP_DEPTH <= ETCS::TAG_BITS,
                  "this test needs one distinct tag bit per slot");
    for (int i = 0; i < PUSH_COUNT; ++i)
    {
        TestInEvent evt{TestEventKind::Async, i % static_cast<int>(ETCS::GAP_DEPTH), i};
        stream.enqueue(evt);
    }
    // Wait for the ordering thread to ingest up to the backpressure limit
    int retry = 0;
    while (stream.events_ingested.load(std::memory_order_acquire) < ETCS::GAP_DEPTH && retry < 2000)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        retry++;
    }
    TEST("window fills to GAP_DEPTH", stream.events_ingested.load() == ETCS::GAP_DEPTH);
    // Wait a bit more to ensure it doesn't ingest any more
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    TEST("ordering thread blocked from ingesting more",
         stream.events_ingested.load() == ETCS::GAP_DEPTH);
    // Release the backpressure by completing all async events
    std::vector<TestWorkResult*> held;
    for (size_t i = 0; i < ETCS::GAP_DEPTH; ++i)
    {
        auto* res = new TestWorkResult();
        res->origin_seq = i;
        held.push_back(res);
        stream.post_completion(res);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    // Wait for the rest to be ingested
    retry = 0;
    while (stream.events_ingested.load(std::memory_order_acquire) < PUSH_COUNT && retry < 2000)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        retry++;
    }
    TEST("backpressure released and remaining events ingested",
         stream.events_ingested.load() == PUSH_COUNT);
    stream.stop();
    for (auto* r : held) delete r;
}
static void test_eventstream_mixed_traffic(ETCS::MemoryArena& arena)
{
    section("EventStream: Mixed Inline/Async/Drop Traffic");
    TestEventStream stream;
    WIRE_CONTEXT();
    stream.start(arena, 4);
    stream.start_workers(2);
    constexpr int N = 5000;
    for (int i = 0; i < N; ++i)
    {
        TestEventKind kind;
        if (i % 5 == 0) kind = TestEventKind::Drop;
        else if (i % 3 == 0) kind = TestEventKind::Async;
        else kind = TestEventKind::Inline;
        // bit 0, bit 1, or empty (independent)
        int tag = -1;
        if (i % 7 == 0) tag = 0;
        else if (i % 11 == 0) tag = 1;
        TestInEvent evt{kind, tag, i};
        stream.enqueue(evt);
    }
    // Expected emits: N - drops
    int expected_emits = N - (N / 5);
    auto emitted = stream.wait_for_emits(expected_emits, 5000);
    stream.stop_workers();
    stream.stop();
    TEST("mixed traffic: correct number of events emitted",
         emitted.size() == static_cast<size_t>(expected_emits));
    // Verify per-tag causal ordering, not global sequence order.
    // Independent events (empty mask) have no ordering guarantee by design.
    auto last_seen = std::unordered_map<int, uint64_t>{};
    bool valid_order = true;
    for (uint64_t seq : emitted)
    {
        // Reconstruct tag from seq (matches enqueue formula, single producer so seq==i)
        int tag = (seq % 7 == 0) ? 0
                : (seq % 11 == 0) ? 1
                : -1;
        if (tag < 0) continue; // no ordering guarantee
        auto [it, inserted] = last_seen.emplace(tag, seq);
        if (!inserted)
        {
            if (seq <= it->second) { valid_order = false; break; }
            it->second = seq;
        }
    }
    TEST("mixed traffic: same-tag sequences are strictly increasing", valid_order);
}
inline uint64_t fast_hash(uint64_t x)
{
    x ^= x >> 30;
    x *= 0xbf58476d1ce4e5b9ULL;
    x ^= x >> 27;
    x *= 0x94d049bb133111ebULL;
    x ^= x >> 31;
    return x;
}
// ── blast / throughput ────────────────────────────────────────────────────────
static void test_eventstream_blast(ETCS::MemoryArena& arena, int TOTAL = 100000)
{
    section("EventStream: Blast — Multi-Producer Async (Pinned)");
    constexpr int PRODUCERS = 4;
    const int     CORES     = get_core_count();
    int PER_PRODUCER    = TOTAL / PRODUCERS;
    int EFFECTIVE_TOTAL = PER_PRODUCER * PRODUCERS;
    TestEventStream stream;
    WIRE_CONTEXT();
    stream.start(arena, PRODUCERS);
    stream.start_workers(CORES > 2 ? CORES - 2 : 2);
    std::atomic<bool> start_flag{false};
    std::vector<std::thread> producers;
    for (int p = 0; p < PRODUCERS; ++p)
    {
        producers.emplace_back([&, p]() {
            pin_thread((p + 1) % CORES);
            while (!start_flag.load(std::memory_order_acquire));
            for (int i = 0; i < PER_PRODUCER; ++i)
            {
                TestEventKind kind = (i % 4 == 0) ? TestEventKind::Inline : TestEventKind::Async;
                int tag = -1;
                if (i % 10 == 0)
                {
                    // Generate a uniquely scattered hash per producer per loop iteration
                    uint64_t h = fast_hash(p * 100000000ULL + i);
                    tag = static_cast<int>(h % 64);
                }
                TestInEvent evt{kind, tag, p * PER_PRODUCER + i};
                stream.enqueue(evt);
            }
        });
    }
    auto t0 = std::chrono::high_resolution_clock::now();
    start_flag.store(true, std::memory_order_release);
    // FIX: Spin strictly on the condition to capture the exact completion time
    while (stream.emit_count_.load(std::memory_order_acquire) < (size_t)EFFECTIVE_TOTAL)
    {
        // Active backoff or simply spin
        int backoff = 0;
        ETCS::LMAXSequentialSharedPage::busySpin(backoff);
    }

    // Timer stops BEFORE the massive vector copy and OS thread teardowns
    auto t1 = std::chrono::high_resolution_clock::now();
    // Now it is safe to pull the data and teardown
    auto emitted = stream.wait_for_emits(EFFECTIVE_TOTAL, 10000);
    for (auto& t : producers) t.join();
    stream.stop_workers();
    stream.stop();

    auto   ns  = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
    double ops = ns > 0 ? (static_cast<double>(EFFECTIVE_TOTAL) / ns) * 1e9 : 0.0;
    double ms  = ns / 1000000.0;

    TEST("blast: all events emitted " , emitted.size() == static_cast<size_t>(EFFECTIVE_TOTAL));
    std::cout << "  [INFO] " << formatWithCommas(EFFECTIVE_TOTAL)
              << " events in " << std::fixed << std::setprecision(3) << ms
              << "ms — " << formatWithCommas(static_cast<long long>(ops)) << " ops/sec\n";
}
// ── direction: checked, and it is deliberately not there ──────────────────────
//
// A produce/consume pair IS directional at the transport -- MirrorBuffer
// carries is_producer_ and hands back isProducer() -- so the obvious question
// is whether the ORDERING mask is directional too. It is not, in either scope,
// and the two places that decide it say so plainly:
//
//   Entity.h streamPairMask<P,C>     return P::readTagClosure() | C::readTagClosure();
//   DynamicLoader.h LoaderStream     m = GetModuleBit(a) | GetModuleBit(b);
//
// A symmetric OR both times. And Entity.h:1873 hands the SAME mask to both
// halves -- `producer.setPairMasks(pair_tag, pair_mod); consumer.setPairMasks(
// pair_tag, pair_mod);` -- so the two ends of one stream are indistinguishable
// to the reorder buffer.
//
// THAT IS THE CORRECT ANSWER, and it is worth pinning down rather than leaving
// as an accident nobody has looked at. The mask says what an operation TOUCHES.
// A P->C call runs P's produce body and C's consume body; so does a C->P call.
// Both touch both closures, so both must serialize against anything else
// touching either. Making the mask directional would say that a P->C stream and
// a C->P stream are independent, which is false -- they run the same two bodies
// -- and the reorder buffer would let them commit out of order against each
// other.
//
// So the test below asserts the symmetry rather than the direction. It is a
// regression guard on a property that is easy to "optimise" away: someone
// looking at resolvePairModuleMask's cache, which IS keyed directionally
// ("a\x1fb" and "b\x1fa" are two entries for one value), could reasonably
// conclude the value should differ too.

static bool masks_equal(const ETCS::TagMask& a, const ETCS::TagMask& b)
{
    for (size_t i = 0; i < ETCS::TAG_WORDS; ++i) if (a.w[i] != b.w[i]) return false;
    return true;
}

// Two types that exist only to be template arguments. streamPairMask reads
// four statics off each -- TAG_MASK and readTagClosure() -- and nothing else,
// so these are never constructed and never need to be.
struct DirProbeP : virtual public ETCS::Entity { WIRE_TYPE_IDENTITY(DirProbeP); };
struct DirProbeC : virtual public ETCS::Entity { WIRE_TYPE_IDENTITY(DirProbeC); };

static void test_eventstream_pair_mask_symmetry()
{
    section("EventStream: Pair Mask Direction (produce/consume symmetry)");

    // Empty TAG_MASK is the foreign-type case and fails shut to all(), which is
    // symmetric for an uninteresting reason. Give them real bits first, the way
    // a module's own ETCS_TAG_DECLARE would.
    DirProbeP::TAG_MASK = ETCS::TagMask::bit(1);
    DirProbeC::TAG_MASK = ETCS::TagMask::bit(4);
    // And distinct closures, so an implementation that returned only one side's
    // would be caught rather than passing by coincidence.
    DirProbeP::TAG_CLOSURE_W[0].store((1ull << 1) | (1ull << 2));
    DirProbeC::TAG_CLOSURE_W[0].store((1ull << 4) | (1ull << 7));
    DirProbeP::TAG_CLOSURE_GEN.fetch_add(1);
    DirProbeC::TAG_CLOSURE_GEN.fetch_add(1);

    const ETCS::TagMask pc = ETCS::streamPairMask<DirProbeP, DirProbeC>();
    const ETCS::TagMask cp = ETCS::streamPairMask<DirProbeC, DirProbeP>();

    TEST("pair mask is direction-symmetric", masks_equal(pc, cp));
    TEST("pair mask carries the producer's closure", pc.test(1) && pc.test(2));
    TEST("pair mask carries the consumer's closure", pc.test(4) && pc.test(7));
    // The union and nothing else: a mask wider than the two closures would
    // serialize against types neither side reaches.
    TEST("pair mask is exactly the union",
         masks_equal(pc, DirProbeP::readTagClosure() | DirProbeC::readTagClosure()));

    std::cout << "  " << FG_DIM
              << "P->C and C->P are the same mask, deliberately: both directions run\n"
              << "  both bodies, so both touch both closures. The bursts below drive\n"
              << "  each direction anyway -- if the numbers ever diverge, something\n"
              << "  other than the mask has become directional." << RESET << "\n";
}

// ── capability edges: the masks the tree actually produces ────────────────────
//
// EVERYTHING ABOVE THIS LINE TESTS THE WORST CASE, and says so in the banner.
// test_eventstream_blast draws its tag from `fast_hash(...) % 64` -- a uniform
// spray over the full 64-bit word, on one in ten events, with nothing tying an
// event's mask to the type that emitted it. That is a fine adversarial probe
// and a poor model of the workload, in three specific ways:
//
//   1. THE BIT SPACE IS NOT 64. A mask bit is a slot in ONE MODULE's tag
//      space, and a module declares its tags in one string. The widest in the
//      tree today declares eight:
//
//        RenderProvider   8    Instance Surface ImageSurface PolygonDrawable2D
//                              CompositeDrawable2D Scene3D Camera3D TextLabel
//        NetworkProvider  8    HttpServer ConnectionManager HTTPParser ...
//        PaintProvider    6    ChessProvider 3   ForumWebsiteProvider 3
//        WindowProvider   1    LayoutProvider 1  DatabaseProvider 1
//
//      Random-over-64 therefore spends most of its entropy on bits no module
//      owns, which UNDERSTATES collision: two RenderProvider events pick from
//      eight slots, not sixty-four.
//
//   2. THE MASK IS A PROPERTY OF THE TYPE, NOT OF THE EVENT. A type's mask is
//      its own bit OR the bits of every type in its module it reaches --
//      TAG_CLOSURE, accumulated once at the typed acquisition sites and
//      monotonic thereafter. It does not vary per event. So the real
//      distribution is a handful of FIXED edge sets, drawn from repeatedly,
//      not a fresh random draw each time.
//
//   3. SELF-COLLISION IS THE COMMON CASE, and it is a guarantee rather than an
//      accident: two operations on one type must serialize. Random masks make
//      that rare; reality makes it the default.
//
// So this section asks the modules themselves what their bit spaces are, one
// type at a time, and bursts against each -- LMAXTesterLoader's many-producer
// pattern (tight write loop, periodic storm pause), measured the same way.

struct ModuleShape
{
    std::string              name;
    std::vector<std::string> tags;    // declaration order; index IS the bit
};

/*
 * Ask every built module for its ordered tag list.
 *
 * dlopen + Module::getTags(), which is the loader's own discovery path
 * (Bundles.h) -- the tags come back in the order ETCS_MODULE_EXPORT_MAIN
 * declared them, which is exactly the order RegisterTagBitIndex assigns bits
 * in. Position in this vector is the bit position, by construction rather
 * than by a table kept in step by hand.
 *
 * NOT registered with the loader. This calls the module's discovery function
 * and nothing else -- no RegisterDynamicLoader, no entities, no ownership. A
 * module that fails to open is skipped with a line, because this section is a
 * measurement and must never be the reason the suite fails.
 */
static std::vector<ModuleShape> discover_module_shapes()
{
    std::vector<ModuleShape> out;
    std::vector<std::string> skipped;
    namespace fs = std::filesystem;

    // getBinDir() is private on Module; recover it from a probe Module's own
    // filename, which is getBinDir() + name + DL_EXTENSION.
    const std::string probe_name = "__etcs_probe__";
    ETCS::Module probe(probe_name);
    const std::string fn = probe.getFilename();
    const std::string suffix = probe_name + DL_EXTENSION;
    if (fn.size() <= suffix.size()) return out;
    const std::string bin = fn.substr(0, fn.size() - suffix.size());

    std::error_code ec;
    std::vector<fs::path> sos;
    for (auto& e : fs::directory_iterator(bin, ec))
        if (!ec && e.path().extension() == DL_EXTENSION) sos.push_back(e.path());
    if (ec) return out;
    std::sort(sos.begin(), sos.end());

    /*
     * STDOUT IS PARKED FOR THE DURATION, and it is not squeamishness: dlopen
     * runs each module's static init, which does its own ThreadPool spin-up
     * and its full loader/module manifest comparison -- around ninety lines
     * per module, eight modules, ahead of a table that is twenty. The noise is
     * correct and belongs in a module load; it does not belong in the middle
     * of a measurement.
     *
     * fd-level rather than std::cout.rdbuf(), because the logs come from
     * inside the freshly-mapped DSOs and go to THEIR streams, not this
     * translation unit's. Restored unconditionally below, including on the
     * throwing path.
     */
    int saved_stdout = dup(STDOUT_FILENO);
    int devnull      = open("/dev/null", O_WRONLY);
    if (devnull >= 0) { std::fflush(stdout); dup2(devnull, STDOUT_FILENO); }

    for (const auto& so : sos)
    {
        const std::string name = so.stem().string();
        void* h = dlopen(so.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (!h) { skipped.push_back(name + ": " + dlerror()); continue; }
        ETCS::Module mod(name);
        mod.adoptLibrary(h);
        try
        {
            const auto& tags = mod.getTags();
            if (tags.empty()) continue;
            ModuleShape shape;
            shape.name = name;
            for (const auto& t : tags) shape.tags.push_back(t.toString());
            out.push_back(std::move(shape));
        }
        catch (const std::exception& e)
        {
            skipped.push_back(name + ": " + e.what());
        }
    }

    std::fflush(stdout);
    if (devnull >= 0) { dup2(saved_stdout, STDOUT_FILENO); close(devnull); }
    if (saved_stdout >= 0) close(saved_stdout);

    for (const auto& sk : skipped)
        std::cout << "  " << FG_YELLOW << "[skip]" << RESET << " " << sk << "\n";
    return out;
}

/*
 * The closure for one type, DERIVED ONCE AND STABLE.
 *
 * A real TAG_CLOSURE is discovered at runtime from which <T>s appear in which
 * members, so this cannot be read out of a .so -- but its SHAPE is what
 * matters here and its shape is knowable: a few edges within the module's own
 * bit space, fixed for the life of the process.
 *
 * Hashed from the module and type name rather than drawn from a generator, so
 * a given type gets the same edges on every run and two runs are comparable.
 * The own bit is always present -- that is not an edge, it is identity.
 *
 * Roughly a third of the module's other types, which is what the tree looks
 * like: Scene3D reaches Camera3D, PaintInput reaches the document, the tool
 * and the palette, and most leaves reach nothing at all.
 */
static uint32_t closure_for(const ModuleShape& m, size_t type_index)
{
    uint32_t bits = uint32_t(1) << type_index;
    for (size_t j = 0; j < m.tags.size() && j < 32; ++j)
    {
        if (j == type_index) continue;
        const uint64_t h = fast_hash(std::hash<std::string>{}(m.name + ":" + m.tags[type_index])
                                     ^ (fast_hash(j + 1) * 0x9e3779b97f4a7c15ULL));
        if ((h % 3) == 0) bits |= (uint32_t(1) << j);
    }
    return bits;
}

static std::string bits_to_string(uint32_t bits, size_t width)
{
    std::string s;
    for (size_t i = 0; i < width && i < 32; ++i) s += (bits & (uint32_t(1) << i)) ? '#' : '.';
    return s;
}

struct BurstResult
{
    double   ops   = 0.0;
    double   ms    = 0.0;
    uint64_t total = 0;
    bool     ok    = false;
};

/*
 * One burst against one fixed mask distribution.
 *
 * `draw` hands back the (tag_bit, closure) an event should carry, given the
 * producer index and its loop counter -- so the caller decides whether this is
 * a single type hammering its own edge set, or a module's whole type list
 * mixed together. Everything else about the burst is identical between them,
 * which is the point: the only variable is the mask.
 *
 * The burst pattern is LMAXTesterLoader's, scaled to run ~40 times in the time
 * one blast takes: a tight enqueue loop interrupted by a storm pause every
 * 20k-40k events, so the ordering thread sees both saturation and drain rather
 * than one steady rate. Sleeps are INSIDE the timed region here, unlike the
 * LMAX burst tests which bracket them out -- this is measuring the stream's
 * throughput under a bursty arrival process, not the producers' write cost.
 */
static BurstResult run_capability_burst(
    ETCS::MemoryArena& arena,
    int TOTAL,
    const std::function<void(int, int, int&, uint32_t&)>& draw)
{
    constexpr int PRODUCERS = 4;
    const int     CORES     = get_core_count();
    const int     PER       = TOTAL / PRODUCERS;
    const int     EFFECTIVE = PER * PRODUCERS;

    TestEventStream stream(static_cast<size_t>(EFFECTIVE) + 1024);
    WIRE_CONTEXT();
    stream.start(arena, PRODUCERS);
    stream.start_workers(CORES > 2 ? CORES - 2 : 2);

    std::atomic<bool> start_flag{false};
    std::vector<std::thread> producers;
    for (int p = 0; p < PRODUCERS; ++p)
    {
        producers.emplace_back([&, p]() {
            pin_thread((p + 1) % CORES);
            std::mt19937 gen(static_cast<uint32_t>(fast_hash(p + 1)));
            std::uniform_int_distribution<> pause_dist(20000, 40000);
            std::uniform_int_distribution<> sleep_dist(2, 8);
            const int pause_modulo = pause_dist(gen);

            while (!start_flag.load(std::memory_order_acquire));
            for (int i = 0; i < PER; ++i)
            {
                if (i && (i % pause_modulo) == 0)
                    std::this_thread::sleep_for(std::chrono::milliseconds(sleep_dist(gen)));

                int      tag_bit = -1;
                uint32_t closure = 0;
                draw(p, i, tag_bit, closure);

                // Three quarters async, so most events take the gap buffer and
                // the worker pool rather than committing inline -- the path the
                // mask actually gates.
                TestEventKind kind = (i % 4 == 0) ? TestEventKind::Inline
                                                  : TestEventKind::Async;
                TestInEvent evt{kind, tag_bit, p * PER + i, closure};
                stream.enqueue(evt);
            }
        });
    }

    auto t0 = std::chrono::high_resolution_clock::now();
    start_flag.store(true, std::memory_order_release);

    // Bounded, unlike the blast's bare spin: this runs ~40 times and a single
    // stall would hang the suite with no indication of which mask did it.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    bool timed_out = false;
    while (stream.emit_count_.load(std::memory_order_acquire) < (size_t)EFFECTIVE)
    {
        if (std::chrono::steady_clock::now() > deadline) { timed_out = true; break; }
        int backoff = 0;
        ETCS::LMAXSequentialSharedPage::busySpin(backoff);
    }
    auto t1 = std::chrono::high_resolution_clock::now();

    const size_t emitted = stream.emit_count_.load(std::memory_order_acquire);
    for (auto& t : producers) t.join();
    stream.stop_workers();
    stream.stop();

    BurstResult r;
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
    r.total = static_cast<uint64_t>(EFFECTIVE);
    r.ms    = ns / 1000000.0;
    r.ops   = ns > 0 ? (static_cast<double>(EFFECTIVE) / ns) * 1e9 : 0.0;
    r.ok    = !timed_out && emitted >= static_cast<size_t>(EFFECTIVE);
    return r;
}

/*
 * Median of three, for the reason LMAXTesterLoader's own median_ring_pass
 * exists: a single burst on a shared machine is not a measurement.
 *
 * Discovered the hard way -- the random-over-64 control came in at 685k on one
 * run and 171k on the next, same binary, same arguments. Any A/B read off
 * single samples at that variance is noise with a ratio printed under it, and
 * the first version of this section reported exactly that.
 *
 * Median rather than mean: the failure mode here is a run that got descheduled
 * behind a build, which is a one-sided outlier. A mean carries it; a median
 * discards it.
 */
static BurstResult median_burst(ETCS::MemoryArena& arena, int TOTAL,
                                const std::function<void(int, int, int&, uint32_t&)>& draw,
                                int samples = 3)
{
    std::vector<BurstResult> rs;
    for (int i = 0; i < samples; ++i)
    {
        BurstResult r = run_capability_burst(arena, TOTAL, draw);
        if (!r.ok) return r;             // a stall is the answer; stop sampling
        rs.push_back(r);
    }
    std::sort(rs.begin(), rs.end(),
              [](const BurstResult& a, const BurstResult& b) { return a.ops < b.ops; });
    return rs[rs.size() / 2];
}

// ── the deterministic instrument ──────────────────────────────────────────────
/*
 * BLOCKED ADMISSIONS -- what the throughput A/B above was a shadow of.
 *
 * The question a mask policy has to answer is not "how fast did it go", it is
 * "how often did this mask stop something from starting". The reorder buffer
 * decides exactly that, once per arrival, in one line of ordering_loop:
 *
 *     reorder_.acquire(in_seq_, mask);
 *     if (!reorder_.blocked(in_seq_)) launch_slot(in_seq_);
 *
 * Counting that branch is the measurement. Throughput reaches the same fact
 * only through the scheduler, and the scheduler on a shared machine answers a
 * different question every run -- this suite's own fixed control came in at
 * 685k, 171k and 374k ops/sec on three consecutive runs of one binary, which
 * is a 4x swing on a number that was not supposed to move at all.
 *
 * So: the REAL GapReorderBuffer, the real acquire/blocked/service, no threads,
 * no clock, no worker pool. Identical traffic replayed under two mask policies.
 * The result is an integer, and it is the same integer on every machine.
 *
 * Two numbers, because they say different things:
 *   blocked     -- arrivals refused. The headline.
 *   collisions  -- summed popcount of blocked_by_, so how many live slots each
 *                  refused arrival met. Width, where blocked is only incidence:
 *                  a mask can block as often and depend on twice as much.
 */
struct AdmissionCount
{
    uint64_t events     = 0;
    uint64_t blocked    = 0;
    uint64_t collisions = 0;
    double pct()   const { return events ? (100.0 * blocked) / events : 0.0; }
    // Blockers per ARRIVAL, not per refusal: the width of the average event's
    // interference, which is the quantity the mask policy actually sets.
    double width() const { return events ? double(collisions) / events : 0.0; }
};

/*
 * WINDOW is how many events are held in flight before the oldest is completed
 * -- the concurrency the ordering thread is given, and the only free parameter
 * here. SWEPT rather than fixed, and that turned out to matter: at a window of
 * 32 both policies refuse ~100% of arrivals and the comparison says nothing.
 * Not a flaw in either mask -- a module declares 3 to 8 types, so past a
 * handful of live slots SOMETHING always matches, whatever the policy. The
 * incidence is a step function and the interesting part is where the step is.
 *
 * Bounded by GAP_DEPTH, above which refusal is slot exhaustion rather than mask
 * blocking (see the REORDER STALL report's own note).
 *
 * Completing in ARRIVAL ORDER is what makes this deterministic. It also makes
 * the oldest live slot always launchable -- everything that could block it is
 * older, and everything older has already committed -- which is handled rather
 * than assumed below.
 */
static const size_t ADMISSION_WINDOWS[] = { 2, 4, 8, 16, 32 };
static constexpr size_t ADMISSION_WINDOW_N =
    sizeof(ADMISSION_WINDOWS) / sizeof(ADMISSION_WINDOWS[0]);
// The window the per-module width column is taken at -- the widest, where every
// policy has as much live traffic to collide with as the buffer will hold.
static constexpr size_t ADMISSION_WIDTH_WINDOW = 32;

static ETCS::TagMask mask_from_bits(uint64_t bits)
{
    ETCS::TagMask m;
    for (size_t b = 0; b < 64; ++b)
        if (bits & (uint64_t(1) << b)) m |= ETCS::TagMask::bit(b);
    return m;
}

static AdmissionCount replay_admissions(size_t N, size_t window,
                                        const std::function<uint64_t(size_t)>& draw)
{
    ETCS::GapReorderBuffer rb;
    std::deque<uint64_t>   live;
    AdmissionCount         out;
    int                    dummy_result = 0;

    auto launch = [&rb](ETCS::GapSlot& s) { rb.mark_running(s.sequence); };
    auto emit   = [](ETCS::GapSlot&) {};
    auto retire = [&](uint64_t seq) {
        // Pending here would mean the oldest slot was still blocked, which the
        // arrival-order argument above says cannot happen. Launched rather than
        // asserted on: a wrong model that quietly deadlocks is worse than one
        // that keeps going and reports a number the comparison still holds for.
        if (rb.slots_[seq % ETCS::GAP_DEPTH].status == ETCS::GapSlot::Status::Pending)
            rb.mark_running(seq);
        rb.complete(seq, &dummy_result);
        rb.service(launch, emit);
    };

    for (size_t i = 0; i < N; ++i)
    {
        rb.acquire(i, mask_from_bits(draw(i)));
        ++out.events;
        const uint64_t bb = rb.blocked_by_[i % ETCS::GAP_DEPTH];
        if (bb)
        {
            ++out.blocked;
            out.collisions += static_cast<uint64_t>(__builtin_popcountll(bb));
        }
        else rb.mark_running(i);      // admitted: launch_slot's own first act

        live.push_back(i);
        if (live.size() >= window)
        {
            const uint64_t oldest = live.front();
            live.pop_front();
            retire(oldest);
        }
    }
    while (!live.empty()) { const uint64_t s = live.front(); live.pop_front(); retire(s); }
    return out;
}

/*
 * The A/B, run on the counts. Same draws for both policies -- one traffic
 * sequence generated up front and indexed twice, so the ONLY difference
 * between the two columns is which mask that draw resolves to.
 */
struct PolicyTotals
{
    // Indexed by ADMISSION_WINDOWS. width() is read at ADMISSION_WIDTH_WINDOW.
    AdmissionCount per_type[ADMISSION_WINDOW_N];
    AdmissionCount per_edge[ADMISSION_WINDOW_N];
    size_t         width_i = 0;
};

static PolicyTotals test_admission_pressure(const std::vector<ModuleShape>& shapes,
                                            size_t N = 200000)
{
    section("EventStream: Blocked Admissions — same traffic, two mask policies");
    std::cout << "  " << FG_DIM
              << "Deterministic: the real GapReorderBuffer, arrival-order completion,\n"
                 "  no threads and no clock. These integers are the same on every machine."
              << RESET << "\n\n";

    PolicyTotals tot;
    for (size_t w = 0; w < ADMISSION_WINDOW_N; ++w)
        if (ADMISSION_WINDOWS[w] == ADMISSION_WIDTH_WINDOW) tot.width_i = w;

    constexpr int FUNCS_PER_TYPE = 6;

    /*
     * WIDTH per module, at the widest window: how many live slots the average
     * arrival's mask met. Incidence is reported in the sweep below instead,
     * because incidence saturates and width does not -- a module of 3 types
     * refuses nearly every arrival under EITHER policy once 32 events are live,
     * while still interfering with twice as many of them under one of them.
     */
    std::cout << "  module                    events   live slots met per arrival\n";
    std::cout << "                                     per-type union   per-func edges\n";
    std::cout << "  ------------------------+--------+----------------+---------------\n";

    for (const auto& m : shapes)
    {
        const size_t N_TYPES = m.tags.size();
        if (N_TYPES < 2) continue;              // no mix, so no policy difference

        std::vector<uint64_t> closures(N_TYPES);
        for (size_t i = 0; i < N_TYPES; ++i) closures[i] = closure_for(m, i);

        // Identical construction to the throughput A/B's, so the two sections
        // are describing the same two policies rather than two similar ones.
        std::vector<uint64_t> edge(N_TYPES * FUNCS_PER_TYPE, 0);
        for (size_t t = 0; t < N_TYPES; ++t)
            for (int f = 0; f < FUNCS_PER_TYPE; ++f)
            {
                uint64_t em = uint64_t(1) << t;
                const uint64_t h = fast_hash(
                    std::hash<std::string>{}(m.name + ":" + m.tags[t]) * 1315423911ull
                    + static_cast<uint64_t>(f) + 1ull);
                if ((h % 3) == 0)
                {
                    const size_t other = static_cast<size_t>((h >> 8) % N_TYPES);
                    if (other != t) em |= (uint64_t(1) << other);
                }
                edge[t * FUNCS_PER_TYPE + f] = em;
            }

        // ONE traffic sequence, indexed by both policies at every window.
        std::vector<uint32_t> seq_t(N), seq_f(N);
        for (size_t i = 0; i < N; ++i)
        {
            const uint64_t h = fast_hash(static_cast<uint64_t>(i) * 7919ull + 1ull);
            seq_t[i] = static_cast<uint32_t>(h % N_TYPES);
            seq_f[i] = static_cast<uint32_t>((h >> 12) % FUNCS_PER_TYPE);
        }
        auto draw_type = [&](size_t i) { return closures[seq_t[i]]; };
        auto draw_edge = [&](size_t i)
        { return edge[seq_t[i] * FUNCS_PER_TYPE + seq_f[i]]; };

        AdmissionCount row_a, row_b;
        for (size_t w = 0; w < ADMISSION_WINDOW_N; ++w)
        {
            const AdmissionCount a = replay_admissions(N, ADMISSION_WINDOWS[w], draw_type);
            const AdmissionCount b = replay_admissions(N, ADMISSION_WINDOWS[w], draw_edge);
            tot.per_type[w].events += a.events; tot.per_type[w].blocked += a.blocked;
            tot.per_type[w].collisions += a.collisions;
            tot.per_edge[w].events += b.events; tot.per_edge[w].blocked += b.blocked;
            tot.per_edge[w].collisions += b.collisions;
            if (w == tot.width_i) { row_a = a; row_b = b; }
        }

        std::string label = "  " + m.name;
        label.resize(24, ' ');
        std::cout << "  " << FG_CYAN << label << RESET
                  << " " << std::setw(8) << formatWithCommas((long long)row_a.events)
                  << "  " << std::setw(14) << std::fixed << std::setprecision(2)
                  << row_a.width()
                  << "  " << FG_GREEN << std::setw(14) << row_b.width() << RESET
                  << (row_b.width() < row_a.width()
                      ? "   " + FG_DIM
                        + std::to_string(row_a.width() / (row_b.width() > 0 ? row_b.width() : 1)).substr(0, 4)
                        + "x narrower" + RESET
                      : "")
                  << "\n";
    }

    /*
     * THE SWEEP. How much traffic can be in flight before the mask starts
     * refusing it -- which is the whole practical question, and the one number
     * a throughput ratio was standing in for.
     */
    std::cout << "\n  refused on arrival, by in-flight window:\n    "
              << std::setw(22) << " ";
    for (size_t w = 0; w < ADMISSION_WINDOW_N; ++w)
        std::cout << std::setw(9) << ADMISSION_WINDOWS[w];
    std::cout << "\n    " << std::setw(22) << std::left << "per-type union (today)"
              << std::right;
    for (size_t w = 0; w < ADMISSION_WINDOW_N; ++w)
        std::cout << std::setw(8) << std::fixed << std::setprecision(1)
                  << tot.per_type[w].pct() << "%";
    std::cout << "\n    " << FG_GREEN << std::setw(22) << std::left
              << "per-function edges" << std::right;
    for (size_t w = 0; w < ADMISSION_WINDOW_N; ++w)
        std::cout << std::setw(8) << tot.per_edge[w].pct() << "%";
    std::cout << RESET << "\n";

    const size_t wi = tot.width_i;
    if (tot.per_type[wi].events)
        std::cout << "\n  " << FG_CYAN << "TOTAL" << RESET << "  over "
                  << formatWithCommas((long long)tot.per_type[wi].events)
                  << " events of identical traffic, window "
                  << ADMISSION_WIDTH_WINDOW << ":\n"
                  << "    per-type union (today)   " << std::setprecision(2)
                  << tot.per_type[wi].width() << " live slots met per arrival\n"
                  << "    per-function edges       "
                  << tot.per_edge[wi].width() << " live slots met per arrival\n"
                  << "  " << FG_DIM
                  << "Same own bit in both, so that difference is the union's surplus\n"
                  << "  and nothing else." << RESET << "\n\n";

    /*
     * Asserted, unlike anything in the throughput section, because this one CAN
     * be: the union CONTAINS every edge set it stands for, so it can only meet
     * more live slots, never fewer -- at every window, not on average. A run
     * where it did would mean the two policies were not being fed the same
     * traffic, which is the one way this instrument can lie.
     */
    bool monotone = true;
    for (size_t w = 0; w < ADMISSION_WINDOW_N; ++w)
        if (tot.per_edge[w].collisions > tot.per_type[w].collisions ||
            tot.per_edge[w].blocked    > tot.per_type[w].blocked) monotone = false;
    TEST("per-function edges never block more than the per-type union", monotone);
    return tot;
}

// ── the section ───────────────────────────────────────────────────────────────

static void test_eventstream_capability_edges(ETCS::MemoryArena& arena,
                                              int PER_TYPE = 120000)
{
    section("EventStream: Capability Edges — Real Module Masks, Bursty Producers");

    const auto shapes = discover_module_shapes();
    if (shapes.empty())
    {
        std::cout << "  " << FG_YELLOW << "[skip]" << RESET
                  << " no modules discoverable -- build them with `ace make modules`.\n";
        return;
    }

    size_t total_types = 0;
    for (const auto& m : shapes) total_types += m.tags.size();
    std::cout << "  " << shapes.size() << " modules, " << total_types
              << " contract types. Bit space per module is its own tag count,\n"
              << "  " << FG_YELLOW << "not 64" << RESET
              << " -- which is the whole difference from the blast above.\n\n";

    /*
     * THE DETERMINISTIC A/B FIRST, and it is the one to read. Everything below
     * it is throughput on a shared machine; this is a count of the decisions
     * the masks actually made. Run from here rather than from main() so it
     * reuses the dlopen sweep above -- same modules, same tag lists, same
     * closures, so the two sections describe one experiment.
     */
    const PolicyTotals adm = test_admission_pressure(shapes);

    section("EventStream: Capability Edges — Real Module Masks, Bursty Producers");
    std::cout << "  " << FG_DIM
              << "Throughput, and read it as ADVISORY -- see the note under the table."
              << RESET << "\n\n";

    std::cout << "  module / type              bits  edges     ops/sec        ms\n";
    std::cout << "  -------------------------+-----+---------+-------------+--------\n";

    double best = 0.0, worst = 1e18;
    std::string best_name, worst_name;
    // Counted rather than asserted per row: a TEST line between every two
    // rows makes the table unreadable, and "which mask stalled" is already on
    // the row itself as [INCOMPLETE].
    int incomplete = 0;
    double pair_fwd = 0.0, pair_rev = 0.0;
    double per_edge_total = 0.0, per_type_total = 0.0;
    int    per_edge_n = 0,       per_type_n = 0;

    for (const auto& m : shapes)
    {
        const size_t N = m.tags.size();
        std::cout << "  " << FG_CYAN << m.name << RESET
                  << "  " << FG_DIM << "(" << N << " tag" << (N == 1 ? "" : "s") << ")"
                  << RESET << "\n";

        // ── one type at a time: every event carries THAT type's mask ──────────
        //
        // The self-collision case, and the one the reorder buffer exists for.
        // Every event here blocks on every other, so this is the floor: what the
        // stream does when a single hot type is saturating it.
        for (size_t i = 0; i < N; ++i)
        {
            const uint32_t closure = closure_for(m, i);
            const int      bit     = static_cast<int>(i);

            BurstResult r = median_burst(
                arena, PER_TYPE,
                [bit, closure](int, int, int& tb, uint32_t& cl) { tb = bit; cl = closure; });

            std::string label = "  " + m.tags[i];
            if (label.size() > 25) label = label.substr(0, 25);
            label.resize(25, ' ');

            std::cout << "  " << label
                      << " " << std::setw(4) << N
                      << "  " << std::setw(8) << bits_to_string(closure, N)
                      << "  " << std::setw(11) << formatWithCommas(static_cast<long long>(r.ops))
                      << "  " << std::setw(7) << std::fixed << std::setprecision(1) << r.ms
                      << (r.ok ? "" : (FG_RED + "  [INCOMPLETE]" + RESET)) << "\n";

            if (!r.ok) ++incomplete;
            if (r.ops > best)  { best  = r.ops; best_name  = m.name + "::" + m.tags[i]; }
            if (r.ops < worst) { worst = r.ops; worst_name = m.name + "::" + m.tags[i]; }
        }

        // ── the whole module mixed: traffic across every type it declares ─────
        //
        // Cross-type traffic over one stream boundary, which is what a module
        // under load actually looks like -- several types live at once, each
        // carrying its own fixed edge set, colliding only where they genuinely
        // reach each other. A module of one type has no mix to run.
        if (N > 1)
        {
            std::vector<uint32_t> closures;
            for (size_t i = 0; i < N; ++i) closures.push_back(closure_for(m, i));

            BurstResult r = median_burst(
                arena, PER_TYPE,
                [&closures, N](int p, int i, int& tb, uint32_t& cl) {
                    const size_t t = fast_hash(static_cast<uint64_t>(p) * 1000003ull + i) % N;
                    tb = static_cast<int>(t);
                    cl = closures[t];
                });

            std::string mixed_label = "  <all types mixed>";
            mixed_label.resize(25, ' ');
            /*
             * ── THE A/B, AND THE REASON THIS SECTION EXISTS ──────────────
             *
             * Same module, same traffic, two mask POLICIES.
             *
             * The row above is what the tree does today: every event a type
             * emits carries that type's own bit OR'd with its whole
             * TAG_CLOSURE -- the union of everything any member of it has
             * ever reached, accumulated for the life of the process and
             * never narrowed. `SetPosition` and `Render` emit the same mask.
             *
             * The row below is the per-work-function grain: an invocation
             * carries its own type's bit plus only the edges THAT FUNCTION
             * touches. Modelled as most functions touching nothing at all,
             * which is what the tree looks like -- a setter reaches nobody,
             * and the two or three that walk another type's graph are the
             * ones that pay.
             *
             * The own bit is in both, always: two operations on one type
             * must serialize whatever either reaches, and that is the
             * guarantee rather than an edge that could be absent. So the
             * difference between these two numbers is exactly the width the
             * union adds over the dependency, and nothing else.
             */
            {
                const int FUNCS_PER_TYPE = 6;
                std::vector<uint32_t> edge(N * FUNCS_PER_TYPE, 0);
                for (size_t t = 0; t < N; ++t)
                    for (int f = 0; f < FUNCS_PER_TYPE; ++f)
                    {
                        uint32_t em = uint32_t(1) << t;     // identity, always
                        const uint64_t h = fast_hash(
                            std::hash<std::string>{}(m.name + ":" + m.tags[t]) * 1315423911ull
                            + static_cast<uint64_t>(f) + 1ull);
                        // One function in three reaches anything at all, and
                        // when it does it reaches one other type -- an EDGE,
                        // not a closure over everything downstream of it.
                        if ((h % 3) == 0 && N > 1)
                        {
                            size_t other = static_cast<size_t>((h >> 8) % N);
                            if (other != t) em |= (uint32_t(1) << other);
                        }
                        edge[t * FUNCS_PER_TYPE + f] = em;
                    }

                BurstResult pe = median_burst(
                    arena, PER_TYPE,
                    [&edge, N, FUNCS_PER_TYPE](int p, int i, int& tb, uint32_t& cl) {
                        const uint64_t h = fast_hash(static_cast<uint64_t>(p) * 7919ull + i);
                        const size_t t = h % N;
                        const int    f = static_cast<int>((h >> 12) % FUNCS_PER_TYPE);
                        tb = static_cast<int>(t);
                        cl = edge[t * FUNCS_PER_TYPE + f];
                    });

                std::string pl = "  <per-edge, per-func>";
                pl.resize(25, ' ');
                std::cout << "  " << FG_GREEN << pl << RESET
                          << " " << std::setw(4) << N
                          << "  " << std::setw(8) << "edges"
                          << "  " << std::setw(11) << formatWithCommas(static_cast<long long>(pe.ops))
                          << "  " << std::setw(7) << std::fixed << std::setprecision(1) << pe.ms
                          << (pe.ok ? "" : (FG_RED + "  [INCOMPLETE]" + RESET)) << "\n";
                if (!pe.ok) ++incomplete;
                per_edge_total += pe.ops; ++per_edge_n;
            }

            std::cout << "  " << FG_MAGENTA << mixed_label << RESET
                      << " " << std::setw(4) << N
                      << "  " << std::setw(8) << "mixed"
                      << "  " << std::setw(11) << formatWithCommas(static_cast<long long>(r.ops))
                      << "  " << std::setw(7) << std::fixed << std::setprecision(1) << r.ms
                      << (r.ok ? "" : (FG_RED + "  [INCOMPLETE]" + RESET)) << "\n";
            if (!r.ok) ++incomplete;
            per_type_total += r.ops; ++per_type_n;

            /*
             * THE PAIR EDGE, BOTH WAYS. A stream call's mask is the union of
             * the two closures (streamPairMask), so this is what a
             * `p.Produce() -> c.Consume()` line actually puts on the ordering
             * thread -- and running it in both directions is how the symmetry
             * asserted above shows up as a number rather than an assertion.
             *
             * Types 0 and 1 rather than a sweep of every pair: N*(N-1) bursts
             * per module is forty minutes, and the property under test does
             * not vary by which pair is picked.
             */
            const uint32_t pair_mask = closure_for(m, 0) | closure_for(m, 1);
            for (int dir = 0; dir < 2; ++dir)
            {
                const int from = dir == 0 ? 0 : 1;
                BurstResult pr = median_burst(
                    arena, PER_TYPE,
                    [pair_mask, from](int, int, int& tb, uint32_t& cl)
                    { tb = from; cl = pair_mask; });

                std::string plabel = std::string("  pair ")
                                   + (dir == 0 ? m.tags[0] + " -> " + m.tags[1]
                                               : m.tags[1] + " -> " + m.tags[0]);
                if (plabel.size() > 25) plabel = plabel.substr(0, 25);
                plabel.resize(25, ' ');
                std::cout << "  " << plabel
                          << " " << std::setw(4) << N
                          << "  " << std::setw(8) << bits_to_string(pair_mask, N)
                          << "  " << std::setw(11) << formatWithCommas(static_cast<long long>(pr.ops))
                          << "  " << std::setw(7) << std::fixed << std::setprecision(1) << pr.ms
                          << (pr.ok ? "" : (FG_RED + "  [INCOMPLETE]" + RESET)) << "\n";
                if (!pr.ok) ++incomplete;
                if (dir == 0) pair_fwd = pr.ops; else pair_rev = pr.ops;
            }

            /*
             * Not a tolerance on the throughput -- that is a measurement on a
             * shared machine and will differ run to run. What is asserted is
             * that neither direction stalled, since a mask that had become
             * directional would show as one side completing and the other not.
             */
            if (pair_fwd > 0.0 && pair_rev > 0.0)
            {
                const double skew = pair_fwd > pair_rev ? pair_fwd / pair_rev
                                                        : pair_rev / pair_fwd;
                if (skew > 3.0)
                    std::cout << "  " << FG_YELLOW
                              << "  ^ directions differ by " << std::setprecision(1) << skew
                              << "x -- the pair mask is meant to be symmetric" << RESET << "\n";
            }
        }
        std::cout << "\n";
    }

    /*
     * The contrast, run last against the identical harness so the number is
     * comparable rather than merely adjacent: the blast's own distribution --
     * a uniform bit over the full 64-wide word on one event in ten -- through
     * this same burst pattern and the same producer count.
     *
     * Whichever way it lands, it is worth having: if random-over-64 is FASTER,
     * the old suite was reporting a best case as a worst case, and every
     * throughput number in this file's history needs reading that way.
     */
    std::cout << "  " << FG_DIM << "baseline, for comparison" << RESET << "\n";
    BurstResult rnd = median_burst(
        arena, PER_TYPE,
        [](int p, int i, int& tb, uint32_t& cl) {
            cl = 0;
            tb = (i % 10 == 0)
               ? static_cast<int>(fast_hash(static_cast<uint64_t>(p) * 100000000ull + i) % 64)
               : -1;
        });
    std::string rnd_label = "  random bit % 64";
    rnd_label.resize(25, ' ');
    std::cout << "  " << rnd_label
              << " " << std::setw(4) << 64
              << "  " << std::setw(8) << "uniform"
              << "  " << std::setw(11) << formatWithCommas(static_cast<long long>(rnd.ops))
              << "  " << std::setw(7) << std::fixed << std::setprecision(1) << rnd.ms << "\n\n";

    std::cout << "  fastest real type: " << FG_GREEN << best_name << RESET
              << "  " << formatWithCommas(static_cast<long long>(best)) << " ops/sec\n";
    std::cout << "  slowest real type: " << FG_YELLOW << worst_name << RESET
              << "  " << formatWithCommas(static_cast<long long>(worst)) << " ops/sec\n";
    std::cout << "  random-over-64:    " << formatWithCommas(static_cast<long long>(rnd.ops))
              << " ops/sec\n";

    /*
     * THE A/B, AVERAGED -- kept, and DEMOTED.
     *
     * It was written as the number this section exists to produce. It is not,
     * because it cannot be: it is a wall-clock ratio taken on a machine that
     * also runs builds, and the fixed control above moves by 4x between runs
     * of the same binary. The blocked-admission counts printed earlier answer
     * the same question without a clock in it. This stays as corroboration --
     * agreeing in direction is worth something; disagreeing would be worth
     * more, as a sign the model and the machine are measuring different things.
     */
    if (per_type_n && per_edge_n)
    {
        const double per_type = per_type_total / per_type_n;
        const double per_edge = per_edge_total / per_edge_n;
        std::cout << "\n  " << FG_CYAN << "MASK POLICY, same traffic, averaged over "
                  << per_type_n << " module(s):" << RESET << "\n"
                  << "    per-type union (today)   "
                  << formatWithCommas(static_cast<long long>(per_type)) << " ops/sec\n"
                  << "    per-function edges       "
                  << formatWithCommas(static_cast<long long>(per_edge)) << " ops/sec"
                  << (per_edge > per_type
                      ? "   " + FG_GREEN + "<-- "
                        + std::to_string(static_cast<int>((per_edge / per_type) * 100) / 100.0).substr(0, 4)
                        + "x" + RESET
                      : "")
                  << "\n    random-over-64 (control) "
                  << formatWithCommas(static_cast<long long>(rnd.ops)) << " ops/sec\n";
    }
    std::cout << "\n";

    /*
     * THE READING, CORRECTED. Printed rather than left to be inferred, because
     * this file has printed the wrong one and the wrong one is persuasive.
     *
     * What it used to say: random-over-64 wins because a uniform bit in a wide
     * word rarely collides, whereas a real mask carries its own type's bit and
     * so same-type work always serializes -- "that cost IS the guarantee, not a
     * regression."
     *
     * Half of that is true and the conclusion does not follow from it. The
     * own-bit collision is real and is genuinely the guarantee. But it is a
     * FLOOR, and it is the same floor under both mask policies -- the deter-
     * ministic table above holds the own bit fixed and still removes a large
     * share of the refusals. Whatever the gap to random measures, it is not
     * that floor, because narrowing the mask moves the gap without touching it.
     *
     * What it measures is EXCESS WIDTH. A mask that stands for a dependency
     * should not lose to a mask that stands for nothing, and by that margin;
     * losing means the ordering is serializing work that does not depend on
     * anything either. TAG_CLOSURE is the union of every causal context a type
     * has ever been in, so `SetPosition` blocks against everything `Render`
     * reaches. The distance to random is roughly how much of that is surplus.
     *
     * The 7.9x this file once shipped as a headline should not be trusted as a
     * quantity in any case: the control alone swung 685k / 171k / 374k ops/sec
     * on three consecutive runs of one binary. The ratio below is printed for
     * the same reason -- direction, not magnitude.
     *
     * (The mixed row still being faster than its module's single-type rows has
     * the old explanation and it survives: several types live at once, each on
     * its own bit, so the buffer finds independent work to commit. Saturating
     * one hot type is the floor.)
     */
    if (rnd.ops > 0.0 && worst < 1e17)
    {
        const double ratio = rnd.ops / worst;
        std::cout << "  " << FG_YELLOW << "Random masks run "
                  << std::fixed << std::setprecision(1) << ratio
                  << "x faster than the slowest real one." << RESET << "\n"
                  << "  " << FG_DIM
                  << "Read as direction only -- the control itself swings ~4x run to run.\n"
                  << "  This file used to read that gap as the price of the guarantee. It\n"
                  << "  is not: a mask standing for a real dependency should not lose to\n"
                  << "  one standing for nothing. The gap is EXCESS WIDTH -- TAG_CLOSURE is\n"
                  << "  the union of every context a type has ever been in, so it orders\n"
                  << "  against far more than any single work function reaches."
                  << RESET << "\n";
    }
    /*
     * And the answer the machine cannot swing, restated at the bottom where the
     * conclusion is drawn, so the two are never read apart.
     */
    const size_t awi = adm.width_i;
    if (adm.per_type[awi].events && adm.per_type[awi].width() > 0.0)
    {
        const double keep = 100.0 * adm.per_edge[awi].width() / adm.per_type[awi].width();
        std::cout << "\n  " << FG_CYAN << "The deterministic answer" << RESET
                  << " (no clock, same on every machine):\n"
                  << "  narrowing the union to per-function edges leaves "
                  << std::fixed << std::setprecision(1) << keep
                  << "% of the interference,\n  and refuses admission to "
                  << std::setprecision(1) << adm.per_edge[0].pct() << "% of arrivals at a "
                  << ADMISSION_WINDOWS[0] << "-deep window where the union refuses "
                  << adm.per_type[0].pct() << "%.\n"
                  << "  " << FG_DIM
                  << "The difference was the union standing in for dependencies that were\n"
                  << "  not there. What it costs in time is what the table above cannot say."
                  << RESET << "\n";
    }

    TEST("every capability burst completed", incomplete == 0);
}

// ── main ──────────────────────────────────────────────────────────────────────
// #include "../core/TempMemoryArena.h"
int main()
{
    WIRE_CONTEXT();
    //ETCS::TempMemoryArena tmp;
    //tmp.touchAll();
    ETCS::MemoryArena& arena = ETCS::MemoryArena::getInstance();
    std::cout << "EventStream GRB test suite [worst-case random bitmasks, THEN real per-module capability edges]\n";
    std::cout << "arena chunk size: " << arena.getChunkSize() << " bytes\n";
    std::cout << "huge pages:       " << (arena.isUsingHugePages() ? "yes" : "no") << "\n";
    std::cout << "GAP_DEPTH:        " << ETCS::GAP_DEPTH << "\n";
    std::cout << "TAG_BITS:         " << ETCS::TAG_BITS << " (" << ETCS::TAG_WORDS << " words)\n";
    std::cout << "SLOT_SIZE:        " << ETCS::LMAXSequentialSharedPage::SLOT_SIZE << " bytes\n";
    std::cout << "sizeof(GapSlot):  " << sizeof(ETCS::GapSlot) << " bytes\n";
    // ── core correctness ──────────────────────────────────────────────────────
    test_eventstream_inline_only(arena);
    test_eventstream_drop_only(arena);
    test_eventstream_admission(arena);
    test_eventstream_drop_abandons(arena);
    test_eventstream_async_ordering(arena);
    test_eventstream_independent_tags(arena);
    test_eventstream_gap_depth_backpressure(arena);
    test_eventstream_mixed_traffic(arena);
    // ── blast scaling ─────────────────────────────────────────────────────────
    test_eventstream_blast(arena, 500);
    test_eventstream_blast(arena, 10000);
    test_eventstream_blast(arena, 100000);
    test_eventstream_blast(arena, 500000);
    test_eventstream_blast(arena, 1000000);
    test_eventstream_blast(arena, 5000000);
    test_eventstream_blast(arena, 10000000);
    // ── capability edges ──────────────────────────────────────────────────────
    // Last, because it dlopens every built module to ask for its tag list and
    // there is no reason to carry those mappings through the primitive tests.
    test_eventstream_pair_mask_symmetry();
    test_eventstream_capability_edges(arena);
    std::cout << "\n────────────────────────────────────\n";
    std::cout << "passed: " << s_passed << "\n";
    std::cout << "failed: " << s_failed << "\n";
    return s_failed == 0 ? 0 : 1;
}