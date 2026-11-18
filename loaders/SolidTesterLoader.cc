// SolidTesterLoader.cc
//
// PLANES, THE SPACE'S PARAMETERS, AND SOLIDS (ontology/Plane.h; Causal.h, "the
// space's parameters"; CausalBase's field and solid pass), run against a tree
// of plain bodies. Each section a constraint from
// ontology/etcs_causal_constraints.md §12-§13:
//
//   1. PLANES. Made from a point and a normal or three points, a plane answers
//      side, distance, projection, a ray's crossing, a reflection; it splits a
//      set of reaches into front, behind and across; a box is its six planes.
//      Beside them, the two directions a game asks for: a facing aimed along
//      a direction (OrderVector::Aim), and a camera's ray through a point of
//      its frame (ViewRay).
//   2. THE FIELD IS THE SPACE'S. A member falls along the field of the nearest
//      container that states one; a container stating its own (zero
//      included) is a pocket of its own, whatever its parent's, and taking the
//      statement back inherits again. The work the field does is counted where
//      it is stated, so the ledger stays exact: what the tree holds, plus what
//      left at the boundary, less the field's work, is what Impulse put in.
//   3. A DROP. A ball dropped on an anchored box bounces lower each time,
//      lands, and rests at the surface: still, held there, ledger exact.
//   4. A ROLL. A resting ball pushed across a surface slows by friction and
//      stops; with no friction it keeps going. Its world says Moving while it
//      rolls (what an observer asks before drawing again), and not after.
//   5. A SLOPE. A ball on a tilted box (row 3) slides downhill.
//   6. TWO FREE BALLS. Equal masses, head on, restitution one: they exchange
//      velocities, Newton's way, and the energy is exact. A body that is not
//      solid in the way is passed straight through.
//   7. A HALF-SPACE. A ball falls onto one plane and rests on it, however far
//      from its centre: a half-space has no reach to miss by.
//   8. THE SAME ROWS. A crowd dropped on a box lands on the same rows stepped
//      pair by pair and through the kd-tree, and twice over.
//   9. A HOLE. A ball rolled over a gap in an anchored green drops through
//      into the cup below -- a container with its own space and its own
//      floor -- is taken into it by fit, and rests on the cup's floor.
//  10. THE SMEAR (§14). A contact is found on the segment a step moves a
//      body along, not where the step leaves it: a ball whose step would
//      carry it through a thin wall, or the ground, meets it; it turns at
//      the face, or the edge, it met, at the angle it met it, and what is
//      left of the step is on the way out. Two free balls that cross inside
//      a step meet. The same rows pair by pair and through the kd-tree.
//  11. THE SPEED OF LIGHT. A space states the fastest anything in it moves:
//      a body pushed past it moves at it and keeps the rest as heat; a room
//      inside inherits it or states its own; 65536 where none is stated.
//
//   ./Run_SolidTesterLoader
//
#include "../ETCS.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

static int g_fail = 0;
static void check(bool ok, const char* what)
{
    std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++g_fail;
}

// A plain body: a sphere by its reach, or a box by its half extents; drag, a
// step, and a rest below a crawl (a still body is still, not dithering).
class Body : public CausalBase<Body>, public DeletableBase<Body>
{
public:
    WIRE_TYPE_IDENTITY(Body)
    Fixed mass = Fixed::One();
    Fixed damping;
    Fixed hx, hy, hz;

    void AdvanceConcrete(Fixed dt) override
    {
        if (damping.IsPositive()) Rows().Dissipate((-(damping * dt)).Exp());
        Fixed vx, vy, vz;
        Rows().Velocity(mass, vx, vy, vz);
        if ((vx * vx + vy * vy + vz * vz) < Fixed::From(1e-6)) { Rows().Rest(); return; }
        Rows().AdvanceBy(vx, vy, vz, dt);
    }
    Fixed MassConcrete() const override { return mass; }
    CausalSolid::Shape DefaultShapeConcrete() const override { return hx.IsPositive() ? CausalSolid::Box : CausalSolid::Sphere; }
    void ExtentConcrete(Fixed& x, Fixed& y, Fixed& z) const override { x = hx; y = hy; z = hz; }
    bool DeleteConcrete() override { return true; }

