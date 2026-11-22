// PlatformTesterLoader.cc
//
// THE EDGES THE CORE FORKS ON PER OS, held under load. Every platform runs it;
// Windows is why it exists (each section is a bug found there), and the
// native run is its baseline.
//
//   1. THREADS THAT END. Many short threads write the module log, round after
//      round: every heap stays valid, and every line arrives whole.
//   2. A PIPE THAT FILLS. A burst many times the pipe's buffer, its reader
//      late: it all arrives, in time, byte for byte.
//   3. A ROUND TRIP. Small messages there and back over two pipes: the median
//      well under one tick of a stock Windows timer (15.6 ms).
//   4. THE STORE. Absolute, and the same from any working directory.
//   5. ENTITIES THAT COME AND GO. Batches spawned and deleted. Once warm and
//      read at rest, the handle count, the memory (an arena reuses same-size
//      blocks: a high water mark, not a slope) and the address space (a
//      finished thread's stack is given back) are where they were.
//   6. THE TIMER. A millisecond's sleep, measured; reported, not judged.
//
//   ./Run_PlatformTesterLoader [scale]     scale multiplies the counts (a soak)
//
#include "../ETCS.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#  include <malloc.h>
#else
#  include <dirent.h>
#  include <unistd.h>
#endif

using Clock = std::chrono::steady_clock;
static double ms_since(Clock::time_point t) { return std::chrono::duration<double, std::milli>(Clock::now() - t).count(); }

static int g_fail = 0;
static void check(bool ok, const std::string& what)
{
    std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", what.c_str());
    if (!ok) ++g_fail;
}

static std::string verb(ETCS::Entity* e, const std::string& action, const std::string& args = "")
{
    ETCS::Buffer a(action.c_str()), d(args.c_str());
    e->call(a, d);
    return d.toString();
}

// Every heap the process has, checked now (Windows); elsewhere the allocator
// aborts on what it finds, so there is nothing to ask.
static bool heaps_valid()
{
#if defined(_WIN32)
    HANDLE heaps[256];
    const DWORD n = GetProcessHeaps(256, heaps);
    for (DWORD i = 0; i < n && i < 256; ++i)
        if (!HeapValidate(heaps[i], 0, nullptr)) return false;
    return _heapchk() == _HEAPOK;
#else
    return true;
#endif
}

static long handles()
{
#if defined(_WIN32)
    DWORD n = 0;
    return GetProcessHandleCount(GetCurrentProcess(), &n) ? long(n) : -1;
#else
    long n = 0;
    if (DIR* d = opendir("/proc/self/fd")) { while (readdir(d)) ++n; closedir(d); }
    return n;
#endif
}

// What memory costs the system, in bytes: committed private pages on Windows
// (what the page file backs), resident pages elsewhere.
static long long committed()
{
#if defined(_WIN32)
    long long total = 0;
    MEMORY_BASIC_INFORMATION mbi;
    for (char* at = nullptr; VirtualQuery(at, &mbi, sizeof mbi) == sizeof mbi; at = static_cast<char*>(mbi.BaseAddress) + mbi.RegionSize)
        if (mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE) total += static_cast<long long>(mbi.RegionSize);
    return total;
#else
    long long size = 0, resident = 0;
    std::ifstream f("/proc/self/statm");
    return (f >> size >> resident) ? resident * sysconf(_SC_PAGESIZE) : -1;
#endif
}

// Address space reserved, committed or not, in bytes.
static long long reserved()
{
#if defined(_WIN32)
    long long total = 0;
    MEMORY_BASIC_INFORMATION mbi;
    for (char* at = nullptr; VirtualQuery(at, &mbi, sizeof mbi) == sizeof mbi; at = static_cast<char*>(mbi.BaseAddress) + mbi.RegionSize)
        if (mbi.State != MEM_FREE && mbi.Type == MEM_PRIVATE) total += static_cast<long long>(mbi.RegionSize);
    return total;
#else
    long long size = 0;
    std::ifstream f("/proc/self/statm");
    return (f >> size) ? size * sysconf(_SC_PAGESIZE) : -1;
#endif
}

static std::string pattern(size_t n, unsigned seed)
{
    std::string s(n, '\0');
    uint32_t x = seed * 2654435761u + 1;
    for (size_t i = 0; i < n; ++i) { x ^= x << 13; x ^= x >> 17; x ^= x << 5; s[i] = static_cast<char>(x); }
    return s;
}

// A Pipe pair, torn down with it. Raising `flag` makes either end give up.
struct PipePair
{
    ETCS::SignalFlag    flag{ 0 };
    ETCS::SignalContext ctx;
    ETCS::MirrorBuffer  prod, cons;
    ETCS::SharedPage*   page = nullptr;
    PipePair()
    {
        ctx.interrupt = &flag;
        prod.bindContext(ctx);
        cons.bindContext(ctx);
        page = ETCS::SharedPage::allocate(ETCS::MemoryArena::getInstance(), 2);
        ETCS::MirrorBuffer::makePair<ETCS::StrategyPipe, ETCS::SharedPage>(prod, cons, page, 1, 2);
    }
    ~PipePair() { ETCS::MirrorBuffer::teardownPair<ETCS::StrategyPipe, ETCS::SharedPage>(prod, cons, page); }
};

