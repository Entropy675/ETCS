#ifndef BASE_Causal_H__
#define BASE_Causal_H__
#include "Causal.h"
#include <algorithm>
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
        if (step_dt.IsPositive()) static_cast<Derived*>(this)->AdvanceConcrete(step_dt);
        const auto& kids = *kidsLocked();   // the tree lock is held: no copy out
        for (Causal_* c : kids) c->InteractUnder(commit_dt, step_dt);
        contactsLocked(kids, step_dt.IsPositive() ? step_dt : commit_dt);
    }

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
        if (n < BroadphaseFrom())
        {
            for (size_t i = 0; i + 1 < n; ++i)
                for (size_t j = i + 1; j < n; ++j) contactLocked(kids[i], kids[j], span);
            return;
        }
        if (!m_broad) m_broad.reset(new Broadphase);
        for (const auto& [i, j] : m_broad->pairs(kids)) contactLocked(kids[i], kids[j], span);
    }

    static void contactLocked(Causal_* ka, Causal_* kb, Fixed span)
    {
        const OrderVector& a = ka->Order4();
        const OrderVector& b = kb->Order4();
        if (!a.MayInteractWith(b)) return;
        const Fixed nx = b.x - a.x, ny = b.y - a.y, nz = b.z - a.z;
        if (nx.IsZero() && ny.IsZero() && nz.IsZero()) return;   // no line between them
        const OrderVector ab = ka->CrossTowardUnder( nx,  ny,  nz, span);
        const OrderVector ba = kb->CrossTowardUnder(-nx, -ny, -nz, span);
        if (ab.energy.IsPositive()) kb->AbsorbUnder(ab);
        if (ba.energy.IsPositive()) ka->AbsorbUnder(ba);
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
    Fixed       m_emitted_out;
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
     * same bytes on every platform the rows are the same on.
     */
    static constexpr unsigned char kStateVersion = 2;
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
        out.push_back(static_cast<char>(kStateVersion));
        putWords(out, m_ov);
        ETCS::Entity::putWord(out, static_cast<int64_t>(m_ticks));
        ETCS::Entity::putWord(out, m_emitted_out.raw);
        putWords(out, m_last_emission);
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
        return fixed_mix(h, static_cast<int64_t>(m_last_emission.Hash()));
    }
    bool unpackState(const ::std::string& in)
    {
        if (in.empty() || static_cast<unsigned char>(in[0]) != kStateVersion) return false;
        size_t at = 1;
        OrderVector rows, last;
        int64_t ticks = 0, out = 0;
        if (!getWords(in, at, rows) || !ETCS::Entity::getWord(in, at, ticks)
            || !ETCS::Entity::getWord(in, at, out) || !getWords(in, at, last) || at != in.size()) return false;
        ::std::lock_guard<::std::recursive_mutex> lk(TreeMutex());
        m_ov            = rows;
        m_ticks         = static_cast<uint64_t>(ticks);
        m_emitted_out   = Fixed::FromRaw(out);
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
