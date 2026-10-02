// CausalTesterLoader.cc
//
// THE CAUSAL FAMILY'S CONSTRAINTS, RUN AGAINST A TREE OF PLAIN BODIES.
//
// OrderVectorTesterLoader holds the rows' own operations to their promises;
// this holds the FAMILY to its (ontology/Causal.h, CausalBase.h) -- the part
// that is about a tree of entities rather than one vector. A Body is the
// smallest Causal leaf there is: drag and a step, nothing else. What is
// checked, each a constraint from ontology/etcs_causal_constraints.md:
//
//   1. THE LEDGER. Over any run, the energy in the tree plus what left at
//      the open boundary is what was put in by Impulse, to the bit.
//   2. THE ARROW. The ordered energy of the whole tree never rises between
//      impulses -- through drag, through emission, through contact.
//   3. THE ROWS HOLD. Every body satisfies OrderVector::Holds after every
//      interaction.
//   4. IDENTITY AND ORDER. The same scene built with its members attached
//      in another order is the same scene: same CausalHash, same rows per
//      member, same draws. Row 0's identity is the state hash, not a RID.
//   5. CONTACT. Two members whose reaches touch exchange along the line
//      between them; a head-on contact hands everything over; the
//      exchange does not depend on which is walked first.
//   6. THE RECORD. An observed history -- spans a clock measured -- replays
//      from the tape onto the same rows.
//   7. ONE HISTORY. A reader under the tree's lock sees one state while a
//      driver runs on another thread.
//   8. THE CLOCK. Ticks count crossings; a body with no heat has no time.
//
//   ./Run_CausalTesterLoader
//
#include "../ETCS.h"
#include <atomic>
#include <cstdio>
#include <thread>
#include <vector>

static int g_fail = 0;
static void check(bool ok, const char* what)
{
    std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++g_fail;
}

// The smallest Causal leaf: a point with mass, drag, and a reach.
class Body : public CausalBase<Body>,
             public DeletableBase<Body>
{
public:
    WIRE_TYPE_IDENTITY(Body)
    Fixed mass    = Fixed::One();
    Fixed damping = Fixed::From(0.5);

    void AdvanceConcrete(Fixed dt) override
    {
        Rows().Dissipate((-(damping * dt)).Exp());
        Rows().Advance(dt, mass);
    }
    Fixed MassConcrete() const override { return mass; }
    bool  DeleteConcrete() override { return true; }

    void Place(double x, double y, double z) { std::lock_guard<std::recursive_mutex> lk(TreeMutex()); Rows().PlaceAt(Fixed::From(x), Fixed::From(y), Fixed::From(z)); }
    void Reach(double r) { std::lock_guard<std::recursive_mutex> lk(TreeMutex()); Rows().radius = Fixed::From(r); }
};

// The whole tree's energy, ordered energy, and whether every row holds.
struct Totals { Fixed energy, kinetic; bool holds = true; size_t bodies = 0; };
static void totals(Body* b, Totals& t)
{
    std::lock_guard<std::recursive_mutex> lk(b->TreeMutex());
    const OrderVector& o = b->Order4();
    t.energy  += o.energy;
    t.kinetic += o.KineticEnergy();
    if (!o.Holds()) t.holds = false;
    ++t.bodies;
    for (Causal_* c : *b->causalChildren()) totals(static_cast<Body*>(c), t);
}

