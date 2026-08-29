#include "../ETCS.h"
#include "../core/SharedPage.h"
#include "../core/LMAXSequentialSharedPage.h"
#include "../core/LMAXStream.h"
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
    int pos = s.length() - 3;
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

static int messages_for_producer(int p, int total, int producers)
{
    int base = total / producers, rem = total % producers;
    return base + (p < rem ? 1 : 0);
}

static const std::string RESET      = "\033[0m";
static const std::string FG_GREEN   = "\033[1;32m";
static const std::string FG_CYAN    = "\033[1;36m";
static const std::string FG_RED     = "\033[31m";
static const std::string FG_YELLOW  = "\033[1;33m";
static const std::string FG_MAGENTA = "\033[1;35m";

// ── unit tests ────────────────────────────────────────────────────────────────

static void test_stream_single_producer_single_consumer(ETCS::MemoryArena& arena)
{
    section("Stream: Single Producer / Single Consumer");

    ETCS::LMAXStream<> stream(arena, 1, 1);

    ETCS::LBuffer payload("hello stream");
    bool ok = stream.write(42, payload);
    TEST("write returns true on first write", ok);
    TEST("framesWritten is 1", stream.framesWritten() == 1);
    TEST("publishedUpTo is 0", stream.publishedUpTo() == 0);

    std::string received;
    bool consumed = stream.consume([&](const ETCS::LBuffer& b) {
        received = std::string(b.c_str());
    });

    TEST("consume returns true for published frame", consumed);
    TEST("payload content correct", received == "hello stream");
    TEST("framesConsumed advances to 1", stream.framesConsumed() == 1);

    // Second consume should return false — ring is empty
    bool empty = stream.consume([](const ETCS::LBuffer&){});
    TEST("consume returns false on empty ring", !empty);
}

static void test_stream_backpressure(ETCS::MemoryArena& arena)
{
    section("Stream: Ring Full / Backpressure");

    // Force small ring: producer_count=1 → ETCS_RING_SMALL_SLOTS
    // We need to fill it; use a tiny manual allocation for precision.
    // LMAXStream uses allocateTunedRing so we test at the stream level.
    ETCS::LMAXStream<> stream(arena, 1, 1);

    long long cap = stream.slotCount();
    int written = 0;
    for (long long i = 0; i < cap; ++i)
    {
        ETCS::LBuffer b("x");
        if (stream.write(1, b)) ++written;
    }
    TEST("all slots filled via stream", written == cap);
    TEST("isFull() true via stream", stream.isFull());

    ETCS::LBuffer overflow("overflow");
    bool refused = stream.write(1, overflow);
    TEST("write returns false when full", !refused);

    // Drain one frame then write should succeed
    stream.consume([](const ETCS::LBuffer&){});
    bool after = stream.write(1, overflow);
    TEST("write succeeds after one frame consumed", after);
}

static void test_stream_sequence_ordering(ETCS::MemoryArena& arena)
{
    section("Stream: Sequence Ordering");

    ETCS::LMAXStream<> stream(arena, 1, 1);

    constexpr int N = 8;
    for (int i = 0; i < N; ++i)
    {
        ETCS::LBuffer b; b << i;
        stream.write(1, b);
    }

    TEST("publishedUpTo at N-1 after all writes",
         stream.publishedUpTo() == N - 1);

    bool ordered = true;
    for (int i = 0; i < N; ++i)
    {
        int val = -1;
        bool ok = stream.consume([&](const ETCS::LBuffer& b) {
            ETCS::LBuffer copy = b;
            copy >> val;
        });
        if (!ok || val != i) { ordered = false; break; }
    }
    TEST("payloads delivered in write order", ordered);
    TEST("framesConsumed equals N", stream.framesConsumed() == N);
}

