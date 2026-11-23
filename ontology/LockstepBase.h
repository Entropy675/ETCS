#ifndef BASE_LOCKSTEP_H__
#define BASE_LOCKSTEP_H__
#include "Lockstep.h"
#include "TraceBase.h"
#include <sstream>

/*
 * Composes TraceBase: claiming Lockstep claims Trace, and a leaf naming
 * both would carry two traces and be ambiguous everywhere -- the
 * lineage-in-the-Base rule ThreadBase states for Threaded.
 *
 * THE TICK LIVES HERE: the count, the wall time the authority owes the
 * world, the ticks stepped but not yet kept as a line, and the snapshot
 * every few. A leaf's step sees m_tick already at the tick being stepped.
 *
 *   StepConcrete  one tick of the world, under the lock
 *   RateConcrete  the ticks a second, the most stepped in one frame, the
 *                 ticks per `tick` line, and the ticks between snapshots
 *
 * The leaf's frame edge calls clockLocked(dt_ms) with the lock held: on
 * the authority that is the world moving; on a mirror, nothing (its world
 * moves as the trace's ticks come in).
 */
ETCS_SUPERTYPE_BASE(Lockstep), public TraceBase<Derived>
{
    ETCS_MAKE_INSTANCE(Lockstep)

    struct Rate { int hz; int catch_up; uint64_t batch; uint64_t snap_every; };
    virtual void StepConcrete() = 0;
    virtual Rate RateConcrete() const = 0;

    uint64_t At() override { std::lock_guard<std::mutex> g(this->m_mu); return m_tick; }
    void Run(uint32_t ticks) override
    {
        std::lock_guard<std::mutex> g(this->m_mu);
        if (!this->m_authority) return;
        const Rate r = RateConcrete();
        for (uint32_t i = 0; i < ticks; ++i)
        {
            stepLocked();
            if (m_tick >= m_kept + r.batch) flushTicksLocked();
        }
        flushTicksLocked();
    }

protected:
    // The authority's frame edge: whole ticks of wall time, a few at most.
    void clockLocked(double dt_ms)
    {
        if (!this->m_authority) return;
        const Rate r = RateConcrete();
        const double tick_ms = 1000.0 / r.hz;
        m_wall_ms += dt_ms;
        int due = static_cast<int>(m_wall_ms / tick_ms);
        m_wall_ms -= due * tick_ms;
        if (due > r.catch_up) due = r.catch_up;
        for (int i = 0; i < due; ++i) stepLocked();
        if (m_tick >= m_kept + r.batch) flushTicksLocked();
    }
    void stepLocked()
    {
        ++m_tick;
        StepConcrete();
        const uint64_t every = RateConcrete().snap_every;
        if (every && m_tick % every == 0) this->snapLocked(m_tick);
    }
    void flushTicksLocked()
    {
        if (m_tick <= m_kept) return;
        m_kept = m_tick;
        this->keepLocked(this->m_me, "tick " + std::to_string(m_tick));
    }

    bool familyLineLocked(const std::string& line) const override
    {
        return line.compare(0, 5, "tick ") == 0 || TraceBase<Derived>::familyLineLocked(line);
    }
    bool applyLineLocked(const std::string& author, const std::string& line) override
    {
        if (line.compare(0, 5, "tick ") != 0) return TraceBase<Derived>::applyLineLocked(author, line);
        std::istringstream in(line.substr(5));
        uint64_t n = 0;
        in >> n;
        while (m_tick < n) stepLocked();
        return true;
    }
    void beforeKeepLocked(const std::string& line) override
    {
        if (line.compare(0, 5, "tick ") != 0) flushTicksLocked();
    }
    // Final: a leaf's own reset is RestartConcrete, which this reaches
    // through TraceBase. A leaf method of this name would replace it.
    void restartLocked() override final
    {
        m_tick = m_kept = 0;
        m_wall_ms = 0.0;
        TraceBase<Derived>::restartLocked();
    }

    uint64_t m_tick = 0;
    uint64_t m_kept = 0;         // the last tick a `tick` line said
    double   m_wall_ms = 0.0;    // the authority's wall time not yet a tick
};

#endif // BASE_LOCKSTEP_H__
