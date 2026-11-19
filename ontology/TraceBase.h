#ifndef BASE_TRACE_H__
#define BASE_TRACE_H__
#include "Trace.h"
#include "Present.h"
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <set>
#include <thread>
#include <type_traits>
#include <vector>

/*
 * THE TRACE LIVES HERE: the lines kept, who this copy is and whose trace it
 * follows, the seats open into an authority, a mirror's outbox, and the
 * generation a stream is checked against. The leaf says only what a line
 * does, what "as made" is, and what its hash covers.
 *
 * ONE LOCK, AND IT IS THIS ONE. A leaf's lines change what its frame edge
 * reads, so the leaf's state and the trace's are under one mutex (m_mu):
 * every *Locked call here and in Lockstep expects it held, and every
 * Concrete is called with it held. Present's lock (beside) is taken before
 * it, never under it.
 *
 *   ApplyConcrete    one line, as its author; false refuses it (and an
 *                    authority keeps no line it refused)
 *   RestartConcrete  everything as made: what a session begins from
 *   HashConcrete     the traced state's hash (not what is drawn)
 *   OpeningConcrete  the line a seat opens with, taken as that seat
 *                    (default: none); the authority's own seat too
 *   LeavingConcrete  the line a seat's stream ending is taken as, as that
 *                    seat (default: none)
 *   EndedConcrete    the authority's trace ended under a mirror
 *   DiffersConcrete  a mirror is not the authority at `at`, said once
 *
 * A SEAT IS ITS STREAM, so it is named once, by its first message, and an
 * authority refuses a second stream under a name already sitting (or its
 * own). Nothing proves the name: what it buys is that a stream speaks for
 * one seat, and a seat's lines are refused whatever they say that is not
 * that seat's to say.
 */
