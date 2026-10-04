/*
 * ReadViewTesterLoader -- an entity read without its lock (core/Entity.h, "THE
 * PUBLISHED VIEWS"; core/Reclaim.h).
 *
 * Writers still exclude each other; readers read an immutable view a writer
 * published, and a replaced view is freed once no reader can hold it. What is
 * proved, in order:
 *
 *   1. The child view is the order the lists always reported: tag groups in
 *      first-attachment order, arrival within a tag -- through adds, removals,
 *      a tag emptied and filled again, and moves -- and getTypedChild finds
 *      exactly the live children under their own tag.
 *   2. The surface view answers as the maps did: flags, values, tags, the
 *      state flags without the in-flight scopes, a value set again, a flag
 *      taken off with its value, a restored value.
 *   3. Readers on other threads walk, hash and read while a writer adds,
 *      deletes, flags and moves: every read is of one view (no RID twice, tag
 *      groups contiguous), and once the writer stops the readers agree with a
 *      fresh read. Run it under ASAN (ace make loader ReadViewTesterLoader
 *      ASAN=1) and a view freed under a reader is a report, not a guess.
 *   4. The ordered view: each tag's own order (an orderable type sorted by its
 *      key, stable by arrival; a plain type by arrival), resorted after a key
 *      moves or a member comes or goes; the sibling order of one child; and
 *      readers on other threads reading it while a writer moves keys, adds
 *      and deletes -- whole groups, no RID twice, and the settled order once
 *      the writer stops.
 *
 *   ./Run_ReadViewTesterLoader
 */
#undef ETCS_PRODUCTION_BUILD
#ifndef ETCS_MODULE_NAME
#define ETCS_MODULE_NAME "ReadViewTester"
#endif
#include "../ETCS.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <iostream>
#include <map>
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

class A : public DeletableBase<A> { public: WIRE_TYPE_IDENTITY(A) bool DeleteConcrete() override { return true; } };
class B : public DeletableBase<B> { public: WIRE_TYPE_IDENTITY(B) bool DeleteConcrete() override { return true; } };
// Ordered by a stored value: a key set through the funnel moves it among its
// siblings (Entity::markStateChange marks the parent's order).
class O : public DeletableBase<O>, public OrderableBase<O>
{
public:
    WIRE_TYPE_IDENTITY(O)
    bool DeleteConcrete() override { return true; }
    int key() const { std::string v; return valueOf(ETCS::Buffer("k"), v) ? std::atoi(v.c_str()) : 0; }
    bool operator<(const O& o) const { return key() < o.key(); }
};
class M : public DeletableBase<M>
{
public:
    WIRE_TYPE_IDENTITY(M)
    static constexpr bool kEtcsMovable = true;
    bool DeleteConcrete() override { return true; }
};

using Refs = std::vector<ETCS::Entity::ChildRef>;
static std::vector<std::pair<std::string, ETCS::RID>> named(ETCS::Entity* e)
{
    Refs r;
    e->getTypedChildRefs(r);
    std::vector<std::pair<std::string, ETCS::RID>> out;
    for (auto& [tag, rid] : r) out.emplace_back(tag->toString(), rid);
    return out;
}
// One read is one view: no RID twice, and each tag's children contiguous.
static bool coherent(const Refs& r)
{
    std::set<ETCS::RID> seen;
    std::set<const ETCS::Buffer*> closed;
    const ETCS::Buffer* cur = nullptr;
    for (auto& [tag, rid] : r)
    {
        if (!seen.insert(rid).second) return false;
        if (tag != cur) { if (closed.count(tag)) return false; if (cur) closed.insert(cur); cur = tag; }
    }
    return true;
}
static void del(ETCS::Entity* e) { e->getOwningArena().deleteEntity(e, true); }