// Raises a pair's flag after `ms`, unless dismissed first.
struct Deadline
{
    std::atomic<bool> done{ false };
    std::thread t;
    Deadline(ETCS::SignalFlag& flag, int ms) : t([this, &flag, ms] {
        const auto end = Clock::now() + std::chrono::milliseconds(ms);
        while (!done && Clock::now() < end) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        if (!done) flag.store(1);
    }) {}
    ~Deadline() { done = true; t.join(); }
};

int main(int argc, char** argv)
{
    WIRE_CONTEXT();
    const int scale = argc > 1 ? std::max(1, std::atoi(argv[1])) : 1;
    std::printf("=== The platform's edges, scale %d ===\n", scale);

    // -- 1. threads that end ---------------------------------------------------
    std::printf("\n-- 1. threads that end --\n");
    {
        const int rounds = 20 * scale, threads = 16, lines = 40;
        const std::string mark = "PLATFORM-" + std::to_string(std::chrono::system_clock::now().time_since_epoch().count());
        const std::string pad(200, 'x');
        const bool was = ETCS::get_log_to_file();
        ETCS::set_log_to_file(true);
        int bad_rounds = 0;
        const auto t0 = Clock::now();
        for (int r = 0; r < rounds; ++r)
        {
            std::vector<std::thread> ts;
            for (int t = 0; t < threads; ++t)
                ts.emplace_back([&, r, t] {
                    for (int l = 0; l < lines; ++l)
                        ETCS_LOG("Platform", mark << " r" << r << " t" << t << " l" << l << " " << pad << " END");
                });
            for (auto& t : ts) t.join();
            if (!heaps_valid()) ++bad_rounds;
        }
        const double took = ms_since(t0);
        ETCS::set_log_to_file(was);
        std::printf("        (%d threads in %.0f ms, %.0f lines a second)\n", rounds * threads, took,
                    1000.0 * rounds * threads * lines / took);
        check(bad_rounds == 0, "a thread that wrote the log and ended leaves every heap valid, every round");

        std::string path = getCurrentModulePath();
        const size_t slash = path.find_last_of("/\\");
        if (slash != std::string::npos) path = path.substr(slash + 1);
        if (const size_t dot = path.rfind('.'); dot != std::string::npos) path = path.substr(0, dot);
        std::ifstream log("logs/" + path + ".log");
        long whole = 0, torn = 0;
        for (std::string line; std::getline(log, line);)
        {
            const size_t at = line.find(mark);
            if (at == std::string::npos) continue;
            const bool one = line.find(mark, at + 1) == std::string::npos;
            const bool ends = line.size() >= 4 && line.compare(line.size() - 4, 4, " END") == 0;
            (one && ends ? whole : torn)++;
        }
        std::printf("        (%ld whole, %ld torn, of %d)\n", whole, torn, rounds * threads * lines);
        check(whole == long(rounds) * threads * lines && torn == 0,
              "...and every line it wrote is in the log, whole: none lost, none run into another");
    }

    // -- 2. a pipe that fills --------------------------------------------------
    std::printf("\n-- 2. a pipe that fills --\n");
    {
        const size_t piece = 64 * 1024;
        const int pieces = 128 * scale;                       // 8 MiB a scale
        PipePair p;
        Deadline limit(p.flag, 20000 * scale);
        std::vector<std::string> sent;
        for (int i = 0; i < pieces; ++i) sent.push_back(pattern(piece, unsigned(i + 1)));
        std::atomic<int> wrote{ 0 };
        std::thread w([&] { for (auto& s : sent) { if (!p.prod.writeMessage(s)) break; ++wrote; } p.prod.closeWrite(); });
        std::this_thread::sleep_for(std::chrono::milliseconds(300));   // the writer fills the pipe first
        const auto t0 = Clock::now();
        int same = 0;
        for (int i = 0; i < pieces; ++i)
        {
            std::string got;
            if (!p.cons.readMessage(got)) break;
            same += got == sent[size_t(i)];
        }
        const double took = ms_since(t0);
        w.join();
        std::printf("        (%d of %d pieces in %.0f ms after the reader started, %.1f MB/s)\n",
                    same, pieces, took, (double(same) * piece / 1e6) / (took / 1000.0));
        check(p.flag.load() == 0, "a burst many times the pipe's buffer, read late, finishes before its deadline");
        check(wrote.load() == pieces && same == pieces, "...every piece written, and read back byte for byte, in order");
    }

    // -- 3. a round trip -------------------------------------------------------
    std::printf("\n-- 3. a round trip --\n");
    {
        const int n = 1000 * scale;
        PipePair there, back;
        Deadline limit(there.flag, 60000 * scale);
        std::thread echo([&] {
            for (std::string m; there.cons.readMessage(m);)
                if (!back.prod.writeMessage(m)) break;
            back.prod.closeWrite();
        });
        std::vector<double> rtt;
        rtt.reserve(size_t(n));
        const std::string ping = pattern(32, 7);
        for (int i = 0; i < n && !there.flag.load(); ++i)
        {
            const auto t0 = Clock::now();
            std::string pong;
            if (!there.prod.writeMessage(ping) || !back.cons.readMessage(pong) || pong != ping) break;
            rtt.push_back(ms_since(t0));
        }
        there.prod.closeWrite();
        echo.join();
        std::sort(rtt.begin(), rtt.end());
        const double med = rtt.empty() ? 1e9 : rtt[rtt.size() / 2];
        const double p99 = rtt.empty() ? 1e9 : rtt[std::min(rtt.size() - 1, rtt.size() * 99 / 100)];
        std::printf("        (%zu round trips: median %.3f ms, 99th %.3f ms, worst %.3f ms)\n",
                    rtt.size(), med, p99, rtt.empty() ? 0.0 : rtt.back());
        check(int(rtt.size()) == n, "every message there comes back, the same");
        check(med < 5.0, "...the median round trip well under one stock Windows timer tick (15.6 ms)");
    }

    // -- 4. the store ----------------------------------------------------------
    std::printf("\n-- 4. the store --\n");
    {
        namespace fs = std::filesystem;
        const std::string here = ETCS::etcs_store_dir();
        std::printf("        (%s)\n", here.c_str());
        check(fs::path(here).is_absolute(), "the store is an absolute path");
        std::error_code ec;
        const fs::path was = fs::current_path(ec);
        fs::current_path(fs::temp_directory_path(ec), ec);
        const std::string there = ETCS::etcs_store_dir();
        fs::current_path(was, ec);
        check(!ec && there == here, "...and the same from another working directory: one key, wherever it is launched");
    }

    // -- 5. entities that come and go ------------------------------------------
    std::printf("\n-- 5. entities that come and go --\n");
    {
        const int batch = 200 * scale;
        int made = 0;
        auto cycle = [&]() {
            for (int i = 0; i < batch; ++i)
            {
                ETCS::Entity* e = ETCS::spawn_entity("LayoutProvider", "Layout", env, loader);
                if (!e) return;
                verb(e, "Layout.Create", "64 64");
                verb(e, "Layout.Delete");                           // its own release; not its removal
                e->getOwningArena().deleteEntity(e, true);
                ++made;
            }
        };
        // Read at rest. Each emptying of the module starts a recheck thread
        // that lives a fifth of a second, so dozens are alive mid-batch, stacks
        // and all; what must not stay is a finished one's. Waiting lets them
        // finish, and one more cycle has the registry reap.
        struct Rest { long handles; long long memory, space; };
        auto at_rest = [&]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(600));
            if (ETCS::Entity* e = ETCS::spawn_entity("LayoutProvider", "Layout", env, loader)) e->getOwningArena().deleteEntity(e, true);
            std::this_thread::sleep_for(std::chrono::milliseconds(600));
            return Rest{ handles(), committed(), reserved() };
        };
        cycle();                                                    // warm: the module loaded, the arena grown
        const Rest a = at_rest();
        const auto t0 = Clock::now();
        for (int i = 0; i < 4; ++i) cycle();
        const double took = ms_since(t0);
        const Rest b = at_rest();
        const double MiB = 1024.0 * 1024.0;
        std::printf("        (%d spawned and deleted; warm, %.3f ms each; at rest: handles %ld -> %ld, memory %+.1f MiB, address space %+.1f MiB)\n",
                    made, took / (4.0 * batch), a.handles, b.handles, double(b.memory - a.memory) / MiB, double(b.space - a.space) / MiB);
        check(made == 5 * batch, "every one spawns");
        if (a.handles > 0) check(b.handles - a.handles <= 8, "...and once warm, four more batches leave the handle count where they found it");
        else std::printf("  --    (this platform does not count its handles; Wine answers 0)\n");
        check(a.memory > 0 && b.memory - a.memory <= 16ll * 1024 * 1024,
              "...the memory at its high water mark: at rest, within 16 MiB of where it was");
        check(a.space > 0 && b.space - a.space <= 64ll * 1024 * 1024,
              "...and the address space within 64 MiB: no thread's stack outlives its thread");
    }

    // -- 6. the timer ----------------------------------------------------------
    std::printf("\n-- 6. the timer --\n");
    {
        double sum = 0, worst = 0;
        const int n = 100;
        for (int i = 0; i < n; ++i)
        {
            const auto t0 = Clock::now();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            const double d = ms_since(t0);
            sum += d; worst = std::max(worst, d);
        }
        std::printf("        (a 1 ms sleep: %.3f ms on average, %.3f at worst)\n", sum / n, worst);
    }

    ETCS::PendingUnloadRegistry::getInstance().join_all();
    std::printf("\n%s (%d failure%s)\n", g_fail ? "FAILED" : "PASSED", g_fail, g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}
