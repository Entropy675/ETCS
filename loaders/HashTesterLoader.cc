/*
 * HashTesterLoader -- the runtime hash, proved against a tree built right here.
 *
 * No module, no window, no device: what is under test is Entity's own hash
 * (core/Entity.h, "THE RUNTIME HASH"), and a provider in the picture would be
 * a second thing that could be wrong. Leaves are allocated off the arena and
 * parented with addTag<T>, the same isolation DrawableOntologyTesterLoader
 * uses.
 *
 * What it proves, in order:
 *
 *   1. A pull is lazy: a second pull with nothing moved recomputes nothing.
 *   2. The surface IS the state: a flag going on moves the hash, and taking
 *      it off again restores the exact value (A-B-A). A type-local variable
 *      moves nothing.
 *   3. A transition reaches every ancestor's hash, and only the path that
 *      moved is recomputed on the next pull.
 *   4. Tag order is state where the runtime reads it (children) and not
 *      where it does not (flags): two trees with the same children attached
 *      in a different tag order hash differently; flags added in a different
 *      order hash the same; and the same tree made again, at other RIDs,
 *      hashes the same -- no RID is in any hash.
 *   5. A child leaving the tree moves the parent's hash.
 *   6. The audit recomputes everything, agrees with the lazy pull on a
 *      clean tree, and REPORTS a divergence when a surface is moved behind
 *      the funnel's back -- then marks it, so the tree above recomputes.
 *   7. The level rule: a parentless entity's node hash is its SHA-256 digest
 *      (first 64 bits), a child's is XXH3, and the audit's root of a
 *      parentless top is that digest.
 *   8. The chain reaches the root: a module's root digest is over its
 *      parentless entities, the global root over every module, and a flag on
 *      one entity deep in a provider moves both -- and A-B-A restores them.
 *      A real provider entity is spawned for this, so the loader's mirror has
 *      rows to walk (the tester's own leaves are in no registry).
 *   9. Depth is not a limit: a chain twenty thousand levels deep is built,
 *      pulled, audited, marked from its leaf and observed at its top, then
 *      deleted as a cascade -- the pull, the mark and the cascade are loops,
 *      not a frame per level (core/Entity.h depth_, MemoryArena::cascade) --
 *      and every level knows how deep it is.
 *
 *   ./Run_HashTesterLoader
 */
#undef ETCS_PRODUCTION_BUILD
#ifndef ETCS_MODULE_NAME
#define ETCS_MODULE_NAME "HashTester"
#endif
#include "../ETCS.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <iostream>
#include <string>
#include <vector>

static int g_fail = 0;
static void check(bool ok, const char* what)
{
    std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++g_fail;
}

// ---------------------------------------------------------------------------
// One leaf that claims Observable (through Resizable), so the wire the funnel
// marks through has a claimant and the self edge can be read back. It carries
// a type-local value that is NOT on the surface, which is what 2 checks.
// ---------------------------------------------------------------------------
class Node : public ResizableBase<Node>,
             public DeletableBase<Node>
{
public:
    WIRE_TYPE_IDENTITY(Node)
    int local_value = 0;
    WindowSize GetSizeConcrete() override { return WindowSize{1, 1}; }
    bool DeleteConcrete() override { return true; }
};

// A second concrete type, so a tree can have two TAGS of children and the
// order they were first attached in can be varied.
class Other : public ResizableBase<Other>,
              public DeletableBase<Other>
{
public:
    WIRE_TYPE_IDENTITY(Other)
    WindowSize GetSizeConcrete() override { return WindowSize{1, 1}; }
    bool DeleteConcrete() override { return true; }
};