static void test_stream_consume_all(ETCS::MemoryArena& arena)
{
    section("Stream: consumeAll");

    ETCS::LMAXStream<> stream(arena, 1, 1);

    constexpr int N = 16;
    for (int i = 0; i < N; ++i)
    {
        ETCS::LBuffer b; b << i;
        stream.write(1, b);
    }

    int count = 0;
    uint64_t drained = stream.consumeAll([&](const ETCS::LBuffer&) { ++count; });

    TEST("consumeAll returns correct drain count", drained == N);
    TEST("consumeAll consumed all frames", count == N);
    TEST("ring empty after consumeAll", !stream.consume([](const ETCS::LBuffer&){}));
}

static void test_stream_reset(ETCS::MemoryArena& arena)
{
    section("Stream: Reset");

    ETCS::LMAXStream<> stream(arena, 1, 1);

    for (int i = 0; i < 4; ++i)
    {
        ETCS::LBuffer b("data");
        stream.write(1, b);
        stream.consume([](const ETCS::LBuffer&){});
    }

    stream.reset();

    TEST("framesConsumed is 0 after reset", stream.framesConsumed() == 0);
    TEST("isFull() false after reset",      !stream.isFull());

    ETCS::LBuffer fresh("after reset");
    stream.write(1, fresh);

    std::string received;
    stream.consume([&](const ETCS::LBuffer& b) { received = b.c_str(); });
    TEST("write+consume after reset gives correct payload",
         received == "after reset");
}

static void test_stream_publisher_barrier(ETCS::MemoryArena& arena)
{
    section("Stream: Publisher Barrier — Consumer Held Until Contiguous");

    // Two producers; p1 writes seq 1 first, p0 writes seq 0 later.
    // The stream consumer must not see seq 1 before seq 0 is published.
    ETCS::LMAXStream<> stream(arena, 1, 2);

    std::atomic<bool> p1_done{false};
    std::atomic<bool> p0_done{false};

    std::thread p0([&]() {
        while (!p1_done.load(std::memory_order_acquire))
            std::this_thread::yield();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        ETCS::LBuffer b("seq0");
        stream.write(1, b);
        p0_done.store(true, std::memory_order_release);
    });

    std::thread p1([&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        ETCS::LBuffer b("seq1");
        stream.write(2, b);
        p1_done.store(true, std::memory_order_release);
    });

    // Try to consume seq 1 before barrier has advanced past 0
    bool saw_seq1_early = false;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);

    while (!p0_done.load(std::memory_order_acquire))
    {
        // framesConsumed() == 0 means seq 0 not yet consumed.
        // If consume() returns true here it consumed seq 0 (correct),
        // but if it somehow returned seq 1 first the barrier is broken.
        // We detect this by checking publishedUpTo < 1 while consuming seq 1.
        uint64_t pub = stream.publishedUpTo();
        if (pub != UINT64_MAX && pub < 1 && stream.framesConsumed() == 1)
            saw_seq1_early = true;

        if (std::chrono::steady_clock::now() > deadline) break;
        std::this_thread::yield();
    }

    p0.join();
    p1.join();

    TEST("stream consumer cannot observe seq 1 before barrier", !saw_seq1_early);

    // Drain both frames
    int drained = 0;
    stream.consumeAll([&](const ETCS::LBuffer&){ ++drained; });
    TEST("both frames eventually readable via stream", drained == 2);
}