    void Place(double x, double y, double z) { std::lock_guard<std::recursive_mutex> lk(TreeMutex()); Rows().PlaceAt(Fixed::From(x), Fixed::From(y), Fixed::From(z)); }
    void Ball(double r) { std::lock_guard<std::recursive_mutex> lk(TreeMutex()); Rows().radius = Fixed::From(r); }
    void Box(double w, double h, double d)
    {
        hx = Fixed::From(w / 2); hy = Fixed::From(h / 2); hz = Fixed::From(d / 2);
        std::lock_guard<std::recursive_mutex> lk(TreeMutex());
        Rows().radius = Fixed::Length(hx, hy, hz);
    }
    double X() const { return Order4().x.ToDouble(); }
    double Y() const { return Order4().y.ToDouble(); }
    double Z() const { return Order4().z.ToDouble(); }
    double Speed() { Fixed vx, vy, vz; Order4().Velocity(mass, vx, vy, vz); return Fixed::Length(vx, vy, vz).ToDouble(); }
    double Vx() { Fixed vx, vy, vz; Order4().Velocity(mass, vx, vy, vz); return vx.ToDouble(); }
};

static const Fixed kDt = Fixed::From(0.016);
static const Fixed kG  = Fixed::From(-9.8);

// The ledger over a tree: energy held + left at the boundary - the fields' work.
static void ledger(Body* b, Fixed& held, Fixed& field)
{
    held += b->Order4().energy;
    field += b->FieldWork();
    for (Causal_* c : *b->causalChildren()) ledger(static_cast<Body*>(c), held, field);
}
static bool ledgerHolds(Body* world, Fixed put_in)
{
    Fixed held, field;
    ledger(world, held, field);
    const Fixed diff = held + world->EmittedOut() - field - put_in;
    const bool ok = diff.Abs().raw <= (int64_t(1) << 20);   // a few last bits per operation, over the run
    if (!ok) std::printf("        (ledger off by %.9f J, %lld raw)\n", diff.ToDouble(), (long long)diff.raw);
    return ok;
}
static Body* world(ETCS::MemoryArena& arena)
{
    Body* w = arena.allocate<Body>();
    w->SetSpace(Fixed::FromInt(1000));
    w->SetGravity(Fixed::Zero(), kG, Fixed::Zero());
    return w;
}
// An anchored box of floor, its top at y = top.
static Body* floorBox(Body* in, double top, double w, double restitution, double friction)
{
    Body* f = in->addTag<Body>();
    f->Box(w, 1.0, w);
    f->Place(0, top - 0.5, 0);
    f->SetSolid(Fixed::From(restitution), Fixed::From(friction));
    f->SetAnchored(true);
    return f;
}
static Body* ball(Body* in, double x, double y, double z, double restitution, double friction)
{
    Body* b = in->addTag<Body>();
    b->Ball(0.5);
    b->Place(x, y, z);
    b->SetSolid(Fixed::From(restitution), Fixed::From(friction));
    return b;
}