int main()
{
    shell_startup();
    WIRE_CONTEXT();
    auto& arena = ETCS::MemoryArena::getInstance();

    std::cout << "=== Runtime hash ===\n";

    // -- 1. laziness ----------------------------------------------------------
    Node* top   = arena.allocate<Node>();
    Node* mid   = top->addTag<Node>();
    Node* leaf  = mid->addTag<Node>();
    (void)leaf;

    check(!top->hashCurrent(), "a fresh node has no current hash");
    const uint64_t h0 = top->getHash();
    check(top->hashCurrent(), "one pull makes it current");
    check(mid->hashCurrent() && leaf->hashCurrent(), "...and every node the pull passed");
    check(top->getHash() == h0, "a second pull with nothing moved answers the same value");
    check(h0 != 0, "and the value is not the empty hash");

    // -- 2. the surface is the state ------------------------------------------
    leaf->local_value = 40;
    check(top->hashCurrent(), "a type-local variable moving is not a transition");
    check(top->getHash() == h0, "...so the hash does not move");

    leaf->addTag(ETCS::Buffer("falling"));
    check(!leaf->hashCurrent(), "a flag going on makes the leaf stale");
    check(!mid->hashCurrent() && !top->hashCurrent(), "...and every ancestor");
    const uint64_t h1 = top->getHash();
    check(h1 != h0, "the root hash moved");

    leaf->removeTag(ETCS::Buffer("falling"));
    check(top->getHash() == h0, "taking the flag off restores the exact value (A-B-A)");

    // -- 3. only the path that moved recomputes --------------------------------
    Node* side = top->addTag<Node>();          // a second branch under root
    top->getHash();
    const uint64_t side_h = side->getHash();
    leaf->addTag(ETCS::Buffer("x"));
    check(side->hashCurrent(), "a transition on one branch leaves the other current");
    top->getHash();
    check(side->getHash() == side_h, "...and its value unchanged after the root pull");
    leaf->removeTag(ETCS::Buffer("x"));

    // -- 4. order --------------------------------------------------------------
    {
        Node* a = arena.allocate<Node>();
        a->addTag<Node>();  a->addTag<Other>();          // Node first, Other second
        Node* b = arena.allocate<Node>();
        b->addTag<Other>(); b->addTag<Node>();           // Other first
        // The two trees differ in RIDs and in tag order. RIDs are not in any
        // hash, so the surfaces are identical and only the order separates
        // the node hashes.
        std::vector<std::pair<ETCS::Buffer, ETCS::RID>> ka, kb;
        a->getTypedChildren(ka); b->getTypedChildren(kb);
        check(ka.size() == 2 && kb.size() == 2, "both parents report two children");
        check(ka[0].first == ETCS::Buffer("Node") && kb[0].first == ETCS::Buffer("Other"),
              "children are reported in first-attachment tag order");
        check(a->surfaceHash() == b->surfaceHash(), "the parents' own surfaces are identical");
        check(a->getHash() != b->getHash(), "...and their node hashes are not: child tag order is state");
        Node* a2 = arena.allocate<Node>();
        a2->addTag<Node>(); a2->addTag<Other>();         // a again, at other RIDs
        check(a2->getHash() == a->getHash(), "the same tree made again at other RIDs hashes the same");

        Node* f1 = arena.allocate<Node>();
        f1->addTag(ETCS::Buffer("p")); f1->addTag(ETCS::Buffer("q"));
        Node* f2 = arena.allocate<Node>();
        f2->addTag(ETCS::Buffer("q")); f2->addTag(ETCS::Buffer("p"));
        check(f1->surfaceHash() == f2->surfaceHash(), "flags added in a different order hash the same");
        check(f1->getHash() == f2->getHash(), "...and so do the nodes (no children, RID not on the surface)");
    }

    // -- 5. a child leaving ----------------------------------------------------
    {
        const uint64_t before = top->getHash();
        // Through the unload event, which is the path a script's Delete and a
        // removeTag of a relation take; the arena's own deleteEntity only
        // knows records it allocated, and a child's record is in its parent's.
        ETCS::EntityUnloadEvent{side}();
        ETCS::PendingUnloadRegistry::getInstance().join_all();
        check(!top->hashCurrent(), "a child leaving makes the parent stale");
        check(top->getHash() != before, "...and moves its hash");
    }

    // -- 6. the root pull and divergence ---------------------------------------
    {
        const uint64_t lazy = top->getHash();
        ETCS::Entity::HashAudit a = etcs_root_hash(top);
        check(a.nodes == 3, "the audit visited the top and every node under it");
        std::printf("        (nodes %zu, diverged %zu, skipped %zu)\n", a.nodes, a.diverged, a.skipped);
        check(a.diverged == 0, "a clean tree has no divergence");
        check(a.top_hash == lazy, "the audit's recompute agrees with the lazy pull");
        bool nonzero = false;
        for (unsigned char c : a.root) nonzero |= (c != 0);
        check(nonzero, "the root digest is set");

        ETCS::Entity::HashAudit again = etcs_root_hash(top);
        check(std::equal(a.root, a.root + 32, again.root), "the root digest is stable across audits");

        // A surface moved BEHIND THE FUNNEL: the leaf's cache is stamped
        // current, then its epoch is rolled back so the funnel's bump is undone
        // -- the only way to produce "current cache, different state" from
        // here, since every real mutator bumps. This is what an unrecorded
        // write looks like to the audit.
        leaf->getHash();
        const uint32_t e = leaf->hashEpoch();
        leaf->addTag(ETCS::Buffer("unseen"));
        leaf->stampHash(leaf->hashCached(), leaf->hashEpoch());   // forge: claim current
        (void)e;
        check(leaf->hashCurrent(), "(setup) the leaf claims a current cache over a moved surface");
        // The ancestors WERE bumped by the flag; roll them so the audit's own
        // marking is what makes them stale, not the setup.
        mid->getHash(); top->getHash();
        check(top->hashCurrent() && mid->hashCurrent(), "(setup) the tree above is current");

        ETCS::Entity::HashAudit d = etcs_root_hash(top);
        std::printf("        (diverged %zu)\n", d.diverged);
        // Three, not one: mid and top were computed from the leaf's lie, so
        // their caches are wrong too. The audit reports the whole path, and
        // the deepest node on it is where the write happened.
        check(d.diverged == 3, "the audit reports the diverged path: leaf, mid, top");
        check(!leaf->hashCurrent(), "...and leaves each diverged node stale, so the next pull recomputes");
        check(leaf->getHash() == leaf->computeNodeHash(nullptr, nullptr), "...to its true hash");
        check(!std::equal(a.root, a.root + 32, d.root), "...and the root digest moved");
        check(top->getHash() == d.top_hash, "the lazy pull after an audit agrees with it");

        leaf->removeTag(ETCS::Buffer("unseen"));
        ETCS::Entity::HashAudit r = etcs_root_hash(top);
        check(std::equal(a.root, a.root + 32, r.root),
              "removing the unseen flag restores the original root digest");
        check(r.diverged == 0, "...with nothing left diverged");
    }

    // -- 7. the level rule ------------------------------------------------------
    {
        check(top->isGlobalScope() && !mid->isGlobalScope(), "top is parentless, mid is not");
        unsigned char d[32], d2[32];
        top->getDigest(d);
        uint64_t first = 0; std::memcpy(&first, d, sizeof(first));
        check(top->getHash() == first, "a parentless node's hash is the first word of its digest");
        ETCS::Entity::HashAudit a = etcs_root_hash(top);
        check(std::equal(d, d + 32, a.root), "...and the audit's root of it is that digest");
        mid->getDigest(d2);
        uint64_t mfirst = 0; std::memcpy(&mfirst, d2, sizeof(mfirst));
        check(mid->getHash() != mfirst, "a child's hash is not its digest's first word (XXH3, not SHA)");
        top->getDigest(d2);
        check(std::equal(d, d + 32, d2), "a second digest pull with nothing moved is identical");
        leaf->addTag(ETCS::Buffer("deep"));
        top->getDigest(d2);
        check(!std::equal(d, d + 32, d2), "a flag three levels down moves the top's digest");
        leaf->removeTag(ETCS::Buffer("deep"));
        top->getDigest(d2);
        check(std::equal(d, d + 32, d2), "...and taking it off restores it (A-B-A)");
    }

    // -- 8. the root above every branch -------------------------------------------
    {
        ETCS::EventNode* ldr = ETCS::etcs_loader_event_node();
        check(ldr != nullptr, "the loader EventNode is reachable");
        ETCS::Entity* img = ETCS::spawn_entity("RenderProvider", "ImageSurface", env, loader);
        check(img != nullptr, "RenderProvider:ImageSurface spawns");
        if (ldr && img)
        {
            img->call("ImageSurface.Create", "8 8", ctx);
            unsigned char g0[32], g1[32], g2[32];
            size_t modules = 0, entities = 0;
            etcs_global_root_hash(ldr, g0, &modules, &entities);
            std::printf("        (global root: %zu module%s, %zu parentless entit%s)\n",
                        modules, modules == 1 ? "" : "s", entities, entities == 1 ? "y" : "ies");
            check(modules >= 1 && entities >= 1, "the global root saw at least one module with an entity");
            size_t m2 = 0, e2 = 0;
            etcs_global_root_hash(ldr, g1, &m2, &e2);
            check(std::equal(g0, g0 + 32, g1) && m2 == modules && e2 == entities,
                  "a second global pull with nothing moved is identical");

            img->addTag(ETCS::Buffer("marked"));
            etcs_global_root_hash(ldr, g2);
            check(!std::equal(g0, g0 + 32, g2), "a flag on a provider's entity moves the global root");
            img->removeTag(ETCS::Buffer("marked"));
            etcs_global_root_hash(ldr, g2);
            check(std::equal(g0, g0 + 32, g2), "...and taking it off restores it (A-B-A)");

            // The module's own root, from the loader's mirror rows for it,
            // must agree with itself across pulls and move with its entity.
            std::vector<const ETCS::RIDListHandle*> rows;
            for (auto& bucket : ldr->ridMirror)
                for (auto& row : bucket.second)
                    if (row.module == ETCS::Buffer("RenderProvider")) rows.push_back(&row.handle);
            check(!rows.empty(), "the loader mirrors RenderProvider's lists");
            unsigned char m0[32], m1[32];
            size_t n0 = 0;
            etcs_module_root_hash(rows, m0, &n0);
            check(n0 >= 1, "the module root counted the parentless ImageSurface");
            img->addTag(ETCS::Buffer("marked"));
            etcs_module_root_hash(rows, m1);
            check(!std::equal(m0, m0 + 32, m1), "a flag moves the module root");
            img->removeTag(ETCS::Buffer("marked"));

            const ETCS::RID img_rid = img->getRID();
            img->call("ImageSurface.Delete", "", ctx);
            ETCS::PendingUnloadRegistry::getInstance().join_all();
            (void)img_rid;
            etcs_global_root_hash(ldr, g2);
            check(!std::equal(g0, g0 + 32, g2), "an entity leaving the registry moves the global root");
        }
    }

    // -- 9. depth ----------------------------------------------------------------
    {
        std::cout << "\n-- 9. a deep chain --\n";
        constexpr uint32_t DEPTH = 20000;
        auto t0 = std::chrono::steady_clock::now();
        auto ms = [&t0]() { const auto now = std::chrono::steady_clock::now(); const double v = std::chrono::duration<double, std::milli>(now - t0).count(); t0 = now; return v; };
        // The same count attached flat first: what an attach costs by itself
        // (the loader stream's round trip, mostly), so the chain's line below
        // reads as the price of depth and not of attaching.
        Node* flat = arena.allocate<Node>();
        for (uint32_t i = 0; i < DEPTH; ++i) flat->addTag<Node>();
        std::printf("        (attached %u flat in %.0f ms)\n", DEPTH, ms());
        arena.deleteEntity(flat, true);
        std::printf("        (cascade-deleted %u flat in %.0f ms)\n", DEPTH, ms());
        Node* root = arena.allocate<Node>();
        Node* tip  = root;
        for (uint32_t i = 0; i < DEPTH; ++i) tip = tip->addTag<Node>();
        std::printf("        (built %u levels in %.0f ms)\n", DEPTH, ms());
        check(root->depth() == 0 && tip->depth() == DEPTH, "every level knows its depth");

        const uint64_t deep0 = root->getHash();
        std::printf("        (pulled in %.0f ms)\n", ms());
        check(root->hashCurrent() && tip->hashCurrent(), "a pull through the whole chain fills every cache");
        ETCS::Entity::HashAudit a = etcs_root_hash(root);
        check(a.nodes == DEPTH + 1 && a.diverged == 0 && a.top_hash == deep0,
              "the audit walks every level and agrees with the pull");

        ETCS::ObserverEdge edge = root->Observe(root->getRID());
        (void)root->TakeObserved(edge);
        tip->addTag(ETCS::Buffer("deep"));
        check(!root->hashCurrent(), "a flag at the tip reaches the root's epoch");
        check(root->TakeObserved(edge), "...and its observer, through every level between");
        check(root->getHash() != deep0, "and the root hash moved");
        tip->removeTag(ETCS::Buffer("deep"));
        check(root->getHash() == deep0, "A-B-A at depth");

        // And down again: the cascade delete of the whole chain is a loop
        // over an explicit stack (MemoryArena::cascade), so a branch this
        // deep goes without a frame per level -- a depth that overflowed
        // the recursive teardown before. (Forty thousand ran too: 0.9 s to
        // delete; what bounds this tester is the BUILD: every attach climbs
        // to the root twice -- the epoch-and-observer mark on the loader's
        // thread, the provenance anchor on the caller's -- and over a chain
        // each level is a cache miss, ~130 ns per level per attach. The
        // flat line above is the same count with nothing to climb.)
        (void)ms();
        arena.deleteEntity(root, true);
        std::printf("        (cascade-deleted %u levels in %.0f ms)\n", DEPTH, ms());
        Node* again = arena.allocate<Node>();
        check(again != nullptr && again->getHash() != 0, "the arena is whole after the cascade: a new node hashes");
        arena.deleteEntity(again, true);
    }

    // -- 10. ties are the attach order, not the map's ----------------------------
    {
        std::cout << "\n-- 10. equal keys keep the order they arrived in --\n";
        Node* parent = arena.allocate<Node>();
        std::vector<ETCS::RID> made;
        for (int i = 0; i < 24; ++i) made.push_back(parent->addTag<Node>()->getRID());   // all compare equal
        std::vector<std::pair<ETCS::Buffer, ETCS::RID>> kids;
        parent->getOrderedTypedChildren(kids);
        std::vector<ETCS::RID> seen;
        for (auto const& k : kids) seen.push_back(k.second);
        check(seen == made, "an ordered read of equivalent children is the attach order");
        kids.clear(); seen.clear();
        parent->getTypedChildren(kids);
        for (auto const& k : kids) seen.push_back(k.second);
        check(seen == made, "...and so is the plain read: the hash map's walk is nobody's order");
        // Build the same tree again and the ordered reads agree child for
        // child -- what a native run and a browser run of one script see.
        Node* twin = arena.allocate<Node>();
        for (int i = 0; i < 24; ++i) twin->addTag<Node>();
        std::vector<std::pair<ETCS::Buffer, ETCS::RID>> a, b;
        parent->getOrderedTypedChildren(a); twin->getOrderedTypedChildren(b);
        bool same = a.size() == b.size();
        for (size_t i = 0; same && i < a.size(); ++i) same = (a[i].first == b[i].first);
        check(same && parent->getHash() == twin->getHash(), "the same tree built twice reads and hashes the same");

        // Members leaving keep the survivors' order, through the tombstones
        // and across the compaction that follows once most have gone
        // (RIDList::forget_arrival); one arriving after that goes last.
        std::vector<ETCS::RID> expect = made;
        const ETCS::Buffer node_tag = a.front().first;
        auto leave = [&](size_t i) {
            ETCS::EntityUnloadEvent{parent->getTypedChild(node_tag, expect[i])}();
            ETCS::PendingUnloadRegistry::getInstance().join_all();
            expect.erase(expect.begin() + i);
        };
        leave(3); leave(0); leave(expect.size() - 1);           // three tombstones
        for (int i = 0; i < 12; ++i) leave(1);                    // past the half: compacted
        expect.push_back(parent->addTag<Node>()->getRID());
        kids.clear(); seen.clear();
        parent->getOrderedTypedChildren(kids);
        for (auto const& k : kids) seen.push_back(k.second);
        check(seen == expect, "members leaving keep the survivors' order; a late arrival goes last");
        kids.clear(); seen.clear();
        parent->getTypedChildren(kids);
        for (auto const& k : kids) seen.push_back(k.second);
        check(seen == expect, "...on the plain read as well");

        arena.deleteEntity(parent, true);
        arena.deleteEntity(twin, true);
    }

    std::printf("\n%s (%d failure%s)\n", g_fail ? "FAILED" : "PASSED",
                g_fail, g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}
