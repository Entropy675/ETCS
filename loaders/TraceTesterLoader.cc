// TraceTesterLoader.cc
//
// THE SESSION FAMILIES, with no game in them: Trace -> Lockstep, and Present
// beside (ontology/Trace.h, Lockstep.h, Present.h). A leaf defined right here
// -- a counter whose time is ticks and whose lines change its rate -- stands
// in for a game, so what is under test is the families and nothing a game
// could get wrong. The streams are fed by hand, message by message, as the
// family's stream bodies (TraceBase.h) feed them: a seat's name and lines
// into the authority, the authority's lines out to a mirror.
//
//   1. ALONE. Practice opens with its own line, taken and not kept; Run steps
//      it; nobody's line is followed or taken.
//   2. THE AUTHORITY keeps every line it takes; ticks go in in batches and
//      always before any other line, so that line lands at the tick it was
//      applied at, and every snapshot is a `hash` line of the trace.
//   3. A SEAT. A stream of this runtime's, named by its first message, sits
//      (opening with the leaf's line, as that seat); the authority's own name
//      or none does not. A seat's lines are taken as it; the leaf refuses what
//      is not that seat's to say, and the family's own words (`tick`, `hash`,
//      `seat`) are nobody's to say -- refused lines are not kept. A mirror's
//      seat says its label and a nonce first, and its own lines are sent, not
//      applied.
//   4. A MIRROR fed the authority's trace from its first line is the
//      authority: the same tick, the same state, the same snapshot -- having
//      checked itself at every `hash` line. A mirror is itself followable.
//   5. DRIFT. A mirror whose state is not the authority's where a `hash` line
//      says replays its trace and is the authority again, untold; one the
//      lines do not decide (a leaf that is not deterministic) is told, once.
//   6. PRESENT. A seat arrives when it sits and departs when its stream ends;
//      the stream ending is also its leaving line. Here while any stream of
//      it is open; a departure from a session gone is nobody's.
//   7. STALE. A stream of the last session is refused once the copy is in
//      another; a trace ending under a copy that no longer mirrors it says
//      nothing.
//   8. A SEAT OVER A LINK IS ITS KEY'S. A seat stream carrying a proven peer
//      (SignalContext::peer, as a link stamps it) is authored as the key's
//      fingerprint whatever it called itself; the label is what people see,
//      and the mirror whose nonce it is learns that seat is its own. The same
//      key on a second stream takes the seat back without a second opening:
//      the first stream's lines and its end are then nobody's.
//
//   ./Run_TraceTesterLoader
//
// No display, no network: one process.

#include "../ETCS.h"

#include <cstdio>
#include <cstdlib>
#include <deque>
#include <sstream>
#include <string>
#include <vector>

static int g_fail = 0;
static void check(bool ok, const char* what)
{
    std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++g_fail;
}

