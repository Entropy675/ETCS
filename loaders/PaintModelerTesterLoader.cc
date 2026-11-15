// PaintModelerTesterLoader.cc
//
// A MODEL, MADE WITHOUT A WINDOW. The page's 3D half (PaintProvider/
// PaintModeler.h) driven the way a pointer would drive it -- through its
// Press/Drag/Release verbs -- and the way a record would, through its lines,
// with the pool and the shapes made by the page's own scripts. What is held to
// account:
//
//   1. A LINE MAKES A PART. An `add` puts a node of the pool where it says, at
//      the size it says (Scene3D's extent is a value on its surface now); a
//      line that cannot be true of the model is refused whole.
//   2. THE POINTER. A press chooses, a press on nothing lets go and a drag
//      there turns the eye; move slides along an axis or on the plane facing
//      the eye; size, turn, snap, add, paint and delete each leave one line.
//   3. UNDO IS A LINE. Every edit can be walked back to an empty model and
//      forward again to the same hash; an edit after an undo drops the redo.
//   4. THE PAGE'S MODE. Open hides the 2D nodes and shows the 3D ones.
//   5. THE RECORD. A guest that joins late has the host's model; each side's
//      lines reach the other; a guest's undo is one more line of the record;
//      a refused proposal costs the guest nothing.
//   6. OUT. The model as .obj, .stl and .glb: what is drawn is what is written,
//      mirrored into the formats' handedness with every face looking out; and
//      as its own lines, read back to the same hash.
//
//   ./Run_PaintModelerTesterLoader
//
// No display, no GPU.

#include "../ETCS.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

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
// add <id> <shape> at (x y z), sized (w h d), coloured (r g b), facing as made.
static std::string add(int id, const char* shape, double x, double y, double z, double w, double h, double d,
                       double r = 0.8, double g = 0.8, double b = 0.85)
{
    return "add " + std::to_string(id) + " " + shape + " " + raw(x) + " " + raw(y) + " " + raw(z) + " "
         + raw(w * 0.5) + " " + raw(h * 0.5) + " " + raw(d * 0.5) + " " + raw(r) + " " + raw(g) + " " + raw(b) + " " + raw(1.0)
         + " " + raw(1.0) + " 0 0 0";
}

struct Status { std::string mode, tool, axis; int sel = -2; unsigned parts = 0; unsigned long long seq = 0; unsigned undo = 0, redo = 0; };
static Status status(ETCS::Entity* m)
{
    Status s;
    std::istringstream in(verb(m, "PaintModeler.Status"));
    in >> s.mode >> s.tool >> s.axis >> s.sel >> s.parts >> s.seq >> s.undo >> s.redo;
    return s;
}
struct Part { std::string shape; double x = 0, y = 0, z = 0, w = 0, h = 0, d = 0, r = 0, g = 0, b = 0, a = 0; bool there = false; };
static Part part(ETCS::Entity* m, int id)
{
    Part p;
    std::istringstream in(verb(m, "PaintModeler.Report", std::to_string(id)));
    p.there = static_cast<bool>(in >> p.shape >> p.x >> p.y >> p.z >> p.w >> p.h >> p.d >> p.r >> p.g >> p.b >> p.a);
    return p;
}
static bool near(double a, double b, double eps = 1e-3) { return std::fabs(a - b) <= eps; }

// One runtime's worth: a world with a model container under it, a camera, a
// modeller, and the pool and shapes the page's scripts make.
struct Rig { ETCS::Entity* world = nullptr; ETCS::Entity* parts = nullptr; ETCS::Entity* eye = nullptr; ETCS::Entity* model = nullptr; };
static std::vector<ETCS::Entity*> children(ETCS::Entity* e)
{
    std::vector<ETCS::Entity*> out;
    std::vector<ETCS::Entity::ChildRef> kids;
    e->getTypedChildRefs(kids);
    for (auto& [tag, rid] : kids)
        if (ETCS::Entity* k = e->getTypedChild(*tag, rid)) out.push_back(k);
    return out;
}
static bool hidden(ETCS::Entity* e)
{
    auto* d = static_cast<Drawable_*>(e->getInterfacePointer(ETCS::Buffer("Drawable")));
    return d && d->Hidden();
}

