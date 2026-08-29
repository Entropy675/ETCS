#ifndef MIRRORBUFFER_H__
#define MIRRORBUFFER_H__
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <cassert>
#include <iostream>
#include <thread>
#include <type_traits>
#ifdef __x86_64__
#  include <immintrin.h>
#endif
#include "Buffer.h"
#include "SignalContext.h"
#include "SharedPage.h"
#include "LMAXSequentialSharedPage.h"

// TODO/Warn: currently wrap logic is untested, I see a potential bug within the LMAX path... (which is the most common)

namespace ETCS
{
// ---------------------------------------------------------------------------
// Strategy tag types — selection is a compile-time decision at the entity
// call site. The base makePair/teardownPair templates are intentionally left
// undefined; an invalid (Strategy, PageType) combination is a linker error,
// not a runtime assertion.
// ---------------------------------------------------------------------------
struct StrategyLMAX   {};   // in-process ring     — payload ptr in LBuffer slot, Buffer on call stack
struct StrategyPipe   {};   // same-machine OOP    — SharedPage staging buffer + fd pair
struct StrategySocket {};   // networked / qrexec  — SharedPage staging buffer + socket fd
// ---------------------------------------------------------------------------
// EntityLocale — runtime locale tag carried on Entity instances.
// Not used for compile-time strategy resolution (that is StrategyFor's job);
// retained for runtime inspection and logging.
// ---------------------------------------------------------------------------
enum class EntityLocale : uint8_t
{
    InProcess,
    OutOfProcess,
    Networked
};
// ---------------------------------------------------------------------------
// Remote — trivial base struct.
// Any tag type whose entity lives on a remote VM inherits from this.
// StrategyFor detects it via std::is_base_of — no macro changes required.
//
// Usage:
//   struct MyRemoteTag : ETCS::Remote { static constexpr const char* TAG = "MyRemote"; };
// ---------------------------------------------------------------------------
struct Remote {};
// ---------------------------------------------------------------------------
// IsRemote<T> — true_type iff T publicly inherits Remote.
// ---------------------------------------------------------------------------
template<typename T>
using IsRemote = std::is_base_of<Remote, T>;
// ---------------------------------------------------------------------------
// StrategyFor<CallerTag, CalleeTag> — compile-time transport selection.
//
// Priority order — Remote wins unconditionally so a remote entity of the
// same tag type is still routed over a socket, not a ring:
//
//   1. IsRemote<CalleeTag>                        → StrategySocket
//   2. is_same<CallerTag, CalleeTag> && !Remote   → StrategyLMAX
//   3. otherwise                                  → StrategyPipe
// ---------------------------------------------------------------------------
// Base case: different non-remote tags, same machine → Pipe
template<typename CallerTag, typename CalleeTag, typename = void>
struct StrategyFor { using type = StrategyPipe; };
// Same tag type, not remote → LMAX (in-process, pointer exchange)
template<typename CallerTag, typename CalleeTag>
struct StrategyFor<CallerTag, CalleeTag,
    std::enable_if_t<
        std::is_same<CallerTag, CalleeTag>::value &&
        !IsRemote<CalleeTag>::value>>
{ using type = StrategyLMAX; };
// Callee inherits Remote → Socket (beats same-type match)
template<typename CallerTag, typename CalleeTag>
struct StrategyFor<CallerTag, CalleeTag,
    std::enable_if_t<IsRemote<CalleeTag>::value>>
{ using type = StrategySocket; };
// ---------------------------------------------------------------------------
// PageFor<Strategy> — maps a strategy to its page type.
//
//   StrategyLMAX   → LMAXSequentialSharedPage
//                    Ring carries Buffer* pointers into the call stack frame.
//                    No payload copy — lifetime guaranteed by call() blocking
//                    on consumer completion before the frame unwinds.
//
//   StrategyPipe   → SharedPage
//                    Staging buffer for fd writes — decouples logical payload
//                    size from OS pipe buffer limits.
//
//   StrategySocket → SharedPage
//                    Same staging role as Pipe, for socket sends.
// ---------------------------------------------------------------------------
template<typename Strategy> struct PageFor;
template<> struct PageFor<StrategyLMAX>   { using type = LMAXSequentialSharedPage; };
template<> struct PageFor<StrategyPipe>   { using type = SharedPage; };
template<> struct PageFor<StrategySocket> { using type = SharedPage; };
// ---------------------------------------------------------------------------
// WireScope — a wrapper stage's own declaration of which transport
// strategies it applies to. Checked once per stage, at chain-resolution
// time (resolveWrapChain, DynamicLoader.h) — never per packet — since a
// wrapper's applicability to a given strategy is a fixed classification
// (Scope() is const), not something that varies call to call.
//
// NetworkOnly is the fit for anything whose whole purpose IS the wire
// (TLSWrapper, WebsocketWrapper) — LMAX never leaves the process, so
// wrapping it would be pure overhead with nothing to protect against.
// All is the fit for anything orthogonal to transport (access control,
// auditing) that should apply uniformly regardless of strategy.
// ---------------------------------------------------------------------------
enum class WireScope : uint8_t
{
    None        = 0,
    LMAX        = 1 << 0,
    Pipe        = 1 << 1,
    Socket      = 1 << 2,
    NetworkOnly = Pipe | Socket,
    All         = LMAX | Pipe | Socket,
};
// Hard ceiling on tag-stack depth for a single MirrorBuffer pair. Lives
// here (not Wrapper.h) since MirrorBuffer is what actually owns the fixed
// arrays this bounds — no allocation anywhere in this file, per its own
// long-standing rule, so this ceiling is load-bearing, not a style choice.
static constexpr size_t MAX_WRAP_STAGES = 8;
// One capability-identity entry in the wire manifest — a wrapper stage's
// own (module, tag) pair, exactly what a LoadEvent needs to bootstrap it
// on the far side. Built once, on the wrap (producer) side, from each
// surviving child's getSourceModule()/getSourceTag(); carried over the
// wire unconditionally (both packProducer and packConsumer serialize the
// same manifest) even though only the unwrap side ever actually consumes
// it for resolution — see resolveWrapChain's own comment (DynamicLoader.h)
// for why the wrap side re-derives its own chain directly from its own
// entity instead of trusting this copy.
struct WrapStageIdentity
{
    ETCS::Buffer module;
    ETCS::Buffer tag;
};
// ---------------------------------------------------------------------------
// MirrorBuffer
//
// Universal transport primitive. Strategy is fixed at makePair time.
//
// LMAX path (in-process):
//   writeRaw(Buffer&) — writes a Buffer* into a ring LBuffer slot.
//                       Buffer lives on the call() stack frame; no copy.
//   readRaw(Buffer&)  — reads the Buffer* from the ring slot and copies out.
//                       Safe because call() blocks until consumer returns.
//
// Pipe / Socket path (cross-process / networked):
//   writeRaw(Buffer&) — stages Buffer into SharedPage, flushes to fd in chunks.
//   readRaw(Buffer&)  — fills from fd into in_, copies one Buffer out.
//
// closeWrite() — signals EOF to the consumer on all three strategies.
//
// MirrorBuffer never allocates. teardownPair is the symmetric release point.
//
// ---------------------------------------------------------------------------
// SIGNAL AUTHORITY NOTE (applies to every blocking loop in this file)
//
// Every wait here tests ctx.isInterrupted() / ctx.isTerminated(), NOT
// `ctx.interrupt && *ctx.interrupt`. The direct-member form reads LOCAL
// authority only, which for a `run` scope or a detached job is a PER-JOB
// flag that Ctrl+C never touches -- the real &g_sig_int lives in
// interrupt_parent for any context more than zero hops from root. The
// is*() getters exist precisely to merge local and inherited authority
// (see SignalContext.h), and reading the raw member instead silently
// discards global signals for exactly the nested cases setParent's own
// comment describes. Every one of these loops previously read the local
// slot directly; if a new blocking wait is added below, it must use the
// getters too.
// ---------------------------------------------------------------------------
//
// ABI-boundary note (pack/unpack):
//   packProducer/packConsumer serialize this object's transport state into a
//   Buffer that crosses the DSO/thread-pool boundary; unpack() reconstructs a
//   *new* MirrorBuffer on the other side from that Buffer. For LMAX, the ring
//   pointer is round-tripped through the rid_or_ptr field. For Pipe/Socket,
//   the staging SharedPage* must ALSO be round-tripped through that same
//   field — it is not implied by the fds, and the stub-side object has no
//   other way to recover it. Losing this pointer leaves shared_page_ null on
//   the unpacked side and writeRaw/readRaw will assert.
//
// Wire-protocol note (wrap/unwrap):
//   A MirrorBuffer pair can carry an ordered stack of IWireWrapper stages —
//   TLS, WebSocket framing, access control, etc — resolved once at
//   makePair/unpack time (never per packet) and applied transparently
//   inside writeRaw/readRaw. From the developer's own stream-function
//   frame, the data is always plaintext going in and plaintext coming
//   out; wrap/unwrap are purely a transport-layer concern, symmetric by
//   construction (Wrap walks the chain in attach order, Unwrap walks it
//   in reverse). See resolveWrapChain's own out-of-line definition
//   (DynamicLoader.h) for the two genuinely different resolution paths
//   (direct entity walk on the wrap side, LoadEvent-bootstrapped
//   capability negotiation on the unwrap side) and WHY they differ.
//
//   LMAX + wrapping specifically: the ring's own zero-copy contract
//   (write a pointer into the CALLER's own stack Buffer, valid because
//   call() blocks until the consumer reads it) cannot survive a wrapper
//   that grows the payload — there is no "caller's own stack Buffer"
//   large enough, and no lifetime-safe place to put a bigger one that
//   writeRaw() itself could own locally (its own frame is long gone by
//   the time an async consumer gets to it). wrap_scratch_pool_ solves
//   this WITHOUT any new synchronization: this MirrorBuffer is the sole
//   producer for its pair (SPSC, same as the ring itself), so a local
//   monotonic counter (wrap_scratch_next_seq_) that advances exactly
//   once per successful ring write stays in lockstep with the ring's own
//   internal sequence_ counter. Indexing the scratch pool by that same
//   counter, masked the identical way the ring indexes its own slots
//   (& (slot_count_-1)), makes reusing a pool slot exactly as safe as
//   the ring reusing its OWN internal LBuffer slot at that index — both
//   are gated by the same "consumer must have markConsumed() the
//   previous occupant" backpressure write() already enforces. No
//   EventStream/custom-event ordering needed: the ring's existing
//   guarantee already covers it.
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// IWireWrapper — the ONLY thing MirrorBuffer knows about a wrapper stage.
//
// Deliberately NOT the real Wrapper_ (ontology/Wrapper.h, the ETCS_DISPATCH_
// METHOD-based CRTP family every concrete TLSWrapper/WebsocketWrapper
// actually derives from). Wrapper_ derives from Entity, and Entity is not
// a complete type anywhere in this file — MirrorBuffer.h is parsed WHILE
// Entity.h is still mid-definition (Entity.h -> Bundles.h -> MirrorBuffer.h),
// so naming Wrapper_ here is structurally impossible, not merely avoided
// by convention.
//
// This interface is what closes that gap: it has no Entity dependency at
// all, so it CAN be complete here. Wrapper_ additionally inherits from
// this (alongside its virtual Entity base) once Entity genuinely is
// complete, in ontology/Wrapper.h.
//
// No separate interface-pointer key or hand-written Wrapper_ constructor
// is needed: ETCS_MAKE_INSTANCE already registers a generic interface
// pointer under the family's own bare name ("Wrapper", same key every
// other Name##_ family gets) pointing at a Wrapper_* subobject address.
// That SAME stored pointer is reinterpreted as IWireWrapper* directly by
// resolveWrapChain/buildWrapManifest (DynamicLoader.h) -- safe PROVIDED
// IWireWrapper is declared as Wrapper_'s FIRST, non-virtual base: under
// the Itanium C++ ABI, the first non-virtual base subobject sits at
// offset 0, so a Wrapper_* and an IWireWrapper* pointing at the same
// object are bit-identical. This is a real dependency on Wrapper_'s own
// declared base order (ontology/Wrapper.h), not a style choice --
// getting it wrong produces a silently mis-adjusted pointer at runtime,
// not a compile error.
//
// Positioned here, immediately before MirrorBuffer itself, rather than up
// alongside WireScope/MAX_WRAP_STAGES — it's defined right where it's
// used, and nothing above this point in the file needs it.
// ---------------------------------------------------------------------------
struct IWireWrapper
{
    virtual ~IWireWrapper() = default;
    virtual void Wrap(ETCS::MBuffer& io, ETCS::SignalContext ctx) = 0;
    virtual void Unwrap(ETCS::MBuffer& io, ETCS::SignalContext ctx) = 0;
    virtual void Close(ETCS::MBuffer& io, ETCS::SignalContext ctx) { (void)io; (void)ctx; }
    virtual ETCS::WireScope Scope() const = 0;
};
struct MirrorBuffer
{
    // config carries the action's parameter payload (query string, args…).
    // in_/out_ are scratch buffers for Pipe/Socket operator<</>>; unused on LMAX.
    //
    // MBuffer, not Buffer — widened from the original 256B sizing so a
    // wrapped/framed wire payload (TLS record overhead, WS framing) has
    // headroom past a plaintext Buffer's own capacity. Strict widen only:
    // every un-wrapped caller (wrap_chain_len_ == 0) behaves byte-identically
    // to before, just with slack it didn't strictly need.
    MBuffer in_;
    MBuffer out_;
    Buffer  config;
private:
    int  read_fd_  = -1;
    int  write_fd_ = -1;
    LMAXSequentialSharedPage* lmax_page_    = nullptr;
    SharedPage*               shared_page_  = nullptr;
    bool         is_producer_   = true;
    uint64_t     writer_rid_    = 0;
    uint64_t     next_read_seq_ = 0;
    bool         debug_         = false;
    SignalContext bound_ctx_;
    enum class ActiveStrategy : uint8_t { LMAX, Pipe, Socket } active_ = ActiveStrategy::Pipe;
    // --- Wrap/unwrap chain state -------------------------------------------
    //
    // wrap_chain_ — resolved, LIVE IWireWrapper* stages, in WRAP order
    // (writeRaw walks 0..len_-1; readRaw walks len_-1..0, the reverse).
    // Populated exactly once, by resolveWrapChain (declared below, defined
    // in DynamicLoader.h where Entity/LoadEvent are complete), called from
    // unpack(). A fixed array, not a vector — no allocation, matching this
    // struct's own long-standing rule.
    //
    // wrap_manifest_ — the WIRE form: (module, tag) identities only, no
    // live pointers (a live pointer means nothing on the other side of a
    // real process boundary). Computed once on the wrap side (buildWrapManifest,
    // also declared below / defined in DynamicLoader.h, called from
    // makePair), mirrored onto BOTH producer and consumer objects, and
    // serialized by both packProducer/packConsumer identically.
    //
    // ephemeral_entities_ — entities this SPECIFIC MirrorBuffer instance
    // bootstrapped via LoadEvent during unpack()'s unwrap branch (never
    // populated on the wrap side, which uses entities it doesn't own).
    // Torn down in ~MirrorBuffer() via DestroyEvent — see that comment.
    IWireWrapper*     wrap_chain_[MAX_WRAP_STAGES]   = {};
    size_t            wrap_chain_len_                = 0;
    WrapStageIdentity wrap_manifest_[MAX_WRAP_STAGES];
    size_t            wrap_manifest_len_              = 0;
    Entity*           ephemeral_entities_[MAX_WRAP_STAGES] = {};
    size_t            ephemeral_count_                = 0;
    // LMAX-wrap only. Allocated once, from owner->getArena(), by
    // buildWrapManifest — see its own comment for why allocation has to
    // happen there (out-of-line, where Entity is complete) rather than
    // inline in makePair's own specialization. Sized to lmax_page_'s own
    // slot_count_ at allocation time; nullptr whenever no LMAX-scoped
    // wrapper is attached (the ordinary, overwhelmingly common case),
    // in which case writeRaw/readRaw's LMAX branch never touches it.
    //
    // wrap_scratch_next_seq_ is PRODUCER-SIDE-ONLY bookkeeping — see the
    // class-level wire-protocol comment above for why this stays in
    // lockstep with the ring's own internal sequence_ with no separate
    // synchronization needed.
    MBuffer* wrap_scratch_pool_     = nullptr;
    uint64_t wrap_scratch_next_seq_ = 0;
    // TAG SCOPE. The connected types' closures, from streamPairMask<P,C>
    // (Entity.h) -- the only place both types are known. Empty from the untyped
    // overload. The trampolines hand it to the ScopeTag, so the call's
    // active_scope flag orders against ops on EITHER side, not just self's.
    //
    // Tag bits crossing a DSO boundary is safe HERE only: TAG_MASK is assigned
    // solely by its own module's ETCS_TAG_DECLARE, so both halves reading
    // non-empty proves the caller declares both types -- one bit space on both
    // ends. Anything else reads empty and degrades to all().
    ETCS::TagMask pair_tag_mask_;
    // MODULE SCOPE. The same pair in loader bits, from
    // Entity::resolvePairModuleMask; all() if either side is unresolved. The
    // half that survives leaving the module: the trampolines publish it into
    // the thread-local PairScope for the call's duration, so an event stepping
    // up from inside carries both modules -- which is what stops an unload of
    // the far side commuting past a call still using it.
    ETCS::TagMask pair_module_mask_;
    // Set by resolveWrapChain's unwrap branch if any stage's LoadEvent
    // bootstrap fails (missing module/type — the far side lacks the
    // capability the manifest asked for). unpack() surfaces this as its
    // own bool return, which the DEFINE_STREAM_FUNC_PRODUCE/_CONSUME
    // trampolines (ETCS_API.h) check and bail on BEFORE ever invoking the
    // developer's own _implProduce_/_implConsume_ body.
    bool unwrap_failed_ = false;
    bool closed_ = false;
    const char* sideStr() const { return is_producer_ ? "PRODUCER" : "CONSUMER"; }
    static void setBlocking(int fd)
    {
        int flags = fcntl(fd, F_GETFL, 0);
        if (flags != -1) fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
    }
    static void setNonBlocking(int fd)
    {
        int flags = fcntl(fd, F_GETFL, 0);
        if (flags != -1) fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }
    // This strategy's own WireScope bit — the mask a stage's Scope()
    // result gets tested against at resolution time. No Entity/ontology
    // dependency at all, so (unlike resolveWrapChain/buildWrapManifest)
    // this stays inline here.
    WireScope currentScopeBit() const
    {
        switch (active_)
        {
            case ActiveStrategy::LMAX:   return WireScope::LMAX;
            case ActiveStrategy::Pipe:   return WireScope::Pipe;
            case ActiveStrategy::Socket: return WireScope::Socket;
        }
        return WireScope::None;
    }
    static bool scopeApplies(WireScope stageScope, WireScope active)
    {
        return (static_cast<uint8_t>(stageScope) & static_cast<uint8_t>(active)) != 0;
    }
    // Spin until the next LMAX frame is published, EOF is reached, or a
    // signal fires.
    //
    // The tombstone check is the LMAX equivalent of Pipe's read()==0 --
    // closeWrite() tombstones the ring, and without testing it here a
    // consumer whose producer has closed spins forever. That is exactly
    // what wedged ServeTree when Listen returned early on a bind()
    // failure: HTTPParser->HTTPParser resolves to StrategyLMAX, so the
    // Pipe EOF path (which does work) was never involved.
    //
    // Checked AFTER acquireRead, not before: a frame published
    // immediately prior to closeWrite() must still be delivered rather
    // than dropped, so the drain has to win the race against the
    // tombstone. This ordering requires that
    // LMAXSequentialSharedPage::acquireRead NOT assert on tombstoned --
    // reading already-published frames after close is legitimate. (The
    // matching assert in write() stays; writing after close IS an error.)
    const LBuffer* lmaxAcquireBlocking(const SignalContext& ctx)
    {
        int retries = 0;
        while (true)
        {
            const LBuffer* buf = lmax_page_->acquireRead(next_read_seq_);
            if (buf) return buf;
            if (lmax_page_->tombstoned.load(std::memory_order_acquire))
                return nullptr; // EOF -- producer closed and nothing left to drain
            if (ctx.isInterrupted() || ctx.isTerminated()) return nullptr;
            LMAXSequentialSharedPage::progressiveYield(retries);
        }
    }
public:
    MirrorBuffer() = default;
    MirrorBuffer(const Buffer& data, SignalContext ctx)
        : config(data), bound_ctx_(ctx) {}
    void bindContext(SignalContext ctx) { bound_ctx_ = ctx; }
    // -----------------------------------------------------------------------
    // makePair<Strategy, PageType> / teardownPair<Strategy, PageType>
    //
    // Primary templates intentionally undefined — only the three explicit
    // specializations below are valid. C++17 requires specializations to be
    // defined outside the class body in namespace scope.
    //
    // owner — the entity whose live addTag'd children get walked for
    // wrapper stages (buildWrapManifest, below). Optional and defaulted
    // to nullptr: every existing call site keeps compiling unchanged: a
    // null owner just means an empty wrap_manifest_ (no wrapping,
    // byte-identical behavior to before this parameter existed).
    // Deliberately NOT gated by Strategy — an All-scope wrapper (access
    // control, say) applies even to an LMAX pair, so the walk itself
    // must run regardless of which strategy this particular pair uses;
    // only each STAGE's own Scope() decides whether it survives into
    // wrap_manifest_ for THIS pair.
    // -----------------------------------------------------------------------
    template<typename Strategy, typename PageType>
    static void makePair(MirrorBuffer& producer,
                         MirrorBuffer& consumer,
                         PageType*     page,
                         uint64_t      writer_rid,
                         uint64_t      reader_rid,
                         Entity*       owner = nullptr);
    template<typename Strategy, typename PageType>
    static void teardownPair(MirrorBuffer& producer,
                             MirrorBuffer& consumer,
                             PageType*     page);
    // -----------------------------------------------------------------------
    // buildWrapManifest(owner) — walks owner's typed_child_order_ (via
    // getTypedChildren/getTypedChild), filters to children tagged
    // "Wrapper" whose Scope() includes this pair's own currentScopeBit(),
    // and records each survivor's (module, tag) identity into
    // wrap_manifest_/wrap_manifest_len_ on THIS object. Called once per
    // makePair, on the producer object; the caller then copies the
    // resulting array (and, for LMAX, wrap_scratch_pool_ — see below)
    // over to the consumer object directly (no reason to repeat the walk
    // — see makePair's own definition).
    //
    // LMAX-strategy responsibility, additionally: if the resulting
    // wrap_manifest_len_ > 0 AND active_ == ActiveStrategy::LMAX, this
    // ALSO allocates wrap_scratch_pool_ — a lmax_page_->slot_count_-sized
    // array of MBuffer, from owner->getArena() — since that allocation
    // needs Entity complete (getArena()) exactly like the manifest walk
    // does, and needs to happen exactly once, at the same call site, for
    // the identical out-of-line reason.
    //
    // Declared here, DEFINED in DynamicLoader.h — needs Entity complete
    // (getTypedChildren/getTypedChild/hasTag/getInterfacePointer/getArena),
    // which is structurally impossible in this file. Same out-of-line
    // split Module/LifetimeOwner/ScopeTag already use elsewhere in this
    // codebase, for the identical reason.
    // -----------------------------------------------------------------------
    void buildWrapManifest(Entity* owner);
    // -----------------------------------------------------------------------
    // resolveWrapChain(handler) — populates wrap_chain_/wrap_chain_len_
    // (and, on the unwrap side only, ephemeral_entities_/ephemeral_count_)
    // from wrap_manifest_, which by the time this runs has already been
    // deserialized off the wire inside unpack(). Branches on is_producer_:
    //
    //   Wrap side (is_producer_ == true): re-walks handler's OWN typed
    //   children directly (identical logic to buildWrapManifest, just
    //   resolving live IWireWrapper* instead of recording identities) —
    //   handler is already known, in-process, nothing to bootstrap.
    //   wrap_manifest_ itself is read from the wire here only for
    //   symmetry/uniformity; this branch does not actually need it, since
    //   the direct walk reproduces the identical, deterministic result.
    //
    //   Unwrap side (is_producer_ == false): treats wrap_manifest_ as a
    //   capability manifest and fires one LoadEvent per entry, in order,
    //   against a single shared, reused stack-local Root (safe to reuse
    //   across every stage: each LoadEvent's own attachModule call hands
    //   the lifetime token off to whatever it spawns, so the Root never
    //   ends up actually holding a module's token between stages). Any
    //   stage failing to load sets unwrap_failed_ and aborts the chain —
    //   this IS the graceful capability-negotiation failure path.
    //
    // Declared here, DEFINED in DynamicLoader.h — needs Entity complete
    // AND LoadEvent's real operator()() body, neither available here.
    // -----------------------------------------------------------------------
    void resolveWrapChain(Entity* handler);
    // -----------------------------------------------------------------------
    // writeRaw — public I/O surface, Buffer on all paths.
    //
    // LMAX:        writes Buffer* into a ring LBuffer slot. No payload copy.
    //              The Buffer must remain live until the consumer reads it —
    //              guaranteed by call() blocking on consumer completion.
    //
    // Pipe/Socket: stages Buffer into SharedPage, flushes to fd in chunks.
    //              When wrap_chain_len_ > 0, the plaintext Buffer is first
    //              copied into a wider local MBuffer and Wrap() is applied
    //              once per stage, in ATTACH order, before staging — so
    //              e.g. a [WebsocketWrapper, TLSWrapper] attach order
    //              frames the payload first, then encrypts the frame,
    //              exactly matching real wss:// layering.
    // -----------------------------------------------------------------------
    bool writeRaw(const Buffer& slot)
    {
        switch (active_)
        {
            case ActiveStrategy::LMAX:
            {
                assert(lmax_page_ && "writeRaw: null LMAX page");
                // Tombstone as EOF, NOT an assert. This is the exact mirror
                // of lmaxAcquireBlocking's own tombstone check (above) --
                // that one exists because the ring has no fd-level EOF for
                // the CONSUMER to discover a dead producer; this one exists
                // because it equally has none for the PRODUCER to discover a
                // dead consumer. Only the first half was ever implemented.
                //
                // On Pipe this direction is already handled and always was:
                // ::write to a closed fd fails, flushStaged returns false,
                // the producer's own loop unwinds. LMAX had an assert where
                // Pipe has an error path, so the identical situation aborted
                // the process instead of returning.
                //
                // Reachable by ordinary use now, not just by a bug: `kill`
                // on a consumer (Entity::interruptScopeAt) collapses the pair
                // while the producer is still looping. Aborting on a user
                // typing a valid command is not a defensible response.
                // Returning false is what a producer body already handles --
                // the same thing a full ring or an interrupted write returns.
                //
                // acquire, not relaxed: the tombstone is a happens-before
                // edge (everything the consumer did before closing must be
                // visible), and the old relaxed load could not carry that.
                if (lmax_page_->tombstoned.load(std::memory_order_acquire))
                    return false;
                if (wrap_chain_len_ == 0)
                {
                    // Fast path — byte-identical to before this feature
                    // existed: pointer into the CALLER's own stack Buffer.
                    // Lifetime guaranteed by the call() frame blocking
                    // contract, exactly as it always has been.
                    LBuffer ptr_slot;
                    const Buffer* ptr = &slot;
                    std::memcpy(ptr_slot.buf, &ptr, sizeof(Buffer*));
                    ptr_slot.written = sizeof(Buffer*);
                    int retries = 0;
                    while (lmax_page_->write(writer_rid_, ptr_slot) == UINT64_MAX)
                    {
                        if (bound_ctx_.isInterrupted() || bound_ctx_.isTerminated())
                            return false;
                        LMAXSequentialSharedPage::progressiveYield(retries);
                    }
                    return true;
                }
                // Wrap path — see this struct's own wire-protocol comment
                // for the full reasoning. The caller's own slot can't be
                // pointed at directly: Wrap() may grow it past
                // Buffer::bufsize, and in any case the result needs to
                // live somewhere the RING's own lifetime discipline
                // covers, not writeRaw()'s own stack frame.
                assert(wrap_scratch_pool_ &&
                    "writeRaw: LMAX pair has a wrap chain but no scratch pool -- "
                    "buildWrapManifest should have allocated one alongside it.");
                MBuffer staged;
                staged.writeRaw(slot.buf, slot.written);
                for (size_t i = 0; i < wrap_chain_len_; ++i)
                    wrap_chain_[i]->Wrap(staged, bound_ctx_);
                size_t scratch_idx = static_cast<size_t>(wrap_scratch_next_seq_)
                                    & static_cast<size_t>(lmax_page_->index_mask_);
                MBuffer& scratch = wrap_scratch_pool_[scratch_idx];
                scratch = staged;
                LBuffer ptr_slot;
                const MBuffer* ptr = &scratch;
                std::memcpy(ptr_slot.buf, &ptr, sizeof(MBuffer*));
                ptr_slot.written = sizeof(MBuffer*);
                int retries = 0;
                while (lmax_page_->write(writer_rid_, ptr_slot) == UINT64_MAX)
                {
                    if (bound_ctx_.isInterrupted() || bound_ctx_.isTerminated())
                        return false;
                    LMAXSequentialSharedPage::progressiveYield(retries);
                }
                ++wrap_scratch_next_seq_;
                return true;
            }
            case ActiveStrategy::Pipe:
            case ActiveStrategy::Socket:
            {
                assert(shared_page_ && "writeRaw: null staging SharedPage");
                if (wrap_chain_len_ == 0)
                    return stageAndFlush(slot); // fast path — byte-identical to pre-wrapper behavior
                MBuffer staged;
                staged.writeRaw(slot.buf, slot.written);
                for (size_t i = 0; i < wrap_chain_len_; ++i)
                    wrap_chain_[i]->Wrap(staged, bound_ctx_);
                return stageAndFlush(staged);
            }
        }
        return false;
    }
    // -----------------------------------------------------------------------
    // readRaw — public I/O surface, Buffer on all paths.
    //
    // LMAX:        reads Buffer* from ring slot, copies Buffer out, marks consumed.
    //              Blocks (spinning) until the frame is published, the ring
    //              is tombstoned (EOF), or a signal fires — see
    //              lmaxAcquireBlocking.
    //
    // Pipe/Socket: reads from fd, copies one Buffer out. Blocking by fd flags.
    //              When wrap_chain_len_ > 0, the wire frame is extracted
    //              into a wider local MBuffer first, Unwrap() is applied
    //              once per stage in REVERSE attach order (peeling layers
    //              off in the opposite order they went on), and only the
    //              final, fully-unwrapped plaintext is copied into the
    //              caller's Buffer& slot.
    // -----------------------------------------------------------------------
    bool readRaw(Buffer& slot)
    {
        switch (active_)
        {
            case ActiveStrategy::LMAX:
            {
                assert(lmax_page_ && "readRaw: null LMAX page");
                const LBuffer* lbuf = lmaxAcquireBlocking(bound_ctx_);
                if (!lbuf) return false;
                if (wrap_chain_len_ == 0)
                {
                    // Fast path — unchanged.
                    const Buffer* ptr = nullptr;
                    std::memcpy(&ptr, lbuf->buf, sizeof(Buffer*));
                    slot = *ptr;
                    lmax_page_->markConsumed(next_read_seq_);
                    ++next_read_seq_;
                    return true;
                }
                // Wrap path — the ring carries a pointer into
                // wrap_scratch_pool_ instead of the producer's own stack
                // Buffer (see writeRaw's mirror-image comment). Copy out
                // BEFORE markConsumed, same discipline the fast path
                // already follows, so the pool slot is free to be reused
                // by a later write the instant this call returns.
                const MBuffer* ptr = nullptr;
                std::memcpy(&ptr, lbuf->buf, sizeof(MBuffer*));
                MBuffer frame = *ptr;
                lmax_page_->markConsumed(next_read_seq_);
                ++next_read_seq_;
                for (size_t i = wrap_chain_len_; i-- > 0; )
                    wrap_chain_[i]->Unwrap(frame, bound_ctx_);
                if (frame.written > Buffer::bufsize)
                {
                    std::cerr << "[MirrorBuffer] readRaw: unwrapped LMAX payload ("
                              << frame.written << "B) exceeds Buffer capacity ("
                              << Buffer::bufsize << "B) -- cannot deliver to caller.\n";
                    return false;
                }
                slot.reset();
                slot.writeRaw(frame.buf, frame.written);
                return true;
            }
            case ActiveStrategy::Pipe:
            case ActiveStrategy::Socket:
            {
                if (wrap_chain_len_ == 0)
                    return fillAndCopyInto(slot, bound_ctx_); // fast path, unchanged
                MBuffer frame;
                if (!fillAndCopyInto(frame, bound_ctx_)) return false;
                for (size_t i = wrap_chain_len_; i-- > 0; )
                    wrap_chain_[i]->Unwrap(frame, bound_ctx_);
                if (frame.written > Buffer::bufsize)
                {
                    std::cerr << "[MirrorBuffer] readRaw: unwrapped payload ("
                              << frame.written << "B) exceeds Buffer capacity ("
                              << Buffer::bufsize << "B) -- cannot deliver to caller.\n";
                    return false;
                }
                slot.reset();
                slot.writeRaw(frame.buf, frame.written);
                return true;
            }
        }
        return false;
    }
    // -----------------------------------------------------------------------
    // closeWrite — signals EOF to the consumer.
    //
    // LMAX:   tombstones the ring; the consumer's lmaxAcquireBlocking sees
    //         it after its next failed acquireRead and returns nullptr.
    // Pipe:   closes write_fd_; consumer sees read() == 0.
    // Socket: shuts down send direction without closing the fd.
    //
    // Idempotent on every strategy -- calling it twice (e.g. a producer
    // that already closed explicitly, plus StreamWriteGuard below) is a
    // no-op the second time.
    // -----------------------------------------------------------------------
    void closeWrite()
    {
        // Stages get their terminator out BEFORE the transport goes away --
        // after the tombstone or the ::close there is nothing left to write
        // through. Forward order, matching Wrap: an outer stage's terminator
        // is itself payload to the stages beneath it.
        //
        // Guarded on wrap_chain_len_ so the ordinary no-wrapper case is
        // byte-identical to before, and on is_producer_ because a consumer's
        // write_fd_ is the Pipe ack channel, not this stream.
        if (wrap_chain_len_ > 0 && is_producer_ && !closed_)
        {
            for (size_t i = 0; i < wrap_chain_len_; ++i)
            {
                MBuffer tail;
                wrap_chain_[i]->Close(tail, bound_ctx_);
                if (tail.written == 0) continue;
                // Wrapped by every stage BELOW this one, exactly as ordinary
                // payload entering at this depth would be.
                for (size_t j = i + 1; j < wrap_chain_len_; ++j)
                    wrap_chain_[j]->Wrap(tail, bound_ctx_);
                stageAndFlush(tail);
            }
        }
        closed_ = true;
        switch (active_)
        {
            case ActiveStrategy::LMAX:
                if (lmax_page_) { lmax_page_->tombstone(); lmax_page_ = nullptr; }
                break;
            case ActiveStrategy::Pipe:
                if (write_fd_ != -1) { ::close(write_fd_); write_fd_ = -1; }
                break;
            case ActiveStrategy::Socket:
                if (write_fd_ != -1) ::shutdown(write_fd_, SHUT_WR);
                break;
        }
    }
    // Non-advancing liveness check.
    bool hasData()
    {
        switch (active_)
        {
            case ActiveStrategy::LMAX:
                return lmax_page_ != nullptr
                    && !lmax_page_->tombstoned.load(std::memory_order_acquire)
                    && lmax_page_->acquireRead(next_read_seq_) != nullptr;
            case ActiveStrategy::Pipe:
            case ActiveStrategy::Socket:
            {
                if (read_fd_ == -1) return false;
                if (in_.read_offset < in_.written) return true;
                in_.reset();
                ssize_t bytes = ::read(read_fd_, in_.buf, MBuffer::bufsize - 1);
                if (bytes > 0)
                {
                    in_.written    = static_cast<size_t>(bytes);
                    in_.buf[bytes] = '\0';
                    return true;
                }
                return false;
            }
        }
        return false;
    }
    bool isOpen() const
    {
        switch (active_)
        {
            case ActiveStrategy::LMAX:
                return lmax_page_ != nullptr
                    && !lmax_page_->tombstoned.load(std::memory_order_acquire);
            case ActiveStrategy::Pipe:
            case ActiveStrategy::Socket:
                return read_fd_ != -1 || write_fd_ != -1;
        }
        return false;
    }
    // Small-value streaming operators — Pipe/Socket operator<< flushes per call.
    // On LMAX accumulate into a Buffer and call writeRaw instead.
    template<typename T>
    MirrorBuffer& operator<<(const T& val)
    {
        out_ << val;
        flush(bound_ctx_);
        return *this;
    }
    template<typename T>
    MirrorBuffer& operator>>(T& val)
    {
        if (in_.read_offset >= in_.written)
            if (!fill(bound_ctx_)) return *this;
        in_ >> val;
        return *this;
    }
    // Pack transport Buffer for cross-ABI handoff into the stub macro.
    //
    // Layout (read by unpack on the other side of the ABI boundary):
    //   bool     is_lmax     — true on LMAX path
    //   bool     is_producer — true when packed by packProducer
    //   int      read_fd     — -1 on LMAX
    //   int      write_fd    — -1 on LMAX
    //   uint64_t rid_or_ptr  — LMAX ring pointer, or staging SharedPage*
    //                          on Pipe/Socket. Always meaningful — never 0
    //                          unless the page genuinely is null.
    //   uint64_t wrap_scratch_ptr — wrap_scratch_pool_'s address. 0 unless
    //                          this is an LMAX pair with a non-empty wrap
    //                          chain (see that member's own comment) —
    //                          Pipe/Socket never populate this field.
    //   int      wrap_count  — wrap_manifest_len_
    //   [string module, string tag] * wrap_count — the capability manifest,
    //                          quoted/token-delimited via the existing
    //                          std::string operator<</>>, so ordinary
    //                          space-delimited extraction on the far side
    //                          just works, same as every other field above it.
    //   Buffer   config      — action parameter payload, ALWAYS LAST — see
    //                          the raw write() below for why this one
    //                          field uses plain write() instead of operator<<.
    //
    // call() in Entity packs the consumer side via packConsumer (blocking)
    // and the producer side via packProducer (non-blocking). The stub macro
    // calls unpack on whichever Buffer it received; is_producer drives the
    // fd blocking mode so each side reconstructs the correct behaviour.
    // On LMAX blocking mode is irrelevant — the ring and lmaxAcquireBlocking
    // handle pacing on the consumer side.
    void packConsumer(MBuffer& transport) const
    {
        bool is_lmax = (active_ == ActiveStrategy::LMAX);
        transport << is_lmax << false; // is_producer = false
        transport << read_fd_ << write_fd_;
        transport << (is_lmax ? reinterpret_cast<uint64_t>(lmax_page_)
                               : reinterpret_cast<uint64_t>(shared_page_));
        transport << reinterpret_cast<uint64_t>(wrap_scratch_pool_);
        // Fixed-width, ahead of the variable-length manifest and of config,
        // which must stay terminal for restAsString().
        for (size_t i = 0; i < ETCS::TAG_WORDS; ++i) transport << pair_tag_mask_.w[i];
        for (size_t i = 0; i < ETCS::TAG_WORDS; ++i) transport << pair_module_mask_.w[i];
        packWrapManifest(transport);
        // Plain write(), not operator<<, for this last field specifically:
        // config is always the terminal field, and unpack() reconstructs
        // it via restAsString() -- a whole-remainder grab, not a
        // delimiter-scanned >> extraction the way every field above this
        // one is read. operator<<'s own trailing space (there to prime
        // the NEXT token-based >> extraction) has nothing after it to
        // serve that purpose here, and restAsString() has no delimiter-
        // stripping step of its own -- so that space was surviving
        // straight into the reconstructed config verbatim, corrupting
        // any consumer that reads its own config as a raw string rather
        // than token-by-token (e.g. a disk path).
        transport.write(config.c_str());
    }
    void packProducer(MBuffer& transport) const
    {
        bool is_lmax = (active_ == ActiveStrategy::LMAX);
        transport << is_lmax << true; // is_producer = true
        transport << read_fd_ << write_fd_;
        transport << (is_lmax ? reinterpret_cast<uint64_t>(lmax_page_)
                               : reinterpret_cast<uint64_t>(shared_page_));
        transport << reinterpret_cast<uint64_t>(wrap_scratch_pool_);
        // Fixed-width, ahead of the variable-length manifest and of config,
        // which must stay terminal for restAsString().
        for (size_t i = 0; i < ETCS::TAG_WORDS; ++i) transport << pair_tag_mask_.w[i];
        for (size_t i = 0; i < ETCS::TAG_WORDS; ++i) transport << pair_module_mask_.w[i];
        packWrapManifest(transport);
        transport.write(config.c_str()); // see packConsumer's own comment
    }
    // unpack — called by DEFINE_STREAM_FUNC_PRODUCE and DEFINE_STREAM_FUNC_CONSUME
    // stubs to rehydrate the MirrorBuffer from the packed transport Buffer.
    //
    // The strategy and all transport state were decided by makePair in call().
    // unpack is purely reconstructive — it reads what call() wrote and restores
    // the correct MirrorBuffer state for whichever side this stub represents.
    //
    // handler — the stub trampoline's own Entity* (same parameter
    // DEFINE_STREAM_FUNC_PRODUCE/_CONSUME already receive and pass through
    // as `self`/`handler`). Used only by resolveWrapChain's wrap-side
    // branch (direct child walk); the unwrap-side branch never touches it,
    // bootstrapping fresh entities instead — see resolveWrapChain's own
    // comment for why that asymmetry is correct rather than an oversight.
    //
    // Returns false iff resolveWrapChain's unwrap branch failed to
    // bootstrap some required capability (unwrap_failed_) — the caller
    // (the stub trampoline, ETCS_API.h) checks this and bails WITHOUT
    // ever invoking the developer's own _implProduce_/_implConsume_ body.
    bool unpack(MBuffer& transport, Entity* handler = nullptr)
    {
        bool     is_lmax        = false;
        bool     is_producer    = false;
        int      r              = -1;
        int      w              = -1;
        uint64_t rid_or_ptr     = 0;
        uint64_t wrap_scratch_p = 0;
        transport >> is_lmax >> is_producer >> r >> w >> rid_or_ptr >> wrap_scratch_p;
        is_producer_       = is_producer;
        next_read_seq_     = 0;
        wrap_scratch_pool_ = reinterpret_cast<MBuffer*>(wrap_scratch_p); // nullptr if 0, as intended
        for (size_t i = 0; i < ETCS::TAG_WORDS; ++i) transport >> pair_tag_mask_.w[i];
        for (size_t i = 0; i < ETCS::TAG_WORDS; ++i) transport >> pair_module_mask_.w[i];
        unpackWrapManifest(transport);
        config.writeString(transport.restAsString().c_str());
        if (is_lmax)
        {
            // Ring pointer packed by call() as uint64_t — safe because the
            // call() frame is still live (consumer stub blocks until complete,
            // producer stub runs on pool thread within that window).
            active_     = ActiveStrategy::LMAX;
            lmax_page_  = reinterpret_cast<LMAXSequentialSharedPage*>(rid_or_ptr);
            writer_rid_ = rid_or_ptr; // for ring slot attribution
            assert(lmax_page_ && "[MirrorBuffer] unpack: null LMAX ring pointer");
        }
        else
        {
            assert(r != -1 && w != -1 && "[MirrorBuffer] unpack: invalid fds");
            // Socket transport is fd-identical to Pipe at the unpack level;
            // strategy distinction only matters in teardownPair.
            active_      = ActiveStrategy::Pipe;
            shared_page_ = reinterpret_cast<SharedPage*>(rid_or_ptr);
            read_fd_     = r;
            write_fd_    = w;
            assert(shared_page_ && "[MirrorBuffer] unpack: null staging SharedPage");
            // Producer gets non-blocking on both ends — it writes and moves on.
            // Consumer gets blocking read so it waits for the producer's data.
            if (is_producer_) { setNonBlocking(read_fd_); setNonBlocking(write_fd_); }
            else              { setBlocking(read_fd_);    setNonBlocking(write_fd_); }
        }
        resolveWrapChain(handler);
        return !unwrap_failed_;
    }
    Buffer getConfig()     { return config; }
    // Set by Entity::call on both halves right after makePair, whose three
    // specializations don't know the tags -- threading them through to carry
    // two values would be worse than setting them alongside.
    void   setPairMasks(const ETCS::TagMask& tag_scope, const ETCS::TagMask& module_scope)
    { pair_tag_mask_ = tag_scope; pair_module_mask_ = module_scope; }
    const ETCS::TagMask& pairTagMask()    const { return pair_tag_mask_; }
    const ETCS::TagMask& pairModuleMask() const { return pair_module_mask_; }
    int    readFd()  const { return read_fd_; }
    int    writeFd() const { return write_fd_; }
    bool   isProducer() const { return is_producer_; }
    MirrorBuffer(const MirrorBuffer&)            = delete;
    MirrorBuffer& operator=(const MirrorBuffer&) = delete;
    MirrorBuffer(MirrorBuffer&& o) noexcept
        : in_(o.in_), out_(o.out_), config(o.config),
          read_fd_(o.read_fd_), write_fd_(o.write_fd_),
          lmax_page_(o.lmax_page_), shared_page_(o.shared_page_),
          is_producer_(o.is_producer_),
          writer_rid_(o.writer_rid_), next_read_seq_(o.next_read_seq_),
          debug_(o.debug_), bound_ctx_(o.bound_ctx_),
          active_(o.active_),
          wrap_chain_len_(o.wrap_chain_len_),
          wrap_manifest_len_(o.wrap_manifest_len_),
          ephemeral_count_(o.ephemeral_count_),
          wrap_scratch_pool_(o.wrap_scratch_pool_),
          wrap_scratch_next_seq_(o.wrap_scratch_next_seq_),
          pair_tag_mask_(o.pair_tag_mask_),
          pair_module_mask_(o.pair_module_mask_),
          unwrap_failed_(o.unwrap_failed_), 
          closed_(o.closed_)
    {
        for (size_t i = 0; i < wrap_chain_len_; ++i)      wrap_chain_[i] = o.wrap_chain_[i];
        for (size_t i = 0; i < wrap_manifest_len_; ++i)   wrap_manifest_[i] = o.wrap_manifest_[i];
        for (size_t i = 0; i < ephemeral_count_; ++i)     ephemeral_entities_[i] = o.ephemeral_entities_[i];
        // Nulling the transport handles on the source is what gives
        // StreamWriteGuard (below) its disarm-on-handoff semantics for
        // free: a producer that moves `stream` onward (Listen -> ListenState)
        // leaves a husk whose closeWrite() is a no-op, so the guard cannot
        // close a stream this object no longer owns.
        o.read_fd_     = -1;
        o.write_fd_    = -1;
        o.lmax_page_   = nullptr;
        o.shared_page_ = nullptr;
        // Ownership of any bootstrapped ephemeral entities moves with the
        // object — zero out the source so ~MirrorBuffer() on the moved-from
        // husk doesn't ALSO fire DestroyEvent for entities this object no
        // longer owns (double-destroy).
        o.ephemeral_count_   = 0;
        // wrap_scratch_pool_ is arena-owned (reclaimed with the arena, not
        // individually freed by this object), so nulling it on the source
        // isn't load-bearing the way ephemeral_count_ is above — done
        // anyway for hygiene, so a moved-from husk can't be mistaken for
        // still holding a usable pool.
        o.wrap_scratch_pool_ = nullptr;
        // Same disarm as the transport handles above: the husk must not emit
        // terminators the new owner is going to emit.
        o.closed_ = true;
    }
    // Declared here (not = default), DEFINED in DynamicLoader.h — needs
    // DestroyEvent, which needs EventNode.h complete, which is not
    // reachable from this file (EventNode.h -> Bundles.h -> MirrorBuffer.h
    // is the same circularity every other Entity-dependent piece in this
    // struct already routes around). Tears down every entity THIS
    // instance bootstrapped via LoadEvent during unpack()'s unwrap branch
    // — never the wrap side, which never owns what it points at.
    ~MirrorBuffer();
private:
    // -----------------------------------------------------------------------
    // stageAndFlush<N> — stages a TBuffer<N> payload (plaintext Buffer on
    // the fast path, or the wrapped MBuffer on the wrap path) into
    // shared_page_ as [size_t len][payload], then flushes to write_fd_.
    // Generic over N so both callers in writeRaw share one implementation
    // rather than duplicating the staging logic per payload width.
    // -----------------------------------------------------------------------
    template<size_t N>
    bool stageAndFlush(const TBuffer<N>& payload)
    {
        size_t len   = payload.written;
        size_t total = sizeof(size_t) + len;
        char* dest = shared_page_->acquireWrite(static_cast<long long>(total));
        if (!dest)
        {
            if (debug_) ETCS_LOG("MirrorBuffer", "[PRODUCER] Staging page full.");
            return false;
        }
        std::memcpy(dest, &len, sizeof(size_t));
        if (len > 0) std::memcpy(dest + sizeof(size_t), payload.buf, len);
        return flushStaged(bound_ctx_);
    }
    // Drain shared_page_ to write_fd_ in chunks until empty or signal fires.
    bool flushStaged(const SignalContext& ctx)
    {
        if (write_fd_ == -1)
        {
            std::cerr << "[" << sideStr() << "] flushStaged: write_fd is -1\n";
            return false;
        }
        long long   total = shared_page_->written.load(std::memory_order_acquire);
        if (total == 0) return true;
        const char* src  = shared_page_->buffer;
        long long   sent = 0;
        while (sent < total)
        {
            ssize_t n = ::write(write_fd_, src + sent, static_cast<size_t>(total - sent));
            if (n > 0) { sent += n; continue; }
            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                if (ctx.isInterrupted() || ctx.isTerminated()) return false;
                continue;
            }
            std::cerr << "[" << sideStr() << "] flushStaged write error: "
                      << std::strerror(errno) << "\n";
            return false;
        }
        shared_page_->reset();
        return true;
    }
    // -----------------------------------------------------------------------
    // tryExtractInto<N> — attempts to extract a length-prefixed frame from
    // in_ into `out` (a TBuffer<N> of whatever width the caller needs —
    // plaintext Buffer on the fast path, wider MBuffer on the wrap path,
    // since a wrapped/framed wire payload can exceed Buffer::bufsize even
    // though the plaintext underneath it does not). Returns true if
    // successful, false if more data is needed.
    //
    // Generic replacement for the old Buffer-only tryExtract — same logic,
    // parameterized over the output width so wrapping doesn't need a
    // second, hand-duplicated copy of this function.
    // -----------------------------------------------------------------------
    template<size_t N>
    bool tryExtractInto(TBuffer<N>& out)
    {
        if (in_.read_offset + sizeof(size_t) > in_.written) return false;
        size_t len = 0;
        std::memcpy(&len, in_.buf + in_.read_offset, sizeof(size_t));
        in_.read_offset += sizeof(size_t);
        if (in_.read_offset + len > in_.written)
        {
            in_.read_offset -= sizeof(size_t); // Rollback and wait for more data
            return false;
        }
        if (len > N)
        {
            std::cerr << "[MirrorBuffer] tryExtractInto: wire frame (" << len
                      << "B) exceeds output capacity (" << N << "B).\n";
            in_.read_offset += len; // still consume it -- stream stays in sync
            out.reset();
            return false;
        }
        out.reset();
        if (len > 0)
        {
            std::memcpy(out.buf, in_.buf + in_.read_offset, len);
            out.written = len;
        }
        in_.read_offset += len;
        return true;
    }
    // Fill in_ from fd, extract one frame into `out`. Handles partial
    // reads cleanly. Generic replacement for the old Buffer-only
    // fillAndCopy, for the identical reason tryExtractInto is generic.
    template<size_t N>
    bool fillAndCopyInto(TBuffer<N>& out, const SignalContext& ctx)
    {
        // Fast path: we already buffered enough data from a previous read
        if (tryExtractInto(out)) return true;
        if (read_fd_ == -1)
        {
            std::cerr << "[" << sideStr() << "] fillAndCopyInto: read_fd is -1\n";
            return false;
        }
        while (true)
        {
            // Compact the buffer: move any unread trailing data to the front
            if (in_.read_offset > 0 && in_.written > in_.read_offset)
            {
                size_t remaining = in_.written - in_.read_offset;
                std::memmove(in_.buf, in_.buf + in_.read_offset, remaining);
                in_.written = remaining;
                in_.read_offset = 0;
            }
            else if (in_.read_offset >= in_.written)
            {
                in_.reset();
            }
            // Calculate available space at the end of the buffer
            size_t space = MBuffer::bufsize - 1 - in_.written;
            if (space == 0) return false; // Buffer full but still can't extract? Stream corrupt.
            // Read a chunk from the fd
            ssize_t bytes = ::read(read_fd_, in_.buf + in_.written, space);
            if (bytes > 0)
            {
                in_.written += static_cast<size_t>(bytes);
                in_.buf[in_.written] = '\0';
                // Try to extract now that we have more data
                if (tryExtractInto(out)) return true;
                continue; // Need more data to complete the frame
            }
            if (bytes == 0)
            {
                ETCS_LOG("MirrorBuffer", "[" << sideStr() << "] Read EOF.");
                return false;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                if (ctx.isInterrupted() || ctx.isTerminated()) return false;
                continue;
            }
            std::cerr << "[" << sideStr() << "] fillAndCopyInto read error: "
                      << std::strerror(errno) << "\n";
            return false;
        }
    }
    // packWrapManifest / unpackWrapManifest — the wire form of
    // wrap_manifest_. Token-delimited via the existing std::string
    // operator<</>> (quoted, space-terminated), so this slots in ahead of
    // config's own raw, unconditional write()/restAsString() pair without
    // disturbing that field's own delimiter-free contract.
    void packWrapManifest(MBuffer& transport) const
    {
        transport << static_cast<int>(wrap_manifest_len_);
        for (size_t i = 0; i < wrap_manifest_len_; ++i)
        {
            transport << wrap_manifest_[i].module.toString();
            transport << wrap_manifest_[i].tag.toString();
        }
    }
    void unpackWrapManifest(MBuffer& transport)
    {
        int count = 0;
        transport >> count;
        wrap_manifest_len_ = 0;
        for (int i = 0; i < count; ++i)
        {
            std::string mod, tag;
            transport >> mod >> tag;
            if (wrap_manifest_len_ < MAX_WRAP_STAGES)
            {
                wrap_manifest_[wrap_manifest_len_].module = ETCS::Buffer(mod.c_str());
                wrap_manifest_[wrap_manifest_len_].tag    = ETCS::Buffer(tag.c_str());
                ++wrap_manifest_len_;
            }
            else
            {
                std::cerr << "[MirrorBuffer] unpackWrapManifest: manifest entry "
                          << i << " dropped -- MAX_WRAP_STAGES (" << MAX_WRAP_STAGES
                          << ") exceeded.\n";
            }
        }
    }
    bool flush(const SignalContext& ctx)
    {
        if (write_fd_ == -1) return false;
        if (out_.written == 0) return true;
        while (true)
        {
            ssize_t n = ::write(write_fd_, out_.buf, out_.written);
            if (n > 0) { out_.reset(); return true; }
            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                if (ctx.isInterrupted() || ctx.isTerminated()) return false;
                continue;
            }
            std::cerr << "[" << sideStr() << "] flush error: "
                      << std::strerror(errno) << "\n";
            return false;
        }
    }
    bool fill(const SignalContext& ctx)
    {
        if (read_fd_ == -1) return false;
        while (true)
        {
            in_.reset();
            ssize_t bytes = ::read(read_fd_, in_.buf, MBuffer::bufsize - 1);
            if (bytes > 0)
            {
                in_.written    = static_cast<size_t>(bytes);
                in_.buf[bytes] = '\0';
                return true;
            }
            if (bytes == 0)
            {
                ETCS_LOG("MirrorBuffer", "[" << sideStr() << "] Read EOF (pipe closed).");
                return false;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                if (ctx.isInterrupted() || ctx.isTerminated()) return false;
                continue;
            }
            std::cerr << "[" << sideStr() << "] fill error: "
                      << std::strerror(errno) << "\n";
            return false;
        }
    }
};
// ---------------------------------------------------------------------------
// StreamWriteGuard — closes a producer's write end when its body returns by
// ANY path: normal completion, an early `return`, or an exception.
//
// Constructed by DEFINE_STREAM_FUNC_PRODUCE's own trampoline (ETCS_API.h),
// around the developer's body, for the same reason ScopeTag is auto-injected
// there: "the write end is closed when the producer finishes" is a contract
// of the produce/consume pairing, not something each producer author should
// have to re-implement correctly on every branch.
//
// The gap this closes was real and reproduced: HTTPParser::Listen has three
// early returns (socket/bind/listen failure) and none of them closed the
// stream, so a bind() failure left ServeTree blocked forever on a producer
// that had already given up. ProduceResponse had the identical defect on its
// own interrupt branch.
//
// Disarm is automatic, via MirrorBuffer's move constructor: a producer that
// hands ownership onward (Listen moving `stream` into an arena-allocated
// ListenState) leaves behind a husk with lmax_page_/write_fd_ nulled, so this
// destructor's closeWrite() is a no-op and the new owner's own teardown stays
// authoritative. No explicit release() call needed, and nothing to forget.
//
// Producers that already call closeWrite() explicitly keep working unchanged
// -- closeWrite() is idempotent on all three strategies.
// ---------------------------------------------------------------------------
struct StreamWriteGuard
{
    MirrorBuffer& s;
    explicit StreamWriteGuard(MirrorBuffer& m) noexcept : s(m) {}
    ~StreamWriteGuard() { s.closeWrite(); }
    StreamWriteGuard(const StreamWriteGuard&)            = delete;
    StreamWriteGuard& operator=(const StreamWriteGuard&) = delete;
    StreamWriteGuard(StreamWriteGuard&&)                 = delete;
    StreamWriteGuard& operator=(StreamWriteGuard&&)      = delete;
};
// ===========================================================================
// makePair explicit specializations — namespace scope (C++17).
// ===========================================================================
// ---- StrategyLMAX -----------------------------------------------------------
// Ring carries Buffer* pointers — no payload copy.
// Buffer lifetime is the call() stack frame, which blocks until consumer returns.
template<>
inline void MirrorBuffer::makePair<StrategyLMAX, LMAXSequentialSharedPage>(
    MirrorBuffer&             producer,
    MirrorBuffer&             consumer,
    LMAXSequentialSharedPage* page,
    uint64_t                  writer_rid,
    uint64_t                  reader_rid,
    Entity*                   owner)
{
    assert(page && "[MirrorBuffer] LMAX makePair: null page");
    producer.active_        = MirrorBuffer::ActiveStrategy::LMAX;
    producer.lmax_page_     = page;
    producer.is_producer_   = true;
    producer.writer_rid_    = writer_rid;
    producer.next_read_seq_ = 0;
    consumer.bound_ctx_     = producer.bound_ctx_;
    consumer.active_        = MirrorBuffer::ActiveStrategy::LMAX;
    consumer.lmax_page_     = page;
    consumer.is_producer_   = false;
    consumer.writer_rid_    = writer_rid;
    consumer.next_read_seq_ = 0;
    // See buildWrapManifest's own comment -- runs regardless of strategy,
    // since an All-scope wrapper legitimately applies to LMAX too. Only
    // computed on producer, then copied verbatim -- both sides must
    // serialize the identical manifest. wrap_scratch_pool_ is a POINTER
    // into arena memory shared by both sides of this pair (same relationship
    // as lmax_page_ itself) -- copied here, not re-allocated, so producer
    // and consumer index the identical pool.
    if (owner)
    {
        producer.buildWrapManifest(owner);
        consumer.wrap_manifest_len_ = producer.wrap_manifest_len_;
        for (size_t i = 0; i < producer.wrap_manifest_len_; ++i)
            consumer.wrap_manifest_[i] = producer.wrap_manifest_[i];
        consumer.wrap_scratch_pool_ = producer.wrap_scratch_pool_;
    }
    if (producer.debug_ || consumer.debug_)
        ETCS_LOG("MirrorBuffer", "LMAX Pair Created."
                 << " writer=" << writer_rid
                 << " reader=" << reader_rid
                 << " slots="  << page->slot_count_);
}
// ---- StrategyPipe -----------------------------------------------------------
// SharedPage stages payloads; fd pair carries them cross-process.
template<>
inline void MirrorBuffer::makePair<StrategyPipe, SharedPage>(
    MirrorBuffer& producer,
    MirrorBuffer& consumer,
    SharedPage*   page,
    uint64_t      writer_rid,
    uint64_t      reader_rid,
    Entity*       owner)
{
    assert(page && "[MirrorBuffer] Pipe makePair: null staging page");
    int fds_data[2], fds_ack[2];
    if (pipe(fds_data) < 0 || pipe(fds_ack) < 0)
        throw std::runtime_error("[MirrorBuffer] Pipe makePair: pipe() failed");
 
    producer.active_      = MirrorBuffer::ActiveStrategy::Pipe;
    producer.shared_page_ = page;
    producer.is_producer_ = true;
    producer.writer_rid_  = writer_rid;
    producer.read_fd_     = fds_ack[0];
    producer.write_fd_    = fds_data[1];
    MirrorBuffer::setNonBlocking(producer.read_fd_);
    MirrorBuffer::setNonBlocking(producer.write_fd_);
    consumer.bound_ctx_   = producer.bound_ctx_;
    consumer.active_      = MirrorBuffer::ActiveStrategy::Pipe;
    consumer.shared_page_ = page;
    consumer.is_producer_ = false;
    consumer.writer_rid_  = writer_rid;
    consumer.read_fd_     = fds_data[0];
    consumer.write_fd_    = fds_ack[1];
    MirrorBuffer::setBlocking(consumer.read_fd_);
    MirrorBuffer::setNonBlocking(consumer.write_fd_);
    if (owner)
    {
        producer.buildWrapManifest(owner);
        consumer.wrap_manifest_len_ = producer.wrap_manifest_len_;
        for (size_t i = 0; i < producer.wrap_manifest_len_; ++i)
            consumer.wrap_manifest_[i] = producer.wrap_manifest_[i];
    }
    if (producer.debug_ || consumer.debug_)
        ETCS_LOG("MirrorBuffer", "Pipe Pair Created."
                 << " P(r:" << producer.read_fd_ << " w:" << producer.write_fd_ << ")"
                 << " C(r:" << consumer.read_fd_ << " w:" << consumer.write_fd_ << ")"
                 << " writer=" << writer_rid << " reader=" << reader_rid);
}
// ---- StrategySocket ---------------------------------------------------------
// writer_rid repurposed as producer socket fd; reader_rid as consumer socket fd.
// Entity is responsible for establishing the socket connection before calling.
template<>
inline void MirrorBuffer::makePair<StrategySocket, SharedPage>(
    MirrorBuffer& producer,
    MirrorBuffer& consumer,
    SharedPage*   page,
    uint64_t      writer_rid,
    uint64_t      reader_rid,
    Entity*       owner)
{
    assert(page && "[MirrorBuffer] Socket makePair: null staging page");
 
    int prod_fd = static_cast<int>(writer_rid);
    int cons_fd = static_cast<int>(reader_rid);
    assert(prod_fd >= 0 && "[MirrorBuffer] Socket makePair: invalid producer fd");
    assert(cons_fd >= 0 && "[MirrorBuffer] Socket makePair: invalid consumer fd");
 
    producer.active_      = MirrorBuffer::ActiveStrategy::Socket;
    producer.shared_page_ = page;
    producer.is_producer_ = true;
    producer.writer_rid_  = writer_rid;
    producer.read_fd_     = -1;
    producer.write_fd_    = prod_fd;
    MirrorBuffer::setNonBlocking(producer.write_fd_);
 
    consumer.bound_ctx_   = producer.bound_ctx_;
    consumer.active_      = MirrorBuffer::ActiveStrategy::Socket;
    consumer.shared_page_ = page;
    consumer.is_producer_ = false;
    consumer.writer_rid_  = writer_rid;
    consumer.read_fd_     = cons_fd;
    consumer.write_fd_    = -1;
    MirrorBuffer::setBlocking(consumer.read_fd_);
 
    if (owner)
    {
        producer.buildWrapManifest(owner);
        consumer.wrap_manifest_len_ = producer.wrap_manifest_len_;
        for (size_t i = 0; i < producer.wrap_manifest_len_; ++i)
            consumer.wrap_manifest_[i] = producer.wrap_manifest_[i];
    }
 
    if (producer.debug_ || consumer.debug_)
        ETCS_LOG("MirrorBuffer", "Socket Pair Created."
                 << " prod_fd=" << prod_fd << " cons_fd=" << cons_fd);
}
 
