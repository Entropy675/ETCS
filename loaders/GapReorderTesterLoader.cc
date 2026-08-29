// gapreorder_test.cc -- GapReorderBuffer / TagMask behaviour suite.
//
// Standalone. Build:
//   g++ -std=c++17 -Wall -Wextra -fsanitize=address,undefined -o gapreorder_test gapreorder_test.cc
//   ./gapreorder_test
//
// The types under test are VERBATIM COPIES of core/Bundles.h's TagMask and
// core/EventStream.h's GapSlot/GapReorderBuffer, pasted below rather than
// included, because including EventStream.h pulls the whole ETCS world in for
// two structs that depend on nothing. That makes this file go stale if either
// is edited -- the GapSlot static_assert is the tripwire, and the block is a
// straight copy/paste to re-sync.
//
// The Rig below mirrors EventStream::ordering_loop, launch_slot, service and
// drain_completions. A result here is a claim about the real dispatch path only
// for as long as that stays true.
#include <cstdint>
#include <cstddef>
#include <type_traits>
#define ETCS_MAX_MODULE_TAGS 256
namespace ETCS {
static constexpr size_t GAP_DEPTH = 64;
static constexpr size_t TAG_BITS  = ETCS_MAX_MODULE_TAGS;
static constexpr size_t TAG_WORDS = (TAG_BITS + 63) / 64;
static_assert(TAG_BITS <= 256,
    "TAG_BITS beyond 256 exceeds the uint8_t index EventNode::tag_bit_index stores");
// TagMask -- fixed-width bitset. Above the MirrorBuffer include because
// MirrorBuffer carries two and Bundles is what pulls it in.
//
// Fixed width, never per-module: GapReorderBuffer is a member of EventStream,
// whose layout is load-bearing across dlopen (see DLInEvent::reply_to).
//
// TWO BIT SPACES SHARE THIS TYPE and must never be OR'd together:
//
//   TAG scope    -- meaningful only inside the EventNode whose tag_bit_index
//                   assigned it; module N's bit 2 and module M's bit 2 are
//                   unrelated types. Assigned by ETCS_TAG_DECLARE.
//   MODULE scope -- one bit per loaded module, from GetModuleBit. The only
//                   scope the loader's own reorder buffer can reason in.
//
// So a mask crossing to the loader is re-expressed, not forwarded --
// LoaderStream::OriginScopeMask. Every name here says which scope it is in.
struct TagMask
{
    uint64_t w[TAG_WORDS]{};

