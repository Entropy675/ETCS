#include "../ETCS.h"
#include "../core/LMAXSequentialSharedPage.h"
#include <thread>
#include <vector>
#include <deque>
#include <atomic>
#include <mutex>
#include <iostream>
#include <iomanip>
#include <sstream>
#include <string>
#include <chrono>
#include <random>
#include <algorithm>
#include <pthread.h>

// ═════════════════════════════════════════════════════════════════════════════
// scstest — SocketConnectionState's claim/reset/finalize discipline, hammered.
//
// WHAT THIS TESTS, AND WHAT IT DOES NOT
//
// This is a MODEL of the state machine in SocketConnectionState.h, not that
// class itself. Driving the real type would mean standing up an arena, an
// entity tree, a ConnectionManager to mint children, mbedtls and io_uring --
// at which point the thing under test is the whole module and the races are
// buried under everything else. What actually needs testing is narrower and
// entirely expressible here: the PROTOCOL of Phase transitions and
// io_inflight_ accounting, and the invariants that protocol is supposed to
// maintain when several threads reach it at once.
//
// The cost of that choice is real and worth stating plainly: if the model
// drifts from SocketConnectionState.h, this suite passes while the production
// code is broken. The model below is written to mirror that file operation
// for operation, and the comments name the corresponding function at each
// step, so a change there is visible as a divergence here. Treat those names
// as the contract.
//
// WHY THE BUG VARIANTS EXIST
//
// A harness for a discipline like this is worthless unless it can be shown to
// FAIL on a violation. So the suite deliberately builds broken variants and
// asserts that each one is caught. If a bug variant ever passes, the harness
// has gone blind and its green result on the correct variant means nothing.
// That meta-check is itself a reported test.
//
// The three variants are not invented failure modes. They are the three
// mistakes actually made while building the TLS termination path:
//
//   LostRelease    -- a path that releases nothing and notifies nobody,
//                     modelled on the first draft of TLSIO::FlushCipherOut,
//                     which balanced its own count on a failed submission but
//                     never invoked the continuation. The count stayed
//                     correct; the caller's state machine simply never learned
//                     it had failed, so a connection sat claimed forever.
//                     Balanced-but-stranded is the hardest of the three to
//                     see by reading.
//
//   DoubleRelease  -- one extra NoteComplete on a path. The count reaches
//                     zero while work is still outstanding, the connection is
//                     republished as Free, and another thread claims it while
//                     the first is still using it. This is the failure the
//                     whole discipline exists to prevent, and the one an
//                     arena makes silent rather than fatal: recycled entity
//                     memory is valid and WRONG, not a segfault.
//
//   FreeOnRecycle  -- tearing the TLS session down in RecycleForNextRequest
//                     instead of finalizeIfDraining. Keep-alive then
//                     re-handshakes on every request, or worse, reads session
//                     state that has been freed under it.
//
// BUILD
//   g++ -std=c++17 -O1 -g -pthread -fsanitize=thread scstest.cc -o scstest
//   g++ -std=c++17 -O1 -g -pthread -fsanitize=address,undefined scstest.cc -o scstest
//
// The invariant checks below catch the ordering and accounting faults on
// their own, without sanitizers. TSan adds detection of genuine data races on
// the fields themselves, which is a different class of bug and worth a
// separate run. Run both.
// ═════════════════════════════════════════════════════════════════════════════

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

static std::string formatWithCommas(long long n)
{
    std::string s = std::to_string(n);
    int pos   = static_cast<int>(s.length()) - 3;
    int limit = (n < 0) ? 1 : 0;
    while (pos > limit) { s.insert(pos, ","); pos -= 3; }
    return s;
}

static const std::string RESET      = "\033[0m";
static const std::string FG_GREEN   = "\033[1;32m";
static const std::string FG_CYAN    = "\033[1;36m";
static const std::string FG_RED     = "\033[31m";
static const std::string FG_YELLOW  = "\033[1;33m";
static const std::string FG_MAGENTA = "\033[1;35m";

// ── the variant under test ────────────────────────────────────────────────────

enum class Variant { Correct, LostRelease, DoubleRelease, FreeOnRecycle };

static const char* variant_name(Variant v)
{
    switch (v)
    {
        case Variant::Correct:       return "correct";
        case Variant::LostRelease:   return "bug1: lost release (balanced, stranded)";
        case Variant::DoubleRelease: return "bug2: double release";
        case Variant::FreeOnRecycle: return "bug3: TLS freed on recycle";
    }
    return "?";
}

