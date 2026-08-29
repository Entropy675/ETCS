#include "../ETCS.h"
#include "../core/SharedPage.h"
#include "../core/LMAXSequentialSharedPage.h"
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

// ── helpers ──────────────────────────────────────────────────────────────────

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

std::string formatWithCommas(int n) {
    std::string s = std::to_string(n);
    int insertPosition = s.length() - 3;
    int limit = (n < 0) ? 1 : 0;
    while (insertPosition > limit) {
        s.insert(insertPosition, ",");
        insertPosition -= 3;
    }
    return s;
}

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

std::string formatDuration(double nanoseconds)
{
    if (nanoseconds <= 0) return "0ns";
    std::ostringstream out;
    out << std::fixed << std::setprecision(1);
    if      (nanoseconds >= 1e9) out << (nanoseconds / 1e9) << "s";
    else if (nanoseconds >= 1e6) out << (nanoseconds / 1e6) << "ms";
    else                         out << nanoseconds << "ns";
    return out.str();
}

std::string formatBytes(uint64_t bytes)
{
    static const std::vector<std::string> units = {"B", "KB", "MB", "GB", "TB"};
    if (bytes == 0) return "0B";
    int magnitude = static_cast<int>(std::log2(bytes) / 10);
    if (magnitude >= static_cast<int>(units.size())) magnitude = units.size() - 1;
    double value = static_cast<double>(bytes) / std::pow(1024, magnitude);
    std::ostringstream out;
    if (magnitude == 0) out << bytes << units[0];
    else out << std::fixed << std::setprecision(1) << value << units[magnitude];
    return out.str();
}

static int messages_for_producer(int p, int total, int producers)
{
    int base      = total / producers;
    int remainder = total % producers;
    return base + (p < remainder ? 1 : 0);
}

// ── color helpers ─────────────────────────────────────────────────────────────

struct RGB {
    uint8_t r, g, b;
    std::string fg() const {
        return "\033[38;2;" + std::to_string(r) + ";" +
               std::to_string(g) + ";" + std::to_string(b) + "m";
    }
};

RGB lighten(RGB color, float factor) {
    auto l = [&](uint8_t c) { return static_cast<uint8_t>(c + (255 - c) * factor); };
    return { l(color.r), l(color.g), l(color.b) };
}

RGB darken(RGB color, float factor) {
    auto d = [&](uint8_t c) { return static_cast<uint8_t>(c * (1.0f - factor)); };
    return { d(color.r), d(color.g), d(color.b) };
}

// Print the table header used by both autotune functions.
static void print_autotune_header(bool show_ms_column = true)
{
    if (show_ms_column)
    {
        std::cout << "\n    bytes    | slot_count |    ops/sec        |    ms      |   avg lat    | note\n";
        std::cout <<   "    ---------+------------+-------------------+------------+--------------+---------\n";
    }
    else
    {
        std::cout << "\n    bytes    | slot_count |    ops/sec        |   avg lat    | note\n";
        std::cout <<   "    ---------+------------+-------------------+--------------+----------\n";
    }
}

// ── ansi constants ────────────────────────────────────────────────────────────

static const std::string RESET      = "\033[0m";
static const std::string FG_GREEN   = "\033[1;32m";
static const std::string FG_CYAN    = "\033[1;36m";
static const std::string FG_RED     = "\033[31m";
static const std::string FG_YELLOW  = "\033[1;33m";
static const std::string FG_MAGENTA = "\033[1;35m";
static const std::string FG_INDIGO  = "\033[38;5;99m";
static const RGB         BASE_GLOW= {145, 55, 35};
static const RGB         BASE_INDIGO= {135, 95, 255};
static const RGB         BASE_CYAN  = {0, 255, 255};