int main(int, char**)
{
    WIRE_CONTEXT();
    std::cout << "=== A model, made without a window ===\n";

    auto make = [&](Rig& r) {
        r.world = ETCS::spawn_entity("RenderProvider", "Scene3D", env, loader);
        r.eye   = ETCS::spawn_entity("RenderProvider", "Camera3D", env, loader);
        r.model = ETCS::spawn_entity("PaintProvider", "PaintModeler", env, loader);
        if (!r.world || !r.eye || !r.model) return false;
        verb(r.world, "Scene3D.Create", "1, 1, 1");
        verb(r.world, "Scene3D.SetVisible", "0");
        verb(r.world, "Scene3D.SetDriven", "1");
        verb(r.eye, "Camera3D.Create", "800, 600");
        verb(r.eye, "Camera3D.SetLens", "50, 0.05, 600");
        verb(r.eye, "Camera3D.SetScene", std::to_string(r.world->getRID()));
        verb(r.model, "PaintModeler.Create");
        ETCS::ExecutionContext page(&root, &ctx);
        page.names["world"] = ETCS::NameBinding{ r.world->getRID(), "RenderProvider", "Scene3D" };
        page.names["model"] = ETCS::NameBinding{ r.model->getRID(), "PaintProvider", "PaintModeler" };
        // As paint_model.etcs has it, and BY a script, as a page's boot runs
        // it: its names are its own, so three models in one process do not
        // take each other's.
        std::istringstream in("run PaintProvider/scripts/paint_model_world.etcs world=world model=model\n");
        if (!ETCS::run_script(in, std::string(ETCS_ACE_ROOT) + "/modules/paint_model_rig.etcs", page)) return false;
        const auto kids = children(r.world);
        if (kids.size() != 1) return false;
        r.parts = kids[0];
        verb(r.model, "PaintModeler.BindCamera", std::to_string(r.eye->getRID()));
        // The eye behind the origin looking along +z, a little above: screen
        // right is +x, and the middle of the frame is the point looked at.
        verb(r.model, "PaintModeler.LookAt", "0, 0.5, 0");
        verb(r.model, "PaintModeler.SetOrbit", "9, 0, 20");
        return true;
    };

    Rig a;
    if (!make(a)) { std::printf("FAILED (could not make the rig)\n"); return 1; }
    ETCS::Entity* m = a.model;
    auto* cam = static_cast<Camera_*>(a.eye->getInterfacePointer(ETCS::Buffer("Camera")));
    const std::string empty_hash = verb(m, "PaintModeler.Hash");

    // -- 1. a line makes a part ------------------------------------------------
    std::cout << "\n-- 1. a line makes a part --\n";
    {
        const Status s = status(m);
        check(s.mode == "alone" && s.tool == "select" && s.axis == "none" && s.sel == -1 && s.parts == 0 && s.undo == 0,
              "an empty model: alone, nothing selected, nothing to undo");
        const auto pool = children(a.parts);
        bool all_hidden = pool.size() == 48;
        for (ETCS::Entity* n : pool) all_hidden = all_hidden && hidden(n);
        check(all_hidden, "the pool is forty-eight nodes, none shown");
    }
    check(verb(m, "PaintModeler.Do", add(0, "box", 0, 0.5, 0, 1, 1, 1)) == "ok", "an add is taken");
    {
        const Part p = part(m, 0);
        check(p.there && p.shape == "box" && near(p.x, 0) && near(p.y, 0.5) && near(p.z, 0) && near(p.w, 1) && near(p.h, 1) && near(p.d, 1)
              && near(p.r, 0.8) && near(p.b, 0.85), "the part is what the line said");
        ETCS::Entity* shown = nullptr; int n_shown = 0;
        for (ETCS::Entity* n : children(a.parts)) if (!hidden(n)) { shown = n; ++n_shown; }
        check(n_shown == 1, "one node of the pool is shown for it");
        if (shown)
        {
            auto* c = static_cast<Causal_*>(shown->getInterfacePointer(ETCS::Buffer("Causal")));
            Fixed x, y, z;
            { std::lock_guard<std::recursive_mutex> lk(c->TreeMutex()); x = c->Order4().x; y = c->Order4().y; z = c->Order4().z; }
            check(near(x.ToDouble(), 0) && near(y.ToDouble(), 0.5) && near(z.ToDouble(), 0), "...standing where the line put it");
            std::string v;
            const bool has = shown->valueOf(ETCS::Buffer("extent"), v);
            int64_t w[3] = { 0, 0, 0 }; size_t at = 0;
            for (int i = 0; i < 3 && has; ++i) ETCS::Entity::getWord(v, at, w[i]);
            check(has && v.size() == 24 && w[0] == Fixed::From(0.5).raw && w[1] == w[0] && w[2] == w[0],
                  "...and its size is on its own surface: the extent, as three half-extent words");
        }
    }
    {
        const std::string before = verb(m, "PaintModeler.Hash");
        const bool refused = verb(m, "PaintModeler.Do", add(0, "box", 1, 0.5, 0, 1, 1, 1)) == "refused"      // the slot is taken
                          && verb(m, "PaintModeler.Do", add(1, "blob", 1, 0.5, 0, 1, 1, 1)) == "refused"     // no such shape
                          && verb(m, "PaintModeler.Do", add(1, "box", 1, 0.5, 0, 1, 0, 1)) == "refused"      // no height
                          && verb(m, "PaintModeler.Do", add(99, "box", 1, 0.5, 0, 1, 1, 1)) == "refused"     // no such slot
                          && verb(m, "PaintModeler.Do", "move 5 0 0 0") == "refused"                         // nothing there
                          && verb(m, "PaintModeler.Do", "move 0 12") == "refused"                            // half a line
                          && verb(m, "PaintModeler.Do", "melt 0") == "refused";
        check(refused && verb(m, "PaintModeler.Hash") == before && status(m).undo == 1,
              "a line that cannot be true of the model is refused whole, and leaves nothing to undo");
    }

    // -- 2. the pointer --------------------------------------------------------
    std::cout << "\n-- 2. the pointer --\n";
    auto press = [&](int x, int y) { verb(m, "PaintModeler.Press", std::to_string(x) + ", " + std::to_string(y)); };
    auto drag = [&](int x, int y) { verb(m, "PaintModeler.Drag", std::to_string(x) + ", " + std::to_string(y)); };
    auto release = [&](int x, int y) { verb(m, "PaintModeler.Release", std::to_string(x) + ", " + std::to_string(y)); };
    auto gesture = [&](int x0, int y0, int x1, int y1) { press(x0, y0); drag((x0 + x1) / 2, (y0 + y1) / 2); drag(x1, y1); release(x1, y1); };

    press(400, 300); release(400, 300);
    check(status(m).sel == 0, "a press on a part chooses it");
    press(20, 20); release(20, 20);
    check(status(m).sel == -1, "a press on nothing lets go of it");
    {
        const ViewFrustum v0 = cam->GetView();
        const std::string before = verb(m, "PaintModeler.Hash");
        gesture(20, 20, 140, 70);
        const ViewFrustum v1 = cam->GetView();
        check((std::fabs(v1.position.x - v0.position.x) > 0.5f || std::fabs(v1.position.y - v0.position.y) > 0.2f)
              && near(v1.look_at.x, v0.look_at.x) && near(v1.look_at.y, v0.look_at.y)
              && verb(m, "PaintModeler.Hash") == before && status(m).undo == 1,
              "a drag on nothing turns the eye about what it looks at, and is not a line");
        verb(m, "PaintModeler.Wheel", "3");
        const ViewFrustum v2 = cam->GetView();
        auto dist = [](const ViewFrustum& v) {
            const float dx = v.position.x - v.look_at.x, dy = v.position.y - v.look_at.y, dz = v.position.z - v.look_at.z;
            return std::sqrt(dx * dx + dy * dy + dz * dz);
        };
        check(dist(v2) < dist(v1) - 0.5f, "the wheel brings it in");
        verb(m, "PaintModeler.SetOrbit", "9, 0, 20");
    }

    const std::string home = verb(m, "PaintModeler.Report", "0");
    verb(m, "PaintModeler.SetTool", "move");
    verb(m, "PaintModeler.SetAxis", "x");
    gesture(400, 300, 500, 300);
    {
        const Part p = part(m, 0);
        std::printf("        (moved to %.3f, %.3f, %.3f)\n", p.x, p.y, p.z);
        check(p.x > 1.2 && p.x < 1.6 && near(p.y, 0.5, 1e-9) && near(p.z, 0, 1e-9) && status(m).undo == 2,
              "move on the x axis: a hundred pixels right is that far along x and nowhere else, one line");
    }
    verb(m, "PaintModeler.SetAxis", "none");
    {
        const Part p0 = part(m, 0);
        gesture(500, 300, 500, 240);
        const Part p1 = part(m, 0);
        check(p1.y > p0.y + 0.5 && near(p1.x, p0.x, 0.02), "move with no axis slides on the plane facing the eye: up the screen is up");
    }
    const std::string moved = verb(m, "PaintModeler.Report", "0");
    check(verb(m, "PaintModeler.Undo") == "ok" && verb(m, "PaintModeler.Undo") == "ok" && verb(m, "PaintModeler.Report", "0") == home,
          "two undos put it back where it was, exactly");
    check(verb(m, "PaintModeler.Redo") == "ok" && verb(m, "PaintModeler.Redo") == "ok" && verb(m, "PaintModeler.Report", "0") == moved
          && verb(m, "PaintModeler.Redo") == "nothing", "two redos take it there again, and there is no third");
    verb(m, "PaintModeler.Undo"); verb(m, "PaintModeler.Undo");

    verb(m, "PaintModeler.SetTool", "size");
    verb(m, "PaintModeler.SetAxis", "y");
    gesture(400, 300, 400, 200);
    {
        const Part p = part(m, 0);
        check(near(p.h, std::exp(1.0), 2e-3) && near(p.w, 1) && near(p.d, 1) && near(p.y, 0.5),
              "size on the y axis: a hundred pixels up is e times as tall, as wide and as deep as before");
    }
    verb(m, "PaintModeler.Undo");
    verb(m, "PaintModeler.SetAxis", "none");
    gesture(400, 300, 400, 200);
    {
        const Part p = part(m, 0);
        check(near(p.h, std::exp(1.0), 2e-3) && near(p.w, p.h) && near(p.d, p.h), "size with no axis grows all three");
    }
    verb(m, "PaintModeler.Undo");

    verb(m, "PaintModeler.SetTool", "turn");
    gesture(400, 300, 500, 300);
    {
        ETCS::Entity* shown = nullptr;
        for (ETCS::Entity* n : children(a.parts)) if (!hidden(n)) shown = n;
        auto* c = shown ? static_cast<Causal_*>(shown->getInterfacePointer(ETCS::Buffer("Causal"))) : nullptr;
        double qy = 0, qw = 0, qx = 1, qz = 1;
        if (c) { std::lock_guard<std::recursive_mutex> lk(c->TreeMutex()); qy = c->Order4().qy.ToDouble(); qw = c->Order4().qw.ToDouble(); qx = c->Order4().qx.ToDouble(); qz = c->Order4().qz.ToDouble(); }
        // 100 pixels at 0.006 rad a pixel is 0.6 rad about y: the spinor (cos 0.3, 0, sin 0.3, 0).
        check(c && near(std::fabs(qy), std::sin(0.3), 2e-3) && near(std::fabs(qw), std::cos(0.3), 2e-3) && near(qx, 0, 1e-4) && near(qz, 0, 1e-4),
              "turn: a hundred pixels is 0.6 rad about the up axis, and the node faces that way");
    }
    verb(m, "PaintModeler.Undo");

    verb(m, "PaintModeler.SetSnap", "0.5 15");
    verb(m, "PaintModeler.SetTool", "move");
    verb(m, "PaintModeler.SetAxis", "x");
    gesture(400, 300, 500, 300);
    check(near(part(m, 0).x, 1.5, 1e-9), "with a half-unit grid the same move lands on 1.5");
    verb(m, "PaintModeler.Undo");
    verb(m, "PaintModeler.SetSnap", "0 0");
    verb(m, "PaintModeler.SetAxis", "none");
    check(verb(m, "PaintModeler.Report", "0") == home && status(m).undo == 1, "...and all of that undone is the part as it was made");

    // Add, paint and delete take their colour from the page's brush.
    ETCS::Entity* brush = ETCS::spawn_entity("PaintProvider", "PaintTool", env, loader);
    verb(brush, "PaintTool.SetColor", "0.9, 0.2, 0.1, 1.0");
    verb(m, "PaintModeler.BindTool", std::to_string(brush->getRID()));
    verb(m, "PaintModeler.SetTool", "add");
    verb(m, "PaintModeler.SetShape", "sphere");
    press(400, 520); release(400, 520);
    {
        const Part p = part(m, 1);
        const Status s = status(m);
        std::printf("        (added at %.3f, %.3f, %.3f)\n", p.x, p.y, p.z);
        check(p.there && p.shape == "sphere" && near(p.y, 0.5, 1e-6) && p.z < -2.0 && near(p.x, 0, 0.02)
              && near(p.r, 0.9) && near(p.g, 0.2) && near(p.b, 0.1) && s.parts == 2 && s.sel == 1,
              "add: a press on the ground stands a part there, in the shape and the brush's colour, chosen");
    }
    press(400, 300); release(400, 300);
    {
        const Part p = part(m, 2);
        check(p.there && p.y > 0.6 && status(m).parts == 3, "...and a press on a part puts one against it, where the pointer's line met it");
    }
    verb(m, "PaintModeler.Undo");
    verb(brush, "PaintTool.SetColor", "0.1, 0.3, 0.9, 1.0");
    verb(m, "PaintModeler.SetTool", "paint");
    press(400, 520); release(400, 520);
    {
        const Part p = part(m, 1);
        check(near(p.r, 0.1) && near(p.g, 0.3) && near(p.b, 0.9) && near(part(m, 0).r, 0.8), "paint: the part under the pointer takes the brush's colour, the other keeps its own");
    }
    verb(m, "PaintModeler.SetTool", "select");
    press(400, 520); release(400, 520);
    verb(m, "PaintModeler.SetShape", "pyramid");
    check(part(m, 1).shape == "pyramid", "a shape named with a part chosen is that part's");
    verb(m, "PaintModeler.SetTool", "delete");
    press(400, 520); release(400, 520);
    check(!part(m, 1).there && status(m).parts == 1 && status(m).sel == -1, "delete: the part under the pointer is gone, and nothing is chosen");
    verb(m, "PaintModeler.SetTool", "select");

    // -- 3. undo is a line -----------------------------------------------------
    std::cout << "\n-- 3. undo is a line --\n";
    {
        const std::string full = verb(m, "PaintModeler.Hash");
        int back = 0;
        while (verb(m, "PaintModeler.Undo") == "ok" && back < 64) ++back;
        check(back == 5 && status(m).parts == 0 && verb(m, "PaintModeler.Hash") == empty_hash,
              "every edit walked back: add, add, paint, shape, delete -- five lines to an empty model");
        int fwd = 0;
        while (verb(m, "PaintModeler.Redo") == "ok" && fwd < 64) ++fwd;
        check(fwd == 5 && verb(m, "PaintModeler.Hash") == full, "...and forward again to the same model");
        verb(m, "PaintModeler.Undo");                               // the delete, off: the pyramid is back
        check(part(m, 1).there && status(m).redo == 1, "an undone delete is the part back in its slot, as it was");
        verb(m, "PaintModeler.Do", "move 1 " + raw(2) + " " + raw(0.5) + " " + raw(-3));
        check(status(m).redo == 0 && verb(m, "PaintModeler.Redo") == "nothing", "an edit after an undo drops what was undone");
    }

    // -- 4. the page's mode ----------------------------------------------------
    std::cout << "\n-- 4. the page's mode --\n";
    {
        ETCS::Entity* two   = ETCS::spawn_entity("RenderProvider", "CompositeDrawable2D", env, loader);
        ETCS::Entity* three = ETCS::spawn_entity("RenderProvider", "CompositeDrawable2D", env, loader);
        verb(two, "CompositeDrawable2D.Create", "64, 64");
        verb(three, "CompositeDrawable2D.Create", "64, 64");
        verb(three, "CompositeDrawable2D.SetHidden", "1");
        verb(m, "PaintModeler.AddPane", "2 " + std::to_string(two->getRID()));
        verb(m, "PaintModeler.AddPane", "3 " + std::to_string(three->getRID()));
        verb(m, "PaintModeler.Open");
        const bool opened = hidden(two) && !hidden(three);
        verb(m, "PaintModeler.Toggle");
        const bool closed = !hidden(two) && hidden(three);
        verb(m, "PaintModeler.Toggle");
        check(opened && closed && hidden(two) && !hidden(three), "open shows the model's nodes and hides the page's; closed, the other way; toggle is whichever it is not");
        verb(m, "PaintModeler.Close");
    }

    // -- 5. the record ---------------------------------------------------------
    std::cout << "\n-- 5. the record --\n";
    Rig b;
    if (!make(b)) { std::printf("FAILED (could not make the second rig)\n"); return 1; }
    ETCS::Entity* g = b.model;
    ETCS::Entity* rec = ETCS::spawn_entity("NetworkProvider", "Ledger", env, loader);
    ETCS::Entity* pro = ETCS::spawn_entity("NetworkProvider", "Ledger", env, loader);
    verb(rec, "Ledger.Author", "host");
    verb(pro, "Ledger.Author", "g1");
    auto head = [&]() { return std::strtoull(verb(rec, "Ledger.Head").c_str(), nullptr, 10); };
    auto same = [&]() { return verb(m, "PaintModeler.Hash") == verb(g, "PaintModeler.Hash"); };
    auto wait = [&](const std::function<bool()>& done) {
        for (int i = 0; i < 600 && !done(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        return done();
    };
    verb(m, "PaintModeler.Host", std::to_string(rec->getRID()) + " " + std::to_string(pro->getRID()) + " host");
    check(status(m).mode == "host" && head() == 2, "hosting: the model as it stands goes into the record first, a line a part");
    verb(g, "PaintModeler.Join", "g1");
    {
        ETCS::ExecutionContext wire(&root, &ctx);
        auto bind = [&](const char* n, ETCS::Entity* e, const char* mod, const char* tag) { wire.names[n] = ETCS::NameBinding{ e->getRID(), mod, tag }; };
        bind("rec", rec, "NetworkProvider", "Ledger"); bind("pro", pro, "NetworkProvider", "Ledger");
        bind("host", m, "PaintProvider", "PaintModeler"); bind("g1", g, "PaintProvider", "PaintModeler");
        std::istringstream lines("detach PaintProvider/scripts/paint_model_judge.etcs src=pro dst=host\n"
                                 "detach PaintProvider/scripts/paint_model_follow.etcs src=rec dst=g1\n"
                                 "detach PaintProvider/scripts/paint_model_emit.etcs src=g1 dst=pro\n");
        check(ETCS::run_script(lines, std::string(ETCS_ACE_ROOT) + "/modules/paint_model_wiring.etcs", wire), "the streams are wired with the page's scripts");
    }
    check(wait(same) && status(g).mode == "guest" && status(g).parts == 2, "a guest that joins late has the host's model");
    verb(m, "PaintModeler.Do", "size 0 " + raw(1) + " " + raw(0.25) + " " + raw(1));
    check(wait(same) && near(part(g, 0).h, 0.5), "the host's line reaches the guest");
    {
        const std::string before = verb(m, "PaintModeler.Hash");
        const unsigned long long seq = head();
        verb(g, "PaintModeler.Do", "color 1 " + raw(0.2) + " " + raw(0.9) + " " + raw(0.4) + " " + raw(1));
        check(wait([&]() { return verb(m, "PaintModeler.Hash") != before; }) && wait(same) && near(part(m, 1).g, 0.9) && head() == seq + 1,
              "the guest's line is proposed, judged, recorded, and is both models'");
        check(wait([&]() { return status(g).undo == 1; }), "...and is the guest's to undo once the record has brought it round");
        verb(g, "PaintModeler.Undo");
        check(wait([&]() { return verb(m, "PaintModeler.Hash") == before; }) && wait(same) && head() == seq + 2,
              "the guest's undo is one more line of the record, and both models are as they were");
        check(wait([&]() { return status(g).redo == 1; }) && status(m).undo == 6, "what it undid is its own to redo; the host's history is untouched");
    }
    {
        const unsigned long long seq = head();
        verb(g, "PaintModeler.Do", "move 6 0 0 0");                 // nothing in that slot: the judge refuses it
        verb(g, "PaintModeler.Do", "move 0 " + raw(-2) + " " + raw(0.25) + " " + raw(1));
        check(wait([&]() { return head() == seq + 1; }) && wait(same) && near(part(g, 0).x, -2), "a refused proposal is never a line; the one after it lands");
        check(wait([&]() { return status(g).undo == 1; }), "...and the refused one left nothing in the guest's history");
    }
    verb(g, "PaintModeler.Leave");
    verb(m, "PaintModeler.Leave");
    check(status(m).mode == "alone" && status(m).parts == 2 && status(g).parts == 2, "leaving keeps each model as it stands");
    // The streams wound up as a closing script winds up what it detached
    // (KartTesterLoader's): each told to stop, then joined.
    {
        auto& detached = ETCS::DetachedRegistry::getInstance();
        for (auto& [id, script] : detached.list()) detached.interrupt(id);
        for (int i = 0; i < 2500 && !detached.all_finished(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(2));
        check(detached.all_finished(), "the three streams end when told to");
        detached.join_all();
    }

    // -- 6. out ----------------------------------------------------------------
    std::cout << "\n-- 6. out --\n";
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / ("etcs_paint_modeler_" + std::to_string(static_cast<long long>(::getpid())));
    fs::create_directories(dir);
    Rig c;
    if (!make(c)) { std::printf("FAILED (could not make the third rig)\n"); return 1; }
    ETCS::Entity* e = c.model;
    check(verb(e, "PaintModeler.Export", (dir / "none.obj").string()).compare(0, 8, "refused:") == 0, "an empty model is not written");
    verb(e, "PaintModeler.Do", add(0, "box", 0, 0.5, 0, 2, 1, 1, 0.9, 0.1, 0.1));
    verb(e, "PaintModeler.Do", add(1, "sphere", 3, 0.5, 2, 1, 1, 1, 0.1, 0.1, 0.9));
    verb(e, "PaintModeler.Do", add(2, "box", -3, 0.5, 0, 2, 1, 1, 0.1, 0.9, 0.1));
    // A quarter turn about y: the spinor (cos 45, 0, sin 45, 0).
    verb(e, "PaintModeler.Do", "turn 2 " + raw(std::sqrt(0.5)) + " 0 " + raw(std::sqrt(0.5)) + " 0");
    const unsigned sphere_tris = 16 * 32 * 2 - 2 * 32, all_tris = 12 + sphere_tris + 12;
    {
        const std::string obj = (dir / "model.obj").string();
        check(verb(e, "PaintModeler.Export", obj) == "3 " + std::to_string(all_tris), "obj: three parts, and every triangle of them");
        struct Group { std::vector<std::array<double, 3>> v; std::vector<std::array<size_t, 3>> f; size_t base = 0; };
        std::vector<Group> groups;
        std::ifstream in(obj);
        std::string line; size_t n_v = 0, n_vn = 0, n_mtl = 0;
        bool mtllib = false;
        while (std::getline(in, line))
        {
            std::istringstream ls(line);
            std::string k; ls >> k;
            if (k == "mtllib") mtllib = true;
            else if (k == "o") { groups.emplace_back(); groups.back().base = n_v; }
            else if (k == "v" && !groups.empty()) { std::array<double, 3> p{}; ls >> p[0] >> p[1] >> p[2]; groups.back().v.push_back(p); ++n_v; }
            else if (k == "vn") ++n_vn;
            else if (k == "usemtl") ++n_mtl;
            else if (k == "f" && !groups.empty())
            {
                std::array<size_t, 3> t{};
                for (int i = 0; i < 3; ++i) { std::string tok; ls >> tok; t[i] = std::strtoull(tok.c_str(), nullptr, 10) - 1 - groups.back().base; }
                groups.back().f.push_back(t);
            }
        }
        size_t n_f = 0;
        for (auto& gr : groups) n_f += gr.f.size();
        check(mtllib && groups.size() == 3 && n_v == 24 + 17 * 33 + 24 && n_vn == n_v && n_f == all_tris && n_mtl == 3,
              "...as three objects with a normal a vertex and a material each");
        auto box_of = [](const Group& gr, double lo[3], double hi[3]) {
            for (int k = 0; k < 3; ++k) { lo[k] = 1e30; hi[k] = -1e30; }
            for (auto& p : gr.v) for (int k = 0; k < 3; ++k) { lo[k] = std::min(lo[k], p[k]); hi[k] = std::max(hi[k], p[k]); }
        };
        bool placed = groups.size() == 3;
        if (placed)
        {
            double lo[3], hi[3];
            box_of(groups[0], lo, hi);
            placed = placed && near(lo[0], -1) && near(hi[0], 1) && near(lo[1], 0) && near(hi[1], 1) && near(lo[2], -0.5) && near(hi[2], 0.5);
            box_of(groups[1], lo, hi);      // the sphere at z = 2 is at z = -2 in the file
            placed = placed && near(lo[0], 2.5) && near(hi[0], 3.5) && near(lo[2], -2.5) && near(hi[2], -1.5);
            box_of(groups[2], lo, hi);      // the turned box: two long in z now, one in x
            placed = placed && near(lo[0], -3.5) && near(hi[0], -2.5) && near(lo[2], -1) && near(hi[2], 1);
        }
        check(placed, "each part where the scene has it, turned as it is turned, with z the other way round");
        bool out = true;
        for (auto& gr : groups)
        {
            double cx = 0, cy = 0, cz = 0;
            for (auto& p : gr.v) { cx += p[0]; cy += p[1]; cz += p[2]; }
            cx /= gr.v.size(); cy /= gr.v.size(); cz /= gr.v.size();
            for (auto& t : gr.f)
            {
                const auto& p = gr.v[t[0]]; const auto& q = gr.v[t[1]]; const auto& r = gr.v[t[2]];
                const double u[3] = { q[0] - p[0], q[1] - p[1], q[2] - p[2] }, v[3] = { r[0] - p[0], r[1] - p[1], r[2] - p[2] };
                const double n[3] = { u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0] };
                const double m3[3] = { (p[0] + q[0] + r[0]) / 3 - cx, (p[1] + q[1] + r[1]) / 3 - cy, (p[2] + q[2] + r[2]) / 3 - cz };
                if (n[0] * m3[0] + n[1] * m3[1] + n[2] * m3[2] <= 0) out = false;
            }
        }
        check(out, "every face is wound to look out, in the file's own handedness");
    }
    {
        const std::string stl = (dir / "model.stl").string();
        const bool wrote = verb(e, "PaintModeler.Export", stl) == "3 " + std::to_string(all_tris);
        std::ifstream in(stl, std::ios::binary);
        char header[80]; uint32_t count = 0;
        in.read(header, 80); in.read(reinterpret_cast<char*>(&count), 4);
        check(wrote && count == all_tris && fs::file_size(stl) == 84 + 50ull * all_tris, "stl: the same triangles, fifty bytes each after the header");
    }
    {
        const std::string glb = (dir / "model.glb").string();
        const bool wrote = verb(e, "PaintModeler.Export", glb) == "3 " + std::to_string(all_tris);
        std::ifstream in(glb, std::ios::binary);
        uint32_t h[5] = {};
        in.read(reinterpret_cast<char*>(h), 20);
        std::string json(h[3], '\0');
        in.read(json.data(), h[3]);
        uint32_t bh[2] = {};
        in.read(reinterpret_cast<char*>(bh), 8);
        auto count = [&](const char* needle) { size_t n = 0; for (size_t at = json.find(needle); at != std::string::npos; at = json.find(needle, at + 1)) ++n; return n; };
        check(wrote && h[0] == 0x46546C67 && h[1] == 2 && h[2] == fs::file_size(glb) && h[4] == 0x4E4F534A && bh[1] == 0x004E4942
              && 12 + 8 + h[3] + 8 + bh[0] == h[2] && h[3] % 4 == 0 && bh[0] % 4 == 0,
              "glb: a header, a JSON chunk and a binary chunk that add up to the file");
        check(count("\"matrix\"") == 3 && count("\"primitives\"") == 3 && count("\"baseColorFactor\"") == 3
              && count("\"POSITION\":0,") == 2 && count("\"componentType\"") == 6
              && json.find("\"byteLength\":" + std::to_string(bh[0]) + "}]}") != std::string::npos,
              "...three nodes and three materials, and the two boxes drawn from one shape stored once");
    }
    check(verb(e, "PaintModeler.Export", (dir / "model.fbx").string()).compare(0, 8, "refused:") == 0, "an ending that names no format here is refused, and says so");
    {
        const std::string lines = (dir / "model.lines").string();
        Rig d;
        const bool made = make(d);
        check(made && verb(e, "PaintModeler.Save", lines) == "ok" && verb(d.model, "PaintModeler.Load", lines) == "ok"
              && verb(d.model, "PaintModeler.Hash") == verb(e, "PaintModeler.Hash") && status(d.model).parts == 3,
              "the model saved as its lines and loaded into another is the same model");
    }
    std::error_code ec;
    fs::remove_all(dir, ec);

    for (Rig* r : { &a, &b, &c }) verb(r->model, "PaintModeler.Delete");
    ETCS::PendingUnloadRegistry::getInstance().join_all();
    std::printf("\n%s (%d failure%s)\n", g_fail ? "FAILED" : "PASSED", g_fail, g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}