// ── violation recording ───────────────────────────────────────────────────────
//
// Counted rather than asserted: an assert aborts on the first violation and
// tells you nothing about whether it was a one-in-a-million race or happening
// constantly. The count is the useful signal, and the first message is kept
// for diagnosis.

struct Violations
{
    std::atomic<long long> count{0};
    std::mutex             first_mutex;
    std::string            first;

    void record(const std::string& what)
    {
        if (count.fetch_add(1, std::memory_order_relaxed) == 0)
        {
            std::lock_guard<std::mutex> lk(first_mutex);
            if (first.empty()) first = what;
        }
    }
};

static Violations g_violations;

// ── the model ─────────────────────────────────────────────────────────────────
//
// Mirrors SocketConnectionState.h. Every method below names the one it stands
// in for; if those diverge, this test is measuring the wrong thing.

struct alignas(64) PooledConn
{
    enum class Phase : uint8_t { Free, Serving, Draining, Clearing };

    // ── the two fields the whole discipline turns on ──────────────────────────
    std::atomic<Phase> phase{Phase::Free};
    // Seeded to 1, exactly as SocketConnectionState does: the dispatch
    // reference exists BEFORE the claimer is handed the connection, so there
    // is no window in which a claimed connection has a zero count and could
    // be finalized out from under its new owner.
    std::atomic<int>   io_inflight{1};

    // ── stand-in for the TLS session ─────────────────────────────────────────
    // tls_live models "the mbedtls_ssl_context is set up". epoch lets a reader
    // notice it was torn down and rebuilt underneath it, which is what a
    // re-handshake would look like from the request's point of view.
    std::atomic<bool>     tls_live{false};
    std::atomic<uint64_t> tls_epoch{0};

    // ── ownership token ──────────────────────────────────────────────────────
    // Stamped by the claimer, checked by that claimer on every subsequent
    // touch. This is what detects a connection being recycled while someone
    // still believes they hold it -- the exact hazard the arena makes silent,
    // since reclaimed entity memory reads as valid rather than faulting.
    std::atomic<uint64_t> owner_epoch{0};

    // ── instrumentation ──────────────────────────────────────────────────────
    std::atomic<long long> claims{0};
    std::atomic<long long> finalizes{0};
    std::atomic<long long> tls_inits{0};
    std::atomic<long long> tls_frees{0};

