// OrderedReadCommitTesterLoader.cc
//
// AN ORDERED READ SEES ONE COMMITTED STATE, AND A KEY THAT MOVES MID-SORT
// CANNOT TAKE IT OUT OF RANGE.
//
// RIDList::collect_ordered sorts by the pointee's live operator<, which reads
// tag state that commits on another thread. Two things are held to account:
//
//   1. The sort itself. std::stable_sort's insertion pass walks without a
//      bound once the relation has answered; a relation that contradicts
//      itself walks it out of the range. detail::guarded_stable_sort checks
//      every index. Section 1 puts both under a relation that answers at
//      random, inside a buffer whose margins are sentinels, and counts how
//      often the relation is asked about a sentinel.
//   2. The read. Section 2 flips `hidden` on a parent's children from another
//      thread while this one reads the ordered list thousands of times: every
//      read must be a whole permutation, and the read after the flipping stops
//      must be the settled order.
//
//   ./Run_OrderedReadCommitTesterLoader

#include "../ETCS.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <vector>

static int g_fail = 0;

static void check(bool ok, const char* what)
{
    std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++g_fail;
}

int main(int, char**)
{
    WIRE_CONTEXT();

    std::printf("\n-- the sort, under a relation that contradicts itself ------------\n");
    {
        constexpr ETCS::RID SENTINEL = ~ETCS::RID(0);
        constexpr size_t MARGIN = 64, N = 48, TRIALS = 2000;
        std::mt19937 rng(7);
        size_t std_touches = 0, guarded_touches = 0, guarded_bad = 0;

        for (size_t t = 0; t < TRIALS; ++t)
        {
            // The range lives in the middle of a buffer; anything the relation
            // is asked about outside it is a sentinel, and answering false there
            // stops the walk at the first one rather than letting it run on.
            std::vector<ETCS::RID> buf(MARGIN + N + MARGIN, SENTINEL);
            for (size_t i = 0; i < N; ++i) buf[MARGIN + i] = i + 1;
            auto liar = [&](ETCS::RID a, ETCS::RID b, size_t& touches) {
                if (a == SENTINEL || b == SENTINEL) { ++touches; return false; }
                return (rng() & 1u) != 0;
            };
            std::stable_sort(buf.begin() + MARGIN, buf.begin() + MARGIN + N,
                             [&](ETCS::RID a, ETCS::RID b) { return liar(a, b, std_touches); });

            std::vector<ETCS::RID> v(N), tmp;
            for (size_t i = 0; i < N; ++i) v[i] = i + 1;
            ETCS::detail::guarded_stable_sort(v, tmp,
                [&](ETCS::RID a, ETCS::RID b) { return liar(a, b, guarded_touches); });
            std::set<ETCS::RID> seen(v.begin(), v.end());
            if (seen.size() != N || *seen.begin() != 1 || *seen.rbegin() != N) ++guarded_bad;
        }
        std::printf("  (std::stable_sort asked about a sentinel %zu time(s) in %zu trials)\n",
                    std_touches, TRIALS);
        // The before: what the old collect_ordered called. Not a requirement of
        // this patch, but the reason for it -- if it ever reads 0 the library's
        // insertion pass is guarded and section 1 is moot.
        check(std_touches > 0, "std::stable_sort leaves its range under a contradicting relation");
        check(guarded_touches == 0, "guarded_stable_sort never asks about anything outside its range");
        check(guarded_bad == 0, "...and always returns a permutation of its input");

        std::vector<ETCS::RID> v = { 5, 1, 4, 1, 3 }, tmp;
        ETCS::detail::guarded_stable_sort(v, tmp, [](ETCS::RID a, ETCS::RID b) { return a < b; });
        check((v == std::vector<ETCS::RID>{ 1, 1, 3, 4, 5 }), "under an honest relation it sorts");
    }

    std::printf("\n-- the read, while tags commit on another thread -----------------\n");

    ETCS::Entity* p = ETCS::spawn_entity("RenderProvider", "CompositeDrawable2D", env, loader);
    check(p != nullptr, "a parent spawns");
    if (!p) return 1;
    p->call("CompositeDrawable2D.Create", "64 64", ctx);

    constexpr int KIDS = 32;
    std::vector<ETCS::Entity*> kids;
    ETCS::ExecSource src{ "OrderedReadCommitTesterLoader", 0 };
    for (int i = 0; i < KIDS; ++i)
    {
        ETCS::Entity* c = ETCS::make_typed_child("RenderProvider", "CompositeDrawable2D", p, src);
        if (!c) break;
        c->call("CompositeDrawable2D.Create", "4 4", ctx);
        c->call("CompositeDrawable2D.SetOrder", std::to_string(i).c_str(), ctx);
        kids.push_back(c);
    }
    check(static_cast<int>(kids.size()) == KIDS, "thirty-two children, ordered 0..31");
    if (static_cast<int>(kids.size()) != KIDS) return 1;
    std::set<ETCS::RID> all;
    for (auto* c : kids) all.insert(c->getRID());

    std::atomic<bool> stop{ false };
    std::atomic<uint64_t> flips{ 0 };
    std::thread flipper([&] {
        std::mt19937 r(11);
        while (!stop.load(std::memory_order_acquire))
        {
            ETCS::Entity* c = kids[r() % KIDS];
            if (r() & 1u) c->addTag(ETCS::Buffer("hidden"));
            else          c->removeTag(ETCS::Buffer("hidden"));
            flips.fetch_add(1, std::memory_order_relaxed);
        }
    });

    size_t reads = 0, broken = 0;
    // Until enough commits have landed to have overlapped many sorts.
    for (; flips.load(std::memory_order_relaxed) < 3000 && reads < 5000000; ++reads)
    {
        std::vector<std::pair<ETCS::Buffer, ETCS::RID>> got;
        p->getOrderedTypedChildren(got);
        std::set<ETCS::RID> s;
        for (auto& g : got) s.insert(g.second);
        if (got.size() != all.size() || s != all) ++broken;
    }
    stop.store(true, std::memory_order_release);
    flipper.join();
    std::printf("  (%zu reads against %llu flag commits)\n", reads,
                static_cast<unsigned long long>(flips.load()));
    check(broken == 0, "every read while flags committed was a whole permutation");

    // Settled: hidden ones first in their stated order, then the shown ones.
    std::vector<ETCS::RID> want;
    for (int pass = 0; pass < 2; ++pass)
        for (auto* c : kids)
            if (c->hasTag(ETCS::Buffer("hidden")) == (pass == 0)) want.push_back(c->getRID());
    std::vector<std::pair<ETCS::Buffer, ETCS::RID>> got;
    p->getOrderedTypedChildren(got);
    std::vector<ETCS::RID> have;
    for (auto& g : got) have.push_back(g.second);
    check(have == want, "the read after the commits stop is the settled order");

    p->call("CompositeDrawable2D.Delete", "", ctx);
    ETCS::PendingUnloadRegistry::getInstance().join_all();

    std::printf("\n%s (%d failure%s)\n", g_fail ? "FAILED" : "PASSED",
                g_fail, g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}
