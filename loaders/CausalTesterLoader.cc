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
//   4. IDENTITY AND ORDER. Row 0 carries the identity tuple -- the identity
//      hash, then the index among twins (creation order, the last key); the
//      step and the hash walk members in canonical order. The same lines
//      give the same hash; a distinguishable member attached elsewhere gives
//      the same hash; twins swapped give another.
//   5. CONTACT. Two members whose reaches touch exchange along the line
//      between them; a head-on contact hands everything over; the
//      exchange does not depend on which is walked first.
//   6. THE RECORD. The observed spans are inputs: the same spans into a
//      fresh tree land on the same rows. What is KEPT is the rows, as the
//      value behind the Causal tag on the surface: captured off one tree,
//      handed to another built by the same lines, same hash, same clock.
//   7. ONE HISTORY. A reader under the tree's lock sees one state while a
//      driver runs on another thread.
//   8. THE CLOCK. Ticks count crossings; a body with no heat has no time.
//
//   ./Run_CausalTesterLoader
//
#include "../ETCS.h"
#include <atomic>
#include <functional>
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
    // Three members under a room: two movers (twins: the same type and
    // tags) and a drifter that may be made distinguishable by a flag.
    // `order` says which to attach when: 0 and 1 the movers, 2 the drifter.
    auto build = [&](std::vector<int> order = {0, 1, 2}, bool odd_drifter = false) -> Body* {
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
        for (int which : order)
        {
            if (which == 0) make(-3, 0, 0, 0.5,  1, 0, 0, 12, 0.1);    // toward +x: will meet the other
            if (which == 1) make( 3, 0, 0, 0.5, -1, 0, 0, 12, 0.1);    // toward -x
            if (which == 2) { Body* d = make(0, 5, 0, 0.5, 0, 0, 1, 4, 1.0); if (odd_drifter) d->addTag(ETCS::Buffer("odd")); }
        }
        return world;
    };

    // -- 1, 2, 3: the ledger, the arrow, the rows ------------------------------
    {
        std::cout << "\n-- 1-3. the ledger, the arrow, the rows --\n";
        Body* world = build();
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
        std::cout << "\n-- 4. identity and order --\n";
        // THE IDENTITY TUPLE: what it is (the identity hash), then which of
        // the indistinguishable ones (the index among twins, in creation
        // order). The step and the hash walk the members in canonical order:
        // by what they are, then creation order between twins.
        Body* a = build({0, 1, 2}, true);     // movers, then an odd drifter
        Body* b = build({2, 0, 1}, true);     // the odd drifter first: distinguishable, so its place is by what it is
        Body* c = build({1, 0, 2}, true);     // the twins swapped: creation order is the last key, and it moved
        Body* d = build({0, 1, 2}, true);     // the same lines again
        a->Run(1500, dt); b->Run(1500, dt); c->Run(1500, dt); d->Run(1500, dt);
        forward_hash = a->CausalHash();
        check(a->CausalHash() == d->CausalHash(), "the same lines twice: the same CausalHash after 1500 ticks");
        check(a->CausalHash() == b->CausalHash(), "a distinguishable member attached in another place: the same hash -- its place is by what it is");
        check(a->CausalHash() != c->CausalHash(), "twins attached in the other order: a different hash -- creation order is the last key");
        // The identities on the rows: the world's is its own (no twin: index
        // 0); the two movers are one identity hash apart only by the index.
        std::vector<Causal_*> ma, mc;
        for (Causal_* r : *a->causalChildren()) for (Causal_* m : *static_cast<Body*>(r)->causalChildren()) ma.push_back(m);
        for (Causal_* r : *c->causalChildren()) for (Causal_* m : *static_cast<Body*>(r)->causalChildren()) mc.push_back(m);
        bool tuple = ma.size() == 3 && mc.size() == 3
                  && ma[0]->Order4().id != ma[1]->Order4().id                        // twins: different indices
                  && ma[0]->Order4().id == mc[0]->Order4().id                        // the first twin is "the first twin" in both
                  && ma[0]->Order4().x  != mc[0]->Order4().x                         // ...but it is the other body
                  && ma[2]->Order4().id != ma[0]->Order4().id && ma[2]->Order4().id != ma[1]->Order4().id
                  && a->Order4().id != 0 && a->Order4().id != ma[0]->Order4().id;
        check(tuple, "row 0 carries the tuple: twins differ by index, the odd one by hash, and the first twin is whichever came first");
        std::printf("        (hash %016llx)\n", static_cast<unsigned long long>(forward_hash));
        arena.deleteEntity(a, true); arena.deleteEntity(b, true);
        arena.deleteEntity(c, true); arena.deleteEntity(d, true);
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
        std::cout << "\n-- 6. the observed spans are the input; the rows are what is kept --\n";
        Body* live = build();
        uint64_t st = 0x2545f4914f6cdd1dull;
        auto next = [&st]() { st ^= st << 13; st ^= st >> 7; st ^= st << 17; return st; };
        std::vector<std::pair<Fixed, Fixed>> spans;
        for (int i = 0; i < 600; ++i)
        {
            const Fixed commit = Fixed::From(static_cast<double>(next() % 1200 + 1) / 1000.0);   // up to 1.2 s, as an entropy clock would
            const Fixed step   = Fixed::From(static_cast<double>(next() % 100) / 1000.0);        // up to 0.1 s, as a motion clock would; sometimes none
            spans.emplace_back(commit, step);
            live->InteractObserved(commit, step);
        }
        // The property: the same spans into a fresh tree land on the same
        // rows. (No tape in the base -- the rows are the fixed point.)
        Body* again = build();
        for (auto& [c, m] : spans) again->InteractObserved(c, m);
        check(again->CausalHash() == live->CausalHash(), "a fresh tree given the same observed spans lands on the same hash");
        check(again->CausalTicks() == live->CausalTicks(), "...and the same clock");

        // What is kept: the "Causal" value on each entity's surface. Captured
        // off the live tree, handed to a fresh tree built by the same lines
        // (what a Persistence resume does), it hashes the same.
        std::vector<std::string> kept;
        std::function<void(Body*)> capture = [&](Body* b) {
            std::string v; check(b->valueOf(ETCS::Buffer("Causal"), v) && v.size() > 1, "the rows read off the surface as the value behind the Causal tag");
            kept.push_back(v);
            for (Causal_* c : *b->causalChildren()) capture(static_cast<Body*>(c));
        };
        capture(live);
        Body* fresh = build();
        size_t at = 0;
        std::function<void(Body*)> restore = [&](Body* b) {
            check(b->restoreValue(ETCS::Buffer("Causal"), kept[at++]), "...and back onto a fresh entity's surface");
            for (Causal_* c : *b->causalChildren()) restore(static_cast<Body*>(c));
        };
        restore(fresh);
        check(fresh->CausalHash() == live->CausalHash() && fresh->CausalTicks() == live->CausalTicks(),
              "a tree rebuilt by the lines and given the captured values is the live tree: same hash, same clock");
        fresh->Run(100, dt); live->Run(100, dt);
        check(fresh->CausalHash() == live->CausalHash(), "...and goes on the same way");
        check(!fresh->restoreValue(ETCS::Buffer("Causal"), std::string("\x09garbage")), "a value of another version is refused");

        // A verb's state is a stored value, not part of the rows: the
        // emissivity rides the funnel, and a restore reaches the step's
        // working copy through onValue.
        std::string ev;
        check(fresh->valueOf(ETCS::Buffer("emissivity"), ev) && fresh->Emissivity() == Fixed::From(0.2),
              "SetEmissivity is the value behind the emissivity flag");
        std::string w; ETCS::Entity::putWord(w, Fixed::From(0.7).raw);
        check(fresh->restoreValue(ETCS::Buffer("emissivity"), w) && fresh->Emissivity() == Fixed::From(0.7),
              "...a restored value reaches the step's copy (onValue)");
        fresh->removeTag(ETCS::Buffer("emissivity"));
        check(fresh->Emissivity() == Fixed::Half(), "...and the flag leaving takes the default back");
        arena.deleteEntity(live, true);
        arena.deleteEntity(again, true);
        arena.deleteEntity(fresh, true);
    }

    // -- 7: one history --------------------------------------------------------
    {
        std::cout << "\n-- 7. one history under a driver and a reader --\n";
        Body* world = build();
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