    // SocketConnectionState::TryClaim
    bool TryClaim(uint64_t epoch)
    {
        Phase expect = Phase::Free;
        if (!phase.compare_exchange_strong(expect, Phase::Serving,
                                           std::memory_order_acq_rel))
            return false;

        // Inherited, never re-seeded here: finalizeIfDraining set it to 1
        // before publishing Free. Re-seeding would be the bug that window
        // exists to avoid.
        int n = io_inflight.load(std::memory_order_acquire);
        if (n != 1)
            g_violations.record("claimed a connection whose io_inflight was "
                                + std::to_string(n) + ", expected the seeded 1");

        owner_epoch.store(epoch, std::memory_order_release);
        claims.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    // SocketConnectionState::NoteSubmit
    void NoteSubmit()
    {
        io_inflight.fetch_add(1, std::memory_order_acq_rel);
    }

    // SocketConnectionState::NoteComplete
    void NoteComplete(Variant v)
    {
        int prev = io_inflight.fetch_sub(1, std::memory_order_acq_rel);
        if (prev <= 0)
        {
            g_violations.record("io_inflight went negative (was " + std::to_string(prev)
                                + ") -- a reference was released twice");
            return;
        }
        if (prev == 1) finalizeIfDraining(v);
    }

    // SocketConnectionState::TLSServerContext init, via GetTLS().Init
    void TLSInit()
    {
        tls_epoch.fetch_add(1, std::memory_order_acq_rel);
        tls_live.store(true, std::memory_order_release);
        tls_inits.fetch_add(1, std::memory_order_relaxed);
    }

    void TLSFree()
    {
        if (tls_live.exchange(false, std::memory_order_acq_rel))
            tls_frees.fetch_add(1, std::memory_order_relaxed);
    }

    // SocketConnectionState::RecycleForNextRequest
    //
    // Per-REQUEST state only. The TLS session belongs to the CONNECTION and
    // must survive this -- keep-alive means many requests over one handshake.
    // Variant::FreeOnRecycle puts the teardown here, which is the mistake.
    void RecycleForNextRequest(Variant v)
    {
        // (parser reset, buffer lengths, page rid -- elided; none of it
        //  participates in the accounting this test is about)
        if (v == Variant::FreeOnRecycle)
            TLSFree();
    }

    // SocketConnectionState::ResetConcrete
    bool Reset(Variant v)
    {
        Phase expect = Phase::Serving;
        if (!phase.compare_exchange_strong(expect, Phase::Draining,
                                           std::memory_order_acq_rel))
            return true;   // already draining or free -- idempotent, as in the real one

        // The real one submits an io_uring cancel here so pending recv/send
        // surface as -ECANCELED promptly. Nothing to model: the completions
        // in this harness are already going to arrive.
        finalizeIfDraining(v);
        return true;
    }

    // SocketConnectionState::finalizeIfDraining
    void finalizeIfDraining(Variant v)
    {
        if (io_inflight.load(std::memory_order_acquire) != 0) return;

        Phase expect = Phase::Draining;
        if (!phase.compare_exchange_strong(expect, Phase::Clearing,
                                           std::memory_order_acq_rel))
            return;   // someone else is finalizing, or we were never draining

        finalizes.fetch_add(1, std::memory_order_relaxed);

        // THE place a TLS session is torn down, and the only one.
        if (v != Variant::FreeOnRecycle) TLSFree();

        owner_epoch.store(0, std::memory_order_release);

        // Seed the NEXT claimer's dispatch reference BEFORE publishing Free.
        // Reversing these two stores is a race with a real name: a claimer
        // that wins between them sees a zero count and can be finalized
        // immediately by an unrelated completion.
        io_inflight.store(1, std::memory_order_release);

        // LAST. Nothing may touch this object after this store.
        phase.store(Phase::Free, std::memory_order_release);
    }

    // Called by an owner before every touch: has anyone recycled this out
    // from under us?
    void assertStillOurs(uint64_t epoch, const char* where)
    {
        uint64_t now = owner_epoch.load(std::memory_order_acquire);
        if (now != epoch)
            g_violations.record(std::string("connection recycled under a live owner at ")
                                + where + " (expected epoch " + std::to_string(epoch)
                                + ", found " + std::to_string(now) + ")");
    }
};

// ── the completion queue ──────────────────────────────────────────────────────
//
// Completions arrive on OTHER threads than the one that submitted, which is
// the entire reason this discipline is hard. A mutex here is deliberate: it
// is not what is under test, and using a lock-free queue would only add a
// second thing that could be wrong.

struct CompletionQueue
{
    std::mutex                                    m;
    std::deque<std::pair<PooledConn*, Variant>>   q;
    std::atomic<bool>                             running{true};

    void push(PooledConn* c, Variant v)
    {
        std::lock_guard<std::mutex> lk(m);
        q.emplace_back(c, v);
    }

    bool pop(PooledConn*& c, Variant& v)
    {
        std::lock_guard<std::mutex> lk(m);
        if (q.empty()) return false;
        c = q.front().first;
        v = q.front().second;
        q.pop_front();
        return true;
    }

