// SessionRestartTesterLoader.cc
//
// EVERY SESSION BEGINS AS MADE, IN THE GAMES THEMSELVES. Practice, Host and
// Join each begin a session (TraceBase::beginLocked), and a beginning resets
// the families -- the trace's lines and labels, the lockstep's tick -- and
// then the game (RestartConcrete). The Counter of TraceTesterLoader proves it
// for the families; this proves it for the kart and the golf, whose own reset
// once took the family's name and with it the family's place
// (LockstepBase::restartLocked is final now). For each game:
//
//   1. Practised, its clock runs.
//   2. Hosting starts from tick 0 and its trace from its first line, not where
//      practice left the clock.
//   3. Hosting again after leaving starts the same: tick 0, and none of the
//      last session's lines.
//   4. Joining starts from tick 0, the mirror empty until the host's first
//      line, wherever its own practice had got to.
//
//   ./Run_SessionRestartTesterLoader

#include "../ETCS.h"

#include <cstdio>
#include <iostream>
#include <sstream>
#include <string>

static int g_fail = 0;
static void check(bool ok, const char* what)
{
    std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++g_fail;
}
static std::string verb(ETCS::Entity* e, const std::string& action, const std::string& args = "")
{
    ETCS::Buffer a(action.c_str()), d(args.c_str());
    e->call(a, d);
    return d.toString();
}
// Both games' Status begin "<tick> <lines> <mode> ...".
struct Status { uint64_t tick = 0, lines = 0; std::string mode; };
static Status status(ETCS::Entity* g, const std::string& type)
{
    Status s;
    std::istringstream in(verb(g, type + ".Status"));
    in >> s.tick >> s.lines >> s.mode;
    return s;
}

// One game's sessions, on two of its instances: a host and a guest.
static void sessions(const std::string& type, ETCS::Entity* host, ETCS::Entity* guest)
{
    std::printf("\n-- %s --\n", type.c_str());
    verb(host, type + ".Practice", "solo");
    verb(host, type + ".Run", "120");
    Status s = status(host, type);
    check(s.mode == "practice" && s.tick == 120, "practised: its clock runs (tick 120)");

    verb(host, type + ".Host", "h 0 t1");
    s = status(host, type);
    std::printf("        (hosting: tick %llu, %llu line(s))\n", (unsigned long long)s.tick, (unsigned long long)s.lines);
    check(s.mode == "host" && s.tick == 0, "hosting starts from tick 0, not where practice left the clock");
    const uint64_t opening = s.lines;

    verb(host, type + ".Run", "40");
    s = status(host, type);
    check(s.tick == 40 && s.lines > opening, "...and runs and keeps from there");

    verb(host, type + ".Leave");
    verb(host, type + ".Host", "h 0 t2");
    s = status(host, type);
    check(s.mode == "host" && s.tick == 0 && s.lines == opening,
          "hosting again after leaving: tick 0, and none of the last session's lines");

    verb(guest, type + ".Practice", "g");
    verb(guest, type + ".Run", "77");
    verb(guest, type + ".Join", "g h");
    s = status(guest, type);
    check(s.mode == "guest" && s.tick == 0 && s.lines == 0,
          "joining starts from tick 0, the mirror empty until the host's first line");
}

int main(int, char**)
{
    WIRE_CONTEXT();
    std::cout << "=== Every session begins as made ===\n";

    ETCS::Entity* kart[2] = {};
    for (auto& b : kart)
    {
        ETCS::Entity* world = ETCS::spawn_entity("RenderProvider", "Scene3D", env, loader);
        b = ETCS::spawn_entity("KartProvider", "KartBattle", env, loader);
        if (!world || !b) { std::printf("FAILED (could not spawn a battle)\n"); return 1; }
        verb(world, "Scene3D.Create", "1, 1, 1");
        verb(world, "Scene3D.SetVisible", "0");
        verb(b, "KartBattle.Create");
        verb(b, "KartBattle.BindWorld", std::to_string(world->getRID()));
        ETCS::ExecutionContext made(&root, &ctx);
        made.names["anchor"] = ETCS::NameBinding{ world->getRID(), "RenderProvider", "Scene3D" };
        made.names["battle"] = ETCS::NameBinding{ b->getRID(), "KartProvider", "KartBattle" };
        std::istringstream in("run KartProvider/scripts/kart_arena.etcs anchor=anchor battle=battle\n");
        if (!ETCS::run_script(in, std::string(ETCS_ACE_ROOT) + "/modules/kart_world.etcs", made)) { std::printf("FAILED (the arena script)\n"); return 1; }
    }
    ETCS::Entity* golf[2] = {};
    for (auto& g : golf)
    {
        ETCS::Entity* world = ETCS::spawn_entity("RenderProvider", "Scene3D", env, loader);
        ETCS::Entity* eye   = ETCS::spawn_entity("RenderProvider", "Camera3D", env, loader);
        g = ETCS::spawn_entity("GolfProvider", "GolfMatch", env, loader);
        if (!world || !eye || !g) { std::printf("FAILED (could not spawn a match)\n"); return 1; }
        verb(world, "Scene3D.Create", "1, 1, 1");
        verb(world, "Scene3D.SetVisible", "0");
        verb(eye, "Camera3D.Create", "1024, 768");
        verb(eye, "Camera3D.SetScene", std::to_string(world->getRID()));
        verb(g, "GolfMatch.Create");
        ETCS::ExecutionContext made(&root, &ctx);
        made.names["anchor"] = ETCS::NameBinding{ world->getRID(), "RenderProvider", "Scene3D" };
        made.names["eye"]    = ETCS::NameBinding{ eye->getRID(), "RenderProvider", "Camera3D" };
        made.names["game"]   = ETCS::NameBinding{ g->getRID(), "GolfProvider", "GolfMatch" };
        std::istringstream in("run GolfProvider/scripts/golf_world.etcs anchor=anchor game=game\n"
                              "run GolfProvider/scripts/golf_play.etcs game=game world=anchor eye=eye\n");
        if (!ETCS::run_script(in, std::string(ETCS_ACE_ROOT) + "/modules/golf_made.etcs", made)) { std::printf("FAILED (the course scripts)\n"); return 1; }
    }

    sessions("KartBattle", kart[0], kart[1]);
    sessions("GolfMatch", golf[0], golf[1]);

    std::printf("\n%s (%d failure%s)\n", g_fail ? "FAILED" : "PASSED", g_fail, g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}