// ===========================================================================
// teardownPair explicit specializations — namespace scope (C++17).
// ===========================================================================
 
template<>
inline void MirrorBuffer::teardownPair<StrategyLMAX, LMAXSequentialSharedPage>(
    MirrorBuffer&             producer,
    MirrorBuffer&             consumer,
    LMAXSequentialSharedPage* page)
{
    // Symmetric lifetime is explicit for this pairing: consumer determines
    // the frame, and when either side goes down the other must follow. On
    // Pipe/Socket that happens for free -- closing an fd is observable at
    // the OS level from the other end, which IS the network connection-drop
    // behavior. LMAX has no such mechanism, so the equivalent has to be
    // raised deliberately here.
    //
    // Raising each side's own LOCAL-most authority (bound_ctx_ is the
    // ScopeTag-derived context after the trampolines' own bindContext call,
    // so .interrupt points at that scope's own Scope::Entry::flag) rather
    // than anything further up: this must stop exactly this pair, not the
    // entity and not the job that launched it -- precisely the granularity
    // `kill <label> <index>` exists to express.
    if (producer.bound_ctx_.interrupt)
        producer.bound_ctx_.interrupt->store(1, std::memory_order_release);
    if (consumer.bound_ctx_.interrupt)
        consumer.bound_ctx_.interrupt->store(1, std::memory_order_release);
    if (producer.debug_ || consumer.debug_)
        ETCS_LOG("MirrorBuffer", "LMAX teardown."
                 << " slots=" << (page ? page->slot_count_ : 0));
 
    if (page) page->tombstone();
    producer.lmax_page_     = nullptr;
    consumer.lmax_page_     = nullptr;
    producer.next_read_seq_ = 0;
    consumer.next_read_seq_ = 0;
}
 