static void execute_mixed_traffic_test(
    ETCS::MemoryArena& arena,
    long long          slot_count,
    int                total_producers,
    std::chrono::milliseconds window_ms,
    const std::string& size_name)
{
    section(("Mixed Traffic — " + size_name).c_str());

    const int burst_count = total_producers / 2;
    const int cont_count  = total_producers - burst_count;

    long long safe_slots = ETCS::LMAXSequentialSharedPage::roundUpToPowerOfTwo(slot_count);
    auto* ring = ETCS::LMAXSequentialSharedPage::allocate(arena, 1, safe_slots);

    std::atomic<bool>     burst_running{true};
    std::atomic<bool>     test_running{true};
    std::atomic<uint64_t> total_consumed{0};
    std::atomic<uint64_t> grand_total_expected{0};

    // ── per-window snapshot state ─────────────────────────────────────────────
    struct WindowSnapshot
    {
        uint64_t consumed;
        uint64_t burst_writes;
        uint64_t cont_writes;
        uint64_t burst_active_ns;
        uint64_t cont_active_ns;
        int      storms_fired;
    };

    std::mutex                  snap_mutex;
    std::vector<WindowSnapshot> snapshots;

    std::vector<std::atomic<uint64_t>> burst_win_writes(burst_count);
    std::vector<std::atomic<uint64_t>> burst_win_ns(burst_count);
    std::vector<std::atomic<uint64_t>> cont_win_writes(cont_count);
    std::vector<std::atomic<uint64_t>> cont_win_ns(cont_count);
    std::atomic<uint64_t>              win_storms{0};

    for (int i = 0; i < burst_count; ++i) { burst_win_writes[i] = 0; burst_win_ns[i] = 0; }
    for (int i = 0; i < cont_count;  ++i) { cont_win_writes[i]  = 0; cont_win_ns[i]  = 0; }

    uint64_t prev_consumed = 0;

    // ── timer thread ──────────────────────────────────────────────────────────
    std::thread timer([&]() {
        while (test_running.load(std::memory_order_acquire))
        {
            std::this_thread::sleep_for(window_ms);
            if (!test_running.load(std::memory_order_acquire)) break;

            WindowSnapshot s{};
            s.consumed = total_consumed.load(std::memory_order_relaxed);

            for (int i = 0; i < burst_count; ++i)
            {
                s.burst_writes    += burst_win_writes[i].exchange(0, std::memory_order_relaxed);
                s.burst_active_ns += burst_win_ns[i].exchange(0, std::memory_order_relaxed);
            }
            for (int i = 0; i < cont_count; ++i)
            {
                s.cont_writes    += cont_win_writes[i].exchange(0, std::memory_order_relaxed);
                s.cont_active_ns += cont_win_ns[i].exchange(0, std::memory_order_relaxed);
            }
            s.storms_fired = static_cast<int>(win_storms.exchange(0, std::memory_order_relaxed));

            std::lock_guard<std::mutex> lk(snap_mutex);
            snapshots.push_back(s);
        }
    });

    // ── consumer ─────────────────────────────────────────────────────────────
    std::thread consumer([&]() {
        pin_thread(0);
        uint64_t expected = 0;
        int retry = 0;

        // while any producer is still alive, drain purely on availability
        while (test_running.load(std::memory_order_acquire))
        {
            const ETCS::LBuffer* r = ring->acquireRead(expected);
            if (!r) { ETCS::LMAXSequentialSharedPage::progressiveYield(retry); continue; }
            retry = 0;
            ring->markConsumed(expected);
            ++expected;
            total_consumed.fetch_add(1, std::memory_order_relaxed);
        }

        // test_running is now false and grand_total_expected is frozen —
        // drain whatever is left in the ring
        retry = 0;
        while (total_consumed.load(std::memory_order_relaxed) < grand_total_expected.load(std::memory_order_relaxed))
        {
            const ETCS::LBuffer* r = ring->acquireRead(expected);
            if (!r) { ETCS::LMAXSequentialSharedPage::progressiveYield(retry); continue; }
            retry = 0;
            ring->markConsumed(expected);
            ++expected;
            total_consumed.fetch_add(1, std::memory_order_relaxed);
        }
    });

    std::vector<std::thread> producers;
    producers.reserve(total_producers);

    // ── burst producers ───────────────────────────────────────────────────────
    const int BURST_EVENTS = 500000;

    for (int i = 0; i < burst_count; ++i)
    {
        grand_total_expected.fetch_add(BURST_EVENTS, std::memory_order_relaxed);

        producers.emplace_back([&, i]() {
            pin_thread((i % (get_core_count() - 1)) + 1);

            std::mt19937 gen(std::random_device{}());
            std::uniform_int_distribution<> pause_dist(10000, 25000);
            std::uniform_int_distribution<> sleep_dist(30, 90);
            const int pause_modulo = pause_dist(gen);

            for (int e = 1; e <= BURST_EVENTS; ++e)
            {
                if (e % pause_modulo == 0)
                {
                    win_storms.fetch_add(1, std::memory_order_relaxed);
                    std::this_thread::sleep_for(std::chrono::milliseconds(sleep_dist(gen)));
                }

                auto t0 = std::chrono::high_resolution_clock::now();
                ETCS::LBuffer b; b << i << e;
                int retry = 0;
                while (ring->write(static_cast<uint64_t>(i + 1), b) == UINT64_MAX)
                    ETCS::LMAXSequentialSharedPage::progressiveYield(retry);
                auto t1 = std::chrono::high_resolution_clock::now();

                burst_win_writes[i].fetch_add(1, std::memory_order_relaxed);
                burst_win_ns[i].fetch_add(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count(),
                    std::memory_order_relaxed);
            }
        });
    }

    // ── continuous producers — loop until burst producers finish ──────────────
    for (int i = 0; i < cont_count; ++i)
    {
        producers.emplace_back([&, i]() {
            pin_thread(((burst_count + i) % (get_core_count() - 1)) + 1);

            int e = 0;
            while (burst_running.load(std::memory_order_acquire))
            {
                auto t0 = std::chrono::high_resolution_clock::now();
                ETCS::LBuffer b; b << (burst_count + i) << e;
                int retry = 0;
                while (ring->write(static_cast<uint64_t>(burst_count + i + 1), b) == UINT64_MAX)
                    ETCS::LMAXSequentialSharedPage::progressiveYield(retry);
                auto t1 = std::chrono::high_resolution_clock::now();

                cont_win_writes[i].fetch_add(1, std::memory_order_relaxed);
                cont_win_ns[i].fetch_add(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count(),
                    std::memory_order_relaxed);

                grand_total_expected.fetch_add(1, std::memory_order_relaxed);
                ++e;
            }
        });
    }

    // burst producers finish first, then signal continuous to stop
    for (int i = 0; i < burst_count; ++i)
        producers[i].join();
    burst_running.store(false, std::memory_order_release);

    // continuous producers drain their current write and exit
    for (int i = burst_count; i < total_producers; ++i)
        producers[i].join();

    // grand_total_expected is now frozen — safe to stop timer and consumer
    test_running.store(false, std::memory_order_release);
    timer.join();
    consumer.join();

    // ── unified table ─────────────────────────────────────────────────────────
    std::cout << "\n  ring: " << formatBytes(safe_slots * 64)
              << "  producers: " << burst_count << " burst + " << cont_count << " continuous"
              << "  window: " << window_ms.count() << "ms\n\n";

    std::cout << "  window | consumed/win |  burst ops/s  burst lat  storms"
              << " |  cont ops/s   cont lat \n";
    std::cout << "  -------+--------------+----------------------------------"
              << "+--------------------------\n";

    std::lock_guard<std::mutex> lk(snap_mutex);
    int win_idx = 0;
    for (auto& s : snapshots)
    {
        uint64_t win_consumed = s.consumed - prev_consumed;
        prev_consumed = s.consumed;

        double burst_ops = (s.burst_active_ns > 0 && s.burst_writes > 0)
            ? (s.burst_writes / (s.burst_active_ns / 1e9)) : 0.0;
        double burst_lat = s.burst_writes > 0
            ? static_cast<double>(s.burst_active_ns) / s.burst_writes : 0.0;

        double cont_ops = (s.cont_active_ns > 0 && s.cont_writes > 0)
            ? (s.cont_writes / (s.cont_active_ns / 1e9)) : 0.0;
        double cont_lat = s.cont_writes > 0
            ? static_cast<double>(s.cont_active_ns) / s.cont_writes : 0.0;

        bool burst_dominant = s.burst_writes >= s.cont_writes;
        std::string row_color = burst_dominant
            ? lighten(BASE_INDIGO, 0.35f).fg()
            : lighten(BASE_CYAN,   0.35f).fg();

        std::cout << row_color
                  << "  " << std::setw(5) << win_idx
                  << "  | " << std::setw(12) << formatWithCommas(static_cast<long long>(win_consumed))
                  << " | " << std::setw(11) << formatWithCommas(static_cast<long long>(burst_ops)) << " ops/s"
                  << "  " << std::setw(9) << formatDuration(burst_lat)
                  << "  " << std::setw(3) << s.storms_fired
                  << " | " << std::setw(11) << formatWithCommas(static_cast<long long>(cont_ops)) << " ops/s"
                  << "  " << std::setw(9) << formatDuration(cont_lat)
                  << RESET << "\n";
        ++win_idx;
    }

    std::cout << "\n  total expected: " << formatWithCommas(grand_total_expected.load())
              << "  consumed: " << formatWithCommas(total_consumed.load()) << "\n";

    TEST("mixed traffic: all writes consumed",
         total_consumed.load() == grand_total_expected.load());
}

static void test_mixed_traffic(
    ETCS::MemoryArena&        arena,
    int                       total_producers,
    std::chrono::milliseconds window_ms,
    long long                 small = ETCS_RING_SMALL_SLOTS,
    long long                 large = ETCS_RING_LARGE_SLOTS)
{
    execute_mixed_traffic_test(arena, small, total_producers, window_ms, "Small Queue");
    execute_mixed_traffic_test(arena, large, total_producers, window_ms, "Large Queue");
}

