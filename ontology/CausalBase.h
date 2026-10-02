#ifndef BASE_Causal_H__
#define BASE_Causal_H__
#include "Causal.h"
#include <memory>
#include <mutex>
#include <vector>

/*
 * THE ROWS LIVE HERE, and so does everything that is true of them whatever
 * the leaf is: the commit of entropy into the container, the tick, the last
 * crossing, the driver, the hash. A leaf says what its own step IS -- what
 * pushes it (a held key, an engine, a slope) and what slows it -- and nothing
 * else, because that is the only part that differs between a box and a kart.
 *
 * THE ORDER OF AN INTERACTION IS THE LAZY-COMMIT CONTRACT. Emission is
 * charged for the interval that just ENDED, over a heat total nothing touched
 * during it; the step then adds the heat the NEXT interval will be charged
 * for. The other way round bills every commit for heat that had not happened
 * yet when the interval started. So: commit, then step, then the members --
 * a member's commit lands in this entity's rows AFTER this entity settled its
 * own interval, which is the interval the member's heat belongs to.
 *
 * WHAT LEAVES GOES UP. The container is the parent entity if it is Causal;
 * a foreign provider's Causal parent takes the crossing exactly as one of
 * this provider's would, through the family, because Absorb is the family's.
 * No parent, or a parent that is not Causal, is the edge of the model, and
 * the heat is counted out rather than lost.
 */
ETCS_SUPERTYPE_BASE(Causal)
{
    ETCS_MAKE_INSTANCE(Causal)

    // ── the leaf's answers ──────────────────────────────────────────────

    // The step: apply whatever pushes and slows this thing over dt, and
    // advance the rows (OrderVector::Advance). The base has already
    // committed the entropy owed. Answer whether the rows moved, so a
    // settled leaf can leave its viewers alone.
    virtual bool StepConcrete(Fixed dt) = 0;

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
        InteractUnder(dt);
    }

    // The interaction, for a caller holding the tree's lock: commit, step,
    // then the members through the same hop.
    void InteractUnder(Fixed dt) override final
    {
        if (!dt.IsPositive()) return;
        OrderVector e;
        if (commitLocked(dt, e)) deliverLocked(e);
        static_cast<Derived*>(this)->StepConcrete(dt);
        for (Causal_* c : *kidsLocked()) c->InteractUnder(dt);   // the tree lock is held: no copy out
    }

    /*
     * ONE LOCK PER TREE. The mutex is the topmost Causal entity's; every
     * public entry here (Interact, Run, CommitEntropy, Impulse, Absorb, the
     * hash) takes it, and a leaf's own writers take it too (Scene3D's
     * verbs and its observed step). Recursive, because a member's crossing
     * is absorbed by its container inside the same interaction, and a leaf
     * calls family verbs from under it. Found by walking the containers
     * once per entry -- a Run of ten thousand ticks walks once.
     */
    ::std::recursive_mutex& TreeMutex() override final
    {
        Causal_* top = this;
        for (Causal_* env = container(); env; env = containerOf(env)) top = env;
        return top == this ? m_tree_mtx : top->TreeMutex();
    }

    // The driver: `ticks` interactions of `dt` each, no clock read. What a
    // headless run does instead of being looked at.
    void Run(uint32_t ticks, Fixed dt)
    {
        if (!dt.IsPositive()) return;
        ::std::lock_guard<::std::recursive_mutex> lk(TreeMutex());
        for (uint32_t i = 0; i < ticks; ++i) InteractUnder(dt);
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

    uint64_t CausalTicks() const override final { return m_ticks; }

    uint64_t CausalHash() override final
    {
        ::std::lock_guard<::std::recursive_mutex> lk(TreeMutex());   // a whole state, not one mid-step
        uint64_t h = fixed_mix(m_ov.Hash(), static_cast<int64_t>(m_ticks));
        for (Causal_* c : *kidsLocked()) h = fixed_mix(h, static_cast<int64_t>(c->CausalHash()));
        return h;
    }

    // ── what the base keeps beside the rows ─────────────────────────────

    // How fast heat leaves this thing into its container, per second. Zero
    // is a perfect insulator, a legitimate thing to be.
    void  SetEmissivity(Fixed per_sec) { if (per_sec.raw >= 0) m_emissivity = per_sec; }
    Fixed Emissivity() const           { return m_emissivity; }

    // Heat that left the model at this entity (it had no Causal container).
    Fixed EmittedOut() const { return m_emitted_out; }

    // The most recent crossing, as the OrderVector it is: what a guard
    // nesting a draw inside this entity's emission reads.
    const OrderVector& LastEmission() const { return m_last_emission; }

    /*
     * Commit every joule owed since the last interaction, into the container.
     * Callable on its own (Scene3D's observed path measures this interval
     * with a different ceiling from the motion's), and what Interact does
     * first. The tick counts emissions that HAPPENED: a thing with no heat to
     * shed has had no time pass for it.
     */
    void CommitEntropy(Fixed dt)
    {
        if (!dt.IsPositive()) return;
        ::std::lock_guard<::std::recursive_mutex> lk(TreeMutex());
        OrderVector e;
        if (commitLocked(dt, e)) deliverLocked(e);
    }

    /*
     * The Causal children, in attach order -- any provider's. KEPT, against
     * this entity's hash epoch: the typed-child lists move only through the
     * funnels that bump it (Entity::markStateChange, on every ancestor of a
     * change), so a driver taking thousands of interactions a second walks
     * the lists once per change in the subtree rather than once per tick --
     * and the walk was most of a tick. Same list, same order, as the walk
     * would give now.
     */
    //
    // A SNAPSHOT GOES OUT, shared: the walker iterates a list nobody will
    // rebuild under it, and takes it with one reference count rather than a
    // copy -- a copy per interaction was the cost of a tick.
    ::std::shared_ptr<const ::std::vector<Causal_*>> causalChildren()
    {
        ::std::lock_guard<::std::recursive_mutex> lk(TreeMutex());
        return kidsLocked();
    }

private:
    // The list, for a caller holding the tree's lock.
    const ::std::shared_ptr<const ::std::vector<Causal_*>>& kidsLocked()
    {
        const uint32_t epoch = this->hashEpoch();
        if (m_kids && m_kids_epoch == epoch) return m_kids;
        auto fresh = ::std::make_shared<::std::vector<Causal_*>>();
        ::std::vector<::std::pair<ETCS::Buffer, ETCS::RID>> kids;
        this->getOrderedTypedChildren(kids);
        for (const auto& entry : kids)
        {
            ETCS::Entity* child = this->getTypedChild(entry.first, entry.second);
            if (!child) continue;
            if (void* c = child->getInterfacePointer(ETCS::Buffer("Causal")))
                fresh->push_back(static_cast<Causal_*>(c));
        }
        m_kids       = ::std::move(fresh);
        m_kids_epoch = epoch;
        return m_kids;
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
        m_last_emission  = e;
        m_ov.interval    = e.interval;
        m_ov.uncertainty = e.uncertainty;
        ++m_ticks;
        return true;
    }

    // The crossing, into the container's rows (the tree's lock is held) or
    // counted out at the edge of the model.
    void deliverLocked(const OrderVector& e)
    {
        if (Causal_* env = container()) env->AbsorbUnder(e);
        else                            m_emitted_out += e.energy;
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
    ETCS::Entity*           m_container_of = nullptr;
    ETCS::RID               m_container_rid = 0;
    Causal_*                m_container    = nullptr;
};

#endif
