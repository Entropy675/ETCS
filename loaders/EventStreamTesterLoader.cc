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
    TestEventStream()
    {
        emitted_buf_.resize(MAX_TRACKED);
    }
    // ── CRTP contract ──────────────────────────────────────────────────────────
    //
    // mask_for runs on the raw event BEFORE on_event, and its answer decides
    // whether on_event may run at all. Hiding EventStream's own all() default,
    // which is what an un-overriding stream inherits.
    ETCS::TagMask mask_for(TestState&, const TestInEvent& evt)
    {
        if (evt.tag_bit < 0) return ETCS::TagMask{};
        return ETCS::TagMask::bit(static_cast<size_t>(evt.tag_bit));
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
        if (emit_idx_ < MAX_TRACKED)
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
// ── main ──────────────────────────────────────────────────────────────────────
// #include "../core/TempMemoryArena.h"
int main()
{
    WIRE_CONTEXT();
    //ETCS::TempMemoryArena tmp;
    //tmp.touchAll();
    ETCS::MemoryArena& arena = ETCS::MemoryArena::getInstance();
    std::cout << "EventStream GRB test suite [Randomized Event Dependency bitmasks - worst case/random performance]\n";
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
    std::cout << "\n────────────────────────────────────\n";
    std::cout << "passed: " << s_passed << "\n";
    std::cout << "failed: " << s_failed << "\n";
    return s_failed == 0 ? 0 : 1;
}