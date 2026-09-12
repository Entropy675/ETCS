#ifndef SUPERTYPE_RESIZABLE_H__
#define SUPERTYPE_RESIZABLE_H__


#include "../core_defs.h"
#include "Observable.h"
#include <cstdint>
#include <mutex>
#include <vector>

// ---------------------------------------------------------------
// WindowSize
// ---------------------------------------------------------------

struct WindowSize
{
    uint32_t width;
    uint32_t height;

    float aspectRatio() const
    {
        if (height == 0) return 1.0f;
        return static_cast<float>(width) / static_cast<float>(height);
    }
};

// ---------------------------------------------------------------
// Resizable
// ---------------------------------------------------------------
//
// Size + resize notification -- split out of what used to be
// Window_ (see Window.h's own comment). Not window-specific: this
// is "a thing with a 2D size that changes and tells listeners" --
// RenderProvider's surfaces compose it at the ontology-family level
// (SurfaceBase.h) because swapchain recreation on resize is not an
// optional per-backend concern the way, say, Deletable is.

/*
 * HOW A FOLLOWER WANTS THE EDGE DELIVERED. Observable states the causal
 * structure -- there is something readable over here -- and stops there; what
 * an instance does about it is the instance's own business. This is that
 * choice, made once at the point of following.
 *
 * Polled: the follower asks, on its own tick. For anything with a clock, and
 * mandatory for anything whose ResizeTo must not run on a foreign thread --
 * VulkanSurface rebuilds a swapchain, so its resize belongs to the frame
 * thread and nowhere else.
 *
 * Pushed: the source calls PollResize on the follower once the size has
 * SETTLED. For a follower with no clock of its own -- a layout solver, which
 * is not in any frame loop and would otherwise never ask. Safe precisely
 * because the push carries no payload: it is a wake, and the follower still
 * reads the size itself, so a wake that arrives late still reads the current
 * value rather than replaying a stale one.
 */
enum class ResizeDelivery { Polled, Pushed };

// How many pump passes of quiet before a pushed follower is woken. Small
// enough to feel immediate, large enough that a drag is one wake and not
// sixty. See settleResize.
static constexpr int RESIZE_SETTLE_FRAMES = 3;

class Resizable_ : virtual public ETCS::Entity
{
public:
    virtual ~Resizable_() = default;

    virtual WindowSize GetSize() = 0;

    /*
     * BE this size. The verb half of the family, which until now was all
     * notification: a Resizable could say how big it is and tell you when
     * that changed, and there was no way to ask it to change.
     *
     * That gap is why a resize event reached a window, recreated its
     * swapchain, and left every surface and compositor beneath it at the
     * size they were spawned at. A listener could hear the new size; it
     * could not act on it without knowing the concrete type on the far end.
     *
     * Default false -- "I have a size but it is not mine to set" is a real
     * and common answer (a window's size belongs to the WM, an image's to
     * the file it came from), and it is the honest one for anything that has
     * not opted in. A caller checks the return rather than assuming.
     */
    virtual bool ResizeTo(WindowSize) { return false; }

    /*
     * Track `source`: whenever it resizes, so does this.
     *
     * THE EDGE IS THE SAME EITHER WAY; only the delivery differs, and the
     * follower picks it (see ResizeDelivery).
     *
     * This used to hand the source a ::std::function and the source used to run
     * it. What was wrong with that was never the pushing -- it was that the
     * push CARRIED THE SIZE. Carrying it meant a listener list to hold, an
     * ordering to define over that list, a coalescer at the far end because a
     * drag delivers sixty, and a callback running on whichever thread noticed.
     *
     * A resize is not a message. The size is already state on the source,
     * readable by anyone holding its RID, so the only thing worth sending is
     * that it moved -- one bit per observer. Once the payload is gone the
     * delivery question gets small: whoever wakes, whenever they wake, reads
     * the LATEST size rather than replaying an old one. That is what makes
     * both Polled and Pushed correct, and what makes deferring a push safe.
     *
     * Registered on the SOURCE because that is where the bit is set, and the
     * source is named by RID because the follower can outlive it.
     */
    void FollowResize(Resizable_* source, ResizeDelivery how = ResizeDelivery::Polled)
    {
        if (!source || source == this) return;
        if (ETCS::IWireObservable* o = etcs_observable_of(source))
            o->Observe(getRID());
        m_resizeSource = source->getRID();

        if (how == ResizeDelivery::Pushed)
        {
            ::std::lock_guard<::std::mutex> lock(source->m_pushMutex);
            for (ETCS::RID r : source->m_pushFollowers) if (r == getRID()) return;
            source->m_pushFollowers.push_back(getRID());
        }
    }