ETCS_SUPERTYPE_BASE(Trace)
{
    ETCS_MAKE_INSTANCE(Trace)

    virtual bool        ApplyConcrete(const std::string& author, const std::string& line) = 0;
    virtual void        RestartConcrete() = 0;
    virtual std::string HashConcrete() = 0;
    virtual std::string OpeningConcrete() { return std::string(); }
    virtual std::string LeavingConcrete() { return std::string(); }
    virtual void        EndedConcrete() {}
    virtual void        DiffersConcrete(uint64_t at, const std::string& mine, const std::string& theirs) { (void)at; (void)mine; (void)theirs; }

    // ── sessions ───────────────────────────────────────────────────────
    // Alone: the authority of a trace nobody follows, so none is kept.
    void Practice(const std::string& name)
    {
        std::lock_guard<std::mutex> g(m_mu);
        beginLocked(name, name, true, false);
    }
    // The authority of a trace others may follow and sit in.
    bool Host(const std::string& name)
    {
        if (name.empty()) return false;
        std::lock_guard<std::mutex> g(m_mu);
        beginLocked(name, name, true, true);
        return true;
    }
    // A mirror of `owner`'s trace, from its first line; this copy's seat
    // is named `name` by its stream's first message.
    bool Join(const std::string& name, const std::string& owner)
    {
        if (name.empty() || owner.empty() || name == owner) return false;
        std::lock_guard<std::mutex> g(m_mu);
        beginLocked(name, owner, false, true);
        m_outbox.push_back(name);
        return true;
    }
    // Out of the session; the state stays where its last line left it.
    void Leave()
    {
        std::lock_guard<std::mutex> g(m_mu);
        nextGenerationLocked();
        m_me.clear(); m_owner.clear(); m_authority = false; m_session = false;
        m_seats.clear(); m_outbox.clear();
    }

    // ── Trace_ ─────────────────────────────────────────────────────────
    bool Follow(uint64_t gen, const std::string& msg) override
    {
        std::lock_guard<std::mutex> g(m_mu);
        if (gen != m_gen) return false;
        if (m_authority) return true;
        uint64_t seq = 0; std::string author, line;
        if (!ParseLine(msg, seq, author, line) || seq < m_lines.size()) return true;
        if (seq > m_lines.size())
        {
            ETCS_LOG("Trace", "line " << seq << " follows " << m_lines.size() << " -- a gap; this mirror stops here.");
            return false;
        }
        if (line.compare(0, 5, "hash ") == 0) checkLocked(line);
        else if (!applyLineLocked(author, line))
            ETCS_LOG("Trace", "line " << seq << " (" << author << " " << line << ") refused here -- this copy is not " << m_owner << "'s.");
        keepLocked(author, line);
        return true;
    }
    bool Lines(uint64_t gen, uint64_t from, std::vector<std::string>& out) override
    {
        std::unique_lock<std::mutex> g(m_mu);
        m_grew.wait_for(g, std::chrono::milliseconds(200), [&] { return gen != m_gen || m_lines.size() > from; });
        if (gen != m_gen) return false;
        for (uint64_t i = from; i < m_lines.size() && out.size() < kLinesAtOnce; ++i) out.push_back(m_lines[i]);
        return true;
    }
    bool Sit(uint64_t gen, const std::string& seat) override
    {
        std::lock_guard<std::mutex> g(m_mu);
        if (gen != m_gen || !m_authority || seat.empty() || seat == m_me || seat.find(' ') != std::string::npos
            || !m_seats.insert(seat).second) return false;
        const std::string open = OpeningConcrete();
        if (!open.empty()) takeLocked(seat, open);
        return true;
    }
    bool Take(uint64_t gen, const std::string& seat, const std::string& line) override
    {
        std::lock_guard<std::mutex> g(m_mu);
        if (gen != m_gen) return false;
        if (m_seats.count(seat) && !familyLineLocked(line)) takeLocked(seat, line);
        return true;
    }
    void Rise(uint64_t gen, const std::string& seat) override
    {
        std::lock_guard<std::mutex> g(m_mu);
        if (gen != m_gen || !m_seats.erase(seat)) return;
        const std::string leaving = LeavingConcrete();
        if (!leaving.empty()) takeLocked(seat, leaving);
    }
    bool NextEmit(uint64_t gen, std::string& out, bool& stale) override
    {
        std::lock_guard<std::mutex> g(m_mu);
        stale = gen != m_gen;
        if (stale || m_outbox.empty()) return false;
        out = std::move(m_outbox.front());
        m_outbox.pop_front();
        return true;
    }
    void Ended(uint64_t gen) override
    {
        std::lock_guard<std::mutex> g(m_mu);
        if (gen == m_gen && m_session && !m_authority) EndedConcrete();
    }
    uint64_t    Generation() override { std::lock_guard<std::mutex> g(m_mu); return m_gen; }
    std::string Owner() override      { std::lock_guard<std::mutex> g(m_mu); return m_owner; }
    std::string Snapshot() override   { std::lock_guard<std::mutex> g(m_mu); return std::to_string(m_snap_at) + " " + m_snap_hash; }

    // "<seq> <author> <line>" apart; false for anything else.
    static bool ParseLine(const std::string& msg, uint64_t& seq, std::string& author, std::string& line)
    {
        const size_t a = msg.find(' ');
        const size_t b = a == std::string::npos ? a : msg.find(' ', a + 1);
        if (b == std::string::npos) return false;
        seq = std::strtoull(msg.c_str(), nullptr, 10);
        author = msg.substr(a + 1, b - a - 1);
        line = msg.substr(b + 1);
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
        return !line.empty() && !author.empty();
    }

protected:
    // A line the family itself writes, which no seat may (Lockstep adds `tick`).
    virtual bool familyLineLocked(const std::string& line) const { return line.compare(0, 5, "hash ") == 0; }
    virtual bool applyLineLocked(const std::string& author, const std::string& line) { return ApplyConcrete(author, line); }
    // Before a line is kept by the authority (Lockstep: the ticks so far go first).
    virtual void beforeKeepLocked(const std::string& line) { (void)line; }
    virtual void restartLocked()
    {
        m_lines.clear();
        m_snap_at = 0; m_snap_hash = "-"; m_differed = false;
        RestartConcrete();
    }
    void nextGenerationLocked() { ++m_gen; m_grew.notify_all(); }

    // A line applied as `author` and kept: the authority's act.
    bool takeLocked(const std::string& author, const std::string& line)
    {
        beforeKeepLocked(line);
        if (!applyLineLocked(author, line)) return false;
        keepLocked(author, line);
        return true;
    }
    // This copy's own line: taken, by an authority; sent, by a mirror.
    void proposeLocked(const std::string& line)
    {
        if (m_authority) takeLocked(m_me, line);
        else if (m_session) m_outbox.push_back(line);
    }
    // Kept only in a session: alone, nobody could follow it.
    void keepLocked(const std::string& author, const std::string& line)
    {
        if (!m_session) return;
        if (m_authority) beforeKeepLocked(line);
        m_lines.push_back(std::to_string(m_lines.size()) + " " + author + " " + line);
        m_grew.notify_all();
    }
    // Where this copy stands at `at`; an authority says so, as a line.
    void snapLocked(uint64_t at)
    {
        m_snap_at = at;
        m_snap_hash = HashConcrete();
        if (m_authority) keepLocked(m_me, "hash " + std::to_string(at) + " " + m_snap_hash);
    }

    std::mutex  m_mu;
    uint64_t    m_gen = 0;
    std::string m_me, m_owner;       // this copy's name; whose trace it is
    bool        m_authority = false, m_session = false;
    uint64_t    m_snap_at = 0;
    std::string m_snap_hash = "-";
    std::vector<std::string> m_lines;   // the trace: "<seq> <author> <line>", seq the index

private:
    static constexpr size_t kLinesAtOnce = 512;

    void beginLocked(const std::string& me, const std::string& owner, bool authority, bool session)
    {
        nextGenerationLocked();
        m_me.clear(); m_owner.clear(); m_authority = false; m_session = false;
        m_seats.clear(); m_outbox.clear();
        restartLocked();
        m_me = me; m_owner = owner; m_authority = authority; m_session = session;
        if (!authority) return;
        const std::string open = OpeningConcrete();
        if (!open.empty()) takeLocked(me, open);
    }
    // A mirror at the authority's `hash <at> <hash>`: the same lines, so the same hash.
    void checkLocked(const std::string& line)
    {
        const size_t sp = line.find(' ', 5);
        if (sp == std::string::npos) return;
        const uint64_t at = std::strtoull(line.c_str() + 5, nullptr, 10);
        const std::string theirs = line.substr(sp + 1);
        m_snap_at = at;
        m_snap_hash = HashConcrete();
        if (m_snap_hash == theirs || m_differed) return;
        m_differed = true;
        DiffersConcrete(at, m_snap_hash, theirs);
    }

    std::condition_variable  m_grew;    // a line kept, or the generation moved
    std::set<std::string>    m_seats;
    std::deque<std::string>  m_outbox;  // a mirror's: its seat's name first, then its lines
    bool                     m_differed = false;
};

