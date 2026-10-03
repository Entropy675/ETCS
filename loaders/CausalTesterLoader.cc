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
//   9. THE BROADPHASE PRUNES ONLY. A crowded container stepped through the
//      kd-tree lands on the rows it lands on pair by pair, to the bit.
//  10. WHAT HOLDS A THING IS WHAT IT FITS IN. A member leaving its
//      container's space moves up, one entering a sibling's moves in, a solid
//      one holds nothing; where it is in the world does not change, nor does
//      the ledger; the lifetime goes with it (deleting where it came from
//      leaves it, deleting where it went takes it).
//
//   ./Run_CausalTesterLoader
//
#include "../ETCS.h"
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
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
    static inline int s_alive = 0;   // section 10: whose cascade took whom
    Body()  { ++s_alive; }
    ~Body() { --s_alive; }

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
        room->SetSpace(Fixed::FromInt(1000));   // the bodies stay in it however far they go
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

    // -- 7b: what a store reads -------------------------------------------------
    {
        std::cout << "\n-- 7b. a frozen read under a driver --\n";
        // A save reads a tree that is moving; etcs_freeze holds it for the
        // copy. Every read, given to a tree built by the same lines (what a
        // resume does), must balance and hash as the read said.
        Body* world = build();
        const Fixed put_in = Fixed::FromInt(12 + 12 + 4);
        std::atomic<bool> stop{false};
        std::thread driver([&]() { while (!stop.load(std::memory_order_acquire)) world->Run(1, dt); });
        int reads = 0, unbalanced = 0, unhashed = 0, unsettled = 0;
        for (int i = 0; i < 200; ++i)
        {
            FrozenTree t;
            if (!etcs_freeze(world, t)) ++unsettled;
            Body* fresh = build();
            FrozenTree ft; etcs_freeze(fresh, ft);
            for (size_t k = 0; k < t.nodes.size() && k < ft.nodes.size(); ++k)
            { ETCS::EnvironmentState st; for (auto& [key, v] : t.nodes[k].kv) st.set(key, v); etcs_restore_values(ft.nodes[k].e, st); }
            Totals tt; totals(fresh, tt);
            if (tt.energy + fresh->EmittedOut() != put_in) ++unbalanced;
            if (fresh->getHash() != t.state_hash) ++unhashed;
            ++reads;
            arena.deleteEntity(fresh, true);
        }
        stop.store(true, std::memory_order_release);
        driver.join();
        // At rest between two reads, the rows' digests stand still: nothing is
        // read again. A step moves some of them: those are.
        FrozenTree a, b, c;
        etcs_freeze(world, a);
        etcs_freeze(world, b, {}, 4, &a);
        world->Run(5, dt);
        etcs_freeze(world, c, {}, 4, &b);
        check(b.copied == 0 && c.copied > 0 && c.copied <= c.nodes.size(),
              "the rows' digest: a tree at rest is not read again, a stepped one is read where it moved");
        check(reads == 200 && unbalanced == 0, "every frozen read of a driven tree is one tick: its energy balances to the bit");
        check(unhashed == 0, "...and a tree given its values hashes as the read said");
        check(unsettled == 0, "...and each read settled");
        std::printf("        (%d reads, %d unbalanced, %d unhashed, ticks %llu)\n", reads, unbalanced, unhashed,
                    static_cast<unsigned long long>(world->CausalTicks()));
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

    // -- 9: the broadphase ----------------------------------------------------
    {
        std::cout << "\n-- 9. the broadphase prunes, the gate decides --\n";
        // A crowd: members packed so contacts are many, a third of them pushed.
        // The same lines stepped once through the kd-tree and once pair by
        // pair (BroadphaseFrom out of reach) must land on the same rows.
        auto crowd = [&](int n, double side, int ticks, size_t from, double* ms) -> uint64_t {
            CausalBase<Body>::BroadphaseFrom() = from;
            Body* world = arena.allocate<Body>();
            world->SetEmissivity(Fixed::From(0.2));
            uint64_t st = 0x2545f4914f6cdd1dull ^ static_cast<uint64_t>(n);
            auto next = [&st]() { st ^= st << 13; st ^= st >> 7; st ^= st << 17; return st; };
            auto unit = [&]() { return static_cast<double>(next() % 1000000) / 1000000.0; };
            std::vector<Body*> still;
            for (int i = 0; i < n; ++i)
            {
                Body* b = world->addTag<Body>();
                b->Place((unit() * 2 - 1) * side, 0, (unit() * 2 - 1) * side);
                b->Reach(0.4 + unit() * 0.6);
                b->damping = Fixed::From(0.2);
                if (i % 3 == 0) b->Impulse(Fixed::From(unit() * 2 - 1), Fixed::Zero(), Fixed::From(unit() * 2 - 1), Fixed::FromInt(4 + static_cast<int64_t>(i % 5)));
                else            still.push_back(b);
            }
            const auto t0 = std::chrono::steady_clock::now();
            world->Run(static_cast<uint32_t>(ticks), dt);
            if (ms) *ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / ticks;
            bool pushed = false;   // a body never pushed that moves was moved by a contact
            for (Body* b : still) if (b->Order4().energy.IsPositive()) pushed = true;
            const uint64_t h = pushed ? world->CausalHash() : 0;
            arena.deleteEntity(world, true);
            return h;
        };
        const size_t kFrom = CausalBase<Body>::BroadphaseFrom();
        const uint64_t kd = crowd(400, 16, 400, 2, nullptr);
        const uint64_t nn = crowd(400, 16, 400, SIZE_MAX, nullptr);
        check(kd != 0 && nn != 0, "400 members in a crowd: contacts moved bodies nothing pushed");
        check(kd == nn, "...and the kd-tree's pairs land on the same rows as every pair, to the bit");
        const uint64_t kd2 = crowd(1000, 40, 200, 2, nullptr);
        const uint64_t nn2 = crowd(1000, 40, 200, SIZE_MAX, nullptr);
        check(kd2 != 0 && kd2 == nn2, "...and 1,000 sparser ones");
        std::printf("        (ms per tick, kd-tree / every pair:");
        for (int n : {8, 16, 24, 32, 48, 64, 128, 1000})
        {
            double a = 0, b = 0;
            crowd(n, std::sqrt(static_cast<double>(n)) * 1.2, n >= 1000 ? 50 : 2000, 2, &a);
            crowd(n, std::sqrt(static_cast<double>(n)) * 1.2, n >= 1000 ? 50 : 2000, SIZE_MAX, &b);
            std::printf(" n=%d %.4f/%.4f", n, a, b);
        }
        std::printf(")\n");
        CausalBase<Body>::BroadphaseFrom() = kFrom;
    }

    // -- 10: fitness ----------------------------------------------------------
    {
        std::cout << "\n-- 10. what holds a thing is what it fits in --\n";
        auto where = [](Body* b, Fixed& x, Fixed& y, Fixed& z) {   // its place in the world
            b->Basis(x, y, z);
            x += b->Order4().x; y += b->Order4().y; z += b->Order4().z;
        };
        // A pen in a pocket in a person in a world, the pen heading out.
        struct Scene { Body *world, *person, *pocket, *pen, *box, *ball, *lump, *chip, *far, *far2; };
        auto scene = [&]() {
            Scene s{};
            s.world  = arena.allocate<Body>();                s.world->SetSpace(Fixed::FromInt(1000));
            s.person = s.world->addTag<Body>();               s.person->SetSpace(Fixed::FromInt(10));
            s.pocket = s.person->addTag<Body>();              s.pocket->Place(2, 0, 0); s.pocket->SetSpace(Fixed::One());
            s.pen    = s.pocket->addTag<Body>();              s.pen->Place(0.5, 0, 0); s.pen->Reach(0.1); s.pen->damping = Fixed::Zero();
            s.pen->Impulse(Fixed::One(), Fixed::Zero(), Fixed::Zero(), Fixed::FromInt(2));
            // A hollow box, and a ball rolling into it (a point reach: no contact first).
            s.box    = s.world->addTag<Body>();               s.box->Place(20, 0, 0); s.box->SetSpace(Fixed::FromInt(3));
            s.ball   = s.world->addTag<Body>();               s.ball->Place(10, 0, 0); s.ball->Reach(0.2); s.ball->damping = Fixed::Zero();
            s.ball->Impulse(Fixed::One(), Fixed::Zero(), Fixed::Zero(), Fixed::FromInt(8));
            // Something solid with something in it; something past the world's space.
            s.lump   = s.world->addTag<Body>();               s.lump->Place(-20, 0, 0);
            s.chip   = s.lump->addTag<Body>();                s.chip->Place(0.1, 0, 0);
            s.far    = s.world->addTag<Body>();               s.far->Place(5000, 0, 0); s.far->Reach(1);
            s.far2   = s.world->addTag<Body>();               s.far2->Place(5001.5, 0, 0); s.far2->Reach(1);
            return s;
        };
        Scene a = scene();
        Fixed cx, cy, cz; where(a.chip, cx, cy, cz);
        const Fixed put_in = Fixed::FromInt(2 + 8);
        a.world->Interact(dt);
        Fixed cx2, cy2, cz2; where(a.chip, cx2, cy2, cz2);
        check(a.chip->getParent() == a.world && cx2 == cx && cy2 == cy && cz2 == cz,
              "a solid thing holds nothing: its member moved up, to the same place in the world");
        check(a.far->getParent() == a.world, "the open boundary keeps what fits nowhere");
        // Each move as it happens: the pen out of the pocket into the person,
        // then out of the person into the world; the ball into the box, then
        // out through the far side.
        int pen_out = -1, pen_up = -1, ball_in = -1, ball_out = -1;
        ETCS::Entity* pen_first = nullptr;
        bool ball_in_frame = false, ledger = true;
        for (int i = 0; i < 400; ++i)
        {
            a.world->Interact(dt);
            Totals t; totals(a.world, t);
            if (t.energy + a.world->EmittedOut() != put_in) ledger = false;
            if (pen_out < 0 && a.pen->getParent() != a.pocket) { pen_out = i; pen_first = a.pen->getParent(); }
            if (pen_out >= 0 && pen_up < 0 && a.pen->getParent() == a.world) pen_up = i;
            if (ball_in < 0 && a.ball->getParent() == a.box) { ball_in = i; ball_in_frame = a.ball->Order4().x.ToDouble() < 0; }
            if (ball_in >= 0 && ball_out < 0 && a.ball->getParent() == a.world) ball_out = i;
        }
        check(pen_out > 0 && pen_first == a.person && pen_up > pen_out,
              "the pen left the pocket's space into the person, then the person's into the world");
        check(ball_in > 0 && ball_in_frame && ball_out > ball_in,
              "the ball rolled into the box's space -- its position now the box's frame -- and out the far side");
        check(ledger, "...and no move changed the ledger: tree plus boundary is what was put in, every tick");
        std::printf("        (pen: out at tick %d, up at %d; ball: in at %d, out at %d)\n", pen_out, pen_up, ball_in, ball_out);
        // Contain keeps where a thing is, exactly; it will not put a thing inside itself.
        Fixed px, py, pz; where(a.pen, px, py, pz);
        check(a.box->Contain(a.pen), "Contain moves a member in from another environment");
        Fixed qx, qy, qz; where(a.pen, qx, qy, qz);
        check(px == qx && py == qy && pz == qz, "...and it is where it was in the world, to the bit");
        check(!a.pocket->Contain(a.person), "nothing goes inside what is inside it");
        std::vector<Causal_*> nearby; a.world->Near(Fixed::FromInt(20), Fixed::Zero(), Fixed::Zero(), Fixed::One(), nearby);
        std::vector<Causal_*> adj; a.far->Adjacent(adj);
        check(adj.size() == 1 && adj[0] == static_cast<Causal_*>(a.far2), "Adjacent: what shares its environment and touches its reach");
        check(nearby.size() == 1 && nearby[0] == static_cast<Causal_*>(a.box), "Near: the members within reach of a point of the frame");
        // The same lines again: the same tree, the same places.
        Scene b = scene();
        for (int i = 0; i < 401; ++i) b.world->Interact(dt);
        b.box->Contain(b.pen);
        check(b.world->CausalHash() == a.world->CausalHash() && b.pen->getParent() == b.box && b.ball->getParent() == b.world,
              "the same lines twice: the same moves, the same hash");
        // The lifetime went with it.
        const int before = Body::s_alive;
        a.pocket->getOwningArena().deleteEntity(a.pocket, true);
        check(Body::s_alive == before - 1 && a.pen->Order4().radius == Fixed::From(0.1),
              "deleting where the pen came from leaves the pen");
        const int before2 = Body::s_alive;
        a.box->getOwningArena().deleteEntity(a.box, true);
        check(Body::s_alive == before2 - 2, "deleting where it went takes it with it");
        arena.deleteEntity(a.world, true);
        arena.deleteEntity(b.world, true);
    }

    std::printf("\n%s (%d failure%s)\n", g_fail ? "FAILED" : "PASSED", g_fail, g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}