// A counter: each tick it gains its rate; `rate <n>` is a seat's own line,
// `bump <n>` the owner's alone; a seat opens with `join` and goes with `leave`.
class Counter : public LockstepBase<Counter>,
                public PresentBase<Counter>,
                public DeletableBase<Counter>
{
public:
    WIRE_TYPE_IDENTITY(Counter)
    bool DeleteConcrete() override { return true; }

    // ── Trace ───────────────────────────────────────────────────────────
    bool ApplyConcrete(const std::string& author, const std::string& line) override
    {
        std::istringstream in(line);
        std::string verb; long n = 0;
        in >> verb;
        if (verb == "join") { for (auto& s : seats) if (s == author) return false; seats.push_back(author); return true; }
        if (verb == "leave") { for (auto i = seats.begin(); i != seats.end(); ++i) if (*i == author) { seats.erase(i); return true; } return false; }
        if (verb == "rate") { if (!(in >> n)) return false; rate = n; return true; }
        if (verb == "bump") { if (author != m_owner || !(in >> n)) return false; value += n; return true; }
        return false;
    }
    void RestartConcrete() override { value = 0; rate = 1; seats.clear(); }
    std::string HashConcrete() override { return std::to_string(value) + ":" + std::to_string(rate) + ":" + std::to_string(seats.size()); }
    std::string OpeningConcrete() override { return "join"; }
    std::string LeavingConcrete() override { return "leave"; }
    void EndedConcrete() override { ended = true; }
    void DiffersConcrete(uint64_t at, const std::string& mine, const std::string& theirs) override
    {
        (void)mine; (void)theirs;
        differed.push_back(at);
    }
    void ResyncedConcrete(uint64_t at) override { resynced.push_back(at); }

    // ── Lockstep ────────────────────────────────────────────────────────
    void StepConcrete() override { value += rate + (noisy ? 1 : 0); }
    Rate RateConcrete() const override { return { 50, 5, 2, 10 }; }

    // ── Present ─────────────────────────────────────────────────────────
    std::string KindConcrete() const override { return "counter"; }
    void ArrivedConcrete(const std::string& who) override  { arrived.push_back(who); }
    void DepartedConcrete(const std::string& who) override { departed.push_back(who); }

    // The test's hands: a line of this copy's own, its state and who it is, under the lock.
    std::string Me()                     { std::lock_guard<std::mutex> g(m_mu); return m_me; }
    std::string Label(const std::string& who) { std::lock_guard<std::mutex> g(m_mu); return labelLocked(who); }
    void Say(const std::string& line) { std::lock_guard<std::mutex> g(m_mu); proposeLocked(line); }
    void Frame(double ms)              { std::lock_guard<std::mutex> g(m_mu); clockLocked(ms); }
    std::string State()                { std::lock_guard<std::mutex> g(m_mu); return std::to_string(m_tick) + " " + HashConcrete(); }
    void Tamper()                      { std::lock_guard<std::mutex> g(m_mu); value += 1000; }
    void Noisy(bool on)                { std::lock_guard<std::mutex> g(m_mu); noisy = on; }

    long value = 0, rate = 1;
    std::vector<std::string> seats, arrived, departed;
    std::vector<uint64_t> differed, resynced;
    bool ended = false, noisy = false;   // noisy: a step the lines do not decide
};

// A seat's stream, by hand: its messages in order, then its end.
struct Fed
{
    std::deque<std::string> q;
    bool readMessage(std::string& m, size_t = 0)
    {
        if (q.empty()) return false;
        m = q.front(); q.pop_front();
        return true;
    }
};

// Everything `c` has kept from `from` on, as its Trace stream hands it.
static std::vector<std::string> lines(Counter* c, uint64_t from = 0)
{
    std::vector<std::string> out, batch;
    while (c->Lines(c->Generation(), from + out.size(), batch) && !batch.empty())
    {
        out.insert(out.end(), batch.begin(), batch.end());
        batch.clear();
    }
    return out;
}
// The lines without their numbers: "<author> <line>|...".
static std::string joined(const std::vector<std::string>& ls)
{
    std::string j;
    for (const auto& l : ls) j += l.substr(l.find(' ') + 1) + "|";
    return j;
}