static void execute_burst_aware_scaling_test(ETCS::MemoryArena& arena, long long requested_slots, int threshold, const std::string& size_name)
{
    section(("Intermittent Burst Scaling — " + size_name).c_str());
    int gap = 32;

    // 1. Ensure safety: Round up to power of two before allocation
    long long slot_count = ETCS::LMAXSequentialSharedPage::roundUpToPowerOfTwo(requested_slots);
    auto* ring = ETCS::LMAXSequentialSharedPage::allocate(arena, 1, slot_count);

    std::atomic<bool>     test_running{true};
    std::atomic<uint64_t> total_expected_writes{0};
    std::atomic<uint64_t> total_consumed{0};

    // 2. Continuous Consumer (Background)
    // Now uses progressive backoff to prevent CPU starvation when the ring is empty
    std::thread consumer([&]() {
        pin_thread(0);
        uint64_t expected = 0;
        int retry = 0;

        while (test_running.load(std::memory_order_acquire) || 
               total_consumed.load(std::memory_order_relaxed) < total_expected_writes.load(std::memory_order_relaxed)) 
        {
            const ETCS::LBuffer* r = ring->acquireRead(expected);
            if (!r) {
                ETCS::LMAXSequentialSharedPage::progressiveYield(retry);
                continue;
            }

            retry = 0; // Reset backoff counter after a successful read
            ring->markConsumed(expected);
            ++expected;
            total_consumed.fetch_add(1, std::memory_order_relaxed);
        }
    });

    print_autotune_header(/*show_ms_column=*/true);

    auto run_phase = [&](int phase_producer_count, const std::string& phase_type, float color_offset) {
        std::vector<std::thread> producers;
        std::vector<uint64_t>    active_nanos(phase_producer_count, 0);
        
        const int BASE_EVENTS = 150000;
        const int EVENT_STEP  = 10000; 
        
        int phase_total = 0;
        for (int i = 0; i < phase_producer_count; ++i) {
            int count = std::max(1000, BASE_EVENTS - (i * EVENT_STEP));
            phase_total += count;
        }

        total_expected_writes.fetch_add(phase_total, std::memory_order_relaxed);
        uint64_t target_consume = total_expected_writes.load(std::memory_order_relaxed);

        for (int i = 0; i < phase_producer_count; ++i) 
        {
            int events_for_this_thread = std::max(1000, BASE_EVENTS - (i * EVENT_STEP));

            producers.emplace_back([&, i, events_for_this_thread]() {
                pin_thread((i % (get_core_count() - 1)) + 1); 

                std::random_device rd;
                std::mt19937 gen(rd());
                std::uniform_int_distribution<> dis(15000, 30000); 
                const int pause_modulo = dis(gen);

                uint64_t thread_active_ns = 0;
                auto work_start = std::chrono::high_resolution_clock::now();

                for (int e = 1; e <= events_for_this_thread; ++e) {
                    if (e % pause_modulo == 0) {
                        // Pause metrics for jitter sleep
                        auto work_pause = std::chrono::high_resolution_clock::now();
                        thread_active_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(work_pause - work_start).count();

                        // Cooling the cache
                        int offset = 32 + (i % 7) * 32;
                        std::string col = lighten(BASE_GLOW, offset + i).fg();
                        //std::cout << col << "[prod " << i << "] (... waiting for: " << offset << "ms)\n";
                        std::this_thread::sleep_for(std::chrono::milliseconds(offset));
                        //std::cout << col << "[prod " << i << "] (waited for: " << offset << "ms ...)\n";

                        work_start = std::chrono::high_resolution_clock::now();
                    }

                    ETCS::LBuffer b;
                    b << i << e;
                    
                    int retry = 0;
                    // 3. Optimized Producer Backoff
                    // Progressive yield reduces CAS contention during high-burst wakeup
                    while (ring->write(static_cast<uint64_t>(i + 1), b) == UINT64_MAX) {
                        ETCS::LMAXSequentialSharedPage::progressiveYield(retry);
                    }
                }
                
                auto final_stop = std::chrono::high_resolution_clock::now();
                thread_active_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(final_stop - work_start).count();
                active_nanos[i] = thread_active_ns;
            });
        }

        for (auto& t : producers) t.join();

        // Ensure consumer has drained the current phase batch before moving to the next
        int drain_retry = 0;
        while(total_consumed.load(std::memory_order_relaxed) < target_consume) {
            ETCS::LMAXSequentialSharedPage::progressiveYield(drain_retry);
        }

        // --- Metrics Calculation ---
        uint64_t avg_active_ns = std::accumulate(active_nanos.begin(), active_nanos.end(), 0ULL) / phase_producer_count;
        double active_ms = avg_active_ns / 1000000.0;
        double active_ops = (active_ms > 0) ? (phase_total / (active_ms / 1000.0)) : 0;
        double active_lat = static_cast<double>(avg_active_ns) / phase_total;

        std::string note = phase_type + " (" + std::to_string(phase_producer_count) + " bursty)";
        std::string line_color = lighten(BASE_INDIGO, color_offset).fg();
        
        std::cout << line_color
                  << "  " << std::setw(10) << formatBytes(slot_count * 64)
                  << " | " << std::setw(10) << formatWithCommas(slot_count)
                  << " | " << std::setw(11) << formatWithCommas(static_cast<long long>(active_ops)) << " ops/s"
                  << " | " << std::setw(7) << (long long)active_ms << " ms*"
                  << " | " << std::setw(12) << formatDuration(active_lat)
                  << " | " << note << RESET << "\n";
    };

    float color_off = 1.0f;
    // Execute standard ramp up
    for (int p = 1; p <= threshold + gap; ++p) {
        run_phase(p, "BURST UP", color_off);
        color_off -= 0.1f;
    }

    std::cout << "    * Note: ms and ops/s calculated using active producer time only (excludes jitter sleeps)\n";

    test_running.store(false, std::memory_order_release);
    consumer.join();
    
    TEST("intermittent scaling: all writes consumed", total_consumed.load() == total_expected_writes.load());
}


static void execute_dynamic_scaling_test(ETCS::MemoryArena& arena, long long slot_count, const std::string& size_name)
{
    section(("Dynamic Producer Scaling — " + size_name).c_str());

    auto* ring = ETCS::LMAXSequentialSharedPage::allocate(arena, 1, slot_count);

    std::atomic<bool>     test_running{true};
    std::atomic<uint64_t> total_expected_writes{0};
    std::atomic<uint64_t> total_consumed{0};

    // 1. Start continuous consumer
    std::thread consumer([&]() {
        pin_thread(0);
        uint64_t expected = 0;
        
        while (test_running.load(std::memory_order_acquire) || 
               total_consumed.load(std::memory_order_relaxed) < total_expected_writes.load(std::memory_order_relaxed)) 
        {
            const ETCS::LBuffer* r = ring->acquireRead(expected);
            if (!r) {
                std::this_thread::yield();
                continue;
            }
            ring->markConsumed(expected);
            ++expected;
            total_consumed.fetch_add(1, std::memory_order_relaxed);
        }
    });

    // Print the standard autotune table header
    print_autotune_header(/*show_ms_column=*/true);

    // 2. Define the phase runner
    auto run_phase = [&](int phase_producer_count, const std::string& phase_type, float color_factor) {
        std::vector<std::thread> producers;
        
        // Bumped event counts up to ensure the timer has enough resolution
        const int BASE_EVENTS = 250000;
        const int EVENT_STEP  = 25000; 
        
        int phase_total = 0;
        for (int i = 0; i < phase_producer_count; ++i) {
            int events_for_this_thread = BASE_EVENTS - (i * EVENT_STEP);
            if (events_for_this_thread < 1000) events_for_this_thread = 1000;
            phase_total += events_for_this_thread;
        }

        // Start timing the phase
        auto t0 = std::chrono::high_resolution_clock::now();
        
        total_expected_writes.fetch_add(phase_total, std::memory_order_relaxed);
        uint64_t target_consume = total_expected_writes.load(std::memory_order_relaxed);

        for (int i = 0; i < phase_producer_count; ++i) 
        {
            int events_for_this_thread = BASE_EVENTS - (i * EVENT_STEP);
            if (events_for_this_thread < 1000) events_for_this_thread = 1000;

            producers.emplace_back([&, i, events_for_this_thread]() {
                pin_thread((i % (get_core_count() - 1)) + 1); 
                
                for (int e = 0; e < events_for_this_thread; ++e) {
                    ETCS::LBuffer b;
                    b << i << e;
                    int retry = 0;
                    
                    while (ring->write(static_cast<uint64_t>(i + 1), b) == UINT64_MAX) {
                        ETCS::LMAXSequentialSharedPage::relax(retry);
                    }
                }
            });
        }

        for (auto& t : producers) t.join();

        // Wait for consumer to drain THIS specific phase's events
        while(total_consumed.load(std::memory_order_relaxed) < target_consume) {
            std::this_thread::yield();
        }

        auto t1 = std::chrono::high_resolution_clock::now();

        // Calculate metrics
        auto   dur      = t1 - t0;
        auto   ms       = std::chrono::duration_cast<std::chrono::milliseconds>(dur).count();
        auto   ns_total = std::chrono::duration_cast<std::chrono::nanoseconds>(dur).count();
        double ops      = ms > 0 ? (phase_total / (ms / 1000.0)) : 0.0;
        double lat_ns   = static_cast<double>(ns_total) / phase_total;

        // Format and print the row in the standard autotune style
        std::string note = phase_type + " (" + std::to_string(phase_producer_count) + " threads)";
        std::string line_color = lighten(BASE_CYAN, color_factor).fg();
        
        std::cout << line_color
                  << "  " << std::setw(10) << formatBytes(slot_count * 64)
                  << " | " << std::setw(10) << formatWithCommas(slot_count)
                  << " | " << std::setw(11) << formatWithCommas(static_cast<long long>(ops)) << " ops/s"
                  << " | " << std::setw(7) << ms << " ms"
                  << " | " << std::setw(12) << formatDuration(lat_ns)
                  << " | " << note << RESET << "\n";
    };

    // 3. Execute Ramp Up (Cyan gradient)
    float color_offset = 1.0f;
    for (int p = 1; p <= 8; ++p) {
        run_phase(p, "RAMP UP  ", std::max(0.1f, color_offset));
        color_offset -= 0.1f;
    }

    std::cout << "    ---------+------------+-------------------+------------+--------------+---------\n";

    // 4. Execute Ramp Down (Magenta static to visually separate)
    for (int p = 7; p >= 1; --p) {
        // Just overriding color output directly for the down phase so it stands out
        std::cout << FG_MAGENTA; 
        run_phase(p, "RAMP DOWN", std::min(0.9f, color_offset)); 
        color_offset += 0.1f;
    }

    // 5. Cleanup and Verification
    test_running.store(false, std::memory_order_release);
    consumer.join();

    uint64_t final_consumed = total_consumed.load();
    uint64_t final_expected = total_expected_writes.load();

    std::cout << "\n  " << FG_GREEN << "Expected: " << formatWithCommas(final_expected) 
              << " | Consumed: " << formatWithCommas(final_consumed) << RESET << "\n";

    TEST("dynamic scaling: all dynamically generated writes successfully consumed", final_consumed == final_expected);
}

