// KartBuildTesterLoader.cc
//
// A SEAT'S BUILD IS ONE LINE, AND THE HOST'S TO TAKE. A driver's garage is
// its own; what it sends is the whole vehicle in a `build` line -- an arena
// vehicle's numbers and look, each `tune`, and parts from a modeller (each a
// shape, a material, a place, a size, a colour and a turn). Every runtime
// builds it from the line, and its seat rides it; the host accepts it,
// rejects it or keeps the driver to the arena's vehicles, and a driver locks
// in only on an arena vehicle or an accepted build. On three runtimes:
//
//   1. ONE LINE, EVERY RUNTIME'S. g1's build, from a modeller's saved lines:
//      as many parts, one mesh a colour, a body the parts' bounds about its
//      ride height, a weight the parts' volumes at their materials'
//      densities. It waits for the host: g1 cannot lock in on it.
//   2. CHECKED WHOLE. A material nobody made: refused, and the build stands.
//   3. ACCEPTED. g1 locks in, and drives it on its own numbers (top 40); the
//      host's own build needs no one's word. Driven, every runtime has the
//      host's hash.
//   4. REJECTED. g1 is back in the kart, the build gone everywhere; sent
//      again, it waits again.
//   5. LOCKED. Kept to the arena's vehicles: the build gone, a new one
//      refused, the moto locked in on; let go, a build is taken again.
//   6. THE SESSION'S. Hosting again, the builds and the locks are gone.
//
//   ./Run_KartBuildTesterLoader
//
// No display, no GPU, no network.

#include "../ETCS.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>

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
static std::string raw(double v) { return std::to_string(static_cast<int64_t>(v * 4294967296.0)); }
// add <id> <shape> at (x y z), sized (w h d), coloured (r g b), facing as made, made of `material`.
static std::string add(int id, const char* shape, double x, double y, double z, double w, double h, double d,
                       double r, double g, double b, const char* material)
{
    return "add " + std::to_string(id) + " " + shape + " " + raw(x) + " " + raw(y) + " " + raw(z) + " "
         + raw(w * 0.5) + " " + raw(h * 0.5) + " " + raw(d * 0.5) + " " + raw(r) + " " + raw(g) + " " + raw(b) + " " + raw(1.0)
         + " " + raw(1.0) + " 0 0 0 " + material;
}
struct Kart { int slot = -1; float speed = 0, y = 0; std::string vehicle; };
static Kart where(ETCS::Entity* b, const std::string& who)
{
    Kart k;
    const std::string row = verb(b, "KartBattle.Kart", who);
    if (row == "-") return k;
    std::istringstream in(row);
    std::string w;
    float f;
    int n;
    in >> k.slot >> f >> f >> f >> f >> k.speed >> n >> w >> n >> k.y >> w >> f >> f >> w >> n >> n >> n >> k.vehicle;
    return k;
}
// LookOf: "<parts> <weight> <colours> <owner> <body>".
struct Look { int parts = -1; float weight = 0; int colours = 0; std::string owner, body; };
static Look look(ETCS::Entity* b, const std::string& name)
{
    Look l;
    std::istringstream in(verb(b, "KartBattle.LookOf", name));
    in >> l.parts >> l.weight >> l.colours >> l.owner;
    std::getline(in, l.body);
    if (!l.body.empty() && l.body[0] == ' ') l.body.erase(0, 1);
    return l;
}
struct Status { uint64_t tick = 0, lines = 0; std::string mode; };
static Status status(ETCS::Entity* b)
{
    Status s;
    std::istringstream in(verb(b, "KartBattle.Status"));
    in >> s.tick >> s.lines >> s.mode;
    return s;
}