int main()
{
    shell_startup();
    WIRE_CONTEXT();
    auto& arena = ETCS::MemoryArena::getInstance();
    const Fixed dt = Fixed::From(0.016);

    std::cout << "=== The Causal family ===\n";

    // A world with a room in it, and three bodies in the room: two that will
    // meet, one that drifts.
    auto build = [&](bool reversed) -> Body* {
        Body* world = arena.allocate<Body>();
        world->SetEmissivity(Fixed::From(0.2));
        Body* room  = world->addTag<Body>();
        room->Place(0, 0, 0); room->SetEmissivity(Fixed::From(0.3));
        auto make = [&](double x, double y, double z, double r, double dx, double dy, double dz, double j, double damp) {
            Body* b = room->addTag<Body>();
            b->Place(x, y, z); b->Reach(r); b->damping = Fixed::From(damp);
            b->Impulse(Fixed::From(dx), Fixed::From(dy), Fixed::From(dz), Fixed::From(j));
            return b;
        };
        if (!reversed)
        {
            make(-3, 0, 0, 0.5,  1, 0, 0, 12, 0.1);    // toward +x: will meet the second
            make( 3, 0, 0, 0.5, -1, 0, 0, 12, 0.1);    // toward -x
            make( 0, 5, 0, 0.5,  0, 0, 1,  4, 1.0);    // drifts off alone
        }
        else
        {
            make( 0, 5, 0, 0.5,  0, 0, 1,  4, 1.0);
            make( 3, 0, 0, 0.5, -1, 0, 0, 12, 0.1);
            make(-3, 0, 0, 0.5,  1, 0, 0, 12, 0.1);
        }
        return world;
    };

    // -- 1, 2, 3: the ledger, the arrow, the rows ------------------------------
    {
        std::cout << "\n-- 1-3. the ledger, the arrow, the rows --\n";
        Body* world = build(false);
        Totals t0; totals(world, t0);
        const Fixed put_in = Fixed::FromInt(12 + 12 + 4);
        check(t0.bodies == 5 && t0.energy == put_in, "what Impulse put in is what the tree holds, to the bit");
        bool ledger = true, arrow = true, holds = true;
        Fixed k_prev = t0.kinetic;
        for (int i = 0; i < 2000; ++i)
        {
            world->Interact(dt);
            Totals t; totals(world, t);
            if (t.energy + world->EmittedOut() != put_in) ledger = false;
            if (t.kinetic.raw > k_prev.raw + OrderVector::kSlackRaw) arrow = false;
            if (!t.holds) holds = false;
            k_prev = t.kinetic;
        }
        check(ledger, "2000 interactions: tree energy plus what left the model is still what was put in, exactly");
        { Totals t; totals(world, t); std::printf("        (held %.9f J + left %.9f J = %.0f J)\n", t.energy.ToDouble(), world->EmittedOut().ToDouble(), put_in.ToDouble()); }
        check(arrow,  "...the tree's ordered energy never rose");
        check(holds,  "...and every body's rows held after every interaction");
        check(world->EmittedOut().IsPositive(), "heat reached the open boundary through two containers");
        arena.deleteEntity(world, true);
    }

    // -- 4: identity and order -------------------------------------------------
    uint64_t forward_hash = 0;
    {
        std::cout << "\n-- 4. the same scene in another order --\n";
        Body* a = build(false);
        Body* b = build(true);
        a->Run(1500, dt); b->Run(1500, dt);
        forward_hash = a->CausalHash();
        check(a->CausalHash() == b->CausalHash(), "members attached in another order: the same CausalHash after 1500 ticks");
        // The identity on the rows is the state hash, and the two worlds
        // agree on it; the members' identities are their own.
        bool ids = a->Order4().id == b->Order4().id && a->Order4().id != 0;
        for (Causal_* c : *a->causalChildren()) if (c->Order4().id == a->Order4().id) ids = false;
        check(ids, "row 0's identity is the state hash: equal across the two builds, and a member's is not its container's");
        std::printf("        (hash %016llx)\n", static_cast<unsigned long long>(forward_hash));
        arena.deleteEntity(a, true);
        arena.deleteEntity(b, true);
    }

    // -- 5: contact ------------------------------------------------------------
    {
        std::cout << "\n-- 5. contact --\n";
        Body* world = arena.allocate<Body>();
        Body* p = world->addTag<Body>(); p->Place(-2, 0, 0); p->Reach(0.5); p->damping = Fixed::Zero();
        Body* q = world->addTag<Body>(); q->Place( 2, 0, 0); q->Reach(0.5); q->damping = Fixed::Zero();
        p->Impulse(Fixed::One(), Fixed::Zero(), Fixed::Zero(), Fixed::FromInt(8));   // v = 4 m/s at m = 1
        bool met = false; int at = -1;
        for (int i = 0; i < 400 && !met; ++i)
        {
            world->Interact(dt);
            if (q->Order4().KineticEnergy().IsPositive()) { met = true; at = i; }
        }
        check(met, "two members whose reaches touch exchange: the second starts moving");
        // The energy moves exactly; |K| of the receiver is 8 to the slack the
        // rows allow (the line between them is not a perfect square's root).
        const Fixed qk = q->Order4().KineticEnergy();
        check(met && p->Order4().KineticEnergy().IsZero() && p->Order4().energy.IsZero()
                  && q->Order4().energy == Fixed::FromInt(8)
                  && qk.raw >= Fixed::FromInt(8).raw - OrderVector::kSlackRaw && qk.raw <= Fixed::FromInt(8).raw + OrderVector::kSlackRaw
                  && q->Order4().Holds(),
              "head-on, equal masses: the mover stops and the other carries all 8 J, ordered, within the slack");
        check(met && q->CausalTicks() == 0 && p->CausalTicks() >= 1, "the crossing ticked the emitter's clock, not the absorber's");
        check(met && (q->Order4().x - p->Order4().x) <= Fixed::One(), "...and it happened where the reaches met (a gap of zero or less)");
        std::printf("        (met at tick %d)\n", at);
        arena.deleteEntity(world, true);
    }

    // -- 6: the record --------------------------------------------------------
    {
        std::cout << "\n-- 6. an observed history replays --\n";
        Body* live = build(false);
        uint64_t st = 0x2545f4914f6cdd1dull;
        auto next = [&st]() { st ^= st << 13; st ^= st >> 7; st ^= st << 17; return st; };
        for (int i = 0; i < 600; ++i)
        {
            const Fixed commit = Fixed::From(static_cast<double>(next() % 1200 + 1) / 1000.0);   // up to 1.2 s, as an entropy clock would
            const Fixed step   = Fixed::From(static_cast<double>(next() % 100) / 1000.0);        // up to 0.1 s, as a motion clock would; sometimes none
            live->InteractObserved(commit, step);
        }
        check(live->ObservedTape().size() == 600 && !live->ObservedTapeFull(), "every observed interaction is on the tape");
        Body* again = build(false);
        again->Replay(live->ObservedTape());
        check(again->CausalHash() == live->CausalHash(), "a fresh tree replaying the tape lands on the same hash");
        check(again->CausalTicks() == live->CausalTicks(), "...and the same clock");
        arena.deleteEntity(live, true);
        arena.deleteEntity(again, true);
    }

    // -- 7: one history --------------------------------------------------------
    {
        std::cout << "\n-- 7. one history under a driver and a reader --\n";
        Body* world = build(false);
        std::atomic<bool> stop{false};
        std::atomic<int>  torn{0}, unheld{0}, reads{0};
        std::thread reader([&]() {
            while (!stop.load(std::memory_order_acquire))
            {
                std::lock_guard<std::recursive_mutex> lk(world->TreeMutex());
                Totals t; totals(world, t);
                const uint64_t h1 = world->CausalHash();
                const uint64_t h2 = world->CausalHash();
                if (h1 != h2) torn.fetch_add(1);
                if (!t.holds) unheld.fetch_add(1);
                reads.fetch_add(1);
            }
        });
        for (int i = 0; i < 200; ++i) world->Run(50, dt);
        stop.store(true, std::memory_order_release);
        reader.join();
        check(torn.load() == 0 && reads.load() > 0, "a reader under the tree's lock saw one state every time while a driver ran 10,000 ticks");
        check(unheld.load() == 0, "...and every row held at every read");
        std::printf("        (%d reads, %d torn, %d unheld)\n", reads.load(), torn.load(), unheld.load());
        arena.deleteEntity(world, true);
    }

    // -- 8: the clock ---------------------------------------------------------
    {
        std::cout << "\n-- 8. emission is the clock --\n";
        Body* world = arena.allocate<Body>();
        Body* cold  = world->addTag<Body>();
        world->Run(100, dt);
        check(cold->CausalTicks() == 0 && world->CausalTicks() == 0, "a body with no heat has no time: no ticks in 100 interactions");
        cold->Impulse(Fixed::One(), Fixed::Zero(), Fixed::Zero(), Fixed::One());
        world->Run(100, dt);
        const uint64_t warm = cold->CausalTicks();
        check(warm > 0, "...and ticks once drag has made heat to shed");
        world->Run(100000, dt);
        check(cold->Order4().Heat().IsZero() && cold->CausalTicks() < 100100 && cold->CausalTicks() > warm,
              "it cools to exactly no heat (the last quantum leaves whole) and its clock stops there");
        arena.deleteEntity(world, true);
    }

    std::printf("\n%s (%d failure%s)\n", g_fail ? "FAILED" : "PASSED", g_fail, g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}
