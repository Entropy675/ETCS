// KartGarageTesterLoader.cc
//
// THE GARAGE IS THIS TAB'S IMAGINATION. A driver builds on the lobby's second
// page -- drafts, kept in the tab's store and in no line -- and the builder
// (PaintProvider's model pane, kart_garage.etcs) takes the window while a
// shape is in hand. "Use it" sends the draft whole, one `build` line. On a
// practising battle with the garage's script, a second without it, and a
// third with the first's store:
//
//   1. THE PAGE. A draft from the vehicle ridden, nobody's but the tab's; each
//      row a number stepped within its bounds; back to the lobby.
//   2. SHAPING. The draft's parts in the builder (a box of its body to begin,
//      in the team's colour); the keys and the pointer are the builder's while
//      it is up, and the menu is down; Enter keeps the shape as the draft's,
//      and the strip's done cell does the same through the router -- still no
//      line.
//   3. USED. The draft sent: ridden, drawn in its colours, and (the host's
//      own) accepted, so it locks in. Kept to the arena's vehicles, it goes,
//      and using it again is refused.
//   4. KEPT. Practising again, the build is gone and the draft is not; a page
//      with the same store reads it back.
//   5. TURNED ON. With no builder on the page, the garage's row keeps the store
//      and asks the page for the builder (Wants).
//   6. THE LOBBY'S. A round begun shuts the builder.
//
//   ./Run_KartGarageTesterLoader
//
// No display, no GPU, no network.

#include "../ETCS.h"

#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <unistd.h>

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
static std::string add(int id, const char* shape, double x, double y, double z, double w, double h, double d,
                       double r, double g, double b, const char* material)
{
    return "add " + std::to_string(id) + " " + shape + " " + raw(x) + " " + raw(y) + " " + raw(z) + " "
         + raw(w * 0.5) + " " + raw(h * 0.5) + " " + raw(d * 0.5) + " " + raw(r) + " " + raw(g) + " " + raw(b) + " " + raw(1.0)
         + " " + raw(1.0) + " 0 0 0 " + material;
}
// Kart's eighteenth word is the vehicle it rides.
static std::string rideOf(ETCS::Entity* b, const std::string& who)
{
    std::istringstream in(verb(b, "KartBattle.Kart", who));
    std::string w;
    for (int i = 0; i < 18 && (in >> w); ++i) {}
    return w;
}
static int partsOf(ETCS::Entity* b, const std::string& name)
{
    std::istringstream in(verb(b, "KartBattle.LookOf", name));
    int n = -1;
    in >> n;
    return n;
}
static std::string field(const std::string& text, int k)
{
    std::istringstream in(text);
    std::string w;
    for (int i = 0; i <= k; ++i) if (!(in >> w)) return std::string();
    return w;
}

