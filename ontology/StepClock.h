#ifndef ONTOLOGY_STEP_CLOCK_H__
#define ONTOLOGY_STEP_CLOCK_H__

#include <chrono>

/*
 * ── STEP CLOCK ───────────────────────────────────────────────────────────
 *
 * HOW LONG SINCE THE LAST TIME I ASKED, WITH A CEILING. That is the whole
 * type, and it is here because this tree had written it four times: twice
 * inside one class with different ceilings and a comment claiming they were
 * the same, once as a frame count in another module, and once more in the
 * family base below this file.
 *
 * NOT A FAMILY, and the distinction is the point of keeping it separate from
 * Animated. The family says an entity is DRIVEN -- somebody else decides when
 * it moves. This says an interval was MEASURED. Most driven things want both,
 * which is why AnimatedBase owns one; but a leaf whose step is caused by
 * something other than a driver still needs the measurement, and making it
 * claim a family to get one would be claiming to be advanced by a clock it
 * does not have. Scene3D is exactly that case: its steps are charged for by
 * being LOOKED AT, so an unobserved scene has had no interaction and owes
 * nothing -- a frame edge advancing it anyway would be a different model.
 *
 * THE CEILING IS A CAUSAL STATEMENT, not a smoothing filter. A stall -- a
 * swapped-out thread, a hidden tab, a breakpoint, a lid closing -- did not
 * contain a minute of motion that nobody saw; it contained no motion. Handing
 * over the whole wall-clock gap would throw a scene across the map or run
 * every animation to its end on the frame the tab comes back. A capped
 * interval loses time rather than sanity.
 *
 * DIFFERENT CEILINGS ARE A REAL CHOICE, which is why it is a constructor
 * argument rather than a constant. What a quarter second means to a fade and
 * what a second means to an entropy accounting are different claims about how
 * much unobserved time is credible, and two clocks in one class disagreeing on
 * it is fine -- what is not fine is the two disagreeing silently.
 *
 * MILLISECONDS, ALWAYS, and a seconds reading is the caller's own multiply.
 * One unit in the type means no call site has to be read twice to find out
 * which one it is in.
 */
class StepClock
{
public:
    // The default is a quarter of a second: long enough that no honest frame
    // reaches it, short enough that nothing lurches when one does.
    explicit StepClock(double cap_ms = 250.0)
        : m_cap_ms(cap_ms > 0.0 ? cap_ms : 0.0) {}

    /*
     * THE MEASUREMENT, AND IT CONSUMES. Reading this moves it -- the point of
     * asking is to be charged for the interval -- so it is deliberately not
     * named like a getter and deliberately not const. The first call answers
     * zero: there is no earlier mark to measure from, and inventing one would
     * make every clock begin with a step whose size depends on when the object
     * happened to be constructed.
     */
    double Take()
    {
        using clock = ::std::chrono::steady_clock;
        const clock::time_point now = clock::now();
        if (m_last == clock::time_point{}) { m_last = now; return 0.0; }
        double dt = ::std::chrono::duration<double, ::std::milli>(now - m_last).count();
        m_last = now;
        return (dt > m_cap_ms) ? m_cap_ms : dt;
    }

    /*
     * MOVE THE MARK WITHOUT BEING CHARGED. For the caller that is about to
     * decide it has nothing to do: without this, a thing that sits settled for
     * a minute and then starts would be handed that whole minute (capped) as
     * its first step, because the last mark is from before the settled period.
     * Whoever asks the "do I have anything to do" question first should keep
     * the clock moving while the answer is no.
     */
    void Skip()
    {
        m_last = ::std::chrono::steady_clock::now();
    }

    double CapMs() const { return m_cap_ms; }

private:
    double m_cap_ms;
    ::std::chrono::steady_clock::time_point m_last{};
};

#endif // ONTOLOGY_STEP_CLOCK_H__