/*
 * THE STREAMS. Spelled once here; each verb is the leaf's, since a verb
 * belongs to its type's own module. A stream's edges are presence when the
 * leaf is also Present (ontology/Present.h, beside this family): a seat
 * arrives when it sits and departs when its stream ends, and a mirror's
 * owner is here for as long as the trace it follows.
 */
template <class Leaf>
inline uint64_t etcs_trace_arrive(Leaf& self, const std::string& who)
{
    if constexpr (std::is_base_of<Present_, Leaf>::value) return self.Arrive(who);
    else { (void)self; (void)who; return 0; }
}
template <class Leaf>
inline void etcs_trace_depart(Leaf& self, uint64_t pgen, const std::string& who)
{
    if constexpr (std::is_base_of<Present_, Leaf>::value) self.Depart(pgen, who);
    else { (void)self; (void)pgen; (void)who; }
}

// trace.Trace(<from>) -> ...: this copy's lines from `from`, then each as kept.
template <class Leaf, class Stream>
inline void etcs_trace_produce(Leaf& self, Stream& stream, ETCS::SignalContext ctx, uint64_t from)
{
    const uint64_t gen = self.Generation();
    std::vector<std::string> batch;
    while (!ctx.isInterrupted() && !ctx.isTerminated() && !stream.readerGone())
    {
        batch.clear();
        if (!self.Lines(gen, from, batch)) break;
        for (const auto& l : batch) { if (!stream.writeMessage(l)) return; ++from; }
    }
}
// theirs.Trace(0) -> mine.Follow(): a mirror, for as long as the trace lasts.
template <class Leaf, class Stream>
inline void etcs_trace_follow(Leaf& self, Stream& stream, ETCS::SignalContext ctx)
{
    const uint64_t gen = self.Generation();
    const std::string owner = self.Owner();
    const uint64_t pgen = etcs_trace_arrive(self, owner);
    std::string m;
    bool ended = true;
    while (!ctx.isInterrupted() && stream.readMessage(m, 1u << 20))
        if (!self.Follow(gen, m)) { ended = false; break; }
    etcs_trace_depart(self, pgen, owner);
    if (ended) self.Ended(gen);
}
// mine.Emit() -> theirs.Take(): a mirror's seat, its name and then its lines.
template <class Leaf, class Stream>
inline void etcs_trace_emit(Leaf& self, Stream& stream, ETCS::SignalContext ctx)
{
    const uint64_t gen = self.Generation();
    std::string line;
    bool stale = false;
    while (!ctx.isInterrupted() && !ctx.isTerminated() && !stream.readerGone())
    {
        if (!self.NextEmit(gen, line, stale))
        {
            if (stale) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        if (!stream.writeMessage(line)) break;
    }
}
// ... -> authority.Take(): a seat, from its name to its stream's end.
template <class Leaf, class Stream>
inline void etcs_trace_take(Leaf& self, Stream& stream, ETCS::SignalContext ctx)
{
    const uint64_t gen = self.Generation();
    std::string seat, line;
    if (!stream.readMessage(seat, 256) || !self.Sit(gen, seat)) return;
    const uint64_t pgen = etcs_trace_arrive(self, seat);
    while (!ctx.isInterrupted() && stream.readMessage(line, 1u << 16))
        if (!self.Take(gen, seat, line)) break;
    etcs_trace_depart(self, pgen, seat);
    self.Rise(gen, seat);
}

#endif // BASE_TRACE_H__