static void test_execute_burst_aware_scaling(ETCS::MemoryArena& arena, long long small = ETCS_RING_SMALL_SLOTS, long long large = ETCS_RING_LARGE_SLOTS, int threshold = ETCS_RING_LARGE_THRESHOLD)
{   
    execute_burst_aware_scaling_test(arena, small, 1, "Small Queue");
    execute_burst_aware_scaling_test(arena, large, threshold, "Large Queue");
}

static void test_arbitrary_producer_scaling(ETCS::MemoryArena& arena, long long small = ETCS_RING_SMALL_SLOTS, long long large = ETCS_RING_LARGE_SLOTS)
{
    execute_dynamic_scaling_test(arena, small, "Small Queue");
    execute_dynamic_scaling_test(arena, large, "Large Queue");
}

// ── autotune shared types ─────────────────────────────────────────────────────

struct RunResult
{
    long long slots;
    double    ops;
    double    lat_ns;
    long long ms;
    bool      passed;
};

struct GlobalResult
{
    int       producers;
    long long slots;
    double    ops;
    double    lat;
};

// ── autotune shared logic ─────────────────────────────────────────────────────

// Run a single timed pass of TOTAL messages through a ring of slot_count slots
// using PRODUCERS pinned producer threads and one pinned consumer on core 0.
// Returns timing/correctness results without any console output.
static RunResult run_ring_pass(
    ETCS::MemoryArena& arena,
    long long slot_count,
    int PRODUCERS,
    int TOTAL,
    bool pin_producers = false)
{
    const int CORES = get_core_count();

    auto* ring = ETCS::LMAXSequentialSharedPage::allocate(arena, 1, slot_count);
    std::atomic<bool> start_flag{false};
    std::atomic<int>  total_consumed{0};

    std::thread consumer([&]() {
        pin_thread(0);
        while (!start_flag.load(std::memory_order_acquire));
        uint64_t expected = 0;
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (total_consumed.load(std::memory_order_relaxed) < TOTAL) {
            const ETCS::LBuffer* r = ring->acquireRead(expected);
            if (!r) {
                if (std::chrono::steady_clock::now() > deadline) break;
                std::this_thread::yield();
                continue;
            }
            ring->markConsumed(expected);
            ++expected;
            total_consumed.fetch_add(1, std::memory_order_relaxed);
        }
    });

    std::vector<std::thread> producers;
    for (int p = 0; p < PRODUCERS; ++p)
    {
        producers.emplace_back([&, p]() {
            if (pin_producers) pin_thread((p + 1) % CORES);
            while (!start_flag.load(std::memory_order_acquire));
            int my_count = messages_for_producer(p, TOTAL, PRODUCERS);
            for (int i = 0; i < my_count; ++i) {
                ETCS::LBuffer b; b << p << i;
                int retry = 0;
                while (ring->write(static_cast<uint64_t>(p), b) == UINT64_MAX)
                    ETCS::LMAXSequentialSharedPage::relax(retry);
            }
        });
    }

    auto t0 = std::chrono::high_resolution_clock::now();
    start_flag.store(true, std::memory_order_release);
    for (auto& t : producers) t.join();
    consumer.join();
    auto t1 = std::chrono::high_resolution_clock::now();

    auto   dur      = t1 - t0;
    auto   ms       = std::chrono::duration_cast<std::chrono::milliseconds>(dur).count();
    auto   ns_total = std::chrono::duration_cast<std::chrono::nanoseconds>(dur).count();
    double ops      = ms > 0 ? (TOTAL / (ms / 1000.0)) : 0.0;
    double lat_ns   = static_cast<double>(ns_total) / TOTAL;
    bool   passed   = total_consumed.load() == TOTAL;

    return {slot_count, ops, lat_ns, ms, passed};
}


// Print one table row for a RunResult, coloring and annotating it according to
// whether it is a new best, a regression, or incomplete. Returns the updated
// color-cycling offset so callers can maintain gradient state across rows.
static double print_autotune_row(
    const RunResult& r,
    bool is_best,
    bool is_first_best,
    double color_offset,
    bool show_ms_column = true)
{
    std::string line_color, note;

    if (!r.passed)
    {
        note = FG_RED + "[INCOMPLETE]" + RESET;
    }
    else if (is_best)
    {
        line_color = is_first_best
            ? FG_GREEN
            : lighten(BASE_CYAN, static_cast<float>(std::max(0.1, color_offset))).fg();
        if (r.ops >= 10000000) line_color = FG_INDIGO;
        note = is_first_best ? "<-- test" : "<-- better";
        color_offset -= 0.08;
    }

    std::cout << line_color
              << "  " << std::setw(10) << formatBytes(r.slots * 64)
              << " | " << std::setw(10) << formatWithCommas(r.slots)
              << " | " << std::setw(11) << formatWithCommas(static_cast<long long>(r.ops)) << " ops/s";

    if (show_ms_column)
        std::cout << " | " << std::setw(7) << r.ms << " ms";

    std::cout << " | " << std::setw(12) << formatDuration(r.lat_ns)
              << " | " << note << RESET << "\n";

    return color_offset;
}