    static TagMask bit(size_t index)
    {
        TagMask m;
        if (index < TAG_BITS) m.w[index >> 6] = uint64_t(1) << (index & 63);
        return m;
    }
    // "Collides with everything." For events that change memory topology rather
    // than one type's data, and for any mask that could not be resolved --
    // acquire() only sees a dependency when BOTH sides are non-empty, so an
    // empty mask slips past a live unload as easily as past anything else.
    static TagMask all()
    {
        TagMask m;
        for (size_t i = 0; i < TAG_WORDS; ++i) m.w[i] = ~uint64_t(0);
        return m;
    }
    bool test(size_t index) const
    {
        return index < TAG_BITS && ((w[index >> 6] >> (index & 63)) & 1ull);
    }
    bool any() const
    {
        for (size_t i = 0; i < TAG_WORDS; ++i) if (w[i]) return true;
        return false;
    }
    // Early-out on the first overlapping word -- acquire() calls this once per
    // live slot per event, the one hot path on this type.
    bool intersects(const TagMask& o) const
    {
        for (size_t i = 0; i < TAG_WORDS; ++i) if (w[i] & o.w[i]) return true;
        return false;
    }
    TagMask& operator|=(const TagMask& o)
    {
        for (size_t i = 0; i < TAG_WORDS; ++i) w[i] |= o.w[i];
        return *this;
    }
    friend TagMask operator|(TagMask a, const TagMask& b) { a |= b; return a; }
    explicit operator bool() const { return any(); }
};
static_assert(std::is_trivially_copyable_v<TagMask>,
              "TagMask rides inside GapSlot across DSO boundaries");
struct GapSlot
{
    enum class Status : uint8_t { Empty, Pending, Running, Complete, Committed };
    uint64_t sequence         = 0;
    TagMask  tag_closure_mask{};
    Status   status           = Status::Empty;
    uint8_t  _pad[7];
    void*    result           = nullptr;
};
// 8 + (TAG_BITS/8) + 1 + 7 + 8. Was 32 back when the mask was a single
// word. Not padded to a cache line: there is exactly one consumer thread
// touching slots_, so there is no false sharing to buy off, and a denser
// array is better for acquire()'s own full scan.
static_assert(sizeof(GapSlot) == 24 + (TAG_BITS / 8), "GapSlot layout changed");
struct GapReorderBuffer
{
    alignas(64) uint64_t blocked_by_[GAP_DEPTH]{};
    GapSlot slots_[GAP_DEPTH]{};
    // acquire — reserve a slot and record what it must wait for. Leaves it
    // PENDING, a state that now survives: the caller asks blocked() and starts
    // the work only if the answer is no; otherwise the slot sits here holding
    // its event until service() launches it.
    //
    // An ADMISSION barrier, which is the point of the restructure. blocked_by_
    // used to be computed after the handler had run, so it could only delay the
    // EMISSION of a result that already happened -- invisible while every
    // handler was synchronous on one thread, worthless the moment one is not.
    // Same computation, moved ahead of dispatch.
    GapSlot& acquire(uint64_t sequence, const TagMask& tag_closure_mask)
    {
        size_t   idx  = sequence % GAP_DEPTH;
        GapSlot& slot = slots_[idx];
        slot.sequence         = sequence;
        slot.tag_closure_mask = tag_closure_mask;
        slot.status           = GapSlot::Status::Pending;
        slot.result           = nullptr;
        blocked_by_[idx]      = 0;
        for (size_t i = 0; i < GAP_DEPTH; ++i)
        {
            if (i == idx) continue;
            const GapSlot& other = slots_[i];
            if (other.status == GapSlot::Status::Empty ||
                other.status == GapSlot::Status::Committed) continue;
            if (other.tag_closure_mask.intersects(tag_closure_mask))
                blocked_by_[idx] |= (uint64_t(1) << i);
        }
        return slot;
    }
    bool blocked(uint64_t sequence) const
    {
        return blocked_by_[sequence % GAP_DEPTH] != 0;
    }
    // Before the handler, never after: service() sweeps for Pending, so a slot
    // still Pending across its own launch is launched again, forever.
    void mark_running(uint64_t sequence)
    {
        slots_[sequence % GAP_DEPTH].status = GapSlot::Status::Running;
    }
    // abandon — hand a slot back uncommitted, for DispatchKind::Drop, which has
    // no completion to order: its handler released its own caller and nothing
    // downstream reads a result. Clearing the bit from everyone else matters --
    // the slot was live from acquire() to here, so later arrivals may already
    // be recorded as blocked by it.
    void abandon(uint64_t sequence)
    {
        size_t idx = sequence % GAP_DEPTH;
        slots_[idx].status = GapSlot::Status::Empty;
        blocked_by_[idx]   = 0;
        const uint64_t my_bit = uint64_t(1) << idx;
        for (size_t j = 0; j < GAP_DEPTH; ++j) blocked_by_[j] &= ~my_bit;
    }
    bool complete(uint64_t sequence, void* result)
    {
        size_t   idx  = sequence % GAP_DEPTH;
        GapSlot& slot = slots_[idx];
        slot.result = result;
        slot.status = GapSlot::Status::Complete;
        return blocked_by_[idx] == 0;
    }
    // service — the engine. Two jobs that feed each other: committing a slot
    // can unblock a PENDING one (which, if synchronous, completes on the spot
    // and unblocks more) or a COMPLETE one. Hence the fixpoint, where this used
    // to be a single pass -- a missed pass was a latency blip for an emission;
    // for a LAUNCH it is work idling with nothing scheduled to notice.
    //
    // launch() must move the slot out of Pending before returning (see
    // mark_running) or the next sweep re-launches it.
    template<typename LaunchFn, typename EmitFn>
    void service(LaunchFn&& launch, EmitFn&& emit)
    {
        bool progress = true;
        while (progress)
        {
            progress = false;
            for (size_t i = 0; i < GAP_DEPTH; ++i)
            {
                GapSlot& slot = slots_[i];
                if (blocked_by_[i] != 0) continue;
                if (slot.status == GapSlot::Status::Pending)
                {
                    launch(slot);       // -> Running, Complete, or Empty (Drop)
                    progress = true;
                    continue;           // whatever it became, next sweep sees it
                }
                if (slot.status != GapSlot::Status::Complete) continue;
                const uint64_t my_bit = uint64_t(1) << i;
                for (size_t j = 0; j < GAP_DEPTH; ++j)
                    if (j != i) blocked_by_[j] &= ~my_bit;
                slot.status = GapSlot::Status::Committed;
                emit(slot);             // THE commit: this is what releases the
                slot.status = GapSlot::Status::Empty;   // blocked caller
                progress = true;
            }
        }
    }
};

}
#include <cstdio>
#include <vector>
#include <string>

