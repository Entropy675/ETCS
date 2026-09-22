#ifndef BASE_PRESENTABLE_H__
#define BASE_PRESENTABLE_H__
#include "Presentable.h"
#include "StepClock.h"
#include "AnimatedBase.h"

/*
 * THE RATE IS COUNTED HERE, ON THE WAY BACK OUT OF EVERY PRESENT.
 *
 * Present is written out rather than left to ETCS_DISPATCH_METHOD for exactly
 * one reason: the macro's forwarder has nowhere to put an "and then". A count
 * kept per backend, at the end of each PresentConcrete, is correct only for as
 * long as every backend remembers -- and the next one to be written is a WebGPU
 * path that has not been written yet. Counting on the way out of the family's
 * own forwarder is the one arrangement a new backend cannot get wrong, because
 * there is nothing for it to do.
 *
 * AVERAGED, NOT INSTANTANEOUS. A frame rate read off a single interval is a
 * number that swings by a third whenever the compositor hesitates, which reads
 * as a broken counter rather than as a busy frame. The exponential average
 * below settles over roughly the last twenty frames -- long enough to be
 * steady, short enough that a real slowdown shows up while it is still
 * happening.
 *
 * THE CEILING IS WHAT KEEPS A STALL OUT OF THE AVERAGE. Without one, a tab that
 * was hidden for a minute comes back and folds one frame at 0.016 fps into the
 * mean, which takes several seconds to wash out. StepClock's ceiling turns that
 * into "the slowest frame we are willing to believe in", and one of those has
 * almost no effect.
 */
/*
 * AND THE FRAME EDGE IS A STEP, NOT A THREAD -- which is why this base also
 * claims Animated.
 *
 * The obvious shape is a standing producer/consumer PAIR: one body paces and
 * writes a token, another reads it and does the walk, with a ring between them
 * to cross the thread boundary. Both are loops that never return, so each holds
 * a thread for the session -- and a produce body's thread comes out of the
 * ThreadPool (ETCS_MODULE_EXPORT_STREAM enqueues it), which is how a pool
 * acquires a MINIMUM size: an image with two standing producers and one worker
 * deadlocks, the second body queued behind one that never finishes. That is not
 * a tuning problem, it is a floor derived from whatever the script happens to
 * open. Nor is the split worth having the other way round -- pacing on one side
 * and the Vulkan submit on the other still leaves one loop holding a worker,
 * and the walk belongs on the presenting thread anyway.
 *
 * A LOOP HOLDS A STACK BETWEEN ITERATIONS; A STEP HOLDS NOTHING. That is the
 * whole difference, and it is why this needs no scheduler, no work-stealing and
 * no way to suspend a body mid-flight -- there is no stack to move, so any
 * thread may run the next step, and there is no thread boundary for a ring to
 * cross.
 *
 * ONE DEFINITION FOR EVERY BACKEND, here rather than per surface, for the same
 * reason the rate is: the next backend is a WebGPU path that has not been
 * written, and this is the arrangement it cannot get wrong. All a backend owes
 * is whether it is ready (below).
 */
ETCS_SUPERTYPE_BASE(Presentable), public AnimatedBase<Derived>
{
    ETCS_MAKE_INSTANCE(Presentable)

    virtual void PresentConcrete() = 0;

    /*
     * Can this thing present RIGHT NOW -- created, not retired, sized. The one
     * thing the family cannot answer for a backend, and a question rather than
     * a flag because every backend already knows. Asked once per visit, which
     * is all a settled or not-yet-created surface costs -- against a thread
     * parked in a wait loop until it could start.
     */
    virtual bool CanPresentConcrete() = 0;

    bool AnimatingConcrete() override
    { return static_cast<Derived*>(this)->CanPresentConcrete(); }

    /*
     * PACED HERE, so the interval is one number in one place instead of a
     * `sleep_for` inside a body. dt is measured (AnimatedBase), so a driver
     * ticking faster than the interval simply does not present on every visit
     * and one ticking slower is not corrected -- which is the honest behaviour:
     * the frame rate is whatever the driver and the work permit, and Fps()
     * reports what actually happened rather than what was asked for.
     *
     * ZERO IS UNPACED: present on every visit and let the driver set the rate,
     * which is the only honest way to ask how fast the pipeline can go.
     */
    void AdvanceConcrete(double dt_ms) override
    {
        if (m_interval_ms > 0.0)
        {
            m_owed_ms += dt_ms;
            if (m_owed_ms < m_interval_ms) return;
            m_owed_ms = 0.0;
        }
        static_cast<Derived*>(this)->RecomposeBound();
        Present();
    }

    // The pacing a script asked for, in ms. Zero means unpaced -- see above.
    void SetFrameInterval(double ms) { m_interval_ms = (ms < 0.0) ? 0.0 : ms; }
    double FrameInterval() const { return m_interval_ms; }

    void Present() override final
    {
        static_cast<Derived*>(this)->PresentConcrete();
        const double dt_ms = m_present_clock.Take();
        if (!(dt_ms > 0.0)) return;         // the first present has no interval
        const float inst = static_cast<float>(1000.0 / dt_ms);
        m_fps = (m_fps <= 0.0f) ? inst : (m_fps * 0.9f + inst * 0.1f);
    }

    // Not locked, and deliberately: this is written by the frame thread and by
    // nothing else, and a reader that gets the previous value has read a frame
    // rate one frame late -- which is what a frame rate is.
    float Fps() const override final { return m_fps; }

private:
    // A quarter second is four frames at fifteen: slower than that and the
    // number being shown is not a frame rate any more, it is a stall.
    StepClock m_present_clock{ 250.0 };
    float     m_fps = 0.0f;

    // ~60Hz by default: a placeholder for asking the swapchain about its
    // present mode.
    double    m_interval_ms = 16.0;
    double    m_owed_ms     = 0.0;
};

#endif