// Run SWEEP_SAMPLES passes for a given slot count and return the median result.
// Using median rather than mean discards OS scheduling outliers in both directions.
static RunResult median_ring_pass(
    ETCS::MemoryArena& arena,
    long long slot_count,
    int PRODUCERS,
    int TOTAL,
    bool pin_producers,
    int SWEEP_SAMPLES = 3)
{
    std::vector<RunResult> samples;
    samples.reserve(SWEEP_SAMPLES);
    for (int i = 0; i < SWEEP_SAMPLES; ++i)
        samples.push_back(run_ring_pass(arena, slot_count, PRODUCERS, TOTAL, pin_producers));

    // If any sample failed, return the first failure
    for (auto& s : samples)
        if (!s.passed) return s;

    std::sort(samples.begin(), samples.end(),
              [](auto& a, auto& b) { return a.ops < b.ops; });

    return samples[SWEEP_SAMPLES / 2]; // middle element
}

// Run a sweep of slot counts for a fixed (PRODUCERS, TOTAL) config, collect
// winners, then run a verification round on them. Returns the verified winner
// list sorted by descending ops. Prints the table inline.
static std::vector<RunResult> run_slot_sweep(
    ETCS::MemoryArena& arena,
    int PRODUCERS,
    int TOTAL,
    bool pin_producers,
    bool show_ms_column,
    int SWEEP_SAMPLES = 3)
{
    const long long MAX_SLOTS = 524288 * 4;

    std::vector<RunResult> winners;
    double best_ops  = 0.0;
    double prev_ops  = 0.0;
    int    declining = 0;
    bool   first_best = true;
    double offset     = 1.0;

    for (long long slot_count = 4; slot_count <= MAX_SLOTS; slot_count *= 2)
    {
        RunResult r = median_ring_pass(arena, slot_count, PRODUCERS, TOTAL,
                                       pin_producers, SWEEP_SAMPLES);

        bool is_best    = r.passed && r.ops > best_ops;
        bool dropped    = prev_ops > 0.0 && r.ops < prev_ops;

        if (is_best)
        {
            best_ops = r.ops;
            winners.push_back(r);
        }

        // Print the row; let the helper handle coloring.
        // For "down" annotation we need to augment note after the helper,
        // so we do a small override here.
        std::string line_color, note;
        if (!r.passed)
        {
            note = FG_RED + "[INCOMPLETE]" + RESET;
            // Print manually so we can break afterward.
            std::cout << "  " << std::setw(10) << formatBytes(r.slots * 64)
                      << " | " << std::setw(10) << formatWithCommas(r.slots)
                      << " | " << std::setw(11) << formatWithCommas(static_cast<long long>(r.ops)) << " ops/s";
            if (show_ms_column) std::cout << " | " << std::setw(7) << r.ms << " ms";
            std::cout << " | " << std::setw(12) << formatDuration(r.lat_ns)
                      << " | " << note << RESET << "\n";
            break;
        }
        else if (is_best)
        {
            offset = print_autotune_row(r, true, first_best, offset, show_ms_column);
            first_best = false;
            declining  = 0;
        }
        else
        {
            if (dropped) note = "down (" + std::to_string(++declining) + ")";
            else         declining = 0;
            std::cout << "  " << std::setw(10) << formatBytes(r.slots * 64)
                      << " | " << std::setw(10) << formatWithCommas(r.slots)
                      << " | " << std::setw(11) << formatWithCommas(static_cast<long long>(r.ops)) << " ops/s";
            if (show_ms_column) std::cout << " | " << std::setw(7) << r.ms << " ms";
            std::cout << " | " << std::setw(12) << formatDuration(r.lat_ns)
                      << " | " << note << RESET << "\n";
        }

        prev_ops = r.ops;
    }

    // Verification round
    if (winners.empty()) return {};

    std::cout << "\n" << FG_MAGENTA << "=== VERIFICATION ROUND ===" << RESET << "\n";
    std::vector<RunResult> verified;

    for (auto& candidate : winners)
    {
        RunResult v = run_ring_pass(arena, candidate.slots, PRODUCERS, TOTAL, pin_producers);
        verified.push_back(v);
        std::cout << "  [" << std::setw(10) << candidate.slots << " slots] "
                  << std::setw(11) << static_cast<long long>(v.ops) << " ops/s"
                  << "  (sweep: " << static_cast<long long>(candidate.ops) << ")\n";
    }

    std::sort(verified.begin(), verified.end(),
              [](auto& a, auto& b) { return a.ops > b.ops; });

    auto& winner = verified[0];
    std::cout << "\n" << FG_YELLOW
              << "  >>> WINNER: " << formatBytes(winner.slots * 64)
              << " | " << winner.slots << " slots"
              << " | " << static_cast<long long>(winner.ops) << " ops/s"
              << " | " << formatDuration(winner.lat_ns)
              << " <<<" << RESET << "\n";

    return verified;
}

// ── autotune tests ────────────────────────────────────────────────────────────

static void test_autotune_slot_count(ETCS::MemoryArena& arena, int PRODUCERS = 4, int TOTAL = 1000000)
{
    std::cout << "\n=== Auto-Tune: Slot Count vs Throughput (" << formatWithCommas(TOTAL) << " messages, "
              << PRODUCERS << " producers) ===\n";

    print_autotune_header(/*show_ms_column=*/true);

    auto verified = run_slot_sweep(arena, PRODUCERS, TOTAL,
                                   /*pin_producers=*/false,
                                   /*show_ms_column=*/true);

    TEST("autotune: verification phase completed", !verified.empty());
}

// Run one full producer-count round for the grand tournament (pinned producers,
// no ms column). Returns the best verified result for this producer count.
static GlobalResult run_autotune_round(ETCS::MemoryArena& arena, int PRODUCERS)
{
    const int TOTAL = 1000000;

    std::cout << "\n" << FG_MAGENTA
              << "┌──────────────────────────────────────────────────────────────────┐\n"
              << "│   " << std::setw(2) << PRODUCERS
              << " Producers (Pinned Cores 1+)                                 │\n"
              << "└──────────────────────────────────────────────────────────────────┘"
              << RESET << "\n";

    print_autotune_header(/*show_ms_column=*/false);

    auto verified = run_slot_sweep(arena, PRODUCERS, TOTAL,
                                   /*pin_producers=*/true,
                                   /*show_ms_column=*/false);

    if (verified.empty()) return {PRODUCERS, 4, 0.0, 0.0};

    auto& w = verified[0];
    return {PRODUCERS, w.slots, w.ops, w.lat_ns};
}