template<>
inline void MirrorBuffer::teardownPair<StrategyPipe, SharedPage>(
    MirrorBuffer& producer,
    MirrorBuffer& consumer,
    SharedPage*   page)
{
    if (producer.debug_ || consumer.debug_)
        ETCS_LOG("MirrorBuffer", "Pipe teardown.");
 
    auto closefd = [](int& fd) { if (fd != -1) { ::close(fd); fd = -1; } };
    closefd(producer.read_fd_);
    closefd(producer.write_fd_);
    closefd(consumer.read_fd_);
    closefd(consumer.write_fd_);
 
    if (page) page->tombstone();
    producer.shared_page_ = nullptr;
    consumer.shared_page_ = nullptr;
}
 
template<>
inline void MirrorBuffer::teardownPair<StrategySocket, SharedPage>(
    MirrorBuffer& producer,
    MirrorBuffer& consumer,
    SharedPage*   page)
{
    if (producer.debug_ || consumer.debug_)
        ETCS_LOG("MirrorBuffer", "Socket teardown.");
 
    // Socket fds are owned by the entity; we close our handle here.
    auto closefd = [](int& fd) { if (fd != -1) { ::close(fd); fd = -1; } };
    closefd(producer.write_fd_);
    closefd(consumer.read_fd_);
 
    if (page) page->tombstone();
    producer.shared_page_ = nullptr;
    consumer.shared_page_ = nullptr;
}
 
} // namespace ETCS
 
#endif // MIRRORBUFFER_H__