    /*
     * Has my source resized, and if so become that size. Called by the
     * follower on its own tick.
     *
     * Returns whether it acted, so a caller with work of its own to do on a
     * resize (rebuilding a swapchain) has the one bit it needs without
     * comparing sizes itself.
     *
     * A new observer starts dirty (ObservableBase), so the FIRST poll after
     * FollowResize always fires -- that is the initial layout, which the old
     * fire-on-registration bought by running the callback inline. Same
     * guarantee, taken on the follower's thread instead of the registrar's.
     */
    bool PollResize()
    {
        if (!m_resizeSource) return false;
        ETCS::Held<Resizable_> src = ETCS::resolve_held<Resizable_>("Resizable", m_resizeSource);
        if (!src) { m_resizeSource = 0; return false; }   // source gone: stop asking

        ETCS::IWireObservable* o = etcs_observable_of(static_cast<ETCS::Entity*>(src.get()));
        if (o && !o->TakeObserved(getRID())) return false;
        return ResizeTo(src->GetSize());
    }

    /*
     * One pump pass of the settle countdown. Called by whatever already pumps
     * this source's events -- GLFWPump::poll, via GLFWWindow::afterPoll.
     *
     * NOT A TICK, and worth being exact about that, because a tick is what
     * this design is trying not to need. Nothing new runs on a schedule: the
     * event pump was already going round every frame, and this rides it. A
     * source nobody pushes to, or one that has not resized, does nothing here
     * but read an int.
     *
     * DEFER, DON'T RATE-LIMIT. Every resize re-arms the counter, so a drag
     * defers the wake for as long as it lasts and delivers exactly one when it
     * stops -- rather than N wakes at some interval, each reading a size that
     * is already wrong. That is the same argument the cursor delta makes one
     * level down (GLFWWindow::PollEventsConcrete): coalesce to what a consumer
     * can actually use, not to what the hardware reports.
     *
     * The trailing edge is the one that matters and it cannot be lost, because
     * the counter only ever reaches zero after a pass in which nothing
     * re-armed it. There is no burst-ends-mid-window hole of the kind a
     * leading-edge debounce has.
     *
     * Returns whether this pass was the one that woke -- the pump ignores it,
     * the ontology tester drives the countdown with it.
     */
    bool settleResize()
    {
        ::std::vector<ETCS::RID> wake;
        {
            ::std::lock_guard<::std::mutex> lock(m_pushMutex);
            if (m_settle < 0) return false;       // idle
            if (--m_settle > 0) return false;     // still moving
            m_settle = -1;
            wake = m_pushFollowers;
        }
        for (ETCS::RID r : wake)
        {
            ETCS::Held<Resizable_> f = ETCS::resolve_held<Resizable_>("Resizable", r);
            if (f) f->PollResize();
        }
        return true;
    }

protected:
    /*
     * Record the new size and say so. Was a listener fan-out; the fan-out is
     * now the observers' own business (see FollowResize).
     *
     * Still named notify because it is still the announcement -- what changed
     * is that the announcement no longer carries the news, it points at it.
     * Nothing happens on this thread beyond a store, some bits, and re-arming
     * the countdown; the wake itself lands on the pump, one settled interval
     * later, and reads the size fresh when it does.
     */
    void notifyResize(WindowSize newSize)
    {
        m_size = newSize;
        etcs_mark_observed(this);
        ::std::lock_guard<::std::mutex> lock(m_pushMutex);
        if (!m_pushFollowers.empty()) m_settle = RESIZE_SETTLE_FRAMES;
    }

    WindowSize m_size = {};

private:
    ETCS::RID m_resizeSource = 0;

    // Followers that asked to be woken rather than to ask. Resizable's own
    // policy, deliberately not on the Observable edge: the edge says a change
    // happened, this says who wants to be told about it without asking.
    mutable ::std::mutex     m_pushMutex;
    ::std::vector<ETCS::RID> m_pushFollowers;
    int                    m_settle = -1;   // pump passes left; -1 = idle
};

#endif