static void test_autotune_grand_tournament(ETCS::MemoryArena& arena)
{
    section("Auto-Tune Grand Tournament");

    std::cout << "\n    bytes    | slot_count |    ops/sec        |   avg lat    | note\n";
    std::cout <<   "    ---------+------------+-------------------+--------------+----------\n";

    constexpr int ROUNDS = 5;
    const int     TOTAL  = 1000000;
    const std::vector<int> producer_configs = {4, 6, 7, 8, 12, 15, 16, 32, 64};

    // Phase 1: one sweep per config to find each config's winning slot count.
    std::vector<GlobalResult> champions;
    for (int p : producer_configs)
        champions.push_back(run_autotune_round(arena, p));

    // Phase 2: re-run each winner's slot count ROUNDS more times and average.
    std::cout << "\n" << FG_CYAN
              << "══════════  averaging " << ROUNDS << " runs at each winning slot count  ══════════"
              << RESET << "\n\n";

    for (auto& c : champions)
    {
        double ops_sum = c.ops; // seed with the sweep-winner result (already one good run)
        double lat_sum = c.lat;

        for (int r = 1; r < ROUNDS; ++r)
        {
            RunResult rr = run_ring_pass(arena, c.slots, c.producers, TOTAL,
                                         /*pin_producers=*/true);
            ops_sum += rr.ops;
            lat_sum += rr.lat_ns;
        }

        c.ops = ops_sum / ROUNDS;
        c.lat = lat_sum / ROUNDS;
    }

    std::cout << "\n\n" << FG_MAGENTA
              << "╔══════════════════════════════════════════════════════════════════╗\n"
              << "║              THE GRAND FINALE  (" << ROUNDS << "-run average)                    ║\n"
              << "╚══════════════════════════════════════════════════════════════════╝"
              << RESET << "\n\n";

    std::sort(champions.begin(), champions.end(),
              [](auto& a, auto& b) { return a.ops > b.ops; });

    for (auto& c : champions)
    {
        std::cout << "  " << std::setw(2) << c.producers << " producers | "
                  << std::setw(10) << formatWithCommas(c.slots) << " slots | "
                  << std::setw(11) << formatWithCommas(static_cast<long long>(c.ops)) << " ops/s | "
                  << formatDuration(c.lat) << "  (avg over " << ROUNDS << " runs)\n";
    }

    auto& winner = champions[0];
    std::cout << "\n" << FG_YELLOW << "🏆 OPTIMUM CONFIG 🏆" << RESET << "\n"
              << "  Producers : " << winner.producers << "\n"
              << "  Slots     : " << formatWithCommas(winner.slots)
              << " (" << formatBytes(winner.slots * 64) << ")\n"
              << "  Throughput: " << formatWithCommas(static_cast<long long>(winner.ops))
              << " ops/s  (avg over " << ROUNDS << " runs)\n"
              << "  Avg Lat   : " << formatDuration(winner.lat) << "\n\n";

    TEST("grand tournament: winner has valid slot count",
         winner.slots > 0 && (winner.slots & (winner.slots - 1)) == 0);

    // ── derive suggested tuned params from tournament data ────────────────────
    //
    // LARGE_SLOTS  — winner's slot count (highest avg throughput config)
    // SMALL_SLOTS  — slot count of the lowest producer config tested
    //                (most representative of cache-hot, low-contention paths)
    // THRESHOLD    — lowest producer count whose winning slot count exceeds
    //                the suggested small ring (i.e. where contention starts
    //                demanding more buffer space); producers below this stay
    //                on the small ring

    // champions is sorted descending by ops; find the lowest-producer entry
    // for the small ring suggestion (need original order — re-sort by producers)
    std::vector<GlobalResult> by_producers = champions;
    std::sort(by_producers.begin(), by_producers.end(),
              [](auto& a, auto& b) { return a.producers < b.producers; });

    long long suggested_large = winner.slots;
    long long suggested_small = by_producers.front().slots; // lowest producer count

    // threshold: first producer count whose winning slots exceed suggested_small
    int suggested_threshold = by_producers.back().producers; // safe fallback
    for (auto& c : by_producers)
    {
        if (c.slots > suggested_small)
        {
            suggested_threshold = c.producers;
            break;
        }
    }
    // threshold is exclusive (producers > threshold use large ring),
    // so subtract 1 to make it the last producer count that stays small
    suggested_threshold = std::max(1, suggested_threshold - 1);

    std::cout << FG_CYAN
              << "┌──────────────────────────────────────────────────────────────────┐\n"
              << "│                  SUGGESTED LMAXAutotuneParams.h                  │\n"
              << "└──────────────────────────────────────────────────────────────────┘\n"
              << RESET
              << "\n"
              << "#ifndef ETCS_AUTOTUNE_DEFS \n"
              << "#define ETCS_AUTOTUNE_DEFS \n"
              << "\n"
              << "#define ETCS_RING_SMALL_SLOTS     "
              << std::setw(10) << suggested_small
              << "  // " << std::setw(8) << formatBytes(suggested_small * 64) << "\n"
              << "#define ETCS_RING_LARGE_SLOTS     "
              << std::setw(10) << suggested_large
              << "  // " << std::setw(8) << formatBytes(suggested_large * 64) << "\n"
              << "#define ETCS_RING_LARGE_THRESHOLD "
              << std::setw(10) << suggested_threshold
              << "  // producers > " << suggested_threshold << " use large ring\n"
              << "\n"
              << "#endif \n";
              
  
    std::cout << "\n─ SUGGESTED PARAMS BURST TESTS ─\n";
    test_arbitrary_producer_scaling(arena, suggested_small, suggested_large);
    std::cout << "\n────────────────────────────────────\n";
    test_execute_burst_aware_scaling(arena, suggested_small, suggested_large, suggested_threshold);
    std::cout << "\n─ SUGGESTED PARAMS BURST TEST END ─\n";
}

// ── unit tests ────────────────────────────────────────────────────────────────

static void test_single_producer_single_consumer(ETCS::MemoryArena& arena)
{
    section("Single Producer / Single Consumer");

    constexpr long long SLOT_COUNT = 16;
    auto* ring = ETCS::LMAXSequentialSharedPage::allocate(arena, 1, SLOT_COUNT);

    ETCS::LBuffer payload("hello ring");
    uint64_t seq = ring->write(42, payload);
    TEST("write returns valid sequence", seq != UINT64_MAX);
    TEST("sequence is 0 on first write", seq == 0);
    TEST("frameCount increments", ring->frameCount() == 1);
    TEST("publishedUpTo advanced to 0", ring->publishedUpTo() == 0);

    const ETCS::LBuffer* result = ring->acquireRead(0);
    TEST("acquireRead returns non-null for published frame", result != nullptr);
    TEST("payload content matches", result && std::string(result->c_str()) == "hello ring");

    ring->markConsumed(0);

    ETCS::LBuffer payload2("second lap");
    for (long long i = 1; i < SLOT_COUNT; ++i)
    {
        ETCS::LBuffer tmp("fill");
        ring->write(42, tmp);
        ring->markConsumed(i);
    }
    uint64_t seq2 = ring->write(42, payload2);
    TEST("slot 0 reused after full lap", seq2 == SLOT_COUNT);
    const ETCS::LBuffer* result2 = ring->acquireRead(SLOT_COUNT);
    TEST("lap-1 payload readable", result2 != nullptr);
    TEST("lap-1 payload content correct",
         result2 && std::string(result2->c_str()) == "second lap");
    ring->markConsumed(SLOT_COUNT);
}

static void test_ring_full_backpressure(ETCS::MemoryArena& arena)
{
    section("Ring Full / Backpressure");

    constexpr long long SLOT_COUNT = 8;
    auto* ring = ETCS::LMAXSequentialSharedPage::allocate(arena, 1, SLOT_COUNT);

    int written = 0;
    for (long long i = 0; i < SLOT_COUNT; ++i)
    {
        ETCS::LBuffer b("x");
        if (ring->write(1, b) != UINT64_MAX) ++written;
    }
    TEST("all slots filled", written == SLOT_COUNT);
    TEST("isFull() true when ring saturated", ring->isFull());

    ETCS::LBuffer overflow("overflow");
    uint64_t refused = ring->write(1, overflow);
    TEST("write refused when full (UINT64_MAX)", refused == UINT64_MAX);

    ring->acquireRead(0);
    ring->markConsumed(0);
    uint64_t after_consume = ring->write(1, overflow);
    TEST("write succeeds after one slot freed", after_consume != UINT64_MAX);
}