int main(int, char**)
{
    WIRE_CONTEXT();
    std::cout << "=== The garage is the lobby's ===\n";
    const std::string store = "/tmp/etcs_kart_garage_test.db", store2 = "/tmp/etcs_kart_garage_test2.db";
    ::unlink(store.c_str()); ::unlink(store2.c_str());

    ETCS::Entity* battle[3] = {};
    ETCS::Entity* db[3] = {};
    for (int k = 0; k < 3; ++k)
    {
        ETCS::Entity* world = ETCS::spawn_entity("RenderProvider", "Scene3D", env, loader);
        battle[k] = ETCS::spawn_entity("KartProvider", "KartBattle", env, loader);
        db[k] = ETCS::spawn_entity("DatabaseProvider", "LocalDatabase", env, loader);
        if (!world || !battle[k] || !db[k]) { std::printf("FAILED (could not spawn)\n"); return 1; }
        verb(world, "Scene3D.Create", "1, 1, 1");
        verb(world, "Scene3D.SetVisible", "0");
        verb(battle[k], "KartBattle.Create");
        verb(battle[k], "KartBattle.BindWorld", std::to_string(world->getRID()));
        ETCS::ExecutionContext arena(&root, &ctx);
        arena.names["anchor"] = ETCS::NameBinding{ world->getRID(), "RenderProvider", "Scene3D" };
        arena.names["battle"] = ETCS::NameBinding{ battle[k]->getRID(), "KartProvider", "KartBattle" };
        std::istringstream in("run KartProvider/scripts/kart_arena.etcs anchor=anchor battle=battle\n");
        if (!ETCS::run_script(in, std::string(ETCS_ACE_ROOT) + "/modules/kart_world.etcs", arena)) { std::printf("FAILED (the arena script)\n"); return 1; }
        if (k < 2) verb(battle[k], "KartBattle.BindBuilderStore", std::to_string(db[k]->getRID()) + ", " + (k == 0 ? store : store2));
    }
    ETCS::Entity* b = battle[0];
    const std::string arena = verb(b, "KartBattle.Vehicles");

    // The garage's script, as the page's extras run it -- its own lines here, so
    // its names (the modeller, the router) are the test's to look at.
    ETCS::Entity* view = ETCS::spawn_entity("RenderProvider", "ImageSurface", env, loader);
    ETCS::Entity* eye = ETCS::spawn_entity("RenderProvider", "Camera3D", env, loader);
    if (!view || !eye) { std::printf("FAILED (could not spawn the surface)\n"); return 1; }
    verb(view, "ImageSurface.Create", "1024, 768");
    verb(eye, "Camera3D.Create", "1024, 768");
    ETCS::ExecutionContext page(&root, &ctx);
    page.names["battle"] = ETCS::NameBinding{ b->getRID(), "KartProvider", "KartBattle" };
    page.names["view"]   = ETCS::NameBinding{ view->getRID(), "RenderProvider", "ImageSurface" };
    page.names["eye"]    = ETCS::NameBinding{ eye->getRID(), "RenderProvider", "Camera3D" };
    {
        const std::string path = std::string(ETCS_ACE_ROOT) + "/modules/KartProvider/scripts/kart_garage.etcs";
        std::ifstream in(path);
        check(in && ETCS::run_script(in, path, page), "the garage's script runs");
    }
    ETCS::Entity* model = page.names.count("model") ? ETCS::etcs_resolve_rid_anywhere(&ETCS::getLoader(), page.names["model"].rid) : nullptr;
    ETCS::Entity* router = page.names.count("router") ? ETCS::etcs_resolve_rid_anywhere(&ETCS::getLoader(), page.names["router"].rid) : nullptr;
    if (!model || !router) { std::printf("FAILED (the garage made no modeller or router)\n"); return 1; }

    auto frame = [&](ETCS::Entity* x, int n = 2) { for (int i = 0; i < n; ++i) verb(x, "KartBattle.Frame", "16"); };
    auto press = [&](const std::string& what) { const std::string r = verb(b, "KartBattle.GaragePress", what); frame(b); return r; };
    auto key = [&](int k) { verb(b, "KartBattle.Key", std::to_string(k) + " 1"); verb(b, "KartBattle.Key", std::to_string(k) + " 0"); frame(b); };
    verb(b, "KartBattle.Practice", "me");
    frame(b, 4);

    // -- 1. the page ---------------------------------------------------------------
    std::cout << "\n-- 1. the page --\n";
    check(press("garage") == "ok" && verb(b, "KartBattle.Garage") == "1 0 -", "the garage's row opens its page: no draft yet");
    press("g_new");
    const std::string mine = field(verb(b, "KartBattle.Garage"), 2);
    std::printf("        (drafted %s)\n", mine.c_str());
    check(mine == "kart-1" && verb(b, "KartBattle.Vehicles") == arena && rideOf(b, "me") == "kart",
          "a draft from the kart, named for it -- no line: nobody's but this tab's, the kart still ridden");
    press("g_r0 next");
    check(verb(b, "KartBattle.TuneOf", mine + " top") == "23", "the first row steps its top speed: 22 to 23");
    press("g_r0 prev"); press("g_r0 prev");
    check(verb(b, "KartBattle.TuneOf", mine + " top") == "21", "...and back, a step at a time");
    for (int i = 0; i < 40; ++i) verb(b, "KartBattle.GaragePress", "g_r0 next");
    frame(b);
    check(verb(b, "KartBattle.TuneOf", mine + " top") == "40", "...no further than its most (40)");
    for (int i = 0; i < 19; ++i) verb(b, "KartBattle.GaragePress", "g_r0 prev");
    press("g_r7 next");
    check(verb(b, "KartBattle.TuneOf", mine + " tyres") == "4 0.7 0.64", "the tyres row stays at its most, four");
    press("g_r7 prev");
    check(verb(b, "KartBattle.TuneOf", mine + " tyres") == "2 0.7 0.64", "...and steps to two, the rest of the line as it was");
    press("g_back");
    check(verb(b, "KartBattle.Garage") == "0 0 " + mine, "back to the lobby's page");

    // -- 2. shaping ------------------------------------------------------------------
    std::cout << "\n-- 2. shaping --\n";
    press("garage");
    press("g_shape");
    check(verb(b, "KartBattle.Garage") == "1 1 " + mine && verb(b, "KartBattle.MenuUp") == "1", "shape it: the builder is up, the pads kept under it");
    const std::string seed = verb(model, "PaintModeler.Report", "0");
    std::printf("        (the builder's first part: %s)\n", seed.c_str());
    check(field(verb(model, "PaintModeler.Status"), 4) == "1" && field(seed, 0) == "box", "with no parts yet, it begins from one: a box");
    check(std::fabs(std::atof(field(seed, 4).c_str()) - 1.1) < 1e-3 && std::fabs(std::atof(field(seed, 7).c_str()) - 0.85) < 1e-3,
          "...of the kart's body (1.1 across), in the team's red");
    key('A');
    check(field(verb(model, "PaintModeler.Status"), 1) == "add", "a key is the builder's: A, its add tool");
    verb(model, "PaintModeler.Do", add(1, "sphere", 0.0, 0.9, -0.5, 0.5, 0.5, 0.5, 1.0, 1.0, 1.0, "foam"));
    verb(model, "PaintModeler.Do", add(2, "box", 0.0, 0.7, 0.4, 0.8, 0.3, 0.6, 0.2, 0.42, 0.8, "plastic"));
    key(257);
    check(verb(b, "KartBattle.Garage") == "1 0 " + mine && partsOf(b, mine) == 3, "Enter keeps it: three parts, the draft's");
    check(rideOf(b, "me") == "kart" && verb(b, "KartBattle.Vehicles") == arena, "...and still no line");
    press("g_shape");
    key(256);
    check(field(verb(b, "KartBattle.Garage"), 1) == "1", "Esc is the builder's (it lets go of the part), not the garage's");
    // The window's frame lays the page out (a node's place lands when it is
    // drawn); here, one draw of it.
    ETCS::Entity* garage = ETCS::etcs_resolve_rid_anywhere(&ETCS::getLoader(), page.names["garage"].rid);
    if (garage) verb(garage, "CompositeDrawable2D.Draw", std::to_string(view->getRID()));
    verb(b, "KartBattle.PointerEvent", "2, 0, 0, 40, 20");
    verb(b, "KartBattle.PointerEvent", "3, 0, 0, 40, 20");
    verb(b, "KartBattle.PointerEvent", "4, 0, 0, 40, 20");
    frame(b);
    check(verb(b, "KartBattle.Garage") == "1 0 " + mine && partsOf(b, mine) == 3,
          "a click on the strip's done, through the router to the battle, keeps it too");

    // -- 3. used ----------------------------------------------------------------------
    std::cout << "\n-- 3. used --\n";
    press("g_use");
    check(rideOf(b, "me") == mine && field(verb(b, "KartBattle.LookOf", mine), 3) == "me", "use it: one `build` line, and the seat rides it");
    check(field(verb(b, "KartBattle.LookOf", mine), 2) == "3", "...drawn as three colours: the team's, the white, the blue");
    check(verb(b, "KartBattle.Standing", "me") == "0 1 0", "...the host's own, so accepted");
    press("g_back");
    press("lockin");
    check(verb(b, "KartBattle.Standing", "me") == "1 1 0", "the lobby's lock-in takes it");
    verb(b, "KartBattle.LockDriver", "me 1");
    frame(b);
    check(rideOf(b, "me") == "kart" && verb(b, "KartBattle.Standing", "me") == "0 0 1", "kept to the arena's vehicles: the build gone, the kart ridden");
    press("garage");
    press("g_use");
    check(rideOf(b, "me") == "kart", "...and using it again is refused");
    verb(b, "KartBattle.LockDriver", "me 0");
    press("g_back");

    // -- 4. kept ----------------------------------------------------------------------
    std::cout << "\n-- 4. kept --\n";
    press("garage"); press("g_use"); press("g_back");
    frame(b, 4);
    verb(b, "KartBattle.Leave");
    verb(b, "KartBattle.Practice", "me");
    frame(b, 4);
    check(rideOf(b, "me") == "kart" && verb(b, "KartBattle.Garage") == "0 0 " + mine, "practising again, the build is gone and the draft is not");
    check(verb(b, "KartBattle.TuneOf", mine + " top") == "21" && partsOf(b, mine) == 3, "...its numbers and its shape");
    {
        ETCS::Entity* third = battle[2];
        verb(third, "KartBattle.BindBuilderStore", std::to_string(db[2]->getRID()) + ", " + store);
        // Bound to this page's builder: what a reloaded page reads is the store.
        verb(third, "KartBattle.BindGarage", std::to_string(page.names["garage"].rid) + ", " + std::to_string(model->getRID()) + ", "
             + std::to_string(router->getRID()) + ", " + std::to_string(view->getRID()) + ", " + std::to_string(eye->getRID()));
        verb(third, "KartBattle.Practice", "me");
        frame(third, 4);
        check(verb(third, "KartBattle.TuneOf", mine + " top") == "21" && partsOf(third, mine) == 3,
              "a page with the same store reads the draft back");
    }

    // -- 5. turned on ------------------------------------------------------------------
    std::cout << "\n-- 5. turned on --\n";
    ETCS::Entity* other = battle[1];
    verb(other, "KartBattle.Practice", "you");
    frame(other, 4);
    check(verb(other, "KartBattle.Wants") == "-", "a page with no builder wants nothing yet");
    verb(other, "KartBattle.GaragePress", "garage");
    frame(other);
    check(verb(other, "KartBattle.Wants") == "builder" && verb(other, "KartBattle.Garage") == "0 0 -",
          "its garage row asks the page for the builder, and opens no page");
    check(::access(store2.c_str(), F_OK) == 0, "...its store made, the file the page looks for");

    // -- 6. the lobby's ------------------------------------------------------------------
    std::cout << "\n-- 6. the lobby's --\n";
    press("garage");
    press("g_shape");
    check(field(verb(b, "KartBattle.Garage"), 1) == "1", "shaping again");
    verb(b, "KartBattle.LockIn", "1");
    verb(b, "KartBattle.StartRound");
    frame(b);
    check(verb(b, "KartBattle.Garage") == "0 0 " + mine && verb(b, "KartBattle.MenuUp") == "0", "a round begun shuts the builder and the page");
    verb(b, "KartBattle.Abandon");

    for (ETCS::Entity* x : battle) verb(x, "KartBattle.Delete");
    ETCS::PendingUnloadRegistry::getInstance().join_all();
    std::printf("\n%s (%d failure%s)\n", g_fail ? "FAILED" : "PASSED", g_fail, g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}