int main()
{
    WIRE_CONTEXT();
    auto& arena = ETCS::MemoryArena::getInstance();
    std::printf("-- the session families --\n");

    Counter* host  = arena.allocate<Counter>();
    Counter* guest = arena.allocate<Counter>();
    Counter* third = arena.allocate<Counter>();
    check(host->getInterfacePointer(ETCS::Buffer("Trace")) && host->getInterfacePointer(ETCS::Buffer("Lockstep"))
          && host->getInterfacePointer(ETCS::Buffer("Present")),
          "one leaf claims Trace and Lockstep through one lineage, and Present beside it");

    // 1. Alone.
    std::printf("\n-- 1. alone --\n");
    host->Practice("solo");
    host->Run(10);
    check(host->seats.size() == 1 && host->State() == "10 10:1:1", "practice: its own opening taken, ten ticks stepped");
    check(lines(host).empty(), "...and nothing kept: nobody could follow it");
    check(host->Follow(host->Generation(), "0 someone rate 5") && host->rate == 1, "...and no trace followed");
    check(!host->Sit(host->Generation() + 1, "x", "x", "-"), "...and a seat of another session refused");

    // 2. The authority.
    std::printf("\n-- 2. the authority --\n");
    host->Host("h");
    check(host->State() == "0 0:1:1", "hosting starts from as made, with the host's seat");
    host->Run(3);
    host->Say("bump 100");
    host->Frame(1000.0 / 50 * 2 + 1);   // two ticks of wall time
    host->Run(1);
    const std::string kept = joined(lines(host));
    std::printf("        (trace: %s)\n", kept.c_str());
    check(kept == "h join|h tick 2|h tick 3|h bump 100|h tick 5|h tick 6|",
          "ticks are kept in pairs, and always before a line taken: it lands at its tick");

    // 3. A seat.
    std::printf("\n-- 3. a seat --\n");
    guest->Join("g", "h");
    std::string out; bool stale = false;
    check(guest->NextEmit(guest->Generation(), out, stale) && out.compare(0, 2, "g ") == 0 && out.size() == 18,
          "a mirror's seat says its label and a nonce first");
    const std::string nonce = out.substr(2);
    guest->Say("rate 3");
    check(guest->NextEmit(guest->Generation(), out, stale) && out == "rate 3" && guest->rate == 1,
          "...then its own lines, sent and not applied here");
    const uint64_t hg = host->Generation();
    const uint64_t tg = host->Sit(hg, "g", "g", nonce);
    check(tg && host->seats.size() == 2, "the seat sits, opening with `join` as itself");
    check(!host->Sit(hg, "h", "h", "-") && !host->Sit(hg, "", "", "-"), "...the host's name or none does not");
    const size_t before = lines(host).size();
    host->Take(hg, "g", tg, out);
    host->Take(hg, "g", tg, "bump 5");      // the owner's alone
    host->Take(hg, "g", tg, "tick 999");    // the family's
    host->Take(hg, "g", tg, "hash 1 x");    // the family's
    host->Take(hg, "g", tg, "seat 1 x");    // the family's
    host->Take(hg, "ghost", tg, "rate 9");  // no such seat
    const std::vector<std::string> after = lines(host, before);
    check(host->rate == 3 && host->value == 106 && host->State().compare(0, 2, "6 ") == 0 && joined(after) == "g rate 3|",
          "its own rate taken as it; the owner's bump, the family's words and an unseated name refused, and not kept");

    // 4. A mirror.
    std::printf("\n-- 4. a mirror --\n");
    host->Run(21);
    uint64_t fed = 0;   // the guest's place in the host's trace, as its follow keeps it
    auto follow = [&]() { for (const auto& l : lines(host, fed)) { guest->Follow(guest->Generation(), l); ++fed; } };
    follow();
    std::printf("        (host %s, guest %s)\n", host->State().c_str(), guest->State().c_str());
    check(host->State() == guest->State(), "the mirror fed the trace is the authority: the same tick, the same state");
    check(host->Snapshot() == guest->Snapshot() && host->Snapshot().compare(0, 3, "20 ") == 0 && guest->differed.empty(),
          "...the same snapshot, every ten ticks -- checked where the trace said it");
    check(joined(lines(guest)) == joined(lines(host)), "...and it kept what it followed: a mirror is followable");
    third->Join("t", "h");     // whose trace it is: the authority's, whoever hands it on
    for (const auto& l : lines(guest)) third->Follow(third->Generation(), l);
    check(third->State() == host->State(), "...and a mirror of the mirror is the authority too");

    // 5. Drift.
    std::printf("\n-- 5. drift --\n");
    guest->Tamper();
    host->Run(10);
    follow();
    check(guest->resynced.size() == 1 && guest->resynced[0] == 30 && guest->differed.empty() && guest->State() == host->State(),
          "a mirror not the authority where a `hash` line says replays its trace and is the authority again, untold");
    guest->Noisy(true);
    host->Run(10);
    follow();
    host->Run(10);
    follow();
    check(guest->differed.size() == 1 && guest->differed[0] == 40 && guest->resynced.size() == 1,
          "...one the lines do not decide is told, once");
    guest->Noisy(false);

    // 6. Present: the host's seat stream for g, by hand (etcs_trace_take).
    std::printf("\n-- 6. present --\n");
    const uint64_t pg = host->Arrive("g");
    const uint64_t pg_again = host->Arrive("g");
    check(pg != 0 && pg_again == pg && host->Here() == "g\n" && host->arrived == std::vector<std::string>{ "g" },
          "a seat arrives as it sits; a second stream of it at once is not a second arrival");
    host->Depart(pg, "g");
    check(host->departed.empty() && host->Here() == "g\n", "...nor is that stream's end a departure while the other is open");
    host->Depart(pg, "g");
    host->Rise(hg, "g", tg);
    check(host->departed == std::vector<std::string>{ "g" } && host->Here().empty(), "...and it departs as its last stream ends");
    check(host->seats.size() == 1 && joined(lines(host)).find("g leave|") != std::string::npos,
          "...whose end is its leaving line, as itself");
    const uint64_t pg2 = host->Arrive("g");
    host->Absent();     // a new session, as a game's Host begins one
    host->Host("h");
    host->Depart(pg2, "g");
    check(host->departed.size() == 1, "a departure from a session gone is nobody's");

    // 7. Stale.
    std::printf("\n-- 7. stale --\n");
    const uint64_t old_gen = guest->Generation();
    guest->Leave();
    check(!guest->Follow(old_gen, "999 h tick 999") && !guest->NextEmit(old_gen, out, stale) && stale
          && !host->Take(hg, "g", tg, "rate 1") && !host->Sit(hg, "g2", "g2", "-"),
          "a stream of the last session is refused, and told it is stale");
    guest->Ended(guest->Generation());
    check(!guest->ended, "...and a trace ending under a copy that no longer mirrors it says nothing");
    guest->Join("g", "h");
    guest->Ended(guest->Generation());
    check(guest->ended, "a trace ending under a mirror is said");

    // 8. A seat over a link.
    std::printf("\n-- 8. a seat over a link --\n");
    Counter* far  = arena.allocate<Counter>();
    Counter* mine = arena.allocate<Counter>();
    far->Host("h");
    mine->Join("pat", "h");
    Fed seat;
    while (mine->NextEmit(mine->Generation(), out, stale)) seat.q.push_back(out);
    mine->Say("rate 7");
    while (mine->NextEmit(mine->Generation(), out, stale)) seat.q.push_back(out);
    ETCS::SignalContext over;                    // as an edge stamps it (Edge.h)
    for (int i = 0; i < 32; ++i) over.peer.key[i] = uint8_t(0xa0 + i);
    char fp[17];
    over.peer.fingerprint(fp);
    etcs_trace_take(*far, seat, over);           // runs the stream to its end
    const std::string kept8 = joined(lines(far));
    std::printf("        (trace: %s)\n", kept8.c_str());
    const std::string k = fp;
    check(kept8.find(k + " join|" + k + " rate 7|" + k + " leave|") != std::string::npos && far->rate == 7
          && kept8.find(" pat join") == std::string::npos,
          "its lines are authored as its key's fingerprint, not as the name it gave");
    check(far->Label(k) == "pat", "...and that name is its label, for people");
    for (const auto& l : lines(far)) mine->Follow(mine->Generation(), l);
    check(mine->Me() == k && mine->Label(k) == "pat" && mine->State() == far->State(),
          "the mirror whose nonce it was knows the seat as its own, and is the authority");
    const uint64_t fg = far->Generation();
    const uint64_t ta = far->Sit(fg, k, "pat", "-"), tb = far->Sit(fg, k, "pat", "-");
    far->Take(fg, k, ta, "rate 1");
    far->Take(fg, k, tb, "rate 2");
    far->Rise(fg, k, ta);
    const std::string mid = joined(lines(far));
    far->Rise(fg, k, tb);
    const std::string end = joined(lines(far));
    auto count = [](const std::string& h, const std::string& n) { size_t c = 0; for (size_t at = h.find(n); at != std::string::npos; at = h.find(n, at + 1)) ++c; return c; };
    check(ta && tb && ta != tb && far->rate == 2 && count(mid, k + " join|") == 2 && count(mid, k + " leave|") == 1,
          "the same key on a second stream takes its seat back: one opening, the first stream's lines unheard");
    check(count(end, k + " leave|") == 2, "...the first stream's end is nobody's, the second's is the seat's");
    ETCS::MemoryArena::getInstance().deleteEntity(far, true);
    ETCS::MemoryArena::getInstance().deleteEntity(mine, true);

    for (Counter* c : { host, guest, third }) ETCS::MemoryArena::getInstance().deleteEntity(c, true);
    ETCS::PendingUnloadRegistry::getInstance().join_all();
    std::printf("\n%s (%d failure%s)\n", g_fail ? "FAILED" : "PASSED", g_fail, g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}