int main()
{
    shell_startup();
    WIRE_CONTEXT();
    auto& arena = ETCS::MemoryArena::getInstance();
    const std::string TA = "A", TB = "B";   // a typed child is listed under its contract tag
    std::cout << "=== Read views ===\n";
    std::printf("        (reclaimer: %s)\n", ETCS::Reclaimer::getInstance().asymmetric()
                ? "asymmetric -- readers do not fence, the collector fences everyone"
                : "symmetric -- readers fence");

    // -- 1. the child view --------------------------------------------------
    {
        std::cout << "\n-- 1. the child view is the lists' order --\n";
        A* root = arena.allocate<A>();
        // The model: tag groups in first-attachment order, arrival within each.
        std::vector<std::string> order;
        std::map<std::string, std::vector<ETCS::RID>> groups;
        auto expect = [&]() {
            std::vector<std::pair<std::string, ETCS::RID>> out;
            for (auto& t : order) for (ETCS::RID r : groups[t]) out.emplace_back(t, r);
            return out;
        };
        auto add = [&](const std::string& t, ETCS::Entity* c) {
            if (std::find(order.begin(), order.end(), t) == order.end()) order.push_back(t);
            groups[t].push_back(c->getRID());
        };
        auto drop = [&](const std::string& t, ETCS::Entity* c) {
            auto& g = groups[t]; g.erase(std::find(g.begin(), g.end(), c->getRID()));
            del(c);
        };
        A* a1 = root->addTag<A>(); add(TA, a1);
        B* b1 = root->addTag<B>(); add(TB, b1);
        A* a2 = root->addTag<A>(); add(TA, a2);
        B* b2 = root->addTag<B>(); add(TB, b2);
        check(named(root) == expect(), "adds: tag groups in first-attachment order, arrival within each");
        drop(TA, a1);
        check(named(root) == expect(), "a removal keeps the rest in order");
        drop(TA, a2);
        A* a3 = root->addTag<A>(); add(TA, a3);
        check(named(root) == expect() && named(root).front().second == a3->getRID(),
              "a tag emptied and filled again comes back in its first-attachment place");
        Refs r; root->getTypedChildRefs(r);
        bool found = true;
        for (auto& [tag, rid] : r) found &= root->getTypedChild(*tag, rid) != nullptr;
        check(found && !root->getTypedChild(ETCS::Buffer(TA), b1->getRID()) && !root->getTypedChild(ETCS::Buffer(TB), a2->getRID()),
              "getTypedChild finds each live child under its own tag, and nothing else");
        // Moves: a movable child leaves one list and joins another.
        A* left  = root->addTag<A>(); add(TA, left);
        A* right = root->addTag<A>(); add(TA, right);
        M* m = left->addTag<M>();
        const bool moved = m->moveTo(right);
        Refs rr; right->getTypedChildRefs(rr);
        check(moved && named(left).empty() && rr.size() == 1 && right->getTypedChild(*rr[0].first, m->getRID()) == m,
              "a move takes the child out of one view and into the other");
        del(root);
    }
    // -- 2. the surface view --------------------------------------------------
    {
        std::cout << "\n-- 2. the surface view answers as the maps did --\n";
        A* e = arena.allocate<A>();
        e->addTag(ETCS::Buffer("lit"));
        e->addTag(ETCS::Buffer("level"), "3");
        e->addTag(ETCS::Buffer("level"), "4");
        std::string v;
        check(e->hasTag(ETCS::Buffer("lit")) && e->hasTag(ETCS::Buffer("level")) && !e->hasTag(ETCS::Buffer("dim")),
              "flags: on, on, and not");
        check(e->valueOf(ETCS::Buffer("level"), v) && v == "4", "a value set again reads as the last set");
        check(e->hasTag(ETCS::Buffer("Deletable")) && !e->hasTag(ETCS::Buffer("Nothing")), "tags: a family marker, and not");
        std::vector<std::pair<std::string, std::string>> kv; e->values(kv);
        check(kv.size() == 1 && kv[0].first == "level" && kv[0].second == "4", "values(): the stored values, by key");
        e->removeTag(ETCS::Buffer("level"));
        kv.clear(); e->values(kv);
        check(!e->hasTag(ETCS::Buffer("level")) && kv.empty() && !e->valueOf(ETCS::Buffer("level"), v),
              "a flag taken off takes its value with it");
        e->addTag(ETCS::Buffer("level"), "1");
        check(e->restoreValue(ETCS::Buffer("level"), "9") && e->valueOf(ETCS::Buffer("level"), v) && v == "9",
              "a restored value reads back");
        std::vector<std::string> fl; e->stateFlags(fl);
        std::sort(fl.begin(), fl.end());
        check(fl == std::vector<std::string>{ "level", "lit" }, "the state flags");
        A* twin = arena.allocate<A>();
        twin->addTag(ETCS::Buffer("level"), "9");
        twin->addTag(ETCS::Buffer("lit"));
        check(twin->getHash() == e->getHash(), "the same surface made another way hashes the same");
        // A flag or value is spliced into the last view, not rebuilt from the
        // maps; a tag going on or off rebuilds. Whichever order the splices
        // came in, the view is the one a rebuild makes.
        const char* keys[] = { "m", "c", "x", "a", "q", "b" };
        A* fwd = arena.allocate<A>();
        A* rev = arena.allocate<A>();
        auto put = [&](A* e, int i) { if (i % 2) e->addTag(ETCS::Buffer(keys[i]), std::string(keys[i])); else e->addTag(ETCS::Buffer(keys[i])); };
        for (int i = 0; i < 6; ++i)  put(fwd, i);
        for (int i = 5; i >= 0; --i) put(rev, i);
        fwd->removeTag(ETCS::Buffer("q")); rev->removeTag(ETCS::Buffer("q"));
        fwd->removeTag(ETCS::Buffer("c")); rev->removeTag(ETCS::Buffer("c"));
        std::vector<std::string> ff, rf; fwd->stateFlags(ff); rev->stateFlags(rf);
        std::vector<std::pair<std::string, std::string>> fv, rv; fwd->values(fv); rev->values(rv);
        check(ff == rf && ff == std::vector<std::string>{ "a", "b", "m", "x" } && fv == rv && fv.size() == 2,
              "flags and values spliced in either order are the same sorted view");
        check(fwd->getHash() == rev->getHash(), "...and hash the same");
        // A value set again leaves its old bytes in the view until enough of
        // the text is dead that a rebuild is cheaper: every write still reads.
        bool all = true;
        for (int i = 0; i < 3000; ++i)
        {
            const std::string want(96, char('a' + i % 26));
            fwd->addTag(ETCS::Buffer("b"), want);
            all &= fwd->valueOf(ETCS::Buffer("b"), v) && v == want && fwd->hasTag(ETCS::Buffer("x"));
        }
        check(all, "3000 value rewrites (past the compaction threshold) all read back, the rest intact");
        del(fwd); del(rev);
        del(e); del(twin);
    }
    // -- 3. readers on other threads ------------------------------------------
    {
        std::cout << "\n-- 3. readers while a writer writes --\n";
        A* root = arena.allocate<A>();
        A* left  = root->addTag<A>();
        A* right = root->addTag<A>();
        std::vector<ETCS::Entity*> live;
        for (int i = 0; i < 50; ++i) live.push_back(i % 2 ? (ETCS::Entity*)root->addTag<A>() : (ETCS::Entity*)root->addTag<B>());
        std::vector<M*> movers;
        for (int i = 0; i < 20; ++i) movers.push_back(left->addTag<M>());
        std::atomic<bool> stop{ false };
        std::atomic<long> reads{ 0 }, torn{ 0 };
        auto reader = [&]() {
            Refs r;
            std::vector<std::pair<std::string, std::string>> kv;
            volatile uint64_t h = 0;
            while (!stop.load(std::memory_order_acquire))
            {
                for (ETCS::Entity* p : { (ETCS::Entity*)root, (ETCS::Entity*)left, (ETCS::Entity*)right })
                {
                    r.clear(); p->getTypedChildRefs(r);
                    if (!coherent(r)) torn.fetch_add(1);
                    for (auto& [tag, rid] : r) (void)p->getTypedChild(*tag, rid);
                }
                kv.clear(); root->values(kv);
                (void)root->hasTag(ETCS::Buffer("flip"));
                h = h + root->getHash();
                reads.fetch_add(1, std::memory_order_relaxed);
            }
        };
        std::thread r1(reader), r2(reader);
        uint64_t st = 0x9e3779b97f4a7c15ull;
        auto next = [&st]() { st ^= st << 13; st ^= st >> 7; st ^= st << 17; return st; };
        int writes = 0;
        for (int i = 0; i < 3000; ++i)
        {
            switch (next() % 5)
            {
                case 0: live.push_back(next() % 2 ? (ETCS::Entity*)root->addTag<A>() : (ETCS::Entity*)root->addTag<B>()); break;
                case 1: if (!live.empty()) { size_t k = next() % live.size(); del(live[k]); live.erase(live.begin() + k); } break;
                case 2: root->addTag(ETCS::Buffer("flip"), std::to_string(i)); break;
                case 3: root->removeTag(ETCS::Buffer("flip")); break;
                case 4: { M* m = movers[next() % movers.size()]; m->moveTo(m->getParent() == left ? (ETCS::Entity*)right : (ETCS::Entity*)left); } break;
            }
            ++writes;
        }
        stop.store(true, std::memory_order_release);
        r1.join(); r2.join();
        check(torn.load() == 0 && reads.load() > 0, "every read was of one view: no RID twice, tag groups contiguous");
        std::set<ETCS::RID> want;
        for (ETCS::Entity* e : live) want.insert(e->getRID());
        want.insert(left->getRID()); want.insert(right->getRID());
        Refs r; root->getTypedChildRefs(r);
        std::set<ETCS::RID> got;
        for (auto& [tag, rid] : r) if (root->getTypedChild(*tag, rid)) got.insert(rid);
        size_t moved = 0; Refs lr, rr; left->getTypedChildRefs(lr); right->getTypedChildRefs(rr);
        moved = lr.size() + rr.size();
        check(got == want && moved == movers.size(), "after the writer stops, a fresh read is exactly what is live");
        std::printf("        (%d writes, %ld reads on two threads)\n", writes, reads.load());
        del(root);
    }

    // -- 4. the ordered view --------------------------------------------------
    {
        std::cout << "\n-- 4. the ordered view --\n";
        A* root = arena.allocate<A>();
        std::vector<O*> os;
        std::vector<ETCS::RID> plain;
        auto setKey = [](O* o, int k) { o->addTag(ETCS::Buffer("k"), std::to_string(k)); };
        const int keys[] = { 5, 1, 3, 1, 4 };
        for (int k : keys) { O* o = root->addTag<O>(); setKey(o, k); os.push_back(o); }
        for (int i = 0; i < 3; ++i) plain.push_back(root->addTag<A>()->getRID());
        // The model: O's group sorted by key, ties by arrival; A's by arrival.
        auto expect = [&]() {
            std::vector<O*> s(os);
            std::stable_sort(s.begin(), s.end(), [](O* a, O* b) { return a->key() < b->key(); });
            std::vector<ETCS::RID> out;
            for (O* o : s) out.push_back(o->getRID());
            out.insert(out.end(), plain.begin(), plain.end());
            return out;
        };
        auto read = [&]() {
            std::vector<std::pair<ETCS::Buffer, ETCS::RID>> kids;
            root->getOrderedTypedChildren(kids);
            std::vector<ETCS::RID> out;
            for (auto& k : kids) out.push_back(k.second);
            return out;
        };
        check(read() == expect(), "each tag in its own order: by key (ties by arrival), then the plain tag by arrival");
        Refs refs; root->getOrderedTypedChildRefs(refs);
        bool same = refs.size() == read().size();
        for (size_t i = 0; same && i < refs.size(); ++i) same = refs[i].second == read()[i] && root->getTypedChild(*refs[i].first, refs[i].second);
        check(same, "the refs form is the same order, under tags that resolve");
        setKey(os[0], 0);
        check(read() == expect(), "a key moved: the next read is resorted");
        setKey(os[1], 9); setKey(os[2], 2); setKey(os[1], 7);
        check(read() == expect(), "a burst of keys, one read: the state at the read");
        O* late = root->addTag<O>(); setKey(late, 2); os.push_back(late);
        del(os[3]); os.erase(os.begin() + 3);
        check(read() == expect(), "a member in and a member out");
        std::vector<ETCS::RID> sib, want;
        for (ETCS::RID r : expect()) if (std::find(plain.begin(), plain.end(), r) == plain.end()) want.push_back(r);
        check(root->collectSiblingOrder(os[2]->getRID(), sib) && sib == want, "a child's sibling order is its own tag's");
        sib.clear();
        check(!root->collectSiblingOrder(root->getRID(), sib) && sib.empty(), "...and an entity not under it has none");

        // Readers while a writer moves keys and changes membership.
        std::atomic<bool> stop{ false };
        std::atomic<long> reads{ 0 }, torn{ 0 };
        auto reader = [&]() {
            Refs r; std::vector<ETCS::RID> s;
            while (!stop.load(std::memory_order_acquire))
            {
                r.clear(); root->getOrderedTypedChildRefs(r);
                if (!coherent(r)) torn.fetch_add(1);
                s.clear(); if (!r.empty()) root->collectSiblingOrder(r.front().second, s);
                reads.fetch_add(1, std::memory_order_relaxed);
            }
        };
        std::thread r1(reader), r2(reader);
        uint64_t st = 0x2545f4914f6cdd1dull;
        auto next = [&st]() { st ^= st << 13; st ^= st >> 7; st ^= st << 17; return st; };
        for (int i = 0; i < 2000; ++i)
        {
            switch (next() % 4)
            {
                case 0: case 1: setKey(os[next() % os.size()], static_cast<int>(next() % 50)); break;
                case 2: { O* o = root->addTag<O>(); setKey(o, static_cast<int>(next() % 50)); os.push_back(o); } break;
                case 3: if (os.size() > 4) { size_t k = next() % os.size(); del(os[k]); os.erase(os.begin() + k); } break;
            }
        }
        stop.store(true, std::memory_order_release);
        r1.join(); r2.join();
        check(torn.load() == 0 && reads.load() > 0, "every ordered read was whole groups, no RID twice");
        check(read() == expect(), "after the writer stops, the read is the settled order");
        std::printf("        (2000 writes, %ld ordered reads on two threads)\n", reads.load());
        del(root);
    }

    std::printf("\n%s (%d failure%s)\n", g_fail ? "FAILED" : "PASSED", g_fail, g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}
