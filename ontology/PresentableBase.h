#ifndef BASE_PRESENTABLE_H__
#define BASE_PRESENTABLE_H__
#include "Presentable.h"
#include "StepClock.h"

/*
 * THE RATE IS COUNTED HERE, ON THE WAY BACK OUT OF EVERY PRESENT.
 *
 * Present is written out rather than left to ETCS_DISPATCH_METHOD for exactly
 * one reason: the macro's forwarder has nowhere to put an "and then". Each
 * backend used to note its own present at the end of its own PresentConcrete,
 * in two copies of the same six lines, which means the rate was correct only
 * for as long as every backend remembered -- and the next one to be written is
 * a WebGPU path that has not been written yet. Counting on the way out of the
 * family's own forwarder is the one arrangement a new backend cannot get wrong,
 * because there is nothing for it to do.
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
ETCS_SUPERTYPE_BASE(Presentable)
{
    ETCS_MAKE_INSTANCE(Presentable)

    virtual void PresentConcrete() = 0;

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
};

#endif
