/*
 * SurfaceBurstTesterLoader -- when an entity's surface is published (core/
 * Entity.h: surfaceView, tagModifyImpl, publishOwed, restoreValues;
 * core/DynamicLoader.h: etcs_tagmodify_settle).
 *
 *   1. BORN WHOLE. Made, an entity's families are on its maps and nothing is
 *      published; the first read builds the view, whole, and so does a first
 *      write; eight readers racing the first read build it once.
 *   2. A WRITE RETURNS READABLE. Eight writers on one entity, each reading
 *      its own write the moment it returns; a reader told of a write by its
 *      writer reads it too.
 *   3. ONE VIEW PER ENTITY PER BURST. Writes held behind one slow event drain
 *      as one burst: one view and one mark each entity they touched. A burst
 *      is no more than the ring holds (64 slots): a hundred writes take more
 *      than one, in far fewer views than writes. An event that is not a flag
 *      or value (a move) runs only once what is before it is published.
 *   4. A RESTORE IS A WRITE. Through the funnel: the surface and the hash
 *      epoch move; a key with no flag lands nowhere; a bound value goes to
 *      its binding and the surface stays.
 *
 *   ./Run_SurfaceBurstTesterLoader
 */
#undef ETCS_PRODUCTION_BUILD
#ifndef ETCS_MODULE_NAME
#define ETCS_MODULE_NAME "SurfaceBurstTester"
#endif
#include "../ETCS.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

static int g_fail = 0;
static void check(bool ok, const char* what)
{
    std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++g_fail;
}

class A : public DeletableBase<A> { public: WIRE_TYPE_IDENTITY(A) bool DeleteConcrete() override { return true; } };
// Two families: two markers its constructors put on the maps.
class W : public DeletableBase<W>, public OrderableBase<W>
{
public:
    WIRE_TYPE_IDENTITY(W)
    bool DeleteConcrete() override { return true; }
    bool operator<(const W&) const { return false; }
};
class M : public DeletableBase<M>
{
public:
    WIRE_TYPE_IDENTITY(M)
    static constexpr bool kEtcsMovable = true;
    bool DeleteConcrete() override { return true; }
};