    bool empty()
    {
        std::lock_guard<std::mutex> lk(m);
        return q.empty();
    }
};

// ── the hammer ────────────────────────────────────────────────────────────────

struct HammerReport
{
    long long violations   = 0;
    long long claims       = 0;
    long long finalizes    = 0;
    long long tls_inits    = 0;
    long long tls_frees    = 0;
    long long stuck        = 0;   // connections not Free at the end
    long long bad_count    = 0;   // connections whose final count isn't the seeded 1
    std::string first_violation;
    double     ms          = 0.0;
};

static HammerReport run_hammer(Variant v,
                               int pool_size,
                               int owner_threads,
                               int completion_threads,
                               int iters_per_owner,
                               bool tls)
{
    g_violations.count.store(0, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lk(g_violations.first_mutex);
        g_violations.first.clear();
    }

    std::vector<PooledConn> pool(pool_size);
    CompletionQueue         cq;
    std::atomic<uint64_t>   epoch_gen{1};
    std::atomic<bool>       start_flag{false};
    const int               CORES = get_core_count();

    // Completion threads: drain the queue and release references. These are
    // the io_uring completion workers.
    std::vector<std::thread> completers;
    for (int i = 0; i < completion_threads; ++i)
    {
        completers.emplace_back([&, i]() {
            pin_thread((i + 1) % CORES);
            while (!start_flag.load(std::memory_order_acquire));
            int retry = 0;
            while (cq.running.load(std::memory_order_acquire) || !cq.empty())
            {
                PooledConn* c = nullptr;
                Variant     cv;
                if (!cq.pop(c, cv))
                {
                    ETCS::LMAXSequentialSharedPage::progressiveYield(retry);
                    continue;
                }
                retry = 0;
                c->NoteComplete(cv);
            }
        });
    }

    // Owner threads: claim, serve some requests, reset, release.
    std::vector<std::thread> owners;
    for (int t = 0; t < owner_threads; ++t)
    {
        owners.emplace_back([&, t]() {
            pin_thread((completion_threads + t + 1) % CORES);
            while (!start_flag.load(std::memory_order_acquire));

            std::mt19937 gen(1234567u + t);
            std::uniform_int_distribution<> req_dist(1, 4);   // keep-alive requests
            std::uniform_int_distribution<> slot_dist(0, pool_size - 1);

            for (int it = 0; it < iters_per_owner; ++it)
            {
                // Acquire: rotate through the pool looking for a Free slot,
                // exactly as ConnectionManager::acquireConnection does.
                PooledConn* c     = nullptr;
                uint64_t    epoch = 0;
                int         start = slot_dist(gen);
                for (int probe = 0; probe < pool_size && !c; ++probe)
                {
                    PooledConn& cand = pool[(start + probe) % pool_size];
                    uint64_t e = epoch_gen.fetch_add(1, std::memory_order_relaxed);
                    if (cand.TryClaim(e)) { c = &cand; epoch = e; }
                }
                if (!c) { std::this_thread::yield(); continue; }  // pool exhausted

                if (tls) c->TLSInit();

                const int requests = req_dist(gen);
                for (int r = 0; r < requests; ++r)
                {
                    c->assertStillOurs(epoch, "before submit");

                    if (tls && !c->tls_live.load(std::memory_order_acquire))
                        g_violations.record("TLS session gone mid-connection -- "
                                            "a keep-alive request would re-handshake "
                                            "or read freed session state");

                    // Arm an operation and hand its completion to another
                    // thread. This is NoteSubmit paired with the completion's
                    // own NoteComplete.
                    c->NoteSubmit();

                    if (v == Variant::LostRelease && r == 0 && (it & 7) == 0)
                    {
                        // The reference is accounted for -- we undo our own
                        // NoteSubmit -- but the continuation is never invoked,
                        // so nothing downstream learns this request happened.
                        // The connection keeps its dispatch reference forever
                        // and never returns to the pool. Counts stay balanced
                        // the whole time, which is what makes it invisible to
                        // a reader checking only that submits match completes.
                        c->NoteComplete(v);
                        goto stranded;
                    }

                    cq.push(c, v);

                    if (v == Variant::DoubleRelease && r == 0 && (it & 7) == 0)
                    {
                        // One extra release. The count reaches zero while the
                        // queued completion is still pending, the connection
                        // is republished, and another owner claims it while we
                        // are still in this loop.
                        c->NoteComplete(v);
                    }

                    if (r + 1 < requests) c->RecycleForNextRequest(v);
                }

                c->assertStillOurs(epoch, "before reset");
                c->Reset(v);
                // The dispatch reference this owner inherited from TryClaim,
                // released exactly once, last -- ConnectionManager::onConnection's
                // own trailing NoteComplete.
                c->NoteComplete(v);

            stranded:;
            }
        });
    }

    auto t0 = std::chrono::high_resolution_clock::now();
    start_flag.store(true, std::memory_order_release);
    for (auto& t : owners) t.join();

    // Owners are done; let the completion threads drain what is left.
    cq.running.store(false, std::memory_order_release);
    for (auto& t : completers) t.join();
    auto t1 = std::chrono::high_resolution_clock::now();

    HammerReport rep;
    rep.ms = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count() / 1e6;

    for (auto& c : pool)
    {
        rep.claims    += c.claims.load();
        rep.finalizes += c.finalizes.load();
        rep.tls_inits += c.tls_inits.load();
        rep.tls_frees += c.tls_frees.load();
        if (c.phase.load() != PooledConn::Phase::Free) ++rep.stuck;
        if (c.io_inflight.load() != 1)                 ++rep.bad_count;
    }

    rep.violations = g_violations.count.load();
    {
        std::lock_guard<std::mutex> lk(g_violations.first_mutex);
        rep.first_violation = g_violations.first;
    }
    return rep;
}

// A variant is CLEAN when every invariant holds: no recorded violation, every
// claim was matched by exactly one finalize, every connection is back in the
// pool with its seeded reference, and every TLS session that was set up was
// torn down exactly once.
static bool report_is_clean(const HammerReport& r, bool tls)
{
    if (r.violations != 0)          return false;
    if (r.stuck      != 0)          return false;
    if (r.bad_count  != 0)          return false;
    if (r.claims     != r.finalizes) return false;
    if (tls && r.tls_inits != r.tls_frees) return false;
    return true;
}

static void print_report(const HammerReport& r, bool tls)
{
    std::cout << "    claims " << std::setw(10) << formatWithCommas(r.claims)
              << "  finalizes " << std::setw(10) << formatWithCommas(r.finalizes)
              << "  stuck " << r.stuck
              << "  bad_count " << r.bad_count;
    if (tls)
        std::cout << "  tls " << r.tls_inits << "/" << r.tls_frees;
    std::cout << "  violations " << formatWithCommas(r.violations)
              << "  (" << std::fixed << std::setprecision(1) << r.ms << "ms)\n";
    if (!r.first_violation.empty())
        std::cout << "    " << FG_RED << "first: " << r.first_violation << RESET << "\n";
}

// ── single-threaded correctness ───────────────────────────────────────────────
//
// Cheap, deterministic, and the first thing to look at when the hammer goes
// red: if these fail, the model is wrong before concurrency is involved.

static void test_lifecycle_basics()
{
    section("Lifecycle — single threaded");

    PooledConn c;
    TEST("starts Free", c.phase.load() == PooledConn::Phase::Free);
    TEST("starts with the seeded dispatch reference", c.io_inflight.load() == 1);

    TEST("claim succeeds on a free connection", c.TryClaim(1));
    TEST("claim moves it to Serving", c.phase.load() == PooledConn::Phase::Serving);
    TEST("claim does not re-seed the count", c.io_inflight.load() == 1);
    TEST("a second claim is refused", !c.TryClaim(2));

    c.NoteSubmit();
    TEST("submit raises the count", c.io_inflight.load() == 2);
    c.NoteComplete(Variant::Correct);
    TEST("complete lowers it again", c.io_inflight.load() == 1);
    TEST("a live connection does not finalize on a non-zero count",
         c.phase.load() == PooledConn::Phase::Serving);

    c.Reset(Variant::Correct);
    TEST("reset with work outstanding only starts the drain",
         c.phase.load() == PooledConn::Phase::Draining);

    c.NoteComplete(Variant::Correct);
    TEST("the last release finalizes", c.finalizes.load() == 1);
    TEST("finalize republishes as Free", c.phase.load() == PooledConn::Phase::Free);
    TEST("finalize re-seeds the next dispatch reference", c.io_inflight.load() == 1);
    TEST("the ownership stamp is cleared", c.owner_epoch.load() == 0);

    TEST("the connection is claimable again", c.TryClaim(3));
}

static void test_tls_survives_recycle()
{
    section("TLS session lifetime — recycle vs finalize");

    PooledConn c;
    c.TryClaim(1);
    c.TLSInit();
    TEST("session live after init", c.tls_live.load());

    c.RecycleForNextRequest(Variant::Correct);
    TEST("session SURVIVES a keep-alive recycle", c.tls_live.load());
    TEST("no teardown was counted", c.tls_frees.load() == 0);

    c.Reset(Variant::Correct);
    c.NoteComplete(Variant::Correct);
    TEST("session torn down by finalize", !c.tls_live.load());
    TEST("torn down exactly once", c.tls_frees.load() == 1);

    // And the broken arrangement, for contrast.
    PooledConn d;
    d.TryClaim(1);
    d.TLSInit();
    d.RecycleForNextRequest(Variant::FreeOnRecycle);
    TEST("bug3 model: recycle destroys the session", !d.tls_live.load());
}

static void test_idempotent_reset()
{
    section("Reset idempotency");

    PooledConn c;
    c.TryClaim(1);
    c.Reset(Variant::Correct);   // no outstanding work -> finalizes immediately?
    // The dispatch reference is still held, so this only starts the drain.
    TEST("reset starts the drain", c.phase.load() == PooledConn::Phase::Draining);
    c.Reset(Variant::Correct);
    c.Reset(Variant::Correct);
    TEST("repeated resets are harmless", c.phase.load() == PooledConn::Phase::Draining);
    TEST("no finalize ran early", c.finalizes.load() == 0);

    c.NoteComplete(Variant::Correct);
    TEST("finalize ran exactly once after the last release", c.finalizes.load() == 1);
}

// ── the meta test ─────────────────────────────────────────────────────────────

int main(int argc, char* argv[])
{
    WIRE_CONTEXT();

    int owner_threads      = 4;
    int completion_threads = 3;
    int pool_size          = 8;
    int iters              = 20000;

    if (argc >= 2) iters = std::atoi(argv[1]);

    std::cout << "SocketConnectionState claim/reset/finalize test suite\n";
    std::cout << "cores:              " << get_core_count() << "\n";
    std::cout << "pool size:          " << pool_size << "\n";
    std::cout << "owner threads:      " << owner_threads << "\n";
    std::cout << "completion threads: " << completion_threads << "\n";
    std::cout << "iterations/owner:   " << formatWithCommas(iters) << "\n";

    test_lifecycle_basics();
    test_tls_survives_recycle();
    test_idempotent_reset();

    // ── the correct variant must be clean ─────────────────────────────────────
    section("Hammer — correct variant");

    std::cout << "  plaintext:\n";
    HammerReport plain = run_hammer(Variant::Correct, pool_size, owner_threads,
                                    completion_threads, iters, /*tls=*/false);
    print_report(plain, false);
    TEST("correct variant is clean (plaintext)", report_is_clean(plain, false));

    std::cout << "  TLS:\n";
    HammerReport tls = run_hammer(Variant::Correct, pool_size, owner_threads,
                                  completion_threads, iters, /*tls=*/true);
    print_report(tls, true);
    TEST("correct variant is clean (TLS)", report_is_clean(tls, true));

    // ── every bug variant must be caught ──────────────────────────────────────
    //
    // This is the part that gives the green above its meaning. A harness that
    // cannot fail is not evidence of anything, so each deliberately broken
    // variant is run and REQUIRED to be detected. If one of these reports
    // PASS, the suite has gone blind and the correct-variant result should not
    // be trusted until it is understood why.
    section("Hammer — bug variants (each MUST be detected)");

    struct BugCase { Variant v; bool tls; };
    const BugCase cases[] = {
        { Variant::LostRelease,   false },
        { Variant::DoubleRelease, false },
        { Variant::FreeOnRecycle, true  },
    };

    int detected = 0;
    for (const auto& bc : cases)
    {
        std::cout << "  " << FG_YELLOW << variant_name(bc.v) << RESET << ":\n";
        HammerReport r = run_hammer(bc.v, pool_size, owner_threads,
                                    completion_threads, iters, bc.tls);
        print_report(r, bc.tls);

        const bool caught = !report_is_clean(r, bc.tls);
        if (caught) ++detected;

        if (caught) { std::cout << "  [PASS] detected: " << variant_name(bc.v) << "\n"; ++s_passed; }
        else
        {
            std::cout << "  " << FG_RED << "[FAIL] NOT detected: " << variant_name(bc.v)
                      << " -- the harness is blind to this failure mode" << RESET << "\n";
            ++s_failed;
        }
    }

    const int total_cases = static_cast<int>(sizeof(cases) / sizeof(cases[0]));
    TEST("harness has teeth: every bug variant was detected", detected == total_cases);

    std::cout << "\n────────────────────────────────────\n";
    std::cout << (s_failed == 0 ? FG_GREEN : FG_RED)
              << "passed: " << s_passed << "\n"
              << "failed: " << s_failed << RESET << "\n";

    if (s_failed == 0)
        std::cout << FG_CYAN
                  << "\n  Note: a clean run here says the ACCOUNTING holds. It says nothing\n"
                     "  about data races on the fields themselves -- rerun under\n"
                     "  -fsanitize=thread for that, and under -fsanitize=address for\n"
                     "  lifetime faults the model deliberately cannot express.\n"
                  << RESET;

    return s_failed == 0 ? 0 : 1;
}