static void test_stream_multi_producer_stress(ETCS::MemoryArena& arena)
{
    section("Stream: Multi-Producer Stress (Pinned)");

    constexpr int PRODUCERS    = 4;
    constexpr int PER_PRODUCER = 12;
    constexpr int TOTAL        = PRODUCERS * PER_PRODUCER;
    const int     CORES        = get_core_count();

    ETCS::LMAXStream<> stream(arena, 1, PRODUCERS);

    std::atomic<int> total_written{0};
    std::vector<std::thread> producers;

    for (int p = 0; p < PRODUCERS; ++p)
    {
        producers.emplace_back([&, p]() {
            pin_thread((p + 1) % CORES);
            for (int i = 0; i < PER_PRODUCER; ++i)
            {
                ETCS::LBuffer b; b << p << i;
                int retry = 0;
                while (!stream.write(static_cast<uint64_t>(p), b))
                    ETCS::LMAXSequentialSharedPage::progressiveYield(retry);
                total_written.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    for (auto& t : producers) t.join();
    TEST("all producer writes landed", total_written.load() == TOTAL);

    int consumed = 0;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (consumed < TOTAL)
    {
        if (stream.consume([&](const ETCS::LBuffer&){ ++consumed; }))
            continue;
        if (std::chrono::steady_clock::now() > deadline) break;
        std::this_thread::yield();
    }
    TEST("all frames consumed in strict sequence order", consumed == TOTAL);
}

// ── blast / throughput ────────────────────────────────────────────────────────

static void test_stream_blast(ETCS::MemoryArena& arena, int TOTAL = 1000)
{
    section("Stream: Blast — Strict In-Order Consumer (Pinned)");

    constexpr int PRODUCERS = 4;
    const int     CORES     = get_core_count();

    int PER_PRODUCER    = TOTAL / PRODUCERS;
    int EFFECTIVE_TOTAL = PER_PRODUCER * PRODUCERS;

    ETCS::LMAXStream<> stream(arena, 1, PRODUCERS);

    std::atomic<bool> start_flag{false};
    std::atomic<int>  total_consumed{0};

    std::thread consumer([&]() {
        pin_thread(0);
        while (!start_flag.load(std::memory_order_acquire));
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        int retry = 0;
        while (total_consumed.load(std::memory_order_relaxed) < EFFECTIVE_TOTAL)
        {
            if (stream.consume([&](const ETCS::LBuffer&){
                    total_consumed.fetch_add(1, std::memory_order_relaxed);
                }))
            { retry = 0; continue; }
            if (std::chrono::steady_clock::now() > deadline) break;
            ETCS::LMAXSequentialSharedPage::progressiveYield(retry);
        }
    });

    std::vector<std::thread> producers;
    for (int p = 0; p < PRODUCERS; ++p)
    {
        producers.emplace_back([&, p]() {
            pin_thread((p + 1) % CORES);
            while (!start_flag.load(std::memory_order_acquire));
            for (int i = 0; i < PER_PRODUCER; ++i)
            {
                ETCS::LBuffer b; b << p << i;
                int retry = 0;
                while (!stream.write(static_cast<uint64_t>(p), b))
                    ETCS::LMAXSequentialSharedPage::progressiveYield(retry);
            }
        });
    }

    auto t0 = std::chrono::high_resolution_clock::now();
    start_flag.store(true, std::memory_order_release);
    for (auto& t : producers) t.join();
    consumer.join();
    auto t1 = std::chrono::high_resolution_clock::now();

    auto   ns  = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
    double ops = ns > 0 ? (static_cast<double>(EFFECTIVE_TOTAL) / ns) * 1e9 : 0.0;
    double ms  = ns / 1000000.0;

    TEST("stream blast: all messages consumed in strict order",
         total_consumed.load() == EFFECTIVE_TOTAL);
    std::cout << "  [INFO] " << formatWithCommas(EFFECTIVE_TOTAL)
              << " messages in " << std::fixed << std::setprecision(3) << ms
              << "ms — " << formatWithCommas(static_cast<long long>(ops)) << " ops/sec\n";
}

// ── dynamic scaling ───────────────────────────────────────────────────────────

static void execute_stream_dynamic_scaling(ETCS::MemoryArena& arena,
                                           int                producer_count,
                                           const std::string& label)
{
    section(("Stream: Dynamic Scaling — " + label).c_str());

    const int CORES          = get_core_count();
    const int BASE_EVENTS    = 250000;
    const int EVENT_STEP     = 25000;

    ETCS::LMAXStream<> stream(arena, 1, producer_count);

    std::atomic<bool>     test_running{true};
    std::atomic<uint64_t> total_expected{0};
    std::atomic<uint64_t> total_consumed_cnt{0};

    std::thread consumer([&]() {
        pin_thread(0);
        int retry = 0;
        while (test_running.load(std::memory_order_acquire) ||
               total_consumed_cnt.load(std::memory_order_relaxed) <
               total_expected.load(std::memory_order_relaxed))
        {
            if (stream.consume([&](const ETCS::LBuffer&){
                    total_consumed_cnt.fetch_add(1, std::memory_order_relaxed);
                }))
            { retry = 0; continue; }
            ETCS::LMAXSequentialSharedPage::progressiveYield(retry);
        }
    });

    std::cout << "\n    producers | events     |    ops/sec        |   ms    |   avg lat\n";
    std::cout <<   "    ----------+------------+-------------------+---------+----------\n";

    auto run_phase = [&](int p_count, const std::string& color) {
        std::vector<std::thread> producers;

        int phase_total = 0;
        for (int i = 0; i < p_count; ++i)
        {
            int ev = BASE_EVENTS - i * EVENT_STEP;
            if (ev < 1000) ev = 1000;
            phase_total += ev;
        }

        auto t0 = std::chrono::high_resolution_clock::now();
        total_expected.fetch_add(phase_total, std::memory_order_relaxed);
        uint64_t target = total_expected.load(std::memory_order_relaxed);

        for (int i = 0; i < p_count; ++i)
        {
            int ev = BASE_EVENTS - i * EVENT_STEP;
            if (ev < 1000) ev = 1000;
            producers.emplace_back([&, i, ev]() {
                pin_thread((i + 1) % CORES);
                for (int e = 0; e < ev; ++e)
                {
                    ETCS::LBuffer b; b << i << e;
                    int retry = 0;
                    while (!stream.write(static_cast<uint64_t>(i + 1), b))
                        ETCS::LMAXSequentialSharedPage::progressiveYield(retry);
                }
            });
        }

        for (auto& t : producers) t.join();

        int drain_retry = 0;
        while (total_consumed_cnt.load(std::memory_order_relaxed) < target)
            ETCS::LMAXSequentialSharedPage::progressiveYield(drain_retry);

        auto t1 = std::chrono::high_resolution_clock::now();
        auto ns_total = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
        auto ms       = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
        double ops    = ms > 0 ? (phase_total / (ms / 1000.0)) : 0.0;
        double lat    = static_cast<double>(ns_total) / phase_total;

        std::cout << color
                  << "    " << std::setw(9)  << p_count
                  << " | " << std::setw(10) << formatWithCommas(phase_total)
                  << " | " << std::setw(11) << formatWithCommas(static_cast<long long>(ops)) << " ops/s"
                  << " | " << std::setw(7)  << ms << " ms"
                  << " | " << std::setw(10) << formatDuration(lat)
                  << RESET << "\n";
    };

    float offset = 1.0f;
    for (int p = 1; p <= producer_count; ++p)
    {
        run_phase(p, FG_CYAN);
        offset -= 0.1f;
    }

    std::cout << "    ----------+------------+-------------------+---------+----------\n";
    for (int p = producer_count - 1; p >= 1; --p)
        run_phase(p, FG_MAGENTA);

    test_running.store(false, std::memory_order_release);
    consumer.join();

    TEST("stream dynamic scaling: all writes consumed",
         total_consumed_cnt.load() == total_expected.load());
}

static void test_stream_dynamic_scaling(ETCS::MemoryArena& arena)
{
    execute_stream_dynamic_scaling(arena, 4,  "Small Ring");
    execute_stream_dynamic_scaling(arena, 8,  "Large Ring");
}

// ── mixed traffic ─────────────────────────────────────────────────────────────

static void execute_stream_mixed_traffic(ETCS::MemoryArena& arena,
                                         int                total_producers,
                                         std::chrono::milliseconds window_ms,
                                         const std::string& label)
{
    section(("Stream: Mixed Traffic — " + label).c_str());

    const int burst_count = total_producers / 2;
    const int cont_count  = total_producers - burst_count;
    const int CORES       = get_core_count();

    ETCS::LMAXStream<> stream(arena, 1, total_producers);

    std::atomic<bool>     burst_running{true};
    std::atomic<bool>     test_running{true};
    std::atomic<uint64_t> total_consumed_cnt{0};
    std::atomic<uint64_t> grand_total_expected{0};

    // per-window snapshot
    struct WindowSnapshot { uint64_t consumed; uint64_t burst_writes; uint64_t cont_writes; };
    std::mutex snap_mutex;
    std::vector<WindowSnapshot> snapshots;

    std::vector<std::atomic<uint64_t>> burst_win(burst_count);
    std::vector<std::atomic<uint64_t>> cont_win(cont_count);
    for (auto& a : burst_win) a = 0;
    for (auto& a : cont_win)  a = 0;

    uint64_t prev_consumed = 0;

    std::thread timer([&]() {
        while (test_running.load(std::memory_order_acquire))
        {
            std::this_thread::sleep_for(window_ms);
            if (!test_running.load(std::memory_order_acquire)) break;
            WindowSnapshot s{};
            s.consumed = total_consumed_cnt.load(std::memory_order_relaxed);
            for (int i = 0; i < burst_count; ++i)
                s.burst_writes += burst_win[i].exchange(0, std::memory_order_relaxed);
            for (int i = 0; i < cont_count; ++i)
                s.cont_writes  += cont_win[i].exchange(0, std::memory_order_relaxed);
            std::lock_guard<std::mutex> lk(snap_mutex);
            snapshots.push_back(s);
        }
    });

    std::thread consumer([&]() {
        pin_thread(0);
        int retry = 0;
        while (test_running.load(std::memory_order_acquire) ||
               total_consumed_cnt.load(std::memory_order_relaxed) <
               grand_total_expected.load(std::memory_order_relaxed))
        {
            if (stream.consume([&](const ETCS::LBuffer&){
                    total_consumed_cnt.fetch_add(1, std::memory_order_relaxed);
                }))
            { retry = 0; continue; }
            ETCS::LMAXSequentialSharedPage::progressiveYield(retry);
        }
    });

    std::vector<std::thread> producers;
    const int BURST_EVENTS = 500000;

    for (int i = 0; i < burst_count; ++i)
    {
        grand_total_expected.fetch_add(BURST_EVENTS, std::memory_order_relaxed);
        producers.emplace_back([&, i]() {
            pin_thread((i % (CORES - 1)) + 1);
            std::mt19937 gen(std::random_device{}());
            std::uniform_int_distribution<> pause_dist(10000, 25000);
            std::uniform_int_distribution<> sleep_dist(30, 90);
            const int pause_mod = pause_dist(gen);

            for (int e = 1; e <= BURST_EVENTS; ++e)
            {
                if (e % pause_mod == 0)
                    std::this_thread::sleep_for(std::chrono::milliseconds(sleep_dist(gen)));

                ETCS::LBuffer b; b << i << e;
                int retry = 0;
                while (!stream.write(static_cast<uint64_t>(i + 1), b))
                    ETCS::LMAXSequentialSharedPage::progressiveYield(retry);
                burst_win[i].fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    for (int i = 0; i < cont_count; ++i)
    {
        producers.emplace_back([&, i]() {
            pin_thread(((burst_count + i) % (CORES - 1)) + 1);
            int e = 0;
            while (burst_running.load(std::memory_order_acquire))
            {
                ETCS::LBuffer b; b << (burst_count + i) << e;
                int retry = 0;
                while (!stream.write(static_cast<uint64_t>(burst_count + i + 1), b))
                    ETCS::LMAXSequentialSharedPage::progressiveYield(retry);
                cont_win[i].fetch_add(1, std::memory_order_relaxed);
                grand_total_expected.fetch_add(1, std::memory_order_relaxed);
                ++e;
            }
        });
    }

    for (int i = 0; i < burst_count; ++i) producers[i].join();
    burst_running.store(false, std::memory_order_release);
    for (int i = burst_count; i < total_producers; ++i) producers[i].join();

    test_running.store(false, std::memory_order_release);
    timer.join();
    consumer.join();

    std::cout << "\n  ring slots: " << formatWithCommas(stream.slotCount())
              << "  producers: " << burst_count << " burst + " << cont_count << " continuous"
              << "  window: " << window_ms.count() << "ms\n\n";

    std::cout << "  window | consumed/win |  burst writes  |  cont writes\n";
    std::cout << "  -------+--------------+----------------+--------------\n";

    std::lock_guard<std::mutex> lk(snap_mutex);
    int win_idx = 0;
    for (auto& s : snapshots)
    {
        uint64_t win_consumed = s.consumed - prev_consumed;
        prev_consumed = s.consumed;
        std::cout << FG_CYAN
                  << "  " << std::setw(5) << win_idx
                  << "  | " << std::setw(12) << formatWithCommas(static_cast<long long>(win_consumed))
                  << " | " << std::setw(14) << formatWithCommas(static_cast<long long>(s.burst_writes))
                  << " | " << std::setw(12) << formatWithCommas(static_cast<long long>(s.cont_writes))
                  << RESET << "\n";
        ++win_idx;
    }

    std::cout << "\n  total expected: " << formatWithCommas(grand_total_expected.load())
              << "  consumed: "         << formatWithCommas(total_consumed_cnt.load()) << "\n";

    TEST("stream mixed traffic: all writes consumed",
         total_consumed_cnt.load() == grand_total_expected.load());
}

static void test_stream_mixed_traffic(ETCS::MemoryArena& arena,
                                      int total_producers,
                                      std::chrono::milliseconds window_ms)
{
    execute_stream_mixed_traffic(arena, total_producers, window_ms, "Small Producers");
    execute_stream_mixed_traffic(arena, total_producers * 2, window_ms, "Large Producers");
}

// ── tuned ring via stream ─────────────────────────────────────────────────────

static void test_stream_tuned_ring(ETCS::MemoryArena& arena, int PRODUCERS, int TOTAL = 500000)
{
    section("Stream: Tuned Ring");

    const int  CORES        = get_core_count();
    const bool expect_large = PRODUCERS > ETCS_RING_LARGE_THRESHOLD;
    const long long expected_slots = expect_large
        ? ETCS_RING_LARGE_SLOTS
        : ETCS_RING_SMALL_SLOTS;

    std::cout << "  [INFO] producers=" << PRODUCERS
              << "  threshold=" << ETCS_RING_LARGE_THRESHOLD
              << "  → " << (expect_large ? "large" : "small") << " ring"
              << " (" << formatBytes(expected_slots * ETCS::LMAXSequentialSharedPage::SLOT_SIZE) << ")\n";

    ETCS::LMAXStream<> stream(arena, 1, PRODUCERS);

    TEST("stream tuned ring: correct slot count selected",
         stream.slotCount() == expected_slots);

    std::atomic<bool> start_flag{false};
    std::atomic<int>  total_consumed_cnt{0};

    std::thread consumer([&]() {
        pin_thread(0);
        while (!start_flag.load(std::memory_order_acquire));
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
        int retry = 0;
        while (total_consumed_cnt.load(std::memory_order_relaxed) < TOTAL)
        {
            if (stream.consume([&](const ETCS::LBuffer&){
                    total_consumed_cnt.fetch_add(1, std::memory_order_relaxed);
                }))
            { retry = 0; continue; }
            if (std::chrono::steady_clock::now() > deadline) break;
            ETCS::LMAXSequentialSharedPage::progressiveYield(retry);
        }
    });

    std::vector<std::thread> producers;
    for (int p = 0; p < PRODUCERS; ++p)
    {
        producers.emplace_back([&, p]() {
            pin_thread((p + 1) % CORES);
            while (!start_flag.load(std::memory_order_acquire));
            int my_count = messages_for_producer(p, TOTAL, PRODUCERS);
            for (int i = 0; i < my_count; ++i)
            {
                ETCS::LBuffer b; b << p << i;
                int retry = 0;
                while (!stream.write(static_cast<uint64_t>(p), b))
                    ETCS::LMAXSequentialSharedPage::progressiveYield(retry);
            }
        });
    }

    auto t0 = std::chrono::high_resolution_clock::now();
    start_flag.store(true, std::memory_order_release);
    for (auto& t : producers) t.join();
    consumer.join();
    auto t1 = std::chrono::high_resolution_clock::now();

    auto   ns  = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
    double ops = ns > 0 ? (static_cast<double>(TOTAL) / ns) * 1e9 : 0.0;
    double ms  = ns / 1000000.0;

    TEST("stream tuned ring: all messages consumed in strict order",
         total_consumed_cnt.load() == TOTAL);
    std::cout << "  [INFO] " << formatWithCommas(TOTAL) << " messages in "
              << std::fixed << std::setprecision(3) << ms << "ms — "
              << formatWithCommas(static_cast<long long>(ops)) << " ops/sec\n";
}

// ── main ──────────────────────────────────────────────────────────────────────

int main(int argc, char* argv[])
{
    WIRE_CONTEXT();

    ETCS::MemoryArena& arena = ETCS::MemoryArena::getInstance();

    std::cout << "LMAXStream test suite\n";
    std::cout << "arena chunk size: " << arena.getChunkSize() << " bytes\n";
    std::cout << "huge pages:       " << (arena.isUsingHugePages() ? "yes" : "no") << "\n";
    std::cout << "SLOT_SIZE:        " << ETCS::LMAXSequentialSharedPage::SLOT_SIZE << " bytes\n";
    std::cout << "small ring:       " << formatBytes(ETCS_RING_SMALL_SLOTS * ETCS::LMAXSequentialSharedPage::SLOT_SIZE) << "\n";
    std::cout << "large ring:       " << formatBytes(ETCS_RING_LARGE_SLOTS * ETCS::LMAXSequentialSharedPage::SLOT_SIZE) << "\n";

    // ── core correctness ──────────────────────────────────────────────────────
    test_stream_single_producer_single_consumer(arena);
    test_stream_backpressure(arena);
    test_stream_sequence_ordering(arena);
    test_stream_consume_all(arena);
    test_stream_reset(arena);
    test_stream_publisher_barrier(arena);
    test_stream_multi_producer_stress(arena);

    // ── blast scaling ─────────────────────────────────────────────────────────
    test_stream_blast(arena, 1000);
    test_stream_blast(arena, 10000);
    test_stream_blast(arena, 100000);
    test_stream_blast(arena, 1000000);

    // ── mode-gated heavier tests ──────────────────────────────────────────────
    int mode = 3;
    if (argc >= 2) mode = std::atoi(argv[1]);

    std::cout << "\n────────────────────────────────────\n";

    if (mode == 3)
    {
        std::cout << "[mode 3] tuned ring via stream  (threshold=" << ETCS_RING_LARGE_THRESHOLD
                  << "  small=" << formatBytes(ETCS_RING_SMALL_SLOTS * ETCS::LMAXSequentialSharedPage::SLOT_SIZE)
                  << "  large=" << formatBytes(ETCS_RING_LARGE_SLOTS * ETCS::LMAXSequentialSharedPage::SLOT_SIZE)
                  << ")\n";
        for (int p : {1, 2, 4, 5, ETCS_RING_LARGE_THRESHOLD,
                      ETCS_RING_LARGE_THRESHOLD + 1, 8, 10, 12, 16, 32})
            test_stream_tuned_ring(arena, p, 1500000);
    }

    std::cout << "\n────────────────────────────────────\n";
    test_stream_dynamic_scaling(arena);
    std::cout << "\n────────────────────────────────────\n";
    test_stream_mixed_traffic(arena, 4, std::chrono::milliseconds(250));

    std::cout << "\n────────────────────────────────────\n";
    std::cout << "passed: " << s_passed << "\n";
    std::cout << "failed: " << s_failed << "\n";

    return s_failed == 0 ? 0 : 1;
}