int main(int, char**)
{
    WIRE_CONTEXT();
    std::cout << "=== A seat's own vehicle is its lines ===\n";

    ETCS::Entity* battle[3] = {};
    for (auto& b : battle)
    {
        ETCS::Entity* world = ETCS::spawn_entity("RenderProvider", "Scene3D", env, loader);
        b = ETCS::spawn_entity("KartProvider", "KartBattle", env, loader);
        if (!world || !b) { std::printf("FAILED (could not spawn)\n"); return 1; }
        verb(world, "Scene3D.Create", "1, 1, 1");
        verb(world, "Scene3D.SetVisible", "0");
        verb(b, "KartBattle.Create");
        verb(b, "KartBattle.BindWorld", std::to_string(world->getRID()));
        ETCS::ExecutionContext arena(&root, &ctx);
        arena.names["anchor"] = ETCS::NameBinding{ world->getRID(), "RenderProvider", "Scene3D" };
        arena.names["battle"] = ETCS::NameBinding{ b->getRID(), "KartProvider", "KartBattle" };
        std::istringstream in("run KartProvider/scripts/kart_arena.etcs anchor=anchor battle=battle\n");
        if (!ETCS::run_script(in, std::string(ETCS_ACE_ROOT) + "/modules/kart_world.etcs", arena)) { std::printf("FAILED (the arena script)\n"); return 1; }
        verb(b, "KartBattle.AddMap", "strip");
        verb(b, "KartBattle.AddSpawn", "-85, -85");
        verb(b, "KartBattle.AddSpawn", "80, -80");
        verb(b, "KartBattle.AddSpawn", "-80, 80");
    }
    ETCS::Entity* host = battle[0];
    ETCS::Entity* g1 = battle[1];
    ETCS::Entity* g2 = battle[2];
    const std::string arena = verb(host, "KartBattle.Vehicles");

    // A modeller, as the garage has one: its pool and shapes, the arena's materials.
    ETCS::Entity* mworld = ETCS::spawn_entity("RenderProvider", "Scene3D", env, loader);
    ETCS::Entity* model = ETCS::spawn_entity("PaintProvider", "PaintModeler", env, loader);
    if (!mworld || !model) { std::printf("FAILED (could not spawn a modeller)\n"); return 1; }
    verb(mworld, "Scene3D.Create", "1, 1, 1");
    verb(mworld, "Scene3D.SetVisible", "0");
    verb(model, "PaintModeler.Create");
    {
        ETCS::ExecutionContext page(&root, &ctx);
        page.names["world"] = ETCS::NameBinding{ mworld->getRID(), "RenderProvider", "Scene3D" };
        page.names["model"] = ETCS::NameBinding{ model->getRID(), "PaintProvider", "PaintModeler" };
        std::istringstream in("run PaintProvider/scripts/paint_model_world.etcs world=world model=model\n");
        if (!ETCS::run_script(in, std::string(ETCS_ACE_ROOT) + "/modules/kart_build_rig.etcs", page)) { std::printf("FAILED (the model's world)\n"); return 1; }
    }
    for (const char* m : { "steel", "alloy", "plastic", "wood", "foam" }) verb(model, "PaintModeler.AddMaterial", m);

    verb(host, "KartBattle.Host", "host 0 t1");
    verb(g1, "KartBattle.Join", "g1 host");
    verb(g2, "KartBattle.Join", "g2 host");
    {
        ETCS::ExecutionContext wire(&root, &ctx);
        auto bind = [&](const char* n, ETCS::Entity* e) { wire.names[n] = ETCS::NameBinding{ e->getRID(), "KartProvider", "KartBattle" }; };
        bind("host", host); bind("g1", g1); bind("g2", g2);
        std::istringstream lines(
            "detach NetworkProvider/scripts/session_follow.etcs src=host dst=g1\n"
            "detach NetworkProvider/scripts/session_emit.etcs src=g1 dst=host\n"
            "detach NetworkProvider/scripts/session_follow.etcs src=host dst=g2\n"
            "detach NetworkProvider/scripts/session_emit.etcs src=g2 dst=host\n");
        check(ETCS::run_script(lines, std::string(ETCS_ACE_ROOT) + "/modules/kart_wiring.etcs", wire), "the streams are wired with the page's scripts");
    }
    auto settle = [&]() {
        const uint64_t t = status(host).tick;
        auto behind = [&](ETCS::Entity* g) { const Status s = status(g); return s.mode == "guest" && s.tick != t; };
        for (int i = 0; i < 400 && (behind(g1) || behind(g2)); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    };
    auto run = [&](uint32_t ticks) { verb(host, "KartBattle.Run", std::to_string(ticks)); settle(); };
    // A round waits for every seated driver to lock in: each says so, and the
    // host starts once its trace has them all.
    auto start = [&]() {
        for (ETCS::Entity* b : battle) verb(b, "KartBattle.LockIn", "1");
        std::string r = verb(host, "KartBattle.StartRound");
        for (int i = 0; i < 1000 && r != "ok"; ++i) { std::this_thread::sleep_for(std::chrono::milliseconds(2)); r = verb(host, "KartBattle.StartRound"); }
        return r;
    };
    // A guest's line reaches the host and comes back round: wait for the host's trace to grow.
    auto said = [&](ETCS::Entity* b, const char* action, const std::string& args) {
        const uint64_t before = status(host).lines;
        const std::string answer = verb(b, action, args);
        for (int i = 0; i < 400 && answer == "ok" && status(host).lines == before; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(2));
        run(2);
        return answer;
    };
    auto key = [&](ETCS::Entity* b, int k, bool down) { verb(b, "KartBattle.Key", std::to_string(k) + " " + (down ? "1" : "0")); };
    for (int i = 0; i < 300 && (where(host, "g1").slot < 0 || where(host, "g2").slot < 0); ++i) run(2);
    check(verb(host, "KartBattle.Configure", "strip 300") == "ok", "three seated, on the strip");
    // A build's text into a file, for Build: "<name>;<base>;<tunes>;parts <n> <parts>".
    const std::string file = "/tmp/etcs_kart_build.text";
    auto send = [&](ETCS::Entity* b, const std::string& text) {
        { std::ofstream f(file); f << text << "\n"; }
        return said(b, "KartBattle.Build", file);
    };
    // The modeller's saved lines as a build's parts: "<n> <shape> <material> <14 raws>...".
    auto partsOf = [&](const std::string& path) {
        std::ifstream f(path);
        std::string out, line;
        int n = 0;
        while (std::getline(f, line))
        {
            std::istringstream in(line);
            std::string op, id, shape, w, material;
            if (!(in >> op >> id >> shape) || op != "add") continue;
            std::string raws;
            for (int k = 0; k < 14 && (in >> w); ++k) raws += " " + w;
            if (!(in >> material)) material = "-";
            out += " " + shape + " " + material + raws;
            ++n;
        }
        return std::to_string(n) + out;
    };

    // -- 1. one line, every runtime's ----------------------------------------------
    std::cout << "\n-- 1. one line, every runtime's --\n";
    // A chassis of steel in the team red, a cab of plastic in blue, a foam ball
    // in white -- metres, y 0 the ground; the moto's tyres ride it at 0.25.
    verb(model, "PaintModeler.Do", add(0, "box", 0.0, 0.35, 0.0, 1.6, 0.3, 2.4, 0.85, 0.18, 0.22, "steel"));
    verb(model, "PaintModeler.Do", add(1, "box", 0.0, 0.8, 0.2, 1.2, 0.5, 1.0, 0.2, 0.42, 0.8, "plastic"));
    verb(model, "PaintModeler.Do", add(2, "sphere", 0.0, 1.2, -0.8, 0.6, 0.6, 0.6, 1.0, 1.0, 1.0, "foam"));
    const std::string path = "/tmp/etcs_kart_build.lines";
    verb(model, "PaintModeler.Save", path);
    std::string saved;
    { std::ifstream f(path); std::stringstream ss; ss << f.rdbuf(); saved = ss.str(); }
    check(saved.find(" steel\n") != std::string::npos && saved.find(" foam\n") != std::string::npos,
          "the modeller saves each part's material after its numbers");
    const std::string mine = "mine;moto;top 40;parts " + partsOf(path);
    check(send(g1, mine) == "ok", "g1 sends its build, made from the moto");
    const double weight = (1.6 * 0.3 * 2.4 * 3.0 + 1.2 * 0.5 * 1.0 * 0.8 + 0.6 * 0.6 * 0.6 * 0.5236 * 0.2) / (1.1 * 0.4 * 2.0);
    const Look h = look(host, "mine"), o = look(g2, "mine");
    std::printf("        (host: %d parts, %.3f karts, %d colours, body %s; worked out %.3f)\n", h.parts, h.weight, h.colours, h.body.c_str(), weight);
    check(where(host, "g1").vehicle == "mine" && where(g2, "g1").vehicle == "mine" && h.owner == "g1",
          "every runtime built it from the line, and g1 rides it");
    check(h.parts == 3 && o.parts == 3 && h.colours == 3 && o.colours == 3,
          "three parts on every runtime, drawn as three colours: the team's, the blue, the white");
    check(std::fabs(h.weight - weight) < 2e-3 && h.weight == o.weight,
          "it weighs its parts at their materials' densities: steel heavy, plastic less, foam next to nothing");
    check(h.body == "1.60000002, 2.5, 2.4000001" && o.body == h.body,
          "its body is the parts' bounds about its ride height: 1.6 across, 2.5 up, 2.4 along");
    check(verb(host, "KartBattle.Vehicles") == arena, "...and no vehicle of the arena's: the build is the seat's");
    said(g1, "KartBattle.LockIn", "1");
    check(verb(host, "KartBattle.Standing", "g1") == "0 0 0", "it waits for the host: g1 cannot lock in on it");

    // -- 2. checked whole ------------------------------------------------------------
    std::cout << "\n-- 2. checked whole --\n";
    {
        std::ofstream f(path);
        f << add(0, "box", 0.0, 0.35, 0.0, 1.6, 0.3, 2.4, 0.85, 0.18, 0.22, "unobtainium") << "\n";
    }
    send(g1, "mine;moto;top 40;parts " + partsOf(path));
    check(look(host, "mine").parts == 3 && look(g2, "mine").parts == 3, "a material nobody made is refused, whole: the build stands");

    // -- 3. accepted -------------------------------------------------------------------
    std::cout << "\n-- 3. accepted --\n";
    check(said(host, "KartBattle.Accept", "g1") == "ok" && verb(g2, "KartBattle.Standing", "g1") == "0 1 0", "the host accepts it");
    said(g1, "KartBattle.LockIn", "1");
    check(verb(g2, "KartBattle.Standing", "g1") == "1 1 0", "...and g1 locks in on it");
    send(host, "ours;truck;top 30");
    check(where(g1, "host").vehicle == "ours" && verb(g1, "KartBattle.Standing", "host") == "0 1 0", "the host's own build needs no one's word");
    check(std::fabs(where(host, "g1").y - 0.25f) < 1e-3f, "g1's build stands on its tyres: its body's centre at 0.25");
    start();
    run(2);
    // A guest's key is a line: on the host's trace before the clock runs on.
    {
        const uint64_t before = status(host).lines;
        key(g1, 'W', true);
        for (int i = 0; i < 400 && status(host).lines == before; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    key(host, 'W', true);
    run(300);
    const float top = where(host, "g1").speed;
    key(host, 'D', true);
    run(50);
    key(host, 'D', false); key(host, 'W', false); key(g1, 'W', false);
    run(20);
    std::printf("        (g1 for 6 s: %.1f u/s)\n", top);
    check(std::fabs(top - 40.0f) < 0.2f, "g1 drives it on its own numbers: it tops out at 40");
    const std::string hh = verb(host, "KartBattle.Hash");
    std::printf("        (%s | %s | %s)\n", hh.c_str(), verb(g1, "KartBattle.Hash").c_str(), verb(g2, "KartBattle.Hash").c_str());
    check(hh == verb(g1, "KartBattle.Hash") && hh == verb(g2, "KartBattle.Hash"), "two builds driven: each guest at the host's tick has its hash");
    verb(host, "KartBattle.Abandon");
    run(2);
    check(verb(g2, "KartBattle.Standing", "g1") == "0 1 0", "a round taken, the next lobby is locked into afresh");

    // -- 4. rejected --------------------------------------------------------------------
    std::cout << "\n-- 4. rejected --\n";
    said(host, "KartBattle.Reject", "g1");
    check(where(host, "g1").vehicle == "kart" && where(g2, "g1").vehicle == "kart" && verb(g2, "KartBattle.LookOf", "mine") == "-",
          "rejected: g1 back in the kart, the build gone on every runtime");
    send(g1, mine);
    check(where(g2, "g1").vehicle == "mine" && verb(host, "KartBattle.Standing", "g1") == "0 0 0", "sent again, it waits again");

    // -- 5. locked -----------------------------------------------------------------------
    std::cout << "\n-- 5. locked --\n";
    said(host, "KartBattle.LockDriver", "g1 1");
    check(where(g2, "g1").vehicle == "kart" && verb(g2, "KartBattle.Standing", "g1") == "0 0 1", "kept to the arena's: the build gone");
    send(g1, mine);
    check(where(host, "g1").vehicle == "kart" && verb(host, "KartBattle.LookOf", "mine") == "-", "...and a new one refused");
    said(g1, "KartBattle.Ride", "moto");
    said(g1, "KartBattle.LockIn", "1");
    check(where(g2, "g1").vehicle == "moto" && verb(host, "KartBattle.Standing", "g1") == "1 0 1", "...the moto taken and locked in on");
    said(host, "KartBattle.LockDriver", "g1 0");
    send(g1, mine);
    check(where(g2, "g1").vehicle == "mine" && verb(host, "KartBattle.Standing", "g1") == "0 0 0", "let go, a build is taken again");

    // -- 6. the session's -----------------------------------------------------------
    std::cout << "\n-- 6. the session's --\n";
    auto& detached = ETCS::DetachedRegistry::getInstance();
    for (auto& [id, script] : detached.list()) detached.interrupt(id);
    for (int i = 0; i < 2500 && !detached.all_finished(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    check(detached.all_finished(), "the four streams end when told to");
    detached.join_all();
    verb(host, "KartBattle.Leave");
    verb(host, "KartBattle.Host", "host 0 t2");
    check(verb(host, "KartBattle.Vehicles") == arena && verb(host, "KartBattle.LookOf", "mine") == "-" && verb(host, "KartBattle.LookOf", "ours") == "-"
          && verb(host, "KartBattle.Standing", "host") == "0 0 0", "hosting again, the builds and the locks are gone");

    for (ETCS::Entity* b : battle) verb(b, "KartBattle.Delete");
    ETCS::PendingUnloadRegistry::getInstance().join_all();
    std::printf("\n%s (%d failure%s)\n", g_fail ? "FAILED" : "PASSED", g_fail, g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}
