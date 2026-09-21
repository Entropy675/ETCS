#ifndef BASE_Animated_H__
#define BASE_Animated_H__
#include "Animated.h"
#include "StepClock.h"

/*
 * THE CLOCK LIVES HERE, and that is the whole reason this base exists.
 *
 * Every animated thing has to answer "how much has happened since last time",
 * and every one that answered it for itself answered it slightly differently:
 * this tree had the same measured-interval-with-a-ceiling written twice inside
 * one class (Scene3D's AdvanceForObserver and commitEntropy, with different
 * ceilings and a comment claiming they matched) and a frame-counted version in
 * another module. The measurement is StepClock now, beside this file, because
 * it is not the family: a leaf whose steps are caused by something other than a
 * driver needs the same clock without the same claim. What this base owns is
 * the DECISION to keep one, and the order it asks its two questions in.
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

#endif
