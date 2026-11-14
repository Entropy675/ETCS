#ifndef BASE_Causal_H__
#define BASE_Causal_H__
#include "Causal.h"
#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>
#include "../libs/nanoflann.h"   // the contacts' broadphase

/*
 * THE ROWS LIVE HERE, and so does everything that is true of them whatever
 * the leaf is: the commit of entropy into the container, the step, the
 * contacts among the members, the last crossing, the driver, the hash. A
 * leaf says what its own step IS -- what pushes it (a held key, an engine, a
 * slope) and what slows it -- and nothing else, because that is the only
 * part that differs between a box and a kart.
 *
 * THE ORDER OF AN INTERACTION IS THE LAZY-COMMIT CONTRACT. Emission is
 * charged for the interval that just ENDED, over a heat total nothing touched
 * during it; the step then adds the heat the NEXT interval will be charged
 * for. The other way round bills every commit for heat that had not happened
 * yet when the interval started. So: commit, then step, then the members --
 * a member's commit lands in this entity's rows AFTER this entity settled its
 * own interval, which is the interval the member's heat belongs to -- then
 * the contacts among the members, from the rows the step left, so a contact
 * is between two bodies at the same instant.
 *
 * ONE CROSSING, TWO ADJACENCIES (Causal.h). What a member sheds goes up into
 * its container through crossTo; what two touching members exchange goes
 * sideways through the same call. No parent, or a parent that is not Causal,
 * is the edge of the model, and the heat is counted out rather than lost.
 *
 * THE CONSTRAINTS THIS FILE UPHOLDS are listed with their checks in
 * ontology/etcs_causal_constraints.md; loaders/CausalTesterLoader.cc runs
 * them against a tree of plain bodies.
 */