static void test_sequence_ordering(ETCS::MemoryArena& arena)
{
    section("Sequence Ordering");

    constexpr long long SLOT_COUNT = 16;
    auto* ring = ETCS::LMAXSequentialSharedPage::allocate(arena, 1, SLOT_COUNT);

    constexpr int N = 8;
    uint64_t seqs[N];
    for (int i = 0; i < N; ++i)
    {
        ETCS::LBuffer b;
        b << i;
        seqs[i] = ring->write(1, b);
    }

    bool monotonic = true;
    for (int i = 1; i < N; ++i)
        if (seqs[i] != seqs[i-1] + 1) { monotonic = false; break; }
    TEST("sequence numbers strictly monotonic", monotonic);
    TEST("publisher_cursor at N-1 after all writes", ring->publishedUpTo() == N - 1);

    bool ordered = true;
    for (int i = 0; i < N; ++i)
    {
        const ETCS::LBuffer* r = ring->acquireRead(seqs[i]);
        if (!r) { ordered = false; break; }
        int val = -1;
        ETCS::LBuffer copy = *r;
        copy >> val;
        if (val != i) { ordered = false; break; }
        ring->markConsumed(seqs[i]);
    }
    TEST("payloads readable in sequence order", ordered);
}

static void test_publisher_barrier_holds_consumer(ETCS::MemoryArena& arena)
{
    section("Publisher Barrier — Consumer Held Until Contiguous");

    constexpr long long SLOT_COUNT = 8;
    auto* ring = ETCS::LMAXSequentialSharedPage::allocate(arena, 1, SLOT_COUNT);

    std::atomic<bool> p0_go{false};
    std::atomic<bool> p1_done{false};

    std::thread p0([&]() {
        while (!p1_done.load(std::memory_order_acquire))
            std::this_thread::yield();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        ETCS::LBuffer b("seq0");
        ring->write(1, b);
        p0_go.store(true, std::memory_order_release);
    });

    std::thread p1([&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        ETCS::LBuffer b("seq1");
        ring->write(2, b);
        p1_done.store(true, std::memory_order_release);
    });

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    bool saw_1_before_barrier = false;

    while (!p0_go.load(std::memory_order_acquire))
    {
        if (ring->acquireRead(1) != nullptr)
        {
            uint64_t pub = ring->publishedUpTo();
            if (pub < 1) saw_1_before_barrier = true;
        }
        if (std::chrono::steady_clock::now() > deadline) break;
        std::this_thread::yield();
    }

    p0.join();
    p1.join();

    TEST("consumer cannot observe seq 1 before barrier reaches it",
         !saw_1_before_barrier);

    const ETCS::LBuffer* r0 = ring->acquireRead(0);
    const ETCS::LBuffer* r1 = ring->acquireRead(1);
    TEST("seq 0 readable after barrier advances", r0 != nullptr);
    TEST("seq 1 readable after barrier advances", r1 != nullptr);
    if (r0) ring->markConsumed(0);
    if (r1) ring->markConsumed(1);
}

static void test_tombstone(ETCS::MemoryArena& arena)
{
    section("Tombstone");

    constexpr long long SLOT_COUNT = 8;
    auto* ring = ETCS::LMAXSequentialSharedPage::allocate(arena, 1, SLOT_COUNT);

    ETCS::LBuffer b("before tombstone");
    ring->write(1, b);
    ring->tombstone();

    TEST("tombstoned flag set after tombstone()",
         ring->tombstoned.load(std::memory_order_relaxed));
}

static void test_reset(ETCS::MemoryArena& arena)
{
    section("Reset");

    constexpr long long SLOT_COUNT = 8;
    auto* ring = ETCS::LMAXSequentialSharedPage::allocate(arena, 1, SLOT_COUNT);

    for (int i = 0; i < 4; ++i)
    {
        ETCS::LBuffer b("data");
        uint64_t s = ring->write(1, b);
        ring->acquireRead(s);
        ring->markConsumed(s);
    }

    ring->reset();
    TEST("frameCount is 0 after reset", ring->frameCount() == 0);
    TEST("isFull() false after reset", !ring->isFull());
    TEST("publishedUpTo is UINT64_MAX sentinel after reset",
         ring->publishedUpTo() == UINT64_MAX);

    ETCS::LBuffer fresh("after reset");
    uint64_t seq = ring->write(1, fresh);
    TEST("write after reset returns seq 0", seq == 0);
    const ETCS::LBuffer* r = ring->acquireRead(0);
    TEST("read after reset returns correct payload",
         r && std::string(r->c_str()) == "after reset");
    ring->markConsumed(0);
}

