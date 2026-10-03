/*
 * ProvenanceTesterLoader -- the record an Environmental entity keeps of how
 * it came to be (core/Provenance.h), and the script it compacts to
 * (etcs_replay_capture, core/Entity.h), proved against a tree built here.
 *
 * No module: the frames a script's executor would open are opened by hand
 * (ETCS::ActionScope), and the leaves are this file's own types, parented
 * with addTag<T> -- the same isolation HashTesterLoader uses.
 *
 * What it proves, in order:
 *
 *   1. Opt-in: nothing is recorded under an entity that does not claim
 *      Environmental.
 *   2. One record per frame: every change an action makes, however many,
 *      is credited to the one line that started it.
 *   3. Compaction: what was made and then removed is gone from the script
 *      and from the log; whatever takes off something a kept line puts on
 *      is kept with it, in order.
 *   4. Names, never RIDs: a child a line made is named from its parent,
 *      type and order; @names in a payload are renamed to match; a stream
 *      line keeps both halves; an Environmental child is captured after
 *      the line that makes it, under the name that line gave it.
 *   5. What cannot be replayed says so: a change outside any action.
 *   6. The same tree made again at other RIDs hashes the same, and so does
 *      the one the captured script describes.
 *   7. EnvironmentState packs, unpacks and migrates its keys.
 *   8. A frame crosses a call on its SignalContext: the callee runs inside
 *      the caller's, and a call carrying none opens its own.
 *   9. A move is its own action: an entity that changes parents
 *      (Entity::moveTo) is recorded as `<to>.Contain(@<it>)` whatever was
 *      running around it, a replay keeps the last move only, and its
 *      lifetime goes with it (its token, not its bytes) -- a move's, and a
 *      deleted parent's handing its children up.
 *
 *   ./Run_ProvenanceTesterLoader
 */
#undef ETCS_PRODUCTION_BUILD
#ifndef ETCS_MODULE_NAME
#define ETCS_MODULE_NAME "ProvenanceTester"
#endif
#include "../ETCS.h"

#include <algorithm>
#include <cstdio>
#include <iostream>
#include <map>
#include <string>
#include <vector>

static int g_fail = 0;
static void check(bool ok, const char* what)
{
    std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++g_fail;
}

// A plain leaf: claims nothing of the family.
class Plain : public DeletableBase<Plain>
{
public:
    WIRE_TYPE_IDENTITY(Plain)
    bool DeleteConcrete() override { return true; }
};

// A movable leaf (etcs_movable): its bytes in a stable home, its lifetime
// token in its parent's chain, so it can change parents. Counted, for §9.
class Mover : public DeletableBase<Mover>
{
public:
    WIRE_TYPE_IDENTITY(Mover)
    static constexpr bool kEtcsMovable = true;
    static inline int s_alive = 0;
    Mover()  { ++s_alive; }
    ~Mover() { --s_alive; }
    bool DeleteConcrete() override { return true; }
};

// An Environmental one, with one value of its own BOUND to its surface
// (Entity::bindValue): read off it when captured, handed back on restore.
class Env : public EnvironmentalBase<Env>, public DeletableBase<Env>
{
public:
    WIRE_TYPE_IDENTITY(Env)
    int value = 0;
    bool drift = false;   // the adversary of a read: the value moves on every one (§7c)
    Env()
    {
        bindValue(ETCS::Buffer("value"), ETCS::Entity::ValueBinding{
            this,
            [](void* self, std::string& out) { Env* me = static_cast<Env*>(self); out = std::to_string(me->drift ? me->value++ : me->value); },
            [](void* self, const std::string& in) { static_cast<Env*>(self)->value = std::stoi(in); return true; },
            [](void* self) { return static_cast<uint64_t>(static_cast<Env*>(self)->value); } });
    }
    bool RebuildLocalConcrete(const ETCS::EnvironmentState&) override { return true; }
    bool ReflectRemoteConcrete(const ETCS::EnvironmentState&) override { return true; }
    bool DeleteConcrete() override { return true; }
};

static ETCS::ActionLine line(ETCS::Entity* on, const std::string& verb, const std::string& payload,
                             std::vector<std::pair<std::string, ETCS::RID>> refs = {})
{ return ETCS::ActionLine{ on->getRID(), verb, payload, std::move(refs), 0, {} }; }