int main()
{
    shell_startup();
    WIRE_CONTEXT();
    auto& arena = ETCS::MemoryArena::getInstance();
    auto F = [](double v) { return Fixed::From(v); };
    std::cout << "=== Planes, the space's parameters, and solids ===\n";

    // -- 1. planes -------------------------------------------------------------
    {
        std::cout << "\n-- 1. planes --\n";
        const Plane up = Plane::FromPointNormal(F(0), F(2), F(0), F(0), F(5), F(0));
        check(up.ny == Fixed::One() && up.d == F(2), "a point and a normal: the normal made unit, the offset its dot");
        check(up.Distance(F(3), F(5), F(-1)) == F(3) && up.Classify(F(0), F(1), F(0)) == Plane::Behind
              && up.Classify(F(0), F(2), F(0)) == Plane::On, "signed distance; behind, on");
        const Plane tri = Plane::FromPoints(F(0), F(0), F(0), F(1), F(0), F(0), F(0), F(1), F(0));
        check(tri.nz == Fixed::One() && !Plane::FromPoints(F(0), F(0), F(0), F(1), F(1), F(1), F(2), F(2), F(2)).Valid(),
              "three points: the right hand gives the front; collinear points give no plane");
        Fixed px = F(3), py = F(7), pz = F(1);
        up.Project(px, py, pz);
        Fixed t;
        const bool hit = up.Intersect(F(0), F(10), F(0), F(0), F(-2), F(0), t);
        const bool away = up.Intersect(F(0), F(10), F(0), F(0), F(2), F(0), t);
        check(py == F(2) && px == F(3) && hit && !away, "projection onto it; a ray crosses it ahead, not behind");
        up.Intersect(F(0), F(10), F(0), F(0), F(-2), F(0), t);
        check(t == F(4), "...at the parameter it should (10 - 2) / 2");
        Fixed vx = F(1), vy = F(-3), vz = F(0);
        up.Reflect(vx, vy, vz);
        Fixed tx, ty, tz;
        const Fixed vn = up.Split(F(1), F(-3), F(0), tx, ty, tz);
        check(vx == F(1) && vy == F(3) && vn == F(-3) && tx == F(1) && ty.IsZero(), "reflect and split a motion");
        std::vector<OrderVector> set(4);
        set[0].PlaceAt(F(0), F(5), F(0)); set[0].radius = F(1);
        set[1].PlaceAt(F(0), F(-5), F(0)); set[1].radius = F(1);
        set[2].PlaceAt(F(0), F(2.5), F(0)); set[2].radius = F(1);
        set[3].PlaceAt(F(4), F(2), F(0));
        std::vector<size_t> front, behind, across;
        up.Bisect(set.data(), set.size(), front, behind, across);
        check(front == std::vector<size_t>{ 0 } && behind == std::vector<size_t>{ 1 } && across == std::vector<size_t>{ 2, 3 },
              "bisect a set: wholly in front, wholly behind, and across (a reach through it, a point on it)");
        Plane faces[6];
        Planes::Box(F(1), F(2), F(3), faces);
        bool inside = true;
        for (const Plane& f : faces) inside &= f.Classify(F(0.5), F(-1.5), F(2.5)) == Plane::Behind;
        check(inside && faces[3].Classify(F(0), F(-3), F(0)) == Plane::InFront, "a box is six planes: inside is behind every face");
        const SolidContact edge = Planes::SphereBox(F(1), F(1), F(1), F(1.3), F(1.3), F(0), F(0.5), Fixed::Zero());
        check(edge.touching && (edge.nx - edge.ny).Abs().raw < 16, "a sphere on a box's edge is pushed out along the diagonal");
        // Aim: a frame's up turned onto a direction, every way including down.
        bool aimed = true;
        const double dirs[][3] = { { 1, 0, 0 }, { 0, 0, -1 }, { 1, 2, 3 }, { 0, -1, 0 }, { 0, 1, 0 } };
        for (const auto& d : dirs)
        {
            OrderVector o;
            o.Aim(F(d[0]), F(d[1]), F(d[2]));
            Fixed ux = Fixed::Zero(), uy = Fixed::One(), uz = Fixed::Zero();
            o.RotateVector(ux, uy, uz);
            const double l = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
            aimed &= std::fabs(ux.ToDouble() - d[0] / l) < 1e-6 && std::fabs(uy.ToDouble() - d[1] / l) < 1e-6
                  && std::fabs(uz.ToDouble() - d[2] / l) < 1e-6 && o.Holds();
        }
        check(aimed, "Aim turns a frame's up onto any direction, straight down included");
        // A camera's ray: the middle of the frame looks along the view; a
        // corner, through the corner of the lens.
        ViewFrustum v{ Point3D{ 0, 3, -10 }, Point3D{ 0, 3, 0 }, Point3D{ 0, 1, 0 }, 90.0f * 3.14159265f / 180.0f, 0.1f, 100.0f };
        Point3D o, d;
        const bool mid = ViewRay(v, 0.5f, 0.5f, 2.0f, o, d) && std::fabs(d.z - 1.0f) < 1e-6f && o.y == 3.0f;
        ViewRay(v, 1.0f, 0.0f, 2.0f, o, d);   // top right: x = 2 * tan 45, y = tan 45, along z 1
        const float k = 1.0f / std::sqrt(1.0f + 4.0f + 1.0f);
        check(mid && std::fabs(d.x - 2 * k) < 1e-5f && std::fabs(d.y - k) < 1e-5f && std::fabs(d.z - k) < 1e-5f,
              "a camera's ray: the frame's middle looks along the view, its corner through the lens's corner");
    }

    // -- 2. the field is the space's -----------------------------------------
    {
        std::cout << "\n-- 2. the field is the space's --\n";
        Body* w = world(arena);
        Body* room = w->addTag<Body>();      // states nothing: the world's field reaches in
        room->SetSpace(Fixed::FromInt(100));
        room->SetAnchored(true);             // a place: it does not fall with its member
        Body* pocket = w->addTag<Body>();    // states zero: a pocket of its own
        pocket->Place(50, 0, 0);
        pocket->SetSpace(Fixed::FromInt(500));   // more room than it takes up in the world: a parameter
        pocket->SetGravity(Fixed::Zero(), Fixed::Zero(), Fixed::Zero());
        pocket->SetAnchored(true);
        Body* faller  = room->addTag<Body>();   faller->Ball(0.1);
        Body* floater = pocket->addTag<Body>(); floater->Ball(0.1);
        w->Run(60, kDt);   // ~one second
        const double fell = -faller->Y(), v = faller->Speed();
        check(std::fabs(fell - 0.5 * 9.8 * 0.96 * 0.96) < 0.2 && std::fabs(v - 9.8 * 0.96) < 0.01,
              "a member of a room that states nothing falls along the world's field");
        check(floater->Y() == 0.0 && floater->Speed() == 0.0, "a pocket stating zero holds its member still, whatever the world's field");
        pocket->InheritGravity();
        w->Run(30, kDt);
        check(floater->Y() < 0.0, "the statement taken back: the pocket inherits the world's field again");
        check(w->FieldWork().IsPositive() && ledgerHolds(w, Fixed::Zero()),
              "the field's work is counted where it is stated, and the ledger holds");
        Body* climber = w->addTag<Body>(); climber->Ball(0.1); climber->Place(-50, 0, 0);
        climber->Impulse(F(0), F(1), F(0), F(50));
        w->Run(200, kDt);
        check(ledgerHolds(w, F(50)), "a body thrown up and falling back: given back and taken again, the ledger holds");
    }

    // -- 3. a drop -------------------------------------------------------------
    {
        std::cout << "\n-- 3. a drop --\n";
        Body* w = world(arena);
        floorBox(w, 0, 10, 1.0, 0.5);
        Body* b = ball(w, 0, 3, 0, 0.6, 0.3);
        std::vector<double> apex;
        double prev = b->Y(), dir = -1;
        for (int i = 0; i < 1200; ++i)
        {
            w->Run(1, kDt);
            const double y = b->Y();
            if (dir > 0 && y < prev) apex.push_back(prev);
            dir = y > prev ? 1 : (y < prev ? -1 : dir);
            prev = y;
        }
        bool lower = apex.size() >= 2;
        for (size_t i = 1; i < apex.size(); ++i) lower &= apex[i] < apex[i - 1];
        check(lower, "it bounces, each apex lower than the last");
        check(std::fabs(b->Y() - 0.5) < 0.002 && b->Speed() == 0.0, "and lands: at rest on the surface");
        const double y = b->Y();
        w->Run(300, kDt);
        check(b->Y() == y && b->Speed() == 0.0, "and stays there: a resting body feels none of the field the surface holds up");
        check(ledgerHolds(w, Fixed::Zero()), "the ledger, through every bounce and the rest");
        std::printf("        (%zu bounces seen, first apex %.3f)\n", apex.size(), apex.empty() ? 0.0 : apex[0]);
    }

    // -- 4. a roll -------------------------------------------------------------
    {
        std::cout << "\n-- 4. a roll --\n";
        bool still_before = false, moving_during = false, still_after = false;
        auto roll = [&](double friction) {
            Body* w = world(arena);
            floorBox(w, 0, 200, 0.0, friction);
            Body* b = ball(w, 0, 0.5, 0, 0.0, friction);
            w->Run(5, kDt);
            still_before = !w->Moving();
            b->Impulse(F(1), F(0), F(0), F(2));   // 2 J: 2 m/s
            w->Run(1, kDt);
            moving_during = w->Moving();
            w->Run(599, kDt);
            still_after = !w->Moving();
            return b;
        };
        Body* rubbed = roll(0.3);
        const double expect = 2.0 * 2.0 / (2 * 0.3 * 9.8);   // v^2 / (2 mu g)
        check(rubbed->Speed() == 0.0 && std::fabs(rubbed->X() - expect) < 0.1 * expect,
              "pushed along a surface with friction, it slows and stops near v^2 / (2 mu g)");
        check(still_before && moving_during && still_after,
              "the world says Moving while it rolls, and not before or once it has stopped");
        Body* slick = roll(0.0);
        check(slick->Speed() > 1.9 && slick->X() > 15, "without friction it keeps going");
        check(std::fabs(rubbed->Y() - 0.5) < 0.002, "...on the surface the whole way");
    }

    // -- 5. a slope ------------------------------------------------------------
    {
        std::cout << "\n-- 5. a slope --\n";
        Body* w = world(arena);
        Body* ramp = w->addTag<Body>();   // a box tilted 20 degrees about z
        ramp->Box(20, 1, 6);
        ramp->Place(0, 0, 0);
        { std::lock_guard<std::recursive_mutex> lk(ramp->TreeMutex()); ramp->Rows().Orient(F(0), F(0), F(1), F(-20 * 3.14159265 / 180)); }
        ramp->SetSolid(F(0), F(0.1));
        ramp->SetAnchored(true);
        // Down the slope is +x (the right end is lower). Above the middle.
        Body* b = ball(w, 0, 1.2, 0, 0.0, 0.1);
        w->Run(240, kDt);
        check(b->X() > 1.0 && b->Y() < 0.8, "on a tilted box it slides downhill");
        check(ledgerHolds(w, Fixed::Zero()), "...and the ledger holds");
    }

    // -- 6. two free balls -------------------------------------------------------
    {
        std::cout << "\n-- 6. two free balls --\n";
        Body* w = arena.allocate<Body>();   // no field: only the contact
        Body* a = ball(w, -2, 0, 0, 1.0, 0.0);
        Body* c = ball(w,  2, 0, 0, 1.0, 0.0);
        a->Impulse(F(1), F(0), F(0), F(2));   // 2 m/s toward c
        w->Run(150, kDt);
        check(std::fabs(a->Speed()) < 0.01 && std::fabs(c->Vx() - 2.0) < 0.01, "head on, equal masses, restitution one: they exchange velocities");
        check(ledgerHolds(w, F(2)), "...and the energy is exact");
        // A body that is not solid is not met: a marker in the way passes.
        Body* v = arena.allocate<Body>();
        Body* runner = ball(v, -2, 0, 0, 1.0, 0.0);
        Body* ghost = v->addTag<Body>(); ghost->Ball(0.5); ghost->Place(0, 0, 0);
        runner->Impulse(F(1), F(0), F(0), F(2));
        v->Run(150, kDt);
        check(std::fabs(runner->Vx() - 2.0) < 1e-6 && ghost->Speed() == 0.0 && runner->X() > 2,
              "a solid meets only solids: one that is not passes straight through");
    }

    // -- 7. a half-space -------------------------------------------------------
    {
        std::cout << "\n-- 7. a half-space --\n";
        Body* w = world(arena);
        Body* ground = w->addTag<Body>();
        ground->Place(0, 0, 0);
        ground->SetSolid(F(0.2), F(0.5));
        ground->SetAnchored(true);
        ground->SetShape(CausalSolid::HalfSpace);
        Body* b = ball(w, 400, 2, -300, 0.2, 0.5);   // far from the plane's centre: no reach to miss by
        w->Run(400, kDt);
        check(std::fabs(b->Y() - 0.5) < 0.002 && b->Speed() == 0.0, "a ball falls onto one plane and rests on it");
    }

    // -- 8. the same rows ------------------------------------------------------
    {
        std::cout << "\n-- 8. the same rows --\n";
        const size_t kFrom = CausalBase<Body>::BroadphaseFrom();
        auto crowd = [&](size_t from) {
            CausalBase<Body>::BroadphaseFrom() = from;
            Body* w = world(arena);
            floorBox(w, 0, 40, 0.5, 0.4);
            for (int i = 0; i < 60; ++i)
            {
                Body* b = ball(w, (i % 8) * 1.3 - 5, 1 + (i / 8) * 1.2, ((i * 7) % 5) - 2.0, 0.5, 0.4);
                b->Impulse(F((i % 3) - 1.0), F(0), F(((i * 5) % 3) - 1.0), F(0.5 + (i % 4)));
            }
            w->Run(400, kDt);
            Fixed put_in;
            for (int i = 0; i < 60; ++i) put_in += F(0.5 + (i % 4));
            const uint64_t h = w->CausalHash();
            return std::make_pair(h, ledgerHolds(w, put_in));
        };
        const auto pairwise = crowd(100000), tree = crowd(16), again = crowd(16);
        CausalBase<Body>::BroadphaseFrom() = kFrom;
        check(pairwise.first == tree.first && tree.first == again.first, "a crowd dropped on a box: the same rows pair by pair, through the kd-tree, and twice");
        check(tree.second, "...and the ledger holds across 60 balls");
    }

    // -- 9. a hole -------------------------------------------------------------
    {
        std::cout << "\n-- 9. a hole --\n";
        Body* w = world(arena);
        // A green with a 1.2-wide square gap at (6, 0): four anchored slabs.
        auto slab = [&](double x, double z, double sx, double sz) {
            Body* s = w->addTag<Body>();
            s->Box(sx, 1.0, sz);
            s->Place(x, -0.5, z);
            s->SetSolid(F(0.3), F(0.05));
            s->SetAnchored(true);
        };
        slab(-1.2, 0, 13.2, 20);      // x in [-7.8, 5.4]
        slab(10.2, 0, 7.6, 20);       // x in [6.6, 14]
        slab(6, -5.3, 1.2, 9.4);      // z in [-10, -0.6]
        slab(6,  5.3, 1.2, 9.4);      // z in [0.6, 10]
        // The cup: below the gap, its own space, its own floor inside it --
        // inside it by fit too: the floor's reach lies within the cup's space,
        // or the first interaction would move it out to the green.
        Body* cup = w->addTag<Body>();
        cup->Place(6, -1.6, 0);
        cup->SetSpace(F(1.1));
        cup->SetAnchored(true);   // a place, not a thing: it does not fall
        Body* bottom = cup->addTag<Body>();
        bottom->Box(1.2, 0.2, 1.2);
        bottom->Place(0, -0.2, 0);   // its top at -1.7 in the world
        bottom->SetSolid(F(0.1), F(0.6));
        bottom->SetAnchored(true);
        Body* b = w->addTag<Body>();
        b->Ball(0.2);
        b->Place(2, 0.2, 0);
        b->SetSolid(F(0.3), F(0.05));
        w->Run(5, kDt);
        b->Impulse(F(1), F(0), F(0), F(2));   // 2 m/s toward the cup, ~0.8 m/s when it gets there
        bool holed = false;
        for (int i = 0; i < 900 && !holed; ++i)
        {
            w->Run(1, kDt);
            holed = b->getParent() == cup;
        }
        check(holed, "rolled at the gap, it drops through and fit takes it into the cup");
        w->Run(300, kDt);
        Fixed bx, by, bz;
        b->Basis(bx, by, bz);
        const double wy = by.ToDouble() + b->Y();
        check(b->getParent() == cup && b->Speed() == 0.0 && std::fabs(wy - (-1.7 + 0.2)) < 0.01,
              "and rests on the cup's own floor, in the cup");
        check(ledgerHolds(w, F(2)), "the ledger, across the move");
    }

    // -- 10. the smear ----------------------------------------------------------
    {
        std::cout << "\n-- 10. the smear --\n";
        const Fixed big = F(0.1);                       // a long frame: the ball moves 1.2 a step
        auto thin = [&](Body* in, double x, double y, double z, double w, double h, double d) {
            Body* f = in->addTag<Body>();
            f->Box(w, h, d);
            f->Place(x, y, z);
            f->SetSolid(F(1.0), F(0.0));
            f->SetAnchored(true);
            return f;
        };
        auto golf = [&](Body* in, double x, double y, double z) { Body* b = ball(in, x, y, z, 1.0, 0.0); b->Ball(0.15); return b; };
        auto vel = [](Body* b, double v[3]) { Fixed x, y, z; b->Order4().Velocity(b->mass, x, y, z); v[0] = x.ToDouble(); v[1] = y.ToDouble(); v[2] = z.ToDouble(); };
        double v[3];
        {
            // Straight at a wall 0.3 thick, twelve units a second: the second
            // step would end 0.4 past it. It meets the near face 0.5 into
            // that step and the other 0.7 of it comes back.
            Body* w = arena.allocate<Body>();
            thin(w, 0, 0, 0, 0.3, 4, 4);
            Body* b = golf(w, -2, 0, 0);
            b->Impulse(F(1), F(0), F(0), F(72));
            w->Run(2, big);
            vel(b, v);
            std::printf("        (at the wall: x %.4f, vx %.3f)\n", b->X(), v[0]);
            check(std::fabs(b->X() + 1.0) < 0.002 && std::fabs(v[0] + 12.0) < 0.01,
                  "a ball whose step would carry it through a thin wall meets the wall, and what is left of the step comes back");
            check(ledgerHolds(w, F(72)), "...the energy exact");
        }
        {
            // Down at a green half a unit thick, the field on: it stays above
            // it, bounces lower, and comes to rest on it.
            Body* w = world(arena);
            Body* green = thin(w, 0, -0.25, 0, 8, 0.5, 8);
            green->SetSolid(F(0.3), F(0.1));
            Body* b = golf(w, 0, 0.6, 0);
            b->SetSolid(F(0.45), F(0.1));
            b->Impulse(F(0), F(-1), F(0), F(72));
            double lowest = b->Y();
            for (int i = 0; i < 80; ++i) { w->Run(1, big); lowest = std::min(lowest, b->Y()); }
            std::printf("        (at the green: lowest y %.4f, at rest at y %.4f, speed %.4f)\n", lowest, b->Y(), b->Speed());
            check(lowest > 0.148, "a ball driven down at the ground faster than the ground is thick does not pass through it");
            check(std::fabs(b->Y() - 0.15) < 0.002 && b->Speed() == 0.0, "...and comes to rest on it");
        }
        {
            // At 45 degrees: the step would end inside the wall. It turns at
            // the face -- the angle out is the angle in -- and ends where the
            // mirror of the step's end is, not pushed out to the face.
            Body* w = arena.allocate<Body>();
            thin(w, 0, 0, 0, 0.3, 4, 8);
            Body* b = golf(w, -1.0, 0, 0);
            const double c = std::sqrt(0.5);
            b->Impulse(F(c), F(0), F(c), F(72));
            w->Run(1, big);
            vel(b, v);
            std::printf("        (off the wall at 45: at %.4f %.4f, velocity %.3f %.3f)\n", b->X(), b->Z(), v[0], v[2]);
            check(std::fabs(v[0] + 12 * c) < 0.01 && std::fabs(v[2] - 12 * c) < 0.01, "a ball that meets a wall at an angle leaves it at that angle");
            check(std::fabs(b->X() - (-0.3 - (1.2 * c - 0.7))) < 0.002 && std::fabs(b->Z() - 1.2 * c) < 0.002,
                  "...from where it met it: the rest of the step is on the way out");
        }
        {
            // At a box's upright edge, along the diagonal: the step ends
            // inside the box. The edge sends it straight back.
            Body* w = arena.allocate<Body>();
            thin(w, 0, 0, 0, 2, 2, 2);
            Body* b = golf(w, -3, 0, -3);
            const double c = std::sqrt(0.5);
            b->Impulse(F(c), F(0), F(c), F(72));
            w->Run(3, big);
            vel(b, v);
            const double met = -1.0 - 0.15 * c, back = 3.6 - (3.0 + met) / c;   // where its middle is at the edge; how far it has come back
            std::printf("        (off the edge: at %.4f %.4f, velocity %.3f %.3f)\n", b->X(), b->Z(), v[0], v[2]);
            check(std::fabs(v[0] + 12 * c) < 0.02 && std::fabs(v[2] + 12 * c) < 0.02 && std::fabs(b->X() - (met - back * c)) < 0.005
                  && std::fabs(b->Z() - (met - back * c)) < 0.005,
                  "a ball that meets a box's edge inside a step turns on the edge, not on the face its step ended nearest");
        }
        {
            // Two free balls, one fast enough to end a step on the other's
            // middle and the next beyond it.
            Body* w = arena.allocate<Body>();
            Body* a = ball(w, -3, 0, 0, 1.0, 0.0);
            Body* c = ball(w,  3, 0, 0, 1.0, 0.0);
            a->Impulse(F(1), F(0), F(0), F(450));   // 30 a second: 3 a step
            w->Run(2, big);
            std::printf("        (two balls: the first at x %.4f, speed %.3f; the second at x %.4f, vx %.3f)\n", a->X(), a->Speed(), c->X(), c->Vx());
            check(std::fabs(a->X() - 2.0) < 0.002 && a->Speed() < 0.01 && std::fabs(c->X() - 4.0) < 0.002 && std::fabs(c->Vx() - 30.0) < 0.01,
                  "two free balls that cross inside a step meet where they touch, and the struck one has the rest of the step");
            check(ledgerHolds(w, F(450)), "...the energy exact");
        }
        {
            // The same rows, pair by pair and through the kd-tree, with balls
            // fast enough to cross each other and the floor inside a step.
            const size_t kFrom = CausalBase<Body>::BroadphaseFrom();
            auto crowd = [&](size_t from) {
                CausalBase<Body>::BroadphaseFrom() = from;
                Body* w = world(arena);
                floorBox(w, 0, 40, 0.5, 0.4);
                thin(w, 9, 2, 0, 0.3, 4, 30);
                for (int i = 0; i < 40; ++i)
                {
                    Body* b = ball(w, (i % 8) * 1.3 - 5, 1 + (i / 8) * 1.2, ((i * 7) % 5) - 2.0, 0.5, 0.4);
                    b->Ball(0.2);
                    b->Impulse(F((i % 3) - 0.5), F(-0.3), F(((i * 5) % 3) - 1.0), F(40 + 20 * (i % 4)));
                }
                double lowest = 9;
                for (int t = 0; t < 120; ++t)
                {
                    w->Run(1, F(0.05));
                    for (Causal_* c : *w->causalChildren()) if (c->Solid().shape == CausalSolid::Sphere) lowest = std::min(lowest, c->Order4().y.ToDouble());
                }
                return std::make_pair(w->CausalHash(), lowest);
            };
            const auto pairwise = crowd(100000), tree = crowd(16), again = crowd(16);
            CausalBase<Body>::BroadphaseFrom() = kFrom;
            std::printf("        (a fast crowd: the lowest any ball got, y %.4f)\n", tree.second);
            check(pairwise.first == tree.first && tree.first == again.first, "a fast crowd: the same rows pair by pair, through the kd-tree, and twice");
            check(tree.second > 0.19, "...and none of it through the floor");
        }
    }

    // -- 11. the speed of light --------------------------------------------------
    {
        std::cout << "\n-- 11. the speed of light --\n";
        auto vx = [](Body* b) { return b->Vx(); };
        // A space that states 16: under it nothing changes; over it the body
        // is slowed to it, and what it lost of its motion is still its energy.
        Body* w = arena.allocate<Body>();
        w->SetLightSpeed(F(16));
        Body* slow = ball(w, 0, 0, 0, 1.0, 0.0);
        Body* fast = ball(w, 0, 0, 10, 1.0, 0.0);
        slow->Impulse(F(1), F(0), F(0), F(72));    // 12 a second
        fast->Impulse(F(1), F(0), F(0), F(800));   // 40 a second, asked for
        w->Run(1, F(0.1));
        std::printf("        (in a space of 16: 12 asked, %.3f; 40 asked, %.3f, moved %.3f in a tenth, energy %.1f)\n",
                    vx(slow), vx(fast), fast->X(), fast->Order4().energy.ToDouble());
        check(std::fabs(vx(slow) - 12.0) < 1e-6 && std::fabs(slow->X() - 1.2) < 1e-6, "a space states its speed of light: a body under it is left as it was");
        check(std::fabs(vx(fast) - 16.0) < 1e-4 && std::fabs(fast->X() - 1.6) < 1e-4, "...one pushed past it moves at it, no further in a step than light goes");
        check(fast->Order4().energy == F(800) && ledgerHolds(w, F(872)), "...and what it lost of its motion it keeps as heat: the ledger holds");
        // Inherited down the tree, a pocket's own whatever its container's, and taken back.
        Body* room = w->addTag<Body>();
        room->SetSpace(F(50));
        Body* in = ball(room, 0, 0, 0, 1.0, 0.0);
        in->Impulse(F(0), F(0), F(1), F(800));
        w->Run(1, F(0.1));
        const double inherited = in->Speed();
        room->SetLightSpeed(F(32));
        in->Impulse(F(0), F(0), F(1), F(800));
        w->Run(1, F(0.1));
        const double pocket = in->Speed();
        room->InheritLightSpeed();
        w->Run(1, F(0.1));
        std::printf("        (in a room in it: %.3f; the room stating 32: %.3f; taken back: %.3f)\n", inherited, pocket, in->Speed());
        check(std::fabs(inherited - 16.0) < 1e-4, "a room that states none passes on the one it is in");
        check(std::fabs(pocket - 32.0) < 1e-4 && std::fabs(in->Speed() - 16.0) < 1e-4, "a room stating its own has its own, and taken back it inherits again");
        // Where nothing states one: 65536 a second.
        Body* open = arena.allocate<Body>();
        Body* far = ball(open, 0, 0, 0, 1.0, 0.0);
        far->Impulse(F(1), F(0), F(0), F(500000));   // 1000 a second
        open->Run(1, F(0.016));
        check(open->LightUnder() == Fixed::FromInt(65536) && std::fabs(vx(far) - 1000.0) < 1e-3,
              "where no space states one it is 65536 a second: a thousand a second is not near it");
    }

    std::printf("\n%s (%d failure%s)\n", g_fail ? "FAILED" : "PASSED", g_fail, g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}
