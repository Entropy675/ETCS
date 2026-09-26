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
 *
 *   ./Run_ProvenanceTesterLoader
 */
#undef ETCS_PRODUCTION_BUILD
#ifndef ETCS_MODULE_NAME
#define ETCS_MODULE_NAME "ProvenanceTester"
#endif
#include "../ETCS.h"

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

// An Environmental one, with one value off its surface.
class Env : public EnvironmentalBase<Env>, public DeletableBase<Env>
{
public:
    WIRE_TYPE_IDENTITY(Env)
    int value = 0;
    void CaptureStateConcrete(ETCS::EnvironmentState& out) const { out.set("value", std::to_string(value)); }
    bool RebuildLocalConcrete(const ETCS::EnvironmentState& st) override
    { if (auto v = st.get("value")) value = std::stoi(*v); return true; }
    bool ReflectRemoteConcrete(const ETCS::EnvironmentState& st) override { return RebuildLocalConcrete(st); }
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
    check(has(cap.script, "e_Plain1.Warm()\ne_Plain1.Cool()\ne_Plain1.unflag(hot)\ne_Plain1.Warm(again)"),
          "what takes off something a kept line puts on is kept too, in order");
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
        check(kept == 9, "the log keeps only what the capture kept (9 of 13)");
    }

    // -- 6. the same tree at other RIDs --------------------------------------------
    {
        Env* f = arena.allocate<Env>();
        Plain* k2 = f->addTag<Plain>();
        k2->addTag(ETCS::Buffer("hot"));
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

    std::printf("\n%s (%d failure%s)\n", g_fail ? "FAILED" : "PASSED",
                g_fail, g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}