static bool has(const std::string& s, const std::string& what) { return s.find(what) != std::string::npos; }

int main()
{
    shell_startup();
    WIRE_CONTEXT();
    auto& arena = ETCS::MemoryArena::getInstance();
    const std::string P = std::string(ETCS_MODULE_NAME) + "::Plain";
    const std::string E = std::string(ETCS_MODULE_NAME) + "::Env";

    std::cout << "=== Provenance ===" << std::endl;

    // -- 1. opt-in -------------------------------------------------------------
    {
        Plain* p = arena.allocate<Plain>();
        { ETCS::ActionScope s(line(p, "Mark", "")); p->addTag(ETCS::Buffer("hot")); }
        check(!p->isEnvironmental() && !p->environmentalWire(), "a plain entity records nothing");
    }

    Env* e = arena.allocate<Env>();
    check(!e->isEnvironmental(), "what an entity does while being made is not recorded");
    ETCS::etcs_supertype_fanout(e);   // joining the graph, as _make_ does it
    check(e->isEnvironmental(), "claiming the family turns recording on once it is in the graph");
    auto logOf = [](ETCS::Entity* x) { return x->environmentalWire()->ActionLog(); };

    // -- 2. one record per frame -----------------------------------------------
    Plain* kid = nullptr;
    {
        ETCS::ActionScope s(line(e, "spawn", P));
        kid = e->addTag<Plain>();
    }
    {
        ETCS::ActionScope s(line(kid, "Warm", ""));
        kid->addTag(ETCS::Buffer("hot"));
        kid->addTag(ETCS::Buffer("lit"));
    }
    {
        auto log = logOf(e);
        check(log.size() == 2, "two actions, two records");
        check(log.size() == 2 && log[1].created.size() == 2, "...the second holding both of its flags");
        check(log.size() == 2 && log[1].line.receiver == kid->getRID(), "...credited to the line's receiver");
    }

    // -- 3. compaction ---------------------------------------------------------
    Plain* scratch = nullptr;
    { ETCS::ActionScope s(line(e, "spawn", P)); scratch = e->addTag<Plain>(); }
    {
        // What DestroyEvent records for a module's Delete (DynamicLoader.h),
        // said here: this file's types are in no module a DestroyEvent could
        // resolve. The script test (DatabaseProvider/scripts/persistence.etcs)
        // goes through the real one.
        ETCS::ActionScope s(line(scratch, "Delete", ""));
        const ETCS::RID gone = scratch->getRID();
        ETCS::EntityUnloadEvent{ scratch }();
        ETCS::PendingUnloadRegistry::getInstance().join_all();
        ETCS::Entity::recordEffect(e, ETCS::Entity::childKey(gone), false);
    }
    { ETCS::ActionScope s(line(kid, "Cool", "")); kid->removeTag(ETCS::Buffer("lit")); }
    { ETCS::ActionScope s(line(kid, "unflag", "hot")); kid->removeTag(ETCS::Buffer("hot")); }
    { ETCS::ActionScope s(line(kid, "Warm", "again")); kid->addTag(ETCS::Buffer("hot")); }
    { ETCS::ActionScope s(line(kid, "Blink", "")); kid->addTag(ETCS::Buffer("flash")); }
    { ETCS::ActionScope s(line(kid, "Unblink", "")); kid->removeTag(ETCS::Buffer("flash")); }
    // A kept line that put on two things, one of which a later line took off.
    { ETCS::ActionScope s(line(kid, "Light", "")); kid->addTag(ETCS::Buffer("glow")); kid->addTag(ETCS::Buffer("spark")); }
    { ETCS::ActionScope s(line(kid, "Dim", "")); kid->removeTag(ETCS::Buffer("spark")); }
    // A value set three times.
    for (int lv = 1; lv <= 3; ++lv)
    { ETCS::ActionScope s(line(kid, "SetLevel", std::to_string(lv))); kid->addTag(ETCS::Buffer("level"), std::to_string(lv)); }

    // -- 4. names ---------------------------------------------------------------
    { ETCS::ActionScope s(line(e, "Link", "to @k", { { "k", kid->getRID() } })); e->addTag(ETCS::Buffer("linked")); }
    {
        ETCS::ActionLine l = line(kid, "Produce", "x");
        l.stream_to   = e->getRID();
        l.stream_verb = "Consume";
        ETCS::ActionScope s(l);
        e->addTag(ETCS::Buffer("fed"));
    }
    Env* inner = nullptr;
    {
        ETCS::ActionScope s(line(e, "spawn", E));
        inner = e->addTag<Env>();
        inner->addTag(ETCS::Buffer("warmed_at_birth"));   // the spawn's doing, not inner's
    }
    { ETCS::ActionScope s(line(inner, "Mark", "")); inner->addTag(ETCS::Buffer("inner_on")); }

    // -- 5. outside any action ---------------------------------------------------
    e->addTag(ETCS::Buffer("stray"));

    ETCS::ReplayCapture cap;
    std::map<ETCS::RID, std::string> names;
    ETCS::etcs_replay_capture(e, "e", names, cap);
    std::printf("%s", cap.script.c_str());
    for (auto& w : cap.warnings) std::printf("        warning: %s\n", w.c_str());

    check(has(cap.script, "e.spawn(" + P + " e_Plain1)"), "the kept child's spawn line names it e_Plain1");
    check(!has(cap.script, "e_Plain2") && !has(cap.script, "Delete"),
          "a child made and deleted leaves no line");
    check(!has(cap.script, "Blink"), "a flag put on and taken off, and nothing else, leaves neither line");
    check(has(cap.script, "e_Plain1.Warm(again)") && !has(cap.script, "e_Plain1.Warm()")
          && !has(cap.script, "Cool") && !has(cap.script, "unflag(hot)"),
          "a flag put on again is kept by its last setter: what the first put on is all undone or set again");
    check(has(cap.script, "e_Plain1.Light()\ne_Plain1.Dim()"),
          "what takes off something a kept line puts on is kept too, in order");
    check(has(cap.script, "e_Plain1.SetLevel(3)") && !has(cap.script, "SetLevel(1)") && !has(cap.script, "SetLevel(2)"),
          "a value set again: only the last set is kept -- the record does not grow with a value that moves");
    check(has(cap.script, "e.Link(to @e_Plain1)"), "an @name in a payload is renamed to the rebuilt child");
    check(has(cap.script, "e_Plain1.Produce(x) -> e.Consume()"), "a stream line keeps both halves");
    check(has(cap.script, "e.spawn(" + E + " e_Env1)\n"), "an Environmental child is made by its parent's line");
    check(has(cap.script, "e_Env1.Mark()"), "...and captured after it, under the name it gave");
    {
        size_t spawns = 0;
        for (size_t at = 0; (at = cap.script.find("e.spawn(" + E, at)) != std::string::npos; ++at) ++spawns;
        check(spawns == 1 && !has(cap.script, "e_Env1.spawn"),
              "what the line that made it did to it is that line's, replayed once");
    }
    check(cap.environmental.size() == 2 && cap.environmental[1].first == "e_Env1",
          "both are reported, parent first");
    bool stray = false;
    for (auto& w : cap.warnings) stray |= has(w, "outside any action");
    check(stray, "a change outside any action is a warning, not a guess");
    check(cap.script.find("RID") == std::string::npos, "no RID is written");
    {
        size_t kept = 0;
        for (auto& r : logOf(e)) (void)r, ++kept;
        check(kept == 9, "the log keeps only what the capture kept (9 of 18)");
    }

    // -- 6. the same tree at other RIDs --------------------------------------------
    {
        Env* f = arena.allocate<Env>();
        Plain* k2 = f->addTag<Plain>();
        k2->addTag(ETCS::Buffer("hot"));
        k2->addTag(ETCS::Buffer("glow"));
        k2->addTag(ETCS::Buffer("level"), "3");
        f->addTag(ETCS::Buffer("linked"));
        f->addTag(ETCS::Buffer("fed"));
        Env* i2 = f->addTag<Env>();
        i2->addTag(ETCS::Buffer("inner_on"));
        i2->addTag(ETCS::Buffer("warmed_at_birth"));
        f->addTag(ETCS::Buffer("stray"));
        check(f->getRID() != e->getRID(), "(setup) the second tree has other RIDs");
        check(f->getHash() == e->getHash(), "the tree the script describes hashes the same as the one captured");
        f->removeTag(ETCS::Buffer("stray"));
        check(f->getHash() != e->getHash(), "...and one flag apart, not");
    }

    // -- 6b. twins at other RIDs --------------------------------------------------
    {
        std::cout << "\n-- 6b. twins at other RIDs --\n";
        // Two twins (one type, one surface) holding different values, attached
        // first-then-second. Made again until the RIDs fall the other way
        // round: the state hash must not notice -- the values walk in the
        // canonical order (identity, then attach order), never by RID.
        auto make = [&arena](Env*& a, Env*& b) {
            Env* r = arena.allocate<Env>(); ETCS::etcs_supertype_fanout(r);
            a = r->addTag<Env>(); b = r->addTag<Env>();
            a->value = 1; b->value = 2;
            return r;
        };
        Env *a0, *b0;
        Env* first = make(a0, b0);
        std::vector<Env*> spent;
        Env* other = nullptr;
        for (int i = 0; i < 64 && !other; ++i)
        {
            Env *a1, *b1;
            Env* r = make(a1, b1);
            if ((a1->getRID() < b1->getRID()) != (a0->getRID() < b0->getRID())) other = r;
            else spent.push_back(r);
        }
        check(other != nullptr, "(setup) a second tree whose twins' RIDs fall the other way round");
        if (other) check(other->getHash() == first->getHash(), "the same state at RIDs in the other order hashes the same");
        b0->value = 3;
        if (other) check(other->getHash() != first->getHash(), "...and the twins' values still count");
        for (Env* r : spent) arena.deleteEntity(r, true);
        if (other) arena.deleteEntity(other, true);
        arena.deleteEntity(first, true);
    }

    // -- 7. EnvironmentState -----------------------------------------------------
    {
        ETCS::EnvironmentState st;
        st.set("old", std::string("a\0b", 3));
        st.set("n", "7");
        ETCS::EnvironmentState back;
        check(back.unpack(st.pack()) && back.kv.size() == 2 && *back.get("old") == std::string("a\0b", 3),
              "state packs and unpacks, binary-safe");
        back.migrate("old=mid\nmid=new\n");
        check(back.get("new") && !back.get("old"), "migration applies its renames oldest first");
        check(!back.unpack(std::string("\x05\x00\x00\x00ab", 6)), "a truncated state is refused");
    }

    // -- 7b. the value surface ----------------------------------------------------
    {
        std::cout << "\n-- 7b. values behind tags --\n";
        Env* v = arena.allocate<Env>();
        ETCS::etcs_supertype_fanout(v);
        const uint64_t id0 = v->identityHash(), s0 = v->getHash();
        v->value = 41;
        ETCS::EnvironmentState st;
        etcs_capture_values(v, st);
        check(st.get("value") && *st.get("value") == "41", "a bound value is read off the surface by the capture");
        check(v->identityHash() == id0, "...and is not in the identity hash: what it is did not change");
        check(v->getHash() != s0, "...and is in the state hash: what is so did");
        const uint64_t s1 = v->getHash();
        v->value = 42;
        check(v->getHash() != s1 && v->identityHash() == id0, "every move of the value moves the state hash and not the identity");

        size_t before = logOf(v).size();
        { ETCS::ActionScope s(line(v, "Set", "k")); v->addTag(ETCS::Buffer("limit"), "7"); }
        std::string got;
        check(v->valueOf(ETCS::Buffer("limit"), got) && got == "7", "a stored value goes on through the funnel and reads back");
        check(logOf(v).size() == before + 1, "...recorded as the action that set it");
        { ETCS::ActionScope s(line(v, "Set", "k")); check(!v->addTag(ETCS::Buffer("limit"), "7"), "setting the same value again is no transition"); }
        { ETCS::ActionScope s(line(v, "Set", "k")); check(v->addTag(ETCS::Buffer("limit"), "8"), "a different value is one"); }
        v->removeTag(ETCS::Buffer("limit"));
        check(!v->valueOf(ETCS::Buffer("limit"), got), "the flag off takes its value with it");

        Env* w = arena.allocate<Env>();
        ETCS::etcs_supertype_fanout(w);
        w->addTag(ETCS::Buffer("limit"), "0");
        ETCS::EnvironmentState keep;
        v->addTag(ETCS::Buffer("limit"), "9");
        etcs_capture_values(v, keep);
        check(etcs_restore_values(w, keep) == 2 && w->value == 42, "a capture restores onto another of the kind: the binding read it back...");
        check(w->valueOf(ETCS::Buffer("limit"), got) && got == "9", "...and the stored value landed");
        check(w->getHash() == v->getHash(), "the two now have one state hash");
        ETCS::EnvironmentState stray; stray.set("nowhere", "1");
        check(etcs_restore_values(w, stray) == 0, "a key the surface has no place for is skipped, and counted out");
    }

    // -- 7c. one state per read ---------------------------------------------------
    {
        std::cout << "\n-- 7c. one state per read --\n";
        // A tree whose child's value moves between any two reads -- what a
        // live scene does to a save -- and the same tree, still.
        Env* d = arena.allocate<Env>(); ETCS::etcs_supertype_fanout(d);
        Env* dc = d->addTag<Env>(); dc->drift = true;
        Env* f = arena.allocate<Env>(); ETCS::etcs_supertype_fanout(f);
        Env* fc = f->addTag<Env>();
        auto onto = [](const FrozenTree& from, ETCS::Entity* root) {
            FrozenTree to; etcs_freeze(root, to);
            for (size_t i = 0; i < from.nodes.size() && i < to.nodes.size(); ++i)
            { ETCS::EnvironmentState st; for (auto& [k, v] : from.nodes[i].kv) st.set(k, v); etcs_restore_values(to.nodes[i].e, st); }
        };

        const uint64_t live = d->getHash();
        ETCS::EnvironmentState kv; etcs_capture_values(dc, kv);
        etcs_restore_values(fc, kv);
        check(f->getHash() != live, "read live, a moving tree's hash and its values are two states");

        FrozenTree t;
        check(etcs_freeze(d, t), "a frozen read of it settles");
        onto(t, f);
        check(f->getHash() == t.state_hash, "frozen, the hash kept is the hash of the values kept");

        int calls = 0;
        FrozenTree t2;
        const bool ok = etcs_freeze(f, t2, [&]() { if (calls++ == 0) f->addTag(ETCS::Buffer("moved")); });
        check(ok && calls == 2, "a change through the funnel during the read moves the root's hash epoch, and the read is made again");
        check(std::find(t2.nodes[0].flags.begin(), t2.nodes[0].flags.end(), "moved") != t2.nodes[0].flags.end()
              && t2.state_hash == f->getHash(), "...and the second read is the state after it");
        arena.deleteEntity(d, true);
        arena.deleteEntity(f, true);

        // Only what moved is read again (the last read given as `prev`).
        Env* g = arena.allocate<Env>(); ETCS::etcs_supertype_fanout(g);
        Env* g1 = g->addTag<Env>();
        Env* g2 = g->addTag<Env>();
        FrozenTree r0; etcs_freeze(g, r0);
        const uint64_t h0 = r0.state_hash;
        FrozenTree r1; etcs_freeze(g, r1, {}, 4, &r0);
        check(r0.copied == 3 && r1.copied == 0 && r1.state_hash == h0,
              "given the last read, with nothing moved, a read reads nothing again -- and is the same state");
        g2->value = 9;
        FrozenTree r2; etcs_freeze(g, r2, {}, 4, &r1);
        check(r2.copied == 1 && r2.state_hash == g->getHash(),
              "a bound value that moved (its digest says so; no epoch did): that node alone is read again");
        g1->addTag(ETCS::Buffer("set"), "x");
        FrozenTree r3; etcs_freeze(g, r3, {}, 4, &r2);
        check(r3.copied == 2 && r3.state_hash == g->getHash(),
              "a stored value set (the epoch moves up its path): the node and the path above it");
        arena.deleteEntity(g, true);
    }

    // -- 8. carried across a call -------------------------------------------------
    {
        ETCS::ActionScope outer(line(e, "Outer", ""));
        ETCS::ActionFrame* here = ETCS::current_action_frame();
        ETCS::SignalContext ctx;
        ctx.frame = here;                               // what WorkBundle::operator() stamps
        ETCS::current_action_frame() = nullptr;         // a module's own slot, empty
        {
            ETCS::ActionScope callee(ctx.frame, kid->getRID(), ETCS::Buffer("Inner"), ETCS::Buffer(""));
            check(ETCS::current_action_frame() == here, "a call carrying a frame runs inside it");
        }
        check(ETCS::current_action_frame() == nullptr, "...and leaves the callee's slot as it found it");
        {
            ETCS::ActionScope own(nullptr, kid->getRID(), ETCS::Buffer("Alone"), ETCS::Buffer("p"));
            ETCS::ActionFrame* f = ETCS::current_action_frame();
            check(f && f != here && f->lazy, "a call carrying none opens its own, lazily");
            if (f) f->settle();
            check(f && f->line.verb == "Alone" && f->line.payload == "p", "...and names itself once settled");
        }
        ETCS::current_action_frame() = here;
        check((ETCS::next_action_frame_id() >> 63) != 0, "frame ids carry their binary's salt");
    }

    // -- 9. a move ----------------------------------------------------------------
    {
        std::cout << "\n-- 9. a move is its own action --\n";
        const std::string M = std::string(ETCS_MODULE_NAME) + "::Mover";
        Env* g = arena.allocate<Env>();
        ETCS::etcs_supertype_fanout(g);
        Plain* left  = nullptr;
        Plain* right = nullptr;
        Mover* m     = nullptr;
        { ETCS::ActionScope s(line(g, "spawn", P)); left  = g->addTag<Plain>(); }
        { ETCS::ActionScope s(line(g, "spawn", P)); right = g->addTag<Plain>(); }
        { ETCS::ActionScope s(line(left, "spawn", M)); m = left->addTag<Mover>(); }
        check(m->isMovable() && !left->isMovable(), "a movable type's child is made movable; a plain one is not");
        // Moves made inside another line: a step's moves are not the step's.
        { ETCS::ActionScope s(line(g, "Run", "1"));
          check(m->moveTo(right), "an entity changes parents, keeping its RID");
          m->moveTo(left);
          m->moveTo(right); }
        check(m->getParent() == right, "...and is where its last move put it");
        Plain* deep = nullptr;
        { ETCS::ActionScope s(line(right, "spawn", P)); deep = right->addTag<Plain>(); }
        check(!right->moveTo(deep) && !m->moveTo(m), "nothing moves inside itself, and a plain entity does not move");
        ETCS::ReplayCapture cap;
        std::map<ETCS::RID, std::string> nm;
        ETCS::etcs_replay_capture(g, "g", nm, cap);
        std::printf("%s", cap.script.c_str());
        size_t contains = 0;
        for (size_t at = 0; (at = cap.script.find(".Contain(", at)) != std::string::npos; ++at) ++contains;
        check(has(cap.script, "g_Plain1.spawn(" + M + " g_Mover1)") && has(cap.script, "g_Plain2.Contain(@g_Mover1)"),
              "the replay makes it where it was made, then moves it where it is");
        check(contains == 1 && !has(cap.script, "Run(1)"), "...the last move only, and never the line it ran under");
        // The lifetime went with it.
        const int alive = Mover::s_alive;
        left->getOwningArena().deleteEntity(left, true);
        check(Mover::s_alive == alive, "deleting where it was made leaves it");
        right->getOwningArena().deleteEntity(right, true);
        check(Mover::s_alive == alive - 1, "deleting where it is takes it");
        // A parent deleted without its children (reparentChildrenTo) hands a
        // movable child up with its token, so nothing keeps its arena alive.
        Plain* holder = nullptr;
        Mover* m2     = nullptr;
        { ETCS::ActionScope s(line(g, "spawn", P)); holder = g->addTag<Plain>(); }
        { ETCS::ActionScope s(line(holder, "spawn", M)); m2 = holder->addTag<Mover>(); }
        const int alive2 = Mover::s_alive;
        holder->getOwningArena().deleteEntity(holder, false);
        check(m2->getParent() == g && &m2->getOwningArena() == &g->getArena() && Mover::s_alive == alive2,
              "a parent deleted without its children hands a movable one up, its lifetime with it");
        arena.deleteEntity(g, true);
        check(Mover::s_alive == alive2 - 1, "...and deleting where it went takes it");
    }

    std::printf("\n%s (%d failure%s)\n", g_fail ? "FAILED" : "PASSED",
                g_fail, g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}