using namespace ETCS;

static int failures = 0;
static void check(bool ok, const char* what)
{
    if (!ok) { ++failures; std::printf("  FAIL  %s\n", what); }
    else       std::printf("  ok    %s\n", what);
}
static void checkeq(const std::vector<uint64_t>& got,
                    const std::vector<uint64_t>& want, const char* what)
{
    bool ok = got == want;
    if (!ok)
    {
        ++failures;
        std::string g, w;
        for (auto x : got)  g += std::to_string(x) + " ";
        for (auto x : want) w += std::to_string(x) + " ";
        std::printf("  FAIL  %s\n          got  [%s]\n          want [%s]\n",
                    what, g.c_str(), w.c_str());
    }
    else std::printf("  ok    %s\n", what);
}

// Rig — EventStream::ordering_loop, launch_slot and drain_completions, reduced
// to the parts the buffer sees. Every method here mirrors one in EventStream.h;
// if that file's control flow changes, this stops being evidence about it.
enum class Kind { Inline, Async, Drop };

struct Rig
{
    GapReorderBuffer      rb;
    Kind                  parked_[GAP_DEPTH]{};   // stands in for EventStream::parked_
    std::vector<uint64_t> ran;                    // handler entry, in order
    std::vector<uint64_t> emitted;                // commit (= caller release), in order

    // EventStream::launch_slot
    void launch(uint64_t seq)
    {
        rb.mark_running(seq);
        ran.push_back(seq);
        switch (parked_[seq % GAP_DEPTH])
        {
            case Kind::Inline: rb.complete(seq, nullptr); break;
            case Kind::Async:  break;                       // worker will post
            case Kind::Drop:   rb.abandon(seq);  break;
        }
    }
    // EventStream::service
    void service()
    {
        rb.service([this](GapSlot& s) { launch(s.sequence); },
                   [this](GapSlot& s) { emitted.push_back(s.sequence); });
    }
    // ordering_loop's per-event body
    void arrive(uint64_t seq, const TagMask& m, Kind k)
    {
        parked_[seq % GAP_DEPTH] = k;
        rb.acquire(seq, m);
        if (!rb.blocked(seq)) launch(seq);
        service();
    }
    // drain_completions: an async worker's WorkResult coming back
    void finish(uint64_t seq) { rb.complete(seq, nullptr); service(); }

    bool pending(uint64_t seq) const
    { return rb.slots_[seq % GAP_DEPTH].status == GapSlot::Status::Pending; }
    bool empty(uint64_t seq) const
    { return rb.slots_[seq % GAP_DEPTH].status == GapSlot::Status::Empty; }
};

