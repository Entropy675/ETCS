#ifndef BASE_Causal_H__
#define BASE_Causal_H__
#include "Causal.h"
#include <algorithm>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

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
     * THE OBSERVED INTERACTION, and the record of it. An observer measures
     * its two intervals (entropy and motion, with different ceilings --
     * StepClock.h) and hands them here as the Fixed values the rows will
     * see; those two numbers are the whole of what the wall clock
     * contributed, so they go on this entity's tape, and a run of the tape
     * through Replay lands on the same rows. Taken at the observed root
     * only: the hop under it steps the members with the same spans.
     */
    struct Span { int64_t commit, step; };
    using Tape = ::std::vector<Span>;
    void InteractObserved(Fixed commit_dt, Fixed step_dt)
    {
        if (!commit_dt.IsPositive() && !step_dt.IsPositive()) return;
        ::std::lock_guard<::std::recursive_mutex> lk(TreeMutex());
        if (m_tape.size() < kTapeMax) m_tape.push_back(Span{ commit_dt.raw, step_dt.raw });
        else                          m_tape_full = true;
        InteractUnder(commit_dt, step_dt);
    }
    const Tape& ObservedTape() const { return m_tape; }
    bool        ObservedTapeFull() const { return m_tape_full; }   // the record stopped; a replay from it is partial
    void Replay(const Tape& tape)
    {
        ::std::lock_guard<::std::recursive_mutex> lk(TreeMutex());
        for (const Span& s : tape) InteractUnder(Fixed::FromRaw(s.commit), Fixed::FromRaw(s.step));
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

    // A multiset over the members: their hashes sorted, so the number does
    // not depend on the order they were attached in -- only on what they are
    // and where their rows stand.
    uint64_t CausalHash() override final
    {
        ::std::lock_guard<::std::recursive_mutex> lk(TreeMutex());   // a whole state, not one mid-step
        refreshIdentityLocked();
        uint64_t h = fixed_mix(m_ov.Hash(), static_cast<int64_t>(m_ticks));
        ::std::vector<uint64_t> kids;
        for (Causal_* c : *kidsLocked()) kids.push_back(c->CausalHash());
        ::std::sort(kids.begin(), kids.end());
        for (uint64_t k : kids) h = fixed_mix(h, static_cast<int64_t>(k));
        return h;
    }

    // ── what the base keeps beside the rows ─────────────────────────────

    // How fast heat leaves this thing into its container, per second. Zero
    // is a perfect insulator, a legitimate thing to be.
    void  SetEmissivity(Fixed per_sec) { if (per_sec.raw >= 0) m_emissivity = per_sec; }
    Fixed Emissivity() const           { return m_emissivity; }

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

private:
    /*
     * CANONICAL ORDER: tags in first-attachment order (that order is state;
     * the merkle hash composes by it), and within a tag by the member's
     * state hash -- what it is -- with attach order only between members of
     * one state, which the physics cannot tell apart anyway (two such
     * members are one identity, Causal.h). Never the hash map's order and
     * never the RID's, so the same script in another runtime, or the same
     * scene rebuilt in another order, walks its members the same way. The
     * same order the contacts pair in.
     */
    const ::std::shared_ptr<const ::std::vector<Causal_*>>& kidsLocked()
    {
        const uint32_t epoch = this->hashEpoch();
        if (m_kids && m_kids_epoch == epoch) return m_kids;
        auto fresh = ::std::make_shared<::std::vector<Causal_*>>();
        ::std::vector<::std::pair<ETCS::Buffer, ETCS::RID>> kids;
        this->getTypedChildren(kids);
        struct Member { ETCS::Buffer tag; uint64_t hash; Causal_* c; };
        ::std::vector<Member> members;
        for (const auto& entry : kids)
        {
            ETCS::Entity* child = this->getTypedChild(entry.first, entry.second);
            if (!child) continue;
            if (void* c = child->getInterfacePointer(ETCS::Buffer("Causal")))
                members.push_back(Member{ entry.first, child->getHash(), static_cast<Causal_*>(c) });
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

    // Row 0's identity is this entity's state hash (OrderVector.h): refreshed
    // when the hash epoch says the state moved, which is the only time the
    // hash can differ. The hash is lazy and cached, so this is one load and
    // a compare per interaction.
    void refreshIdentityLocked()
    {
        const uint32_t epoch = this->hashEpoch();
        if (m_id_epoch == epoch) return;
        m_ov.id    = this->getHash();
        m_id_epoch = epoch;
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
     * not depend on which of the pair is walked first. Every pair, which is
     * n^2 over the members -- a broadphase is an implementation choice this
     * does not make yet; the gate (GapTo) is the constraint.
     */
    void contactsLocked(const ::std::vector<Causal_*>& kids, Fixed span)
    {
        const size_t n = kids.size();
        for (size_t i = 0; i + 1 < n; ++i)
            for (size_t j = i + 1; j < n; ++j)
            {
                const OrderVector& a = kids[i]->Order4();
                const OrderVector& b = kids[j]->Order4();
                if (!a.MayInteractWith(b)) continue;
                const Fixed nx = b.x - a.x, ny = b.y - a.y, nz = b.z - a.z;
                if (nx.IsZero() && ny.IsZero() && nz.IsZero()) continue;   // no line between them
                const OrderVector ab = kids[i]->CrossTowardUnder( nx,  ny,  nz, span);
                const OrderVector ba = kids[j]->CrossTowardUnder(-nx, -ny, -nz, span);
                if (ab.energy.IsPositive()) kids[j]->AbsorbUnder(ab);
                if (ba.energy.IsPositive()) kids[i]->AbsorbUnder(ba);
            }
    }

    // The parent, while it is one. Under an arena teardown an ancestor has
    // run ~Entity() before this entity's retire reaches it (the same stop
    // Entity::markStateChange makes): a destructed parent has no Causal
    // half to ask for -- its vtable is gone -- so it is the edge of the model.
    static ETCS::Entity* liveParent(const ETCS::Entity* e)
    {
        ETCS::Entity* p = e->getParent();
        return (p && !p->isDestructed()) ? p : nullptr;
    }

    static Causal_* containerOf(Causal_* c)
    {
        ETCS::Entity* p = liveParent(c);
        if (!p) return nullptr;
        void* raw = p->getInterfacePointer(ETCS::Buffer("Causal"));
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
            void* c = p ? p->getInterfacePointer(ETCS::Buffer("Causal")) : nullptr;
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
    uint32_t                m_kids_epoch = 0;          // hash_epoch_ starts at 1: the first ask walks
    uint32_t                m_id_epoch   = 0;
    ETCS::Entity*           m_container_of = nullptr;
    ETCS::RID               m_container_rid = 0;
    Causal_*                m_container    = nullptr;

    // The observed spans, oldest first; bounded, and the bound is announced
    // (ObservedTapeFull) rather than the record silently wrapping.
    static constexpr size_t kTapeMax = size_t(1) << 20;   // ~4.6 hours at 60 Hz
    Tape m_tape;
    bool m_tape_full = false;
};

#endif