static void del(ETCS::Entity* e) { e->getOwningArena().deleteEntity(e, true); }
static bool reads(ETCS::Entity* e, const std::string& key, const std::string& want)
{
    std::string v;
    return e->valueOf(ETCS::Buffer(key.c_str()), v) && v == want;
}
static void sleepMs(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

// The stream held: an event whose apply sleeps, so whatever is posted behind
// it is on the ring when the stream comes back to it -- one burst.
static bool slowApply(ETCS::Entity*, const ETCS::Buffer&, bool, const std::string*) { sleepMs(200); return false; }
struct Hold
{
    std::unique_ptr<ETCS::TagModifyEvent> ev;
    bool posted = false;
    explicit Hold(ETCS::Entity* on) : ev(new ETCS::TagModifyEvent(on, ETCS::Buffer("held"), false, &slowApply)) { posted = ev->post(); }
    ~Hold() { if (posted) ev->wait(); }
};
using KV = std::vector<std::pair<std::string, std::string>>;
static KV values(int from, int n, const std::string& v)
{
    KV kv;
    for (int i = from; i < from + n; ++i) kv.emplace_back("v" + std::to_string(i), v);
    return kv;
}

int main()
{
    shell_startup();
    WIRE_CONTEXT();
    auto& arena = ETCS::MemoryArena::getInstance();
    std::cout << "=== When a surface is published ===\n";

    // -- 1. born whole ---------------------------------------------------------
    {
        std::cout << "\n-- 1. born whole --\n";
        W* w = arena.allocate<W>();
        check(w->surfaceVersion() == 0, "made: its families on the maps, nothing published");
        check(w->hasTag(ETCS::Buffer("Deletable")) && w->hasTag(ETCS::Buffer("Orderable")),
              "the first read finds every family...");
        check(w->surfaceVersion() == 1, "...in the one view it built, whole");

        W* x = arena.allocate<W>();
        x->addTag(ETCS::Buffer("lit"));
        check(x->surfaceVersion() == 1 && x->hasTag(ETCS::Buffer("Deletable")) && x->hasTag(ETCS::Buffer("lit")),
              "a first write builds it whole too: one view, the families and the flag");

        W* y = arena.allocate<W>();
        std::atomic<bool> go{ false };
        std::atomic<int> found{ 0 };
        std::vector<std::thread> ts;
        for (int i = 0; i < 8; ++i)
            ts.emplace_back([&] { while (!go.load()) {} if (y->hasTag(ETCS::Buffer("Orderable"))) ++found; });
        go = true;
        for (auto& t : ts) t.join();
        check(found == 8 && y->surfaceVersion() == 1, "eight readers racing the first read: every one finds it, built once");
        del(w); del(x); del(y);
    }

    // -- 2. a write returns readable -------------------------------------------
    {
        std::cout << "\n-- 2. a write returns readable --\n";
        A* e = arena.allocate<A>();
        constexpr int kWriters = 8, kWrites = 300;
        std::atomic<int> own_missed{ 0 }, told_missed{ 0 };
        std::atomic<int> last[kWriters];
        for (auto& l : last) l = -1;
        std::atomic<bool> done{ false };
        std::vector<std::thread> ws;
        for (int t = 0; t < kWriters; ++t)
            ws.emplace_back([&, t] {
                const std::string key = "k" + std::to_string(t);
                for (int i = 0; i < kWrites; ++i)
                {
                    e->addTag(ETCS::Buffer(key.c_str()), std::to_string(i));
                    if (!reads(e, key, std::to_string(i))) ++own_missed;
                    last[t].store(i);
                }
            });
        // Told by a writer (its `last`), a reader on another thread reads at
        // least that: what has returned is everyone's.
        std::thread reader([&] {
            while (!done.load())
                for (int t = 0; t < kWriters; ++t)
                {
                    const int told = last[t].load();
                    if (told < 0) continue;
                    std::string v;
                    if (!e->valueOf(ETCS::Buffer(("k" + std::to_string(t)).c_str()), v) || std::atoi(v.c_str()) < told) ++told_missed;
                }
        });
        for (auto& t : ws) t.join();
        done = true;
        reader.join();
        std::printf("        (%d writes, %llu views)\n", kWriters * kWrites, (unsigned long long)e->surfaceVersion());
        check(own_missed == 0, "eight writers on one entity: each reads its own write the moment it returns");
        check(told_missed == 0, "...and a reader told of a write by its writer reads it too");
        del(e);
    }

    // -- 3. one view per entity per burst --------------------------------------
    {
        std::cout << "\n-- 3. one view per entity per burst --\n";
        A* holder = arena.allocate<A>();
        W* y = arena.allocate<W>();
        W* z = arena.allocate<W>();
        for (int i = 0; i < 100; ++i) { const std::string k = "v" + std::to_string(i); y->addTag(ETCS::Buffer(k.c_str())); z->addTag(ETCS::Buffer(k.c_str())); }

        uint64_t v0 = y->surfaceVersion();
        uint32_t e0 = y->hashEpoch();
        size_t landed = 0;
        { Hold h(holder); landed = y->restoreValues(values(0, 10, "a")); }
        check(landed == 10 && reads(y, "v0", "a") && reads(y, "v9", "a"), "ten writes held behind one event all land...");
        check(y->surfaceVersion() - v0 == 1 && y->hashEpoch() - e0 == 1, "...as one burst: one view, one mark");

        v0 = y->surfaceVersion(); e0 = y->hashEpoch();
        const uint64_t zv0 = z->surfaceVersion();
        {
            Hold h(holder);
            std::thread t1([&] { y->restoreValues(values(0, 5, "b")); });
            std::thread t2([&] { z->restoreValues(values(0, 5, "b")); });
            t1.join(); t2.join();
        }
        check(y->surfaceVersion() - v0 == 1 && z->surfaceVersion() - zv0 == 1 && reads(y, "v4", "b") && reads(z, "v4", "b"),
              "two writers, two entities, one burst: one view each");

        v0 = y->surfaceVersion(); e0 = y->hashEpoch();
        { Hold h(holder); landed = y->restoreValues(values(0, 100, "c")); }
        const uint64_t views = y->surfaceVersion() - v0;
        std::printf("        (a hundred writes: %llu views, %u marks)\n", (unsigned long long)views, y->hashEpoch() - e0);
        check(landed == 100 && reads(y, "v99", "c"), "a hundred writes behind one event, more than the ring holds, all land...");
        check(views >= 2 && views < 100 && y->hashEpoch() - e0 == views,
              "...in more than one burst and far fewer views than writes, a mark a view");

        // [held] [a write to y] [a move] [held]: the move is not owed, so the
        // write before it is published before it runs; its writer is woken by
        // that, its mover at once -- while the second hold still sleeps.
        A* left = arena.allocate<A>();
        A* right = arena.allocate<A>();
        M* m = left->addTag<M>();
        {
            Hold first(holder);
            std::thread writer([&] { y->restoreValues({ { "v1", "before-the-move" } }); });
            sleepMs(30);
            bool moved = false;
            std::thread mover([&] { moved = m->moveTo(right); });
            sleepMs(30);
            Hold second(holder);
            mover.join();
            check(moved && reads(y, "v1", "before-the-move"),
                  "a move behind a write runs once the write is published: when it returns, the write reads");
            writer.join();
        }
        del(holder); del(y); del(z); del(left); del(right);
    }

    // -- 4. a restore is a write -----------------------------------------------
    {
        std::cout << "\n-- 4. a restore is a write --\n";
        A* r = arena.allocate<A>();
        r->addTag(ETCS::Buffer("level"), "1");
        uint64_t v0 = r->surfaceVersion();
        uint32_t e0 = r->hashEpoch();
        check(r->restoreValue(ETCS::Buffer("level"), "9") && reads(r, "level", "9"), "a restore lands on a flag the replay put back...");
        check(r->surfaceVersion() > v0 && r->hashEpoch() > e0, "...through the funnel: its surface and its hash epoch move");

        v0 = r->surfaceVersion();
        check(!r->restoreValue(ETCS::Buffer("nowhere"), "1") && r->surfaceVersion() == v0,
              "a key with no flag: nowhere to land, nothing published");

        static std::string held = "x";
        ETCS::Entity::ValueBinding b;
        b.self  = &held;
        b.write = [](void* s, std::string& out) { out = *static_cast<std::string*>(s); };
        b.read  = [](void* s, const std::string& in) { *static_cast<std::string*>(s) = in; return true; };
        r->bindValue(ETCS::Buffer("bound"), b);
        v0 = r->surfaceVersion();
        check(r->restoreValue(ETCS::Buffer("bound"), "y") && held == "y" && r->surfaceVersion() == v0,
              "a bound value goes to its binding; the surface stays");
        del(r);
    }

    std::printf("\n%s (%d failure%s)\n", g_fail ? "FAILED" : "PASSED", g_fail, g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}
