#ifndef BASE_Animated_H__
#define BASE_Animated_H__
#include "Animated.h"
#include "StepClock.h"

/*
 * THE CLOCK LIVES HERE, and that is the whole reason this base exists.
 *
 * Every animated thing has to answer "how much has happened since last time",
 * and a leaf that answers it for itself answers it slightly differently from the
 * next (StepClock.h on the copies this replaces). The measurement is StepClock,
 * beside this file, because it is not the family: a leaf whose steps are caused
 * by something other than a driver needs the same clock without the same claim.
 * What this base owns is the DECISION to keep one, and the order it asks its
 * two questions in.
 *
 * MEASURED, NOT COUNTED, and the difference is not stylistic.
 *
 * A leaf that counts VISITS is writing a rate in units of "however often
 * somebody happens to come by": it runs at half speed on a 30Hz display, at
 * double speed if a second driver appears, and at no speed at all in a session
 * with no frame edge. A leaf handed an INTERVAL is writing a rate in seconds,
 * which is the same rate under all three. The visit count is not offered for
 * that reason -- if it were, it would be used.
 *
 * THIS IS ALSO WHAT MAKES MORE THAN ONE DRIVER SAFE. Two surfaces each running
 * their own frame edge both advance the family; the second call of a pair
 * measures almost no time and therefore advances almost nothing, so the total
 * rate is the same as with one driver. There is no token to compare, no
 * designated driver to configure and nothing to get wrong when a second window
 * opens. The cost is one extra virtual call per visit per driver.
 *
 * THE STALL CEILING IS A CAUSAL STATEMENT rather than a smoothing filter, and
 * StepClock says why. The default quarter second is this family's choice of how
 * much unobserved time is credible for an animation, not a general one.
 */
ETCS_SUPERTYPE_BASE(Animated)
{
    ETCS_MAKE_INSTANCE(Animated)

    // The leaf's own two answers. Animating is asked every visit and is
    // expected to be cheap -- it is what a settled entity costs.
    virtual bool AnimatingConcrete() = 0;

    // dt_ms is the measured interval since this entity's last visit, capped
    // (see above), and 0 on the first one. A leaf that wants a fixed number of
    // steps writes a rate: not `x += 0.085` but `x += 0.5 * dt_ms / 1000`.
    virtual void AdvanceConcrete(double dt_ms) = 0;

    bool Animating() override final
    {
        return static_cast<Derived*>(this)->AnimatingConcrete();
    }

    void Advance() override final
    {
        // SETTLED STILL MOVES THE MARK. Skip rather than an early return with
        // the clock left behind: otherwise the first step after a long settled
        // period measures the gap since the animation last RAN instead of since
        // this visit, and starting an animation would begin with one enormous
        // frame. StepClock::Skip is exactly this caller.
        if (!Animating()) { m_clock.Skip(); return; }
        static_cast<Derived*>(this)->AdvanceConcrete(m_clock.Take());
    }

    // The clock this base is keeping, for a leaf or a test that wants to read
    // the ceiling rather than infer it.
    double AnimatedStallCapMs() const { return m_clock.CapMs(); }

private:
    StepClock m_clock{};
};


/*
 * ADVANCE EVERYTHING THAT CLAIMS Animated, ONCE.
 *
 * The family's driver, and it lives here rather than in core because core is
 * deliberately ignorant of every family by name (Entity.h says so at
 * ETCS_MAKE_INSTANCE). collect_family is the generic half; naming Animated is
 * this file's business.
 *
 * A SESSION NEEDS EXACTLY ONE CALLER. A frame edge, a page's timer, a test
 * stepping by hand -- any of them, and it advances every Animated leaf in every
 * loaded image. See ontology/Animated.h on why the driver is not named by the
 * family, and the header above on why a second driver would still be correct
 * rather than merely tolerated.
 *
 * NO LIST IS KEPT ACROSS CALLS, on purpose. A leaf that appeared since the last
 * tick is advanced on this one and a leaf that died is skipped, because
 * collect_family re-reads the claim instead of a cache -- which is the whole
 * reason a driver can know nothing about what it drives. The cost is one family
 * walk per tick, against visits that are doing real work.
 *
 * Returns how many were advanced, so a caller can say the edge is alive once
 * without saying it every frame.
 */
inline size_t etcs_advance_animated()
{
    static thread_local ::std::vector<Animated_*> live;
    live.clear();
    ETCS::collect_family<Animated_>("Animated", live);
    for (Animated_* a : live) if (a) a->Advance();
    return live.size();
}

#endif