ETCS_SUPERTYPE_BASE(Causal)
{
    ETCS_MAKE_INSTANCE(Causal)

    /*
     * THE ROWS ARE ON THE TAG SURFACE, behind the family's own tag: the
     * value of "Causal" is this entity's rows, clock, emissivity and what it
     * has shed at the boundary, read when the surface is captured and handed
     * back when it is restored (Entity::bindValue). Bound, not written --
     * the rows move on every interaction and the funnel is an ordered
     * event; what the surface holds is the way to read them. This is the
     * one place the physics is kept: a Persistence under the root stores
     * it, and a resume puts it back, with no store of the family's own.
     */
    // (The family's constructor is the macro's; the binding is made by a
    // member built after the Entity base is, which is all it needs.)

    // ── the leaf's answers ──────────────────────────────────────────────

    // The step: apply whatever pushes and slows this thing over dt, and
    // advance the rows (OrderVector::Advance). The base has already
    // committed the entropy owed.
    virtual void AdvanceConcrete(Fixed dt) = 0;

    // The body's mass, for the rate row's reading as a speed. One, unless a
    // leaf says otherwise.
    virtual Fixed MassConcrete() const { return Fixed::One(); }

    // What a solid of this leaf is shaped as until SetShape says (a box for a
    // leaf with extents), and its half extents. The base's default is the
    // reach: a sphere, or the cube about it.
    virtual CausalSolid::Shape DefaultShapeConcrete() const { return CausalSolid::Sphere; }
    virtual void ExtentConcrete(Fixed& hx, Fixed& hy, Fixed& hz) const { hx = hy = hz = m_ov.radius; }

    // ── the family ──────────────────────────────────────────────────────

    const OrderVector& Order4() const override final { return m_ov; }
    OrderVector&       Rows()                        { return m_ov; }

    void Interact(Fixed dt) override final
    {
        if (!dt.IsPositive()) return;
        ::std::lock_guard<::std::recursive_mutex> lk(TreeMutex());
        InteractUnder(dt, dt);
    }

    /*
     * THE OBSERVED INTERACTION. An observer measures its two intervals
     * (entropy and motion, with different ceilings -- StepClock.h) and hands
     * them here as the Fixed values the rows will see: those two numbers
     * are the whole of what the wall clock contributed, and the rows after
     * it are their fixed point -- which is what the surface keeps (the
     * "Causal" value), not the spans. Taken at the observed root only: the
     * hop under it steps the members with the same spans.
     */
    void InteractObserved(Fixed commit_dt, Fixed step_dt)
    {
        if (!commit_dt.IsPositive() && !step_dt.IsPositive()) return;
        ::std::lock_guard<::std::recursive_mutex> lk(TreeMutex());
        InteractUnder(commit_dt, step_dt);
    }

    // The driver: `ticks` interactions of `dt` each, no clock read. What a
    // headless run does instead of being looked at.
    void Run(uint32_t ticks, Fixed dt)
    {
        if (!dt.IsPositive()) return;
        ::std::lock_guard<::std::recursive_mutex> lk(TreeMutex());
        for (uint32_t i = 0; i < ticks; ++i) InteractUnder(dt, dt);
    }

    void InteractUnder(Fixed commit_dt, Fixed step_dt) override final
    {
        refreshIdentityLocked();
        OrderVector e;
        if (commitLocked(commit_dt, e)) crossTo(container(), e);
        if (step_dt.IsPositive())
        {
            fieldLocked(step_dt);
            static_cast<Derived*>(this)->AdvanceConcrete(step_dt);
        }
        const auto& now = kidsLocked();
        bool moving = m_ov.KineticEnergy().IsPositive();
        if (now->empty()) { m_moving.store(moving, ::std::memory_order_relaxed); return; }   // a leaf: nothing below to step, touch or fit
        // The tree lock is held: the list is not copied, only kept -- a move
        // below changes the membership, and the list in hand must outlive it.
        const auto held = now;
        const auto& kids = *held;
        for (Causal_* c : kids) c->InteractUnder(commit_dt, step_dt);
        contactsLocked(kids, step_dt.IsPositive() ? step_dt : commit_dt);
        fitLocked(kids);
        // After the contacts: a body they brought to rest is not moving. A
        // member fit moved elsewhere is its new container's to report.
        for (Causal_* c : kids) moving = moving || c->Order4().KineticEnergy().IsPositive() || c->Moving();
        m_moving.store(moving, ::std::memory_order_relaxed);
    }

    bool Moving() const override final { return m_moving.load(::std::memory_order_relaxed); }

    /*
     * ONE LOCK PER TREE. The mutex is the topmost Causal entity's; every
     * public entry here takes it, and a leaf's own writers take it too
     * (Scene3D's verbs and its observed step). Recursive, because a member's
     * crossing is absorbed by its container inside the same interaction, and
     * a leaf calls family verbs from under it. Found by walking the
     * containers once per entry -- a Run of ten thousand ticks walks once.
     */
    ::std::recursive_mutex& TreeMutex() override final
    {
        Causal_* top = this;
        for (Causal_* env = container(); env; env = containerOf(env)) top = env;
        return top == this ? m_tree_mtx : top->TreeMutex();
    }

    void Impulse(Fixed dx, Fixed dy, Fixed dz, Fixed joules) override final
    {
        ::std::lock_guard<::std::recursive_mutex> lk(TreeMutex());
        m_ov.Impulse(dx, dy, dz, joules);
    }

    void Absorb(const OrderVector& crossing) override final
    {
        ::std::lock_guard<::std::recursive_mutex> lk(TreeMutex());
        m_ov.Absorb(crossing);
    }
    void AbsorbUnder(const OrderVector& crossing) override final { m_ov.Absorb(crossing); }

    OrderVector CrossTowardUnder(Fixed nx, Fixed ny, Fixed nz, Fixed span) override final
    {
        OrderVector e = m_ov.CrossToward(nx, ny, nz, span);
        if (e.energy.IsPositive()) noteCrossing(e);
        return e;
    }

    void PlaceUnder(Fixed x, Fixed y, Fixed z) override final { m_ov.PlaceAt(x, y, z); }
    OrderVector& RowsUnder() override final { return m_ov; }
    Fixed MassUnder() override final { return static_cast<Derived*>(this)->MassConcrete(); }
    void SupportUnder(bool resting, Fixed nx, Fixed ny, Fixed nz) override final
    {
        m_resting = resting; m_snx = nx; m_sny = ny; m_snz = nz;
    }

    uint64_t CausalTicks() const override final { return m_ticks; }

    // The members in canonical order (kidsLocked): the number depends on
    // what they are, where their rows stand, and the order they stand in --
    // which is the order the step walks them. Distinguishable members order
    // by what they are; twins by creation order, the last key.
    uint64_t CausalHash() override final
    {
        ::std::lock_guard<::std::recursive_mutex> lk(TreeMutex());   // a whole state, not one mid-step
        refreshIdentityLocked();
        uint64_t h = fixed_mix(m_ov.Hash(), static_cast<int64_t>(m_ticks));
        for (Causal_* c : *kidsLocked()) h = fixed_mix(h, static_cast<int64_t>(c->CausalHash()));
        return h;
    }

    // ── the environment (Causal.h) ──────────────────────────────────────

    // A Causal thing changes parents as it changes what it fits in, so the
    // family's leaves are made movable (core/Entity.h, etcs_movable).
    static constexpr bool kEtcsMovable = true;

    Causal_* Environment() override final
    {
        ::std::lock_guard<::std::recursive_mutex> lk(TreeMutex());
        return container();
    }

    void Basis(Fixed& x, Fixed& y, Fixed& z) override final
    {
        ::std::lock_guard<::std::recursive_mutex> lk(TreeMutex());   // one whole chain, not a mid-step one
        x = y = z = Fixed::Zero();
        for (Causal_* env = container(); env; env = containerOf(env))
        {
            const OrderVector& o = env->Order4();
            x += o.x; y += o.y; z += o.z;
        }
    }

    // The space this provides: a verb's state, so the value behind "space"
    // (through the funnel, recorded and kept); m_space is the step's copy.
    void SetSpace(Fixed radius)
    {
        if (radius.raw < 0) return;
        ::std::string v;
        ETCS::Entity::putWord(v, radius.raw);
        this->addTag("space", v);
    }
    Fixed Space() const override final { return m_space; }

    /*
     * THE FIELD THIS SPACE HAS, stated (Causal.h, "the space's parameters"): a
     * verb's state, so the value behind "gravity" (through the funnel,
     * recorded and kept). Stating it -- zero included -- is what makes it this
     * space's own; InheritGravity takes the statement back, and the space
     * passes on its container's field again.
     */
    void SetGravity(Fixed gx, Fixed gy, Fixed gz)
    {
        ::std::string v;
        ETCS::Entity::putWord(v, gx.raw); ETCS::Entity::putWord(v, gy.raw); ETCS::Entity::putWord(v, gz.raw);
        this->addTag("gravity", v);
    }
    void InheritGravity() { this->removeTag(ETCS::Buffer("gravity")); }
    // The work the field this space states has done on what is inside it:
    // the energy it put in, net of what climbing bodies gave back.
    Fixed FieldWork() const { return m_field_work; }

    Causal_* FieldUnder(Fixed& gx, Fixed& gy, Fixed& gz) override final
    {
        if (m_gravity_set) { gx = m_gx; gy = m_gy; gz = m_gz; return this; }
        Causal_* up = container();
        if (up) return up->FieldUnder(gx, gy, gz);
        gx = gy = gz = Fixed::Zero();
        return nullptr;
    }
    void CountFieldWorkUnder(Fixed joules) override final { m_field_work += joules; }

    /*
     * MADE SOLID (CausalSolid): restitution and friction in [0,1]; the value
     * behind "solid", with the shape (SetShape) beside them. Only a pair of
     * solids meets by shape; anything else touches the way it always has.
     */
    void SetSolid(Fixed restitution, Fixed friction)
    {
        auto unit = [](Fixed f) { return f.raw < 0 ? Fixed::Zero() : (f > Fixed::One() ? Fixed::One() : f); };
        putSolid(m_shape_set ? m_shape : static_cast<Derived*>(this)->DefaultShapeConcrete(), unit(restitution), unit(friction));
    }
    // A shape outright, solid from then on (Off: not solid any more).
    void SetShape(CausalSolid::Shape shape)
    {
        if (shape == CausalSolid::Off) { this->removeTag(ETCS::Buffer("solid")); return; }
        putSolid(shape, m_solid.restitution, m_solid.friction);
    }
    /*
     * ANCHORED: held where it is in its container -- the field moves it not,
     * a contact moves it not, motion handed to it becomes heat. What a place
     * is, as against a thing: a green, a wall, a cup. A container that is not
     * anchored is a thing, and falls in its own container's field carrying
     * what is inside it (frames are translations); its members feel the field
     * in that frame too. The value behind "anchored".
     */
    void SetAnchored(bool on)
    {
        if (!on) { this->removeTag(ETCS::Buffer("anchored")); return; }
        ::std::string v;
        ETCS::Entity::putWord(v, 1);
        this->addTag("anchored", v);
    }
    bool Anchored() const { return m_anchored; }
    CausalSolid Solid() override final
    {
        CausalSolid c = m_solid;
        c.anchored = m_anchored;
        if (c.shape == CausalSolid::Box) static_cast<Derived*>(this)->ExtentConcrete(c.hx, c.hy, c.hz);
        return c;
    }

    void Near(Fixed x, Fixed y, Fixed z, Fixed r, ::std::vector<Causal_*>& out) override final
    {
        ::std::lock_guard<::std::recursive_mutex> lk(TreeMutex());
        OrderVector probe;
        probe.PlaceAt(x, y, z);
        probe.radius = r;
        for (Causal_* c : *kidsLocked()) if (probe.MayInteractWith(c->Order4())) out.push_back(c);
    }

    void Adjacent(::std::vector<Causal_*>& out) override final
    {
        ::std::lock_guard<::std::recursive_mutex> lk(TreeMutex());
        Causal_* env = container();
        if (!env) return;
        const size_t from = out.size();
        env->Near(m_ov.x, m_ov.y, m_ov.z, m_ov.radius, out);
        out.erase(::std::remove(out.begin() + static_cast<ptrdiff_t>(from), out.end(), static_cast<Causal_*>(this)), out.end());
    }

    /*
     * A MOVE: the member changes parents (Entity::moveTo -- the funnel's
     * event, its own recorded action) and its position is restated in this
     * frame, so where it is in the world does not change. Under both trees'
     * locks when it comes from another tree (std::lock: no order to agree
     * on), so no step sees it half moved. Its other rows are not restated:
     * frames are translations and carry no motion of their own.
     */
    bool Contain(Causal_* member) override final
    {
        if (!member || member == static_cast<Causal_*>(this)) return false;
        ::std::recursive_mutex& mine   = TreeMutex();
        ::std::recursive_mutex& theirs = member->TreeMutex();
        ::std::unique_lock<::std::recursive_mutex> a(mine, ::std::defer_lock), b(theirs, ::std::defer_lock);
        if (&mine == &theirs) a.lock(); else ::std::lock(a, b);
        Fixed mx, my, mz, tx, ty, tz;
        member->Basis(mx, my, mz);
        Basis(tx, ty, tz);
        const OrderVector& m = member->Order4();
        const Fixed nx = mx + m.x - (tx + m_ov.x);
        const Fixed ny = my + m.y - (ty + m_ov.y);
        const Fixed nz = mz + m.z - (tz + m_ov.z);
        if (!member->moveTo(this)) return false;
        member->PlaceUnder(nx, ny, nz);
        return true;
    }

    // ── what the base keeps beside the rows ─────────────────────────────

    // How fast heat leaves this thing into its container, per second. Zero
    // is a perfect insulator, a legitimate thing to be. A verb's state, so
    // the value behind "emissivity" (through the funnel); m_emissivity is the
    // step's working copy, refreshed in onValue.
    void SetEmissivity(Fixed per_sec)
    {
        if (per_sec.raw < 0) return;
        ::std::string v;
        ETCS::Entity::putWord(v, per_sec.raw);
        this->addTag("emissivity", v);
    }
    Fixed Emissivity() const { return m_emissivity; }

    void onValue(const ETCS::Buffer& key, const ::std::string* value) override
    {
        if (key == ETCS::Buffer("emissivity"))
        {
            int64_t raw = Fixed::Half().raw;   // gone with its flag: the default again
            size_t at = 0;
            if (value && !ETCS::Entity::getWord(*value, at, raw)) return;
            ::std::lock_guard<::std::recursive_mutex> lk(TreeMutex());
            m_emissivity = Fixed::FromRaw(raw);
        }
        else if (key == ETCS::Buffer("gravity"))
        {
            int64_t w[3] = { 0, 0, 0 };
            size_t at = 0;
            const bool set = value != nullptr;   // gone with its flag: inherited again
            if (set) for (int64_t& x : w) if (!ETCS::Entity::getWord(*value, at, x)) return;
            ::std::lock_guard<::std::recursive_mutex> lk(TreeMutex());
            m_gravity_set = set;
            m_gx = Fixed::FromRaw(w[0]); m_gy = Fixed::FromRaw(w[1]); m_gz = Fixed::FromRaw(w[2]);
        }
        else if (key == ETCS::Buffer("solid"))
        {
            int64_t w[3] = { 0, 0, 0 };
            size_t at = 0;
            if (value) for (int64_t& x : w) if (!ETCS::Entity::getWord(*value, at, x)) return;
            ::std::lock_guard<::std::recursive_mutex> lk(TreeMutex());
            m_solid.shape       = value ? static_cast<CausalSolid::Shape>(w[0] & 3) : CausalSolid::Off;   // gone: not solid
            m_shape_set         = value != nullptr;
            m_shape             = m_solid.shape;
            m_solid.restitution = Fixed::FromRaw(w[1]);
            m_solid.friction    = Fixed::FromRaw(w[2]);
            if (!value) m_resting = false;
        }
        else if (key == ETCS::Buffer("anchored"))
        {
            ::std::lock_guard<::std::recursive_mutex> lk(TreeMutex());
            m_anchored = value != nullptr;
        }
        else if (key == ETCS::Buffer("space"))
        {
            int64_t raw = 0;   // gone with its flag: solid again
            size_t at = 0;
            if (value && !ETCS::Entity::getWord(*value, at, raw)) return;
            ::std::lock_guard<::std::recursive_mutex> lk(TreeMutex());
            m_space = Fixed::FromRaw(raw);
        }
    }

    // Energy that left the model at this entity (it had no Causal container).
    Fixed EmittedOut() const { return m_emitted_out; }

    // The most recent crossing this entity made, as the OrderVector it is:
    // what a guard nesting a draw inside this entity's emission reads.
    const OrderVector& LastEmission() const { return m_last_emission; }

    /*
     * Commit every joule owed since the last interaction, into the container,
     * on its own -- a verb's use; an interaction does it first. The tick
     * counts crossings that HAPPENED: a thing with no heat to shed has had no
     * time pass for it.
     */
    void CommitEntropy(Fixed dt)
    {
        if (!dt.IsPositive()) return;
        ::std::lock_guard<::std::recursive_mutex> lk(TreeMutex());
        refreshIdentityLocked();
        OrderVector e;
        if (commitLocked(dt, e)) crossTo(container(), e);
    }

    /*
     * The Causal members, in canonical order -- any provider's. KEPT against
     * this entity's hash epoch: the typed-child lists and the children's own
     * hashes move only through the funnels that bump it (Entity::
     * markStateChange, on every ancestor of a change), so a driver taking
     * thousands of interactions a second rebuilds the list once per change
     * in the subtree rather than once per tick.
     *
     * A SNAPSHOT GOES OUT, shared: the walker iterates a list nobody will
     * rebuild under it, and takes it with one reference count rather than a
     * copy.
     */
    ::std::shared_ptr<const ::std::vector<Causal_*>> causalChildren()
    {
        ::std::lock_guard<::std::recursive_mutex> lk(TreeMutex());
        return kidsLocked();
    }

    // The member count from which the contacts ask the kd-tree rather than
    // every pair (contactsLocked). Per leaf type; a test sets it out of reach
    // to step the same tree pair by pair and compare.
    static size_t& BroadphaseFrom() { static size_t n = 16; return n; }   // measured: the tree wins from ~16 (CausalTester §9)

private:
    /*
     * CANONICAL ORDER: tags in first-attachment order (that order is state;
     * the merkle hash composes by it), within a tag by the member's identity
     * hash -- what it is -- and between twins by attach order: creation
     * order, the last key, the same one the identity tuple and a persistence
     * record end on. Never the hash map's order and never the RID's. The
     * same script in another runtime walks its members the same way; a
     * scene rebuilt with its distinguishable members in another order walks
     * them the same way too; twins swapped are twins swapped. The same order
     * the hash composes in and the contacts pair in.
     */
    const ::std::shared_ptr<const ::std::vector<Causal_*>>& kidsLocked()
    {
        const uint32_t epoch = this->hashEpoch();
        if (m_kids && m_kids_epoch == epoch) return m_kids;
        auto fresh = ::std::make_shared<::std::vector<Causal_*>>();
        ::std::vector<ETCS::Entity::ChildRef> kids;
        this->getTypedChildRefs(kids);
        struct Member { const ETCS::Buffer* tag; uint64_t hash; Causal_* c; };   // a tag is its key's address
        ::std::vector<Member> members;
        for (const auto& entry : kids)
        {
            ETCS::Entity* child = this->getTypedChild(*entry.first, entry.second);
            if (!child) continue;
            if (void* c = child->getInterfacePointer(causalKey()))
                members.push_back(Member{ entry.first, child->identityHash(), static_cast<Causal_*>(c) });
        }
        for (auto lo = members.begin(); lo != members.end(); )
        {
            auto hi = lo + 1;
            while (hi != members.end() && hi->tag == lo->tag) ++hi;
            ::std::stable_sort(lo, hi, [](const Member& a, const Member& b) { return a.hash < b.hash; });
            lo = hi;
        }
        for (const Member& m : members) fresh->push_back(m.c);
        m_kids       = ::std::move(fresh);
        m_kids_epoch = epoch;
        return m_kids;
    }

    // Row 0's identity is this entity's identity tuple (OrderVector.h): the
    // identity hash, and where it stands among its twins (Entity::
    // siblingIndex) -- what it is, then which of the indistinguishable ones.
    // Refreshed when the hash epoch says the surface moved, which is the only
    // time either can differ (a twin arriving or leaving bumps the parent and
    // every ancestor, not this entity -- so the parent's epoch is read too).
    // Both halves are cached on their side, so this is two loads and two
    // compares per interaction.
    void refreshIdentityLocked()
    {
        const uint32_t epoch = this->hashEpoch();
        ETCS::Entity* p = liveParent(this);
        const uint32_t pepoch = p ? p->hashEpoch() : 0;
        if (m_id_epoch == epoch && m_id_pepoch == pepoch) return;
        m_ov.id     = fixed_mix(this->identityHash(), static_cast<int64_t>(this->siblingIndex()));
        m_id_epoch  = epoch;
        m_id_pepoch = pepoch;
    }

    // The emission owed for dt, taken out of the rows. Under the tree lock.
    bool commitLocked(Fixed dt, OrderVector& e)
    {
        if (!dt.IsPositive()) return false;
        if (dt != m_share_dt || m_emissivity != m_share_k)
        {
            m_share_dt = dt; m_share_k = m_emissivity;
            m_share = OrderVector::EmissionShare(dt, m_emissivity);
        }
        e = m_ov.CommitShare(m_share, dt);
        if (!e.energy.IsPositive()) return false;
        noteCrossing(e);
        return true;
    }

    // A crossing this entity made: its clock, its last emission, the step's
    // meta on its own rows.
    void noteCrossing(const OrderVector& e)
    {
        m_last_emission  = e;
        m_ov.interval    = e.interval;
        m_ov.uncertainty = e.uncertainty;
        ++m_ticks;
    }

    // THE ONE CROSSING: into an adjacent entity's rows (the tree's lock is
    // held), or counted out at the open boundary when there is none.
    void crossTo(Causal_* to, const OrderVector& e)
    {
        if (to) to->AbsorbUnder(e);
        else    m_emitted_out += e.energy;
    }

    /*
     * THE CONTACTS AMONG THE MEMBERS: every pair whose reaches touch, in the
     * container's frame, from the rows the step left. Both crossings are
     * taken before either lands, so the exchange is simultaneous and does
     * not depend on which of the pair is walked first.
     *
     * THE ORDER OF THE PAIRS IS PART OF THE RESULT: a pair's crossings change
     * energy rows the later pairs read. Positions and reaches do not move in
     * the pass (CrossToward and Absorb touch energy and the kinetic row only),
     * so which pairs may touch is fixed before it starts. A small container
     * asks every pair; a large one asks a kd-tree for a superset (Broadphase)
     * and the same gate decides -- the same pairs, in the same (i, j) order,
     * the same rows to the bit.
     */
    void contactsLocked(const ::std::vector<Causal_*>& kids, Fixed span)
    {
        const size_t n = kids.size();
        // The solids, read once a pass. None (every scene before solids): the
        // pass is exactly what it was.
        m_solids.resize(n);
        bool any = false, plane = false;
        for (size_t i = 0; i < n; ++i)
        {
            m_solids[i] = kids[i]->Solid();
            any   |= m_solids[i].shape != CausalSolid::Off;
            plane |= m_solids[i].shape == CausalSolid::HalfSpace;
        }
        m_solid_pairs.clear();
        auto pair = [&](size_t i, size_t j) {
            if (contactLocked(kids[i], kids[j], any ? &m_solids[i] : nullptr, any ? &m_solids[j] : nullptr, span))
                m_solid_pairs.emplace_back(static_cast<uint32_t>(i), static_cast<uint32_t>(j));
        };
        if (n < BroadphaseFrom())
        {
            for (size_t i = 0; i + 1 < n; ++i)
                for (size_t j = i + 1; j < n; ++j) pair(i, j);
        }
        else
        {
            if (!m_broad) m_broad.reset(new Broadphase);
            const auto& found = m_broad->pairs(kids);
            if (!plane) for (const auto& [i, j] : found) pair(i, j);
            else
            {
                // A half-space has no reach to find it by: it meets every
                // solid, merged into the tree's pairs in the (i, j) order the
                // pair-by-pair walk takes.
                ::std::vector<::std::pair<uint32_t, uint32_t>> all(found.begin(), found.end());
                for (size_t h = 0; h < n; ++h)
                    if (m_solids[h].shape == CausalSolid::HalfSpace)
                        for (size_t k = 0; k < n; ++k)
                            if (k != h && m_solids[k].shape != CausalSolid::Off)
                                all.emplace_back(static_cast<uint32_t>(h < k ? h : k), static_cast<uint32_t>(h < k ? k : h));
                ::std::sort(all.begin(), all.end());
                all.erase(::std::unique(all.begin(), all.end()), all.end());
                for (const auto& [i, j] : all) pair(i, j);
            }
        }
        if (!m_solid_pairs.empty() || any) solidPassLocked(kids, span);
    }

    // True when the pair is two solids: they meet by shape (solidPassLocked),
    // after every transmission of the pass, so which pairs meet is decided
    // from the rows the pass started with on both walks.
    static bool contactLocked(Causal_* ka, Causal_* kb, const CausalSolid* sa, const CausalSolid* sb, Fixed span)
    {
        const OrderVector& a = ka->Order4();
        const OrderVector& b = kb->Order4();
        if (sa && sb)
        {
            const bool solidA = sa->shape != CausalSolid::Off, solidB = sb->shape != CausalSolid::Off;
            if (solidA && solidB)
                return sa->shape == CausalSolid::HalfSpace || sb->shape == CausalSolid::HalfSpace || a.MayInteractWith(b);
            // A solid meets only solids: what is not solid passes through it
            // (a marker, an arrow drawn in the scene), and the two contact
            // models never meet in one pair.
            if (solidA || solidB) return false;
        }
        if (!a.MayInteractWith(b)) return false;
        const Fixed nx = b.x - a.x, ny = b.y - a.y, nz = b.z - a.z;
        if (nx.IsZero() && ny.IsZero() && nz.IsZero()) return false;   // no line between them
        const OrderVector ab = ka->CrossTowardUnder( nx,  ny,  nz, span);
        const OrderVector ba = kb->CrossTowardUnder(-nx, -ny, -nz, span);
        if (ab.energy.IsPositive()) kb->AbsorbUnder(ab);
        if (ba.energy.IsPositive()) ka->AbsorbUnder(ba);
        return false;
    }

    /*
     * THE FIELD, before the step: this body's velocity moves by what its
     * space's field does over dt (OrderVector::Accelerate), and the work is
     * counted where the field is stated. A body resting on a surface (the last
     * solid contact said so) feels only the part along it -- the rest is what
     * the surface holds up -- so a resting body stays exactly at rest and a
     * body on a slope slides. An anchored body is held: whatever motion it was
     * handed becomes heat, and it does not step anywhere.
     */
    void putSolid(CausalSolid::Shape shape, Fixed restitution, Fixed friction)
    {
        ::std::string v;
        ETCS::Entity::putWord(v, static_cast<int64_t>(shape));
        ETCS::Entity::putWord(v, restitution.raw);
        ETCS::Entity::putWord(v, friction.raw);
        this->addTag("solid", v);
    }
    void fieldLocked(Fixed dt)
    {
        if (m_anchored) { m_ov.Rest(); return; }
        Causal_* up = container();
        if (!up) return;
        Fixed gx, gy, gz;
        Causal_* src = up->FieldUnder(gx, gy, gz);
        if (!src) return;
        if (m_resting)
        {
            const Fixed gn = gx * m_snx + gy * m_sny + gz * m_snz;
            if (gn.raw < 0) { gx -= m_snx * gn; gy -= m_sny * gn; gz -= m_snz * gn; }
        }
        if (gx.IsZero() && gy.IsZero() && gz.IsZero()) return;
        src->CountFieldWorkUnder(m_ov.Accelerate(gx, gy, gz, dt, static_cast<Derived*>(this)->MassConcrete()));
    }

    /*
     * THE SOLID CONTACTS, after the transmissions, over the pairs of solids the
     * gate found (contactsLocked), in that order. Each pair, a sphere against
     * a sphere, a box or a half-space (two boxes do not meet yet):
     *
     *   apart     the sphere is pushed out along the contact normal by how deep
     *             it is (both, by their shares of the motion, when neither is
     *             anchored) -- positions move here, and only here
     *   bounce    the motion along the normal, if closing, is turned back times
     *             the restitution; a closing speed below what the field adds in
     *             two steps is not a bounce but a landing, and stops
     *   rub       the motion across the normal loses what the friction takes
     *             of the load -- the bounce's impulse, or what the field presses
     *             a resting body in with -- never more than stops it
     *
     * Motion lost is heat in the body that lost it (OrderVector::SetVelocity);
     * motion handed to the other side leaves the giver's heat and arrives in
     * the taker's first, so the ledger is exact. A body left closing slower
     * than a landing, with the field pressing it in, is RESTING on the other
     * (SupportUnder) until the next pass says otherwise.
     */
    void solidPassLocked(const ::std::vector<Causal_*>& kids, Fixed span)
    {
        for (size_t i = 0; i < kids.size(); ++i)
            if (m_solids[i].shape != CausalSolid::Off && !m_solids[i].anchored)
                kids[i]->SupportUnder(false, Fixed::Zero(), Fixed::Zero(), Fixed::Zero());
        if (m_solid_pairs.empty()) return;
        Fixed gx, gy, gz;
        if (!FieldUnder(gx, gy, gz)) gx = gy = gz = Fixed::Zero();
        for (const auto& [i, j] : m_solid_pairs) solidPairLocked(kids[i], m_solids[i], kids[j], m_solids[j], gx, gy, gz, span);
    }

    static void solidPairLocked(Causal_* ka, const CausalSolid& sa, Causal_* kb, const CausalSolid& sb,
                                Fixed gx, Fixed gy, Fixed gz, Fixed dt)
    {
        // The sphere is S; the other is O; the normal points out of O into S.
        Causal_* kS = ka; Causal_* kO = kb;
        const CausalSolid* cS = &sa; const CausalSolid* cO = &sb;
        if (sa.shape != CausalSolid::Sphere) { ::std::swap(kS, kO); ::std::swap(cS, cO); }
        if (cS->shape != CausalSolid::Sphere) return;   // box against box: not yet
        if (cS->anchored && cO->anchored) return;
        OrderVector& S = kS->RowsUnder();
        OrderVector& O = kO->RowsUnder();
        const Fixed slack = Fixed::FromRaw(Fixed::ONE >> 10);   // ~0.001: a resting body counts at zero depth
        SolidContact c;
        if (cO->shape == CausalSolid::Sphere)
            c = Planes::SphereSphere(S.x, S.y, S.z, S.radius, O.x, O.y, O.z, O.radius, slack);
        else if (cO->shape == CausalSolid::Box)
        {
            Fixed lx = S.x - O.x, ly = S.y - O.y, lz = S.z - O.z;
            O.UnrotateVector(lx, ly, lz);
            c = Planes::SphereBox(cO->hx, cO->hy, cO->hz, lx, ly, lz, S.radius, slack);
            if (c.touching) O.RotateVector(c.nx, c.ny, c.nz);
        }
        else
        {
            Fixed ux = Fixed::Zero(), uy = Fixed::One(), uz = Fixed::Zero();
            O.RotateVector(ux, uy, uz);
            c = Planes::SphereHalfSpace(Plane::FromPointNormal(O.x, O.y, O.z, ux, uy, uz), S.x, S.y, S.z, S.radius, slack);
        }
        if (!c.touching) return;

        const Fixed mS = kS->MassUnder(), mO = kO->MassUnder();
        const Fixed iS = cS->anchored || !mS.IsPositive() ? Fixed::Zero() : Fixed::One() / mS;
        const Fixed iO = cO->anchored || !mO.IsPositive() ? Fixed::Zero() : Fixed::One() / mO;
        const Fixed isum = iS + iO;
        if (!isum.IsPositive()) return;

        // Apart.
        if (c.depth.IsPositive())
        {
            const Fixed ps = c.depth * (iS / isum), po = c.depth * (iO / isum);
            S.PlaceAt(S.x + c.nx * ps, S.y + c.ny * ps, S.z + c.nz * ps);
            if (po.IsPositive()) O.PlaceAt(O.x - c.nx * po, O.y - c.ny * po, O.z - c.nz * po);
        }

        // Bounce and rub, as velocities.
        Fixed vSx, vSy, vSz, vOx, vOy, vOz;
        S.Velocity(mS, vSx, vSy, vSz);
        O.Velocity(mO, vOx, vOy, vOz);
        const Fixed rx = vSx - vOx, ry = vSy - vOy, rz = vSz - vOz;
        const Fixed vn = rx * c.nx + ry * c.ny + rz * c.nz;
        const Fixed gn = gx * c.nx + gy * c.ny + gz * c.nz;           // < 0: the field presses S into O
        const Fixed fall = (gn.raw < 0 ? -gn : Fixed::Zero()) * dt;   // what the field adds along n in one step
        const Fixed landing = Fixed::FromInt(2) * fall + Fixed::FromRaw(Fixed::ONE >> 10);
        Fixed jn;                                                     // the normal impulse (per unit inverse mass)
        if (vn.raw < 0)
        {
            /*
             * THE BOUNCE IS OF THE APPROACH, NOT OF THE STEP'S OWN FALL. The
             * closing speed includes what the field added in the step that
             * carried the body in, and the push back out lands it where it
             * started that step: bouncing that too returns more than fell, and
             * a body near rest hops forever. So the restitution is of the
             * approach less one step of the field, and a bounce smaller than
             * two steps of it is a landing.
             */
            const Fixed e = (cS->restitution + cO->restitution) * Fixed::Half();
            Fixed back = e * Fixed::Max(Fixed::Zero(), -vn - fall);
            if (back < landing) back = Fixed::Zero();
            jn = (back - vn) / isum;
        }
        // The load friction works against: the impulse, and what the field
        // presses a resting body in with over the step.
        Fixed load = jn;
        if (gn.raw < 0 && vn < landing) load += (-gn) * dt / isum;
        const Fixed tx = rx - c.nx * vn, ty = ry - c.ny * vn, tz = rz - c.nz * vn;
        const Fixed vt = Fixed::Length(tx, ty, tz);
        Fixed jt;
        if (vt.IsPositive())
        {
            const Fixed mu = (cS->friction + cO->friction) * Fixed::Half();
            jt = Fixed::Min(mu * load, vt / isum);
        }
        if (jn.IsPositive() || jt.IsPositive())
        {
            const Fixed ux = vt.IsPositive() ? tx / vt : Fixed::Zero();
            const Fixed uy = vt.IsPositive() ? ty / vt : Fixed::Zero();
            const Fixed uz = vt.IsPositive() ? tz / vt : Fixed::Zero();
            const Fixed px = c.nx * jn - ux * jt, py = c.ny * jn - uy * jt, pz = c.nz * jn - uz * jt;
            vSx += px * iS; vSy += py * iS; vSz += pz * iS;
            vOx -= px * iO; vOy -= py * iO; vOz -= pz * iO;
            // Spend before gaining. Against an anchored body only the free one
            // moves, and it can only lose (restitution and friction are at
            // most one). Between two free ones, the one that lost sets its
            // motion first, and what the other gained leaves that one's heat
            // and arrives in the other's before its motion is set.
            if (!iO.IsPositive()) S.SetVelocity(mS, vSx, vSy, vSz);
            else if (!iS.IsPositive()) O.SetVelocity(mO, vOx, vOy, vOz);
            else
            {
                const Fixed gainS = Fixed::Half() * mS * (vSx * vSx + vSy * vSy + vSz * vSz) - S.KineticEnergy();
                const Fixed gainO = Fixed::Half() * mO * (vOx * vOx + vOy * vOy + vOz * vOz) - O.KineticEnergy();
                auto hand = [](OrderVector& giver, Fixed mg, Fixed gx_, Fixed gy_, Fixed gz_,
                               OrderVector& taker, Fixed mt, Fixed tx_, Fixed ty_, Fixed tz_, Fixed gain) {
                    giver.SetVelocity(mg, gx_, gy_, gz_);
                    if (gain.IsPositive()) taker.Absorb(giver.Emit(gain));
                    taker.SetVelocity(mt, tx_, ty_, tz_);
                };
                if (gainS.IsPositive()) hand(O, mO, vOx, vOy, vOz, S, mS, vSx, vSy, vSz, gainS);
                else                    hand(S, mS, vSx, vSy, vSz, O, mO, vOx, vOy, vOz, gainO);
            }
        }
        // Resting: closing no faster than a landing, the field pressing in.
        const Fixed after = (vSx - vOx) * c.nx + (vSy - vOy) * c.ny + (vSz - vOz) * c.nz;
        if (gn.raw < 0 && after < landing && !cS->anchored) kS->SupportUnder(true, c.nx, c.ny, c.nz);
        if (gn.IsPositive() && after < landing && !cO->anchored && cO->shape == CausalSolid::Sphere)
            kO->SupportUnder(true, -c.nx, -c.ny, -c.nz);
    }

    /*
     * FITNESS, after the contacts, from the rows they left: what each member
     * is in. A member inside the space of a sibling (InsideOf) moves into the
     * smallest such sibling -- the most local environment that holds it; one
     * outside this entity's own space moves up to this entity's container
     * (the open boundary keeps what fits nowhere). Decided from the rows
     * before anything moves, applied in canonical order; a member something
     * is moving into stays put this time, so no pair can swap into each
     * other. One level per interaction: a member still outside goes on up at
     * its new container's next one.
     */
    void fitLocked(const ::std::vector<Causal_*>& kids)
    {
        const size_t n = kids.size();
        if (n == 0) return;
        Causal_* up = container();
        const bool broad = m_broad && n >= BroadphaseFrom();   // contactsLocked built it from these rows
        ::std::vector<size_t> into;
        ::std::vector<Fixed>  room;
        for (size_t b = 0; b < n; ++b)
        {
            const Fixed sp = kids[b]->Space();
            if (!sp.IsPositive()) continue;
            if (into.empty()) { into.assign(n, n); room.assign(n, Fixed::Zero()); }
            const OrderVector& B = kids[b]->Order4();
            auto consider = [&](size_t a) {
                if (a == b || !kids[a]->Order4().InsideOf(B.x, B.y, B.z, sp)) return;
                if (into[a] == n || sp < room[a]) { into[a] = b; room[a] = sp; }   // ties: the first in canonical order
            };
            if (broad) for (const auto& h : m_broad->within(b, sp)) consider(h.first);
            else       for (size_t a = 0; a < n; ++a) consider(a);
        }
        ::std::vector<::std::pair<size_t, Causal_*>> moves;
        for (size_t a = 0; a < n; ++a)
        {
            if (!into.empty() && into[a] != n) moves.emplace_back(a, kids[into[a]]);
            else if (up && !kids[a]->Order4().InsideOf(Fixed::Zero(), Fixed::Zero(), Fixed::Zero(), m_space))
                moves.emplace_back(a, up);
        }
        if (moves.empty()) return;
        ::std::vector<bool> target(n, false);
        for (const auto& mv : moves) if (mv.second != up) target[into[mv.first]] = true;
        for (const auto& [a, to] : moves) if (!target[a]) to->Contain(kids[a]);
    }

    /*
     * The pairs that MAY touch, from a kd-tree over the members' positions
     * (nanoflann, libs/), rebuilt each pass: they all moved. Each pair is
     * found from its larger reach (distance <= ri + rj <= 2 max), padded past
     * what the doubles and the gate's floored squares can differ by. Pruning
     * only -- the gate keeps the n^2 loop's pairs, and a pair it drops costs a
     * gate call, never a wrong row. Allocated by the first large pass, so a
     * box carries one pointer.
     */
    struct Broadphase
    {
        struct Cloud
        {
            ::std::vector<double> xyz;
            size_t kdtree_get_point_count() const { return xyz.size() / 3; }
            double kdtree_get_pt(size_t i, size_t d) const { return xyz[i * 3 + d]; }
            template <class B> bool kdtree_get_bbox(B&) const { return false; }
        };
        using Tree = nanoflann::KDTreeSingleIndexAdaptor<nanoflann::L2_Simple_Adaptor<double, Cloud>, Cloud, 3, uint32_t>;

        Cloud cloud;
        ::std::vector<double> reach;
        ::std::vector<nanoflann::ResultItem<uint32_t, double>> hits;
        ::std::vector<::std::pair<uint32_t, uint32_t>> out;
        Tree tree{ 3, cloud, nanoflann::KDTreeSingleIndexAdaptorParams(16, nanoflann::KDTreeSingleIndexAdaptorFlags::SkipInitialBuildIndex) };

        const ::std::vector<::std::pair<uint32_t, uint32_t>>& pairs(const ::std::vector<Causal_*>& kids)
        {
            const size_t n = kids.size();
            cloud.xyz.resize(n * 3);
            reach.resize(n);
            for (size_t i = 0; i < n; ++i)
            {
                const OrderVector& a = kids[i]->Order4();
                cloud.xyz[i * 3] = a.x.ToDouble(); cloud.xyz[i * 3 + 1] = a.y.ToDouble(); cloud.xyz[i * 3 + 2] = a.z.ToDouble();
                reach[i] = a.radius.ToDouble();
            }
            tree.buildIndex();
            out.clear();
            for (size_t i = 0; i < n; ++i)
            {
                const double r = 2.0 * reach[i] * (1.0 + 1e-9) + 1e-3;
                hits.clear();
                (void)tree.radiusSearch(&cloud.xyz[i * 3], r * r, hits, nanoflann::SearchParameters(0, false));
                for (const auto& h : hits)
                {
                    const size_t j = h.first;
                    // Once per pair: from the larger reach, the lower index on a tie.
                    if (j == i || reach[j] > reach[i] || (reach[j] == reach[i] && j < i)) continue;
                    out.emplace_back(static_cast<uint32_t>(i < j ? i : j), static_cast<uint32_t>(i < j ? j : i));
                }
            }
            ::std::sort(out.begin(), out.end());
            return out;
        }

        // The members whose position is within `r` of member b's, from the tree
        // the last pairs() built: fitness's candidates, the exact test after.
        // In no order: each candidate is judged on its own (fitLocked).
        const ::std::vector<nanoflann::ResultItem<uint32_t, double>>& within(size_t b, Fixed r)
        {
            const double d = r.ToDouble() * (1.0 + 1e-9) + 1e-3;
            hits.clear();
            (void)tree.radiusSearch(&cloud.xyz[b * 3], d * d, hits, nanoflann::SearchParameters(0, false));
            return hits;
        }
    };

    // The parent, while it is one. Under an arena teardown an ancestor has
    // run ~Entity() before this entity's retire reaches it (the same stop
    // Entity::markStateChange makes): a destructed parent has no Causal
    // half to ask for -- its vtable is gone -- so it is the edge of the model.
    static ETCS::Entity* liveParent(const ETCS::Entity* e)
    {
        ETCS::Entity* p = e->getParent();
        return (p && !p->isDestructed()) ? p : nullptr;
    }

    // The family's interface name, made once: a Buffer is 256 bytes zeroed on
    // construction, and TreeMutex() asks for it on every entry.
    static const ETCS::Buffer& causalKey() { static const ETCS::Buffer k("Causal"); return k; }

    static Causal_* containerOf(Causal_* c)
    {
        ETCS::Entity* p = liveParent(c);
        if (!p) return nullptr;
        void* raw = p->getInterfacePointer(causalKey());
        return raw ? static_cast<Causal_*>(raw) : nullptr;
    }

    // The parent's Causal half, looked up again only when the parent is a
    // different entity from last time.
    // Keyed on the parent's RID as well as its address: an arena hands a
    // dead entity's bytes to the next allocation of that size, so the same
    // address can be a different entity -- never the same RID.
    Causal_* container()
    {
        ETCS::Entity* p = liveParent(this);
        const ETCS::RID prid = p ? p->getRID() : 0;
        if (p != m_container_of || prid != m_container_rid)
        {
            m_container_of  = p;
            m_container_rid = prid;
            void* c = p ? p->getInterfacePointer(causalKey()) : nullptr;
            m_container     = c ? static_cast<Causal_*>(c) : nullptr;
        }
        return m_container;
    }

    OrderVector m_ov;
    OrderVector m_last_emission{};
    Fixed       m_emissivity = Fixed::Half();
    Fixed       m_space;   // zero: solid, holds nothing by fit (SetSpace)
    Fixed       m_emitted_out;
    Fixed       m_field_work;   // what the field this space states did (CountFieldWorkUnder)
    Fixed       m_gx, m_gy, m_gz;   // the stated field (SetGravity), when m_gravity_set
    bool        m_gravity_set = false;
    CausalSolid m_solid;        // the step's copy of "solid" (shape Off: not solid)
    bool        m_anchored = false;   // "anchored"
    CausalSolid::Shape m_shape = CausalSolid::Off;
    bool        m_shape_set = false;
    // The surface this body rested on at the last solid contact (SupportUnder):
    // transient, re-found by the first contact after a resume.
    bool        m_resting = false;
    Fixed       m_snx, m_sny, m_snz;
    ::std::atomic<bool> m_moving{ false };   // Moving(): written under the lock, read by observers
    ::std::vector<CausalSolid> m_solids;                      // contactsLocked's per-pass copy
    ::std::vector<::std::pair<uint32_t, uint32_t>> m_solid_pairs;
    uint64_t    m_ticks = 0;
    ::std::recursive_mutex m_tree_mtx;   // the tree's, when this is its top (TreeMutex)

    Fixed       m_share_dt, m_share_k, m_share;   // 1 - exp(-k dt) for the last (dt, k) seen

    ::std::shared_ptr<const ::std::vector<Causal_*>> m_kids;
    ::std::unique_ptr<Broadphase> m_broad;   // contactsLocked, once a pass is large
    uint32_t                m_kids_epoch = 0;          // hash_epoch_ starts at 1: the first ask walks
    uint32_t                m_id_epoch   = 0;
    uint32_t                m_id_pepoch  = 0;
    ETCS::Entity*           m_container_of = nullptr;
    ETCS::RID               m_container_rid = 0;
    Causal_*                m_container    = nullptr;

    /*
     * THE "Causal" VALUE: a version byte, then the words as they are --
     * the eighteen of the rows, the clock and what left at the boundary,
     * and the eighteen of the last crossing. Everything CausalHash reads
     * and everything that moves without a verb; the emissivity is a verb's
     * and has its own value (SetEmissivity). Little-endian raw words; the
     * same bytes on every platform the rows are the same on. Version 3 adds
     * one word, the work this space's field has done (FieldWork), and is
     * written only when there is some: a space with no field keeps version
     * 2's bytes and digest.
     */
    static constexpr unsigned char kStateVersion = 2;
    static constexpr unsigned char kStateVersionField = 3;
    static void putWords(::std::string& out, const OrderVector& o)
    {
        const int64_t w[] = { o.x.raw, o.y.raw, o.z.raw, static_cast<int64_t>(o.id),
                              o.ox.raw, o.oy.raw, o.oz.raw, o.energy.raw,
                              o.fx.raw, o.fy.raw, o.fz.raw, o.radius.raw,
                              o.qx.raw, o.qy.raw, o.qz.raw, o.qw.raw,
                              o.interval.raw, static_cast<int64_t>(o.uncertainty) };
        for (int64_t v : w) ETCS::Entity::putWord(out, v);
    }
    static bool getWords(const ::std::string& in, size_t& at, OrderVector& o)
    {
        int64_t w[18];
        for (int64_t& v : w) if (!ETCS::Entity::getWord(in, at, v)) return false;
        o.x = Fixed::FromRaw(w[0]); o.y = Fixed::FromRaw(w[1]); o.z = Fixed::FromRaw(w[2]); o.id = static_cast<uint64_t>(w[3]);
        o.ox = Fixed::FromRaw(w[4]); o.oy = Fixed::FromRaw(w[5]); o.oz = Fixed::FromRaw(w[6]); o.energy = Fixed::FromRaw(w[7]);
        o.fx = Fixed::FromRaw(w[8]); o.fy = Fixed::FromRaw(w[9]); o.fz = Fixed::FromRaw(w[10]); o.radius = Fixed::FromRaw(w[11]);
        o.qx = Fixed::FromRaw(w[12]); o.qy = Fixed::FromRaw(w[13]); o.qz = Fixed::FromRaw(w[14]); o.qw = Fixed::FromRaw(w[15]);
        o.interval = Fixed::FromRaw(w[16]); o.uncertainty = static_cast<uint64_t>(w[17]);
        return true;
    }
    void packState(::std::string& out)
    {
        ::std::lock_guard<::std::recursive_mutex> lk(TreeMutex());
        refreshIdentityLocked();
        const bool field = !m_field_work.IsZero();
        out.push_back(static_cast<char>(field ? kStateVersionField : kStateVersion));
        putWords(out, m_ov);
        ETCS::Entity::putWord(out, static_cast<int64_t>(m_ticks));
        ETCS::Entity::putWord(out, m_emitted_out.raw);
        putWords(out, m_last_emission);
        if (field) ETCS::Entity::putWord(out, m_field_work.raw);
    }
    // The "Causal" value's digest: what it packs, hashed instead of written
    // -- a reader asking only whether it moved. Under the tree's lock, so a
    // whole tick or none.
    uint64_t stateDigest()
    {
        ::std::lock_guard<::std::recursive_mutex> lk(TreeMutex());
        refreshIdentityLocked();
        uint64_t h = fixed_mix(m_ov.Hash(), static_cast<int64_t>(m_ticks));
        h = fixed_mix(h, m_emitted_out.raw);
        h = fixed_mix(h, static_cast<int64_t>(m_last_emission.Hash()));
        return m_field_work.IsZero() ? h : fixed_mix(h, m_field_work.raw);
    }
    bool unpackState(const ::std::string& in)
    {
        if (in.empty()) return false;
        const unsigned char version = static_cast<unsigned char>(in[0]);
        if (version != kStateVersion && version != kStateVersionField) return false;
        size_t at = 1;
        OrderVector rows, last;
        int64_t ticks = 0, out = 0, field = 0;
        if (!getWords(in, at, rows) || !ETCS::Entity::getWord(in, at, ticks)
            || !ETCS::Entity::getWord(in, at, out) || !getWords(in, at, last)) return false;
        if (version == kStateVersionField && !ETCS::Entity::getWord(in, at, field)) return false;
        if (at != in.size()) return false;
        ::std::lock_guard<::std::recursive_mutex> lk(TreeMutex());
        m_ov            = rows;
        m_ticks         = static_cast<uint64_t>(ticks);
        m_emitted_out   = Fixed::FromRaw(out);
        m_field_work    = Fixed::FromRaw(field);
        m_last_emission = last;
        m_share_dt = Fixed::Zero(); m_share_k = Fixed::Zero();   // the share cache keys on (dt, k): recompute
        m_id_epoch = 0; m_id_pepoch = 0;                        // the identity is this entity's, not the record's
        refreshIdentityLocked();
        return true;
    }

    struct Binder
    {
        explicit Binder(CausalBase* b)
        {
            b->bindValue(ETCS::Buffer("Causal"), ETCS::Entity::ValueBinding{
                b,
                [](void* self, ::std::string& out) { static_cast<CausalBase*>(self)->packState(out); },
                [](void* self, const ::std::string& in) { return static_cast<CausalBase*>(self)->unpackState(in); },
                [](void* self) { return static_cast<CausalBase*>(self)->stateDigest(); } });
        }
    };
    Binder m_binder{ this };
};

#endif