int main()
{
    const TagMask A  = TagMask::bit(1);
    const TagMask B  = TagMask::bit(2);
    const TagMask C  = TagMask::bit(200);   // second word -- exercises TAG_WORDS
    const TagMask AB = A | B;

    std::printf("\n-- 1. synchronous handlers under all(): unchanged behaviour\n");
    {
        // The migration case. Every stream that does not override mask_for gets
        // all(), so every slot blocks on every earlier one -- and because an
        // Inline handler completes before the next event is read, no slot is
        // ever actually occupied when the next one arrives. Strict FIFO, which
        // is what one ordering thread already did.
        Rig r;
        for (uint64_t s = 0; s < 5; ++s) r.arrive(s, TagMask::all(), Kind::Inline);
        checkeq(r.ran,     {0,1,2,3,4}, "five all() Inline events run in order");
        checkeq(r.emitted, {0,1,2,3,4}, "and commit in order");
    }

    std::printf("\n-- 2. Drop never occupies a slot past its handler\n");
    {
        Rig r;
        for (uint64_t s = 0; s < 3; ++s) r.arrive(s, TagMask::all(), Kind::Drop);
        checkeq(r.ran,     {0,1,2}, "all three ran");
        checkeq(r.emitted, {},      "and none committed -- Drop releases its own caller");
        check(r.empty(2), "slot is Empty, so nothing later can block on it");
    }

    std::printf("\n-- 3. ADMISSION: a blocked handler does not run\n");
    {
        // The whole restructure in one test. Under the old shape both handlers
        // ran immediately and the mask only reordered their emissions.
        Rig r;
        r.arrive(0, A, Kind::Async);
        r.arrive(1, A, Kind::Async);
        checkeq(r.ran, {0}, "the second same-type event has NOT run");
        check(r.pending(1), "it is parked Pending, holding its event");
        r.finish(0);
        checkeq(r.ran,     {0,1}, "and runs only once its blocker committed");
        checkeq(r.emitted, {0},   "1 is Async and still Running, so has not committed");
        r.finish(1);
        checkeq(r.emitted, {0,1}, "commits in order once its own work lands");
    }

    std::printf("\n-- 4. disjoint masks are admitted concurrently\n");
    {
        Rig r;
        r.arrive(0, A, Kind::Async);
        r.arrive(1, B, Kind::Async);
        r.arrive(2, C, Kind::Async);
        checkeq(r.ran, {0,1,2}, "all three run at once -- nothing intersects");
        r.finish(1); r.finish(2); r.finish(0);
        checkeq(r.emitted, {1,2,0}, "and commit as they finish, not as they arrived");
    }

    std::printf("\n-- 5. fixpoint: one commit cascades through a chain\n");
    {
        // A <- AB <- B, all Inline. Only A is admitted on arrival; the other two
        // park. When A commits, service() must launch AB, notice it completed,
        // commit it, launch B, and commit B -- all before returning. A single
        // pass would leave B sitting.
        Rig r;
        r.arrive(0, A,  Kind::Async);
        r.arrive(1, AB, Kind::Inline);
        r.arrive(2, B,  Kind::Inline);
        checkeq(r.ran, {0}, "only the head is admitted");
        check(r.pending(1) && r.pending(2), "both dependents parked");
        r.finish(0);
        checkeq(r.ran,     {0,1,2}, "one completion cascades the whole chain");
        checkeq(r.emitted, {0,1,2}, "committed in dependency order");
    }

    std::printf("\n-- 6. all() is a barrier in BOTH directions\n");
    {
        Rig r;
        r.arrive(0, A,             Kind::Async);
        r.arrive(1, TagMask::all(), Kind::Inline);
        check(r.pending(1), "all() waits on an unrelated live A");
        r.arrive(2, C,             Kind::Inline);
        check(r.pending(2), "and an unrelated C waits on the all() barrier");
        r.finish(0);
        checkeq(r.ran,     {0,1,2}, "strict order through the barrier");
        checkeq(r.emitted, {0,1,2}, "");
    }

    std::printf("\n-- 7. the empty mask is still fully permissive\n");
    {
        // Unchanged, and still the reason every default is all(): intersects()
        // needs BOTH sides non-empty, so an empty mask is admitted straight past
        // a barrier. Nothing in the new admission path closes this -- only the
        // shut defaults do.
        Rig r;
        r.arrive(0, TagMask::all(), Kind::Async);
        r.arrive(1, TagMask{},      Kind::Inline);
        checkeq(r.ran,     {0,1}, "empty mask admitted alongside an all() barrier");
        checkeq(r.emitted, {1},   "and commits ahead of it");
    }

    std::printf("\n-- 8. slot wrap-around at GAP_DEPTH\n");
    {
        Rig r;
        for (uint64_t s = 0; s < GAP_DEPTH; ++s) r.arrive(s, A, Kind::Inline);
        check(r.emitted.size() == GAP_DEPTH, "64 same-type Inline events committed");
        r.arrive(GAP_DEPTH, A, Kind::Async);
        check(r.ran.back() == GAP_DEPTH, "the wrapped slot admits -- no stale dependency");
    }

    std::printf("\n-- 9. a Drop arriving blocked still waits to run\n");
    {
        // Drop does not mean "unordered". It means "no completion to order".
        // Admission still applies, which is what lets a Drop-returning stream
        // adopt a real mask_for later without changing its handlers.
        Rig r;
        r.arrive(0, A, Kind::Async);
        r.arrive(1, A, Kind::Drop);
        checkeq(r.ran, {0}, "the Drop has not run");
        r.finish(0);
        checkeq(r.ran, {0,1}, "it runs once admitted");
        check(r.empty(1), "then abandons its slot");
    }

    std::printf("\n%s  (%d failure%s)\n",
                failures ? "FAILED" : "PASSED", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}