static void test_multi_producer(ETCS::MemoryArena& arena)
{
    section("Multi-Producer Stress (Pinned)");

    constexpr long long SLOT_COUNT   = 64;
    constexpr int       PRODUCERS    = 4;
    constexpr int       PER_PRODUCER = 12;
    constexpr int       TOTAL        = PRODUCERS * PER_PRODUCER;
    const int           CORES        = get_core_count();

    auto* ring = ETCS::LMAXSequentialSharedPage::allocate(arena, 1, SLOT_COUNT);

    std::atomic<int> total_written{0};
    std::vector<std::thread> producers;

    for (int p = 0; p < PRODUCERS; ++p)
    {
        producers.emplace_back([&, p, CORES]() {
            pin_thread((p + 1) % CORES);
            for (int i = 0; i < PER_PRODUCER; ++i)
            {
                ETCS::LBuffer b;
                b << p << i;
                int retry = 0;
                while (ring->write(static_cast<uint64_t>(p), b) == UINT64_MAX)
                    ETCS::LMAXSequentialSharedPage::relax(retry);
                total_written.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    for (auto& t : producers) t.join();

    TEST("all producer writes landed", total_written.load() == TOTAL);
    TEST("frameCount matches total writes",
         ring->frameCount() == static_cast<uint64_t>(TOTAL));

    int consumed = 0;
    for (uint64_t s = 0; s < static_cast<uint64_t>(TOTAL); ++s)
    {
        const ETCS::LBuffer* r = nullptr;
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (!(r = ring->acquireRead(s)))
        {
            if (std::chrono::steady_clock::now() > deadline) break;
            std::this_thread::yield();
        }
        if (r) { ring->markConsumed(s); ++consumed; }
    }
    TEST("all frames consumed in strict sequence order", consumed == TOTAL);
}

static void test_writer_rid_preserved(ETCS::MemoryArena& arena)
{
    section("writer_rid Preservation");

    constexpr long long SLOT_COUNT = 8;
    auto* ring = ETCS::LMAXSequentialSharedPage::allocate(arena, 1, SLOT_COUNT);

    ETCS::LBuffer b("rid check");
    ring->write(0xDEADBEEF, b);

    const char* slot = ring->buffer;
    const ETCS::SequentialFrame* frame =
        reinterpret_cast<const ETCS::SequentialFrame*>(slot);

    while (!frame->ready.load(std::memory_order_acquire))
        std::this_thread::yield();

    TEST("writer_rid preserved in frame header", frame->writer_rid == 0xDEADBEEF);
    ring->acquireRead(0);
    ring->markConsumed(0);
}

static void test_lmax_blast_strict_consumer(ETCS::MemoryArena& arena, int TOTAL = 1000)
{
    section("LMAX Blast — Strict In-Order Consumer (Pinned)");

    constexpr long long SLOT_COUNT = 16;
    constexpr int       PRODUCERS  = 4;
    const int           CORES      = get_core_count();

    int PER_PRODUCER    = TOTAL / PRODUCERS;
    int EFFECTIVE_TOTAL = PER_PRODUCER * PRODUCERS;

    auto* ring = ETCS::LMAXSequentialSharedPage::allocate(arena, 1, SLOT_COUNT);

    std::atomic<bool> start_flag{false};
    std::atomic<int>  total_consumed{0};

    std::thread consumer([&]() {
        pin_thread(0);
        while (!start_flag.load(std::memory_order_acquire));
        uint64_t expected = 0;
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (total_consumed.load(std::memory_order_relaxed) < EFFECTIVE_TOTAL) {
            const ETCS::LBuffer* r = ring->acquireRead(expected);
            if (!r) {
                if (std::chrono::steady_clock::now() > deadline) break;
                std::this_thread::yield();
                continue;
            }
            ring->markConsumed(expected);
            ++expected;
            total_consumed.fetch_add(1, std::memory_order_relaxed);
        }
    });

    std::vector<std::thread> producers;
    for (int p = 0; p < PRODUCERS; ++p)
    {
        producers.emplace_back([&, p, CORES]() {
            pin_thread((p + 1) % CORES);
            while (!start_flag.load(std::memory_order_acquire));
            for (int i = 0; i < PER_PRODUCER; ++i) {
                ETCS::LBuffer b;
                b << p << i;
                int retry = 0;
                while (ring->write(static_cast<uint64_t>(p), b) == UINT64_MAX)
                    ETCS::LMAXSequentialSharedPage::relax(retry);
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

    TEST("blast: all messages consumed in strict order",
         total_consumed.load() == EFFECTIVE_TOTAL);
    std::cout << "  [INFO] " << EFFECTIVE_TOTAL << " messages in "
              << std::fixed << std::setprecision(3) << ms << "ms ("
              << ns << " ns) — " << static_cast<long long>(ops) << " ops/sec\n";
}

static void test_tuned_ring(ETCS::MemoryArena& arena, int PRODUCERS, int TOTAL = 500000)
{
    section("Tuned Ring");

    const int  CORES = get_core_count();
    const bool expect_large = PRODUCERS > ETCS_RING_LARGE_THRESHOLD;
    const long long expected_slots = expect_large
        ? ETCS_RING_LARGE_SLOTS
        : ETCS_RING_SMALL_SLOTS;

    std::cout << "  [INFO] producers=" << PRODUCERS
              << "  threshold=" << ETCS_RING_LARGE_THRESHOLD
              << "  → " << (expect_large ? "large" : "small") << " ring"
              << " (" << formatBytes(expected_slots * ETCS::LMAXSequentialSharedPage::SLOT_SIZE) << ")\n";

    auto* ring = ETCS::allocateTunedRing(arena, 1, PRODUCERS);

    // Verify the ring was allocated with the expected slot count
    // by checking isFull() after filling exactly expected_slots entries.
    // We drain as we go to avoid backpressure for large slot counts.
    TEST("tuned ring: correct slot count selected",
         ring->slot_count_ == expected_slots);

    // Functional stress: all messages arrive in order
    std::atomic<bool> start_flag{false};
    std::atomic<int>  total_consumed{0};

    std::thread consumer([&]() {
        pin_thread(0);
        while (!start_flag.load(std::memory_order_acquire));
        uint64_t expected = 0;
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
        while (total_consumed.load(std::memory_order_relaxed) < TOTAL) {
            const ETCS::LBuffer* r = ring->acquireRead(expected);
            if (!r) {
                if (std::chrono::steady_clock::now() > deadline) break;
                std::this_thread::yield();
                continue;
            }
            ring->markConsumed(expected);
            ++expected;
            total_consumed.fetch_add(1, std::memory_order_relaxed);
        }
    });

    std::vector<std::thread> producers;
    for (int p = 0; p < PRODUCERS; ++p)
    {
        producers.emplace_back([&, p]() {
            pin_thread((p + 1) % CORES);
            while (!start_flag.load(std::memory_order_acquire));
            int my_count = messages_for_producer(p, TOTAL, PRODUCERS);
            for (int i = 0; i < my_count; ++i) {
                ETCS::LBuffer b; b << p << i;
                int retry = 0;
                while (ring->write(static_cast<uint64_t>(p), b) == UINT64_MAX)
                    ETCS::LMAXSequentialSharedPage::relax(retry);
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

    TEST("tuned ring: all messages consumed in strict order",
         total_consumed.load() == TOTAL);
    std::cout << "  [INFO] " << formatWithCommas(TOTAL) << " messages in "
              << std::fixed << std::setprecision(3) << ms << "ms — "
              << formatWithCommas(static_cast<long long>(ops)) << " ops/sec\n";
}

// ── main ──────────────────────────────────────────────────────────────────────

int main(int argc, char* argv[])
{
    WIRE_CONTEXT();

    ETCS::MemoryArena& arena = ETCS::MemoryArena::getInstance();

    std::cout << "LMAXSequentialSharedPage test suite\n";
    std::cout << "arena chunk size:  " << arena.getChunkSize() << " bytes\n";
    std::cout << "huge pages:        " << (arena.isUsingHugePages() ? "yes" : "no") << "\n";
    std::cout << "SLOT_SIZE:         " << ETCS::LMAXSequentialSharedPage::SLOT_SIZE << " bytes\n";

    // Core correctness tests — always run regardless of mode
    test_single_producer_single_consumer(arena);
    test_ring_full_backpressure(arena);
    test_sequence_ordering(arena);
    test_publisher_barrier_holds_consumer(arena);
    test_tombstone(arena);
    test_reset(arena);
    test_multi_producer(arena);
    test_writer_rid_preserved(arena);
    test_lmax_blast_strict_consumer(arena);
    test_lmax_blast_strict_consumer(arena, 10000);
    test_lmax_blast_strict_consumer(arena, 100000);
    test_lmax_blast_strict_consumer(arena, 1000000);

    int mode = 3; // default: tuned ring
    if (argc >= 2) mode = std::atoi(argv[1]);

    std::cout << "\n────────────────────────────────────\n";

    if (mode == 1)
    {
        std::cout << "[mode 1] autotune slot count\n";
        test_autotune_slot_count(arena, 4,  1500000);
        test_autotune_slot_count(arena, 6,  1500000);
        test_autotune_slot_count(arena, 7,  1500000);
        test_autotune_slot_count(arena, 8,  1500000);
        test_autotune_slot_count(arena, 15, 1500000);
        test_autotune_slot_count(arena, 32, 1500000);
    }
    else if (mode == 2)
    {
        std::cout << "[mode 2] grand tournament\n";
        test_autotune_grand_tournament(arena);
    }
    else
    {
        std::cout << "[mode 3] tuned ring  (threshold=" << ETCS_RING_LARGE_THRESHOLD
                  << "  small=" << formatBytes(ETCS_RING_SMALL_SLOTS * ETCS::LMAXSequentialSharedPage::SLOT_SIZE)
                  << "  large=" << formatBytes(ETCS_RING_LARGE_SLOTS * ETCS::LMAXSequentialSharedPage::SLOT_SIZE)
                  << ")\n";
        for (int p : {1, 2, 4, 5, ETCS_RING_LARGE_THRESHOLD,
                      ETCS_RING_LARGE_THRESHOLD + 1, 8, 10, 12, 16, 32})
            test_tuned_ring(arena, p, 1500000);
    }

    std::cout << "\n────────────────────────────────────\n";
    test_arbitrary_producer_scaling(arena);
    std::cout << "\n────────────────────────────────────\n";
    test_execute_burst_aware_scaling(arena);
    std::cout << "\n────────────────────────────────────\n";
    test_mixed_traffic(arena, 8, std::chrono::milliseconds(250));
    std::cout << "\n────────────────────────────────────\n";
    std::cout << "passed: " << s_passed << "\n";
    std::cout << "failed: " << s_failed << "\n";

    return s_failed == 0 ? 0 : 1;
}
