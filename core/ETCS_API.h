#ifndef ETCS_API_H__
#define ETCS_API_H__
// ETCS_DLL_EXPORTS will be defined ONLY when building the ETCS DLL project.
// It tells the compiler to EXPORT symbols, else these headers are used to IMPORT symbols.
#ifdef ETCS_DLL_EXPORTS
    #ifdef _WIN32 // Windows-specific export/import
        #define ETCS_API __declspec(dllexport)
    #else // Linux/macOS (GCC/Clang) - visibility default often handles it
        #define ETCS_API __attribute__((visibility("default")))
    #endif
#else
    // When NOT building the DLL, we are using it, so we IMPORT symbols.
    #ifdef _WIN32
        #define ETCS_API __declspec(dllimport)
    #else
        #define ETCS_API
    #endif
#endif
// flags for make with -DETCS_PRODUCTION_BUILD module side (see ETCS.h for loader side)
#ifdef ETCS_PRODUCTION_BUILD
    #define ETCS_LOG_TO_FILE
    #undef  ETCS_REPL_SHELL
    // #undef  ETCS_VERBOSE_DISPATCH
#endif
// #define DEBUG_STATIC_GET_INSTANCE_ORDER
// #define ETCS_LOG_FUNCTION_EVOCATION_PATH
// The following values affects ABI compatibility - changing requires updating version number, rebuilding module DLLs
#define DEFAULT_PLATFORM_PAGE_SIZE (1ULL << 21)
// Default memory arena size for any external dictionary
#define DEFAULT_EXTERNAL_DICTIONARY_INITIAL_SIZE (DEFAULT_PLATFORM_PAGE_SIZE)
// Default memory arena size for RuntimeDictionary
#define DEFAULT_RUNTIME_DICTIONARY_INITIAL_SIZE (DEFAULT_PLATFORM_PAGE_SIZE*2)
// Default ID size for everything
#define ID_SIZE_TYPE long long
// The max size a tag can be in a given version
#define MAX_TAG_BUFFER_SIZE             256
// ---------------------------------------------------------------------
// Module tag budget — ONE derivation, three consumers.
//
// ETCS_MAX_MODULE_TAGS is the number of distinct contract tags a single
// module may declare. It is ALSO ETCS::TAG_BITS (EventStream.h reads it
// directly rather than restating it), because those are the same fact:
// one bit per tag in a tag_closure_mask, so the reorder buffer can tell
// same-type operations (which must serialize against each other) from
// different-type ones (which may commit independently).
//
// These three ceilings used to be independent numbers that quietly
// disagreed: the mask was 64 bits, the Tags string was capped at 2048
// bytes, and the static_assert said 64. Crossing 64 tags was a build
// error; the 2048-byte cap would have bound first for any realistic tag
// name anyway; and nothing tied either to the mask width. Deriving them
// from one constant is what makes "the Tags string is allowed to carry
// exactly as many tags as the mask can represent" true rather than
// approximately true.
//
// ETCS_MAX_TAG_NAME_SIZE is a name plus its separator: 31 characters and
// one space. The arithmetic below is exact, not rounded up -- 256 tags of
// 31 characters, 255 separators between them, and one NUL is 8192 bytes
// precisely, which is what sizeof(Tags) measures in
// ETCS_MODULE_EXPORT_MAIN's own assert.
//
// A module needing more than this many ontology leaf types is the signal
// to split it. Raising ETCS_MAX_MODULE_TAGS is possible (TAG_WORDS scales
// with it) but is bounded at 256 by EventNode::tag_bit_index's own
// uint8_t index, and is an ABI change: GapSlot grows, so EventStream's
// layout moves, so every module must be rebuilt against the same value.
// ---------------------------------------------------------------------
#define ETCS_MAX_TAG_NAME_SIZE          32
#define ETCS_MAX_MODULE_TAGS            256
// Cross-ABI-boundary manifest transport (TagName_List / module tag
// discovery — see discoverActions/discoverTags in DynamicLoader.h).
// Deliberately separate from MAX_TAG_BUFFER_SIZE and not reused for
// anything else: this traffic scales with action/tag COUNT rather than
// any single item's size, which is a different shape than ordinary Buffer
// usage. Sized so a module declaring the full ETCS_MAX_MODULE_TAGS worth
// of maximum-length tag names fits EXACTLY, rather than to an arena page
// granularity picked independently of what it actually has to carry --
// see the derivation comment above.
#define MAX_BOUNDARY_BUFFER_SIZE        (ETCS_MAX_MODULE_TAGS * ETCS_MAX_TAG_NAME_SIZE)
// Cross-ABI-boundary MirrorBuffer stream transport (packConsumer/
// packProducer/unpack). Separate from MAX_BOUNDARY_BUFFER_SIZE above --
// that one is manifest/tag-discovery traffic; this is fds + one page
// pointer + two pair masks + wrap-chain manifest + config.
//
// 2048 -> 4096 for the pair masks: TAG_WORDS uint64_t each as decimal text, so
// up to 336 bytes together -- a sixth of the old budget, in a field that also
// holds config. Cheaper than making config's headroom depend on tag count.
#define MAX_MIRROR_TRANSPORT_SIZE       4096
#define ETCS_NETWORK_MAX_HEADER_SIZE    8*8192
#define ETCS_SLOT_SIZE                  64
#define ETCS_SEQUENTIAL_FRAME_SIZE      32
#define ETCS_BUFFER_METADATA_SIZE       16
#define ETCS_RID_SIZE                   uint64_t
#define MAX_LMAX_BUFFER_SIZE   (ETCS_SLOT_SIZE - ETCS_BUFFER_METADATA_SIZE - ETCS_SEQUENTIAL_FRAME_SIZE)
// you get 16 bytes, fit a ptr & action type
#define DEFAULT_THREAD_POOL_THREADS  4
// can maybe be std::thread::hardware_concurrency()
// default hash size/type
#define HASH_TYPE uint64_t
#define BASE_SOURCE_STRING "ETCS_Kernel_v1.0"
// change this value on ABI change, modules reject at hash change anyways but this is good tracking
#include <cstdint>
#include <cstring>
#include <atomic>   // WIRE_TYPE_IDENTITY's TAG_CLOSURE generation counter
#include <string>
#include <cstddef>
#include <unordered_map>
#include <map>
#include <sstream>
#include <vector>
// ====================================================================
// PLATFORM-SPECIFIC HEADER INCLUDES
// ====================================================================
#ifdef _WIN32HASH_TYPE
    #include <windows.h>
    #include <direct.h> // For _getcwd
    using library_handle_t = HMODULE;
    #define DL_EXTENSION ".dll"
    #define RENAME_CMD(old_name, new_name) MoveFileExA(old_name.c_str(), new_name.c_str(), MOVEFILE_REPLACE_EXISTING)
    #define GET_CWD() _getcwd(nullptr, 0)
#else
    #include <dlfcn.h>
    #include <unistd.h> // For getcwd
    #include <sys/stat.h> // For stat
    using library_handle_t = void*;
    #define DL_EXTENSION ".so"
    #define RENAME_CMD(old_name, new_name) std::rename(old_name.c_str(), new_name.c_str())
    #define GET_CWD() getcwd(nullptr, 0)
#endif
#include "Buffer.h"
#include "Bundles.h"
#include "ThreadPool.h"
#include "MemoryArena.h"
#include "CommandExecutor.h"
// ---------------------------------------------------------------------------
// ETCS_CPU_RELAX — portable spin-loop hint.
//
// x86/x64 : PAUSE   (~140 cycles on Skylake+, de-pipelines the spin loop)
// aarch64 : YIELD   (architecturally a hint; a NOP on Cortex-A76/Pi 5)
// other   : no-op
//
// The inline asm form is used on the GCC/Clang ARM path rather than __yield()
// from <arm_acle.h>: same instruction, no header availability question, and it
// encodes as a NOP on pre-ARMv6K. The "memory" clobber is load-bearing — it
// stops the compiler hoisting the watched atomic load out of the spin loop.
//
// NOTE: this properly belongs in core_defs.h — ThreadPool and every other
// spinning site wants the same macro. It is defined here (guarded) only so
// this header stays self-contained; moving it up is then a no-op.
// ---------------------------------------------------------------------------
#ifndef ETCS_CPU_RELAX
  #if defined(_MSC_VER)
    #include <intrin.h>
    #if defined(_M_ARM) || defined(_M_ARM64)
      #define ETCS_CPU_RELAX() __yield()
    #else
      #define ETCS_CPU_RELAX() _mm_pause()
    #endif
  #elif defined(__x86_64__) || defined(__i386__)
    #include <xmmintrin.h>
    #define ETCS_CPU_RELAX() _mm_pause()
  #elif defined(__aarch64__) || defined(__arm__)
    #define ETCS_CPU_RELAX() __asm__ __volatile__("yield" ::: "memory")
  #else
    #define ETCS_CPU_RELAX() ((void)0)
  #endif
#endif
// ---------------------------------------------------------------------------
// Backoff thresholds.
//
// These are NOT arch-portable constants. On x86 each PAUSE burns real cycles,
// so a 64-iteration hint stage is a meaningful backoff. On aarch64 YIELD is
// effectively free, so the same count is a near-tight spin — more interconnect
// traffic, less actual waiting. Thresholds are scaled down accordingly.
// ---------------------------------------------------------------------------
#ifndef ETCS_SPIN_HINT_LIMIT
  #if defined(__aarch64__) || defined(__arm__) || defined(_M_ARM) || defined(_M_ARM64)
    #define ETCS_SPIN_HINT_LIMIT     8
    #define ETCS_SPIN_YIELD_LIMIT   32
  #else
    #define ETCS_SPIN_HINT_LIMIT    64
    #define ETCS_SPIN_YIELD_LIMIT  100
  #endif
#endif
// ---------------------------------------------------------------------------
// ETCS_SLEEP_MS — interruptible millisecond sleep.
//
// Uses TIMER_ABSTIME so signals cause early return rather than restart.
// Returns true if sleep completed, false if interrupted (EINTR).
//
// This pairs with ETCS_CPU_RELAX: use SPIN for <1ms, SLEEP for longer waits.
// ---------------------------------------------------------------------------
#include <time.h>
#include <errno.h>
inline bool ETCS_SLEEP_MS(uint32_t ms)
{
    if (ms == 0) return true;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    ts.tv_sec  += ms / 1000;
    ts.tv_nsec += static_cast<long>(ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L)
    {
        ts.tv_sec  += 1;
        ts.tv_nsec -= 1000000000L;
    }
    // TIMER_ABSTIME: if interrupted by signal, returns EINTR immediately
    // with remaining time already accounted for in our absolute target.
    int rc = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, nullptr);
    if (rc == EINTR)
    {
        // Sleep was interrupted — caller should check ctx.isInterrupted()
        return false;
    }
    return true;
}
// a standard macro for declaring a template leaf node (call within public: block)
//
// TAG_MASK is this TYPE's own tag_closure_mask -- static, so it is reachable
// as type information (Foo::TAG_MASK) with no instance in hand, exactly like
// TAG immediately above it. Declared here but ASSIGNED by ETCS_TAG_DECLARE
// (below), because the bit position comes from the module's own Tags string,
// which only exists in the module's .cc after ETCS_MODULE_EXPORT_MAIN has run
// -- it is not visible while this header is being parsed.
//
// inline static rather than an out-of-line definition: a type may carry
// WIRE_TYPE_IDENTITY without ever appearing in a tag block (an internal type
// that is never a dispatchable contract leaf), and such a type must still
// link. Those get an empty mask, which is the correct answer for them -- they
// have no contract identity for the reorder buffer to order against.
//
// myTagMask() is the virtual accessor Entity::addTag/removeTag reach it
// through, since those are non-template members with no T in hand. Same
// shape myTag()/getRID() already establish; Entity's own base implementation
// returns an empty mask, so a type without this macro degrades to today's
// behaviour rather than failing to compile.
#define WIRE_TYPE_IDENTITY(tag) \
private: \
    const uint64_t m_rid = ETCS::generateRID<tag>(); \
public: \
    static constexpr const char* TAG = #tag; \
    /* CONTRACT_TAG -- the name the rest of the system knows this type by. TAG \
     * is stringized from this macro's argument, the CONCRETE class, and a \
     * typedef cannot rename a static: PicoHTTPParser::TAG stays \
     * "PicoHTTPParser" while type_owner_index, the module ridMap and \
     * Module::catalog() are keyed "HTTPParser". Those three want this. \
     * \
     * Defaults to TAG -- right when they coincide and for a type with no tag \
     * block. ETCS_TAG_DECLARE overwrites it with the tag block's name, the \
     * only authoritative statement of contract identity; the CRTP family is \
     * not one (PicoHTTPParser's is Parser, SocketConnectionState has two). \
     * \
     * Runtime, not constexpr: the typedef and the tag block are both parsed \
     * after this class. Assigned at static init, before any entity exists to \
     * call addTag<T>/spawn<T>. */ \
    inline static const char* CONTRACT_TAG = TAG; \
    inline static ETCS::TagMask TAG_MASK{}; \
    /* TAG_CLOSURE -- every other type in this module that this one holds a \
     * reference to, accumulated at the typed acquisition sites. What makes \
     * tag_closure_mask a CLOSURE rather than a type name. \
     * \
     * An op on ChessNode touches every ChessGame it addTag'd, through raw \
     * derefs off a cached vector -- untyped, unmaskable at the point of use. \
     * But each pointer arrived through exactly one typed call, \
     * addTag<ChessGame>(), and flipping the bit there covers every untyped \
     * use afterwards. \
     * \
     * PER TYPE: what a TYPE reaches is a property of the code -- which <T>s \
     * appear in which members -- and would be constexpr if a template could \
     * see its own instantiation sites. Discovered at first acquisition \
     * instead. Instance separation is never this mask's job; that is which \
     * EventStream an event lands on. \
     * \
     * MONOTONIC, never cleared, not even by removeTag: a type that reached \
     * another once may again, and un-setting is the one fail-open move here. \
     * \
     * SAME MODULE ONLY, self-guarded -- TAG_MASK is assigned by this module's \
     * own ETCS_TAG_DECLARE, so a foreign type reads empty and contributes \
     * nothing (see addTagTrampoline). Two modules' tag bits must never meet. */ \
    inline static std::atomic<uint64_t> TAG_CLOSURE_W[ETCS::TAG_WORDS]{}; \
    /* Atomic words plus a generation: a reader on any pool worker can be \
     * mid-read while the loader's ordering thread records a first \
     * acquisition. The atomics make each word coherent (and keep TSan quiet); \
     * the generation, bumped AFTER them, catches a read torn ACROSS words -- \
     * sampled either side, falling back to all(), so tearing fails SHUT \
     * instead of dropping a bit. All writes are on that one thread, so there \
     * is no writer-writer case; fetch_or covers it regardless. */ \
    inline static std::atomic<uint32_t> TAG_CLOSURE_GEN{0}; \
    void* getTrueType() override { return static_cast<void*>(this); } \
    const char* myTag() override { return TAG; } \
    const ETCS::TagMask& myTagMask() const override { return TAG_MASK; } \
    /* STATIC so a type-only caller can ask -- streamPairMask<P,C> has types \
     * and no instances. By value: the source grows under the caller, so a \
     * reference would be a data race. */ \
    static ETCS::TagMask readTagClosure() \
    { \
        const uint32_t g0 = TAG_CLOSURE_GEN.load(std::memory_order_acquire); \
        ETCS::TagMask m; \
        for (size_t i = 0; i < ETCS::TAG_WORDS; ++i) \
            m.w[i] = TAG_CLOSURE_W[i].load(std::memory_order_relaxed); \
        if (TAG_CLOSURE_GEN.load(std::memory_order_acquire) != g0) \
            return ETCS::TagMask::all(); \
        return TAG_MASK | m; \
    } \
    ETCS::TagMask myTagClosure() const override { return readTagClosure(); } \
    void noteAcquires(const ETCS::TagMask& other) override \
    { \
        if (!other.any()) return; \
        for (size_t i = 0; i < ETCS::TAG_WORDS; ++i) \
            if (other.w[i]) TAG_CLOSURE_W[i].fetch_or(other.w[i], std::memory_order_relaxed); \
        TAG_CLOSURE_GEN.fetch_add(1, std::memory_order_release); \
    } \
    uint64_t getRID() const override { return m_rid; } \
// for the locals ctx and root, auto registering ctx to globals.
// Order matters: setParent must run BEFORE Root copies ctx by value, or
// the copy stored inside root.ctx_ would be taken before parent
// authority (interrupt_parent, etc.) was ever wired -- meaning
// root.ctx_.isInterrupted() could never see a real SIGINT at all,
// defeating the entire point of Root now carrying its own SignalContext.
#define WIRE_CONTEXT()                          \
    ETCS::SignalContext ctx;                    \
    ctx.setParent(&ETCS::RootSignalContext());  \
    ETCS::Root root(ctx);                       \
    ETCS::ExecSource loader{"(loader)", 0};     \
    ETCS::ExecutionContext env(&root, &ctx);

// --- Arity-scaling expansion machinery (FE_*/FE_FIXED_*/FE_C_*,
// GET_MACRO*, FOR_EACH*, ETCS_DISPATCH_SELECT*) lives in a generated
// file, not here. Regenerate via generate_arity_macros.py whenever the
// arity ceiling (currently 64) needs to change — never hand-edit the
// generated file directly, and never hand-edit a replacement for it
// here; that's exactly the drift the generator exists to prevent.
#include "AirtyMacros_Generated.h"
// see root/dev_tools/generate_airty_macros.py to generate
#define ETCS_COUNT_ARGS(...) \
    GET_MACRO(__VA_ARGS__,64,63,62,61,60,59,58,57,56,55,54,53,52,51,50,49,48,47,46,45,44,43,42,41,40,39,38,37,36,35,34,33,32,31,30,29,28,27,26,25,24,23,22,21,20,19,18,17,16,15,14,13,12,11,10,9,8,7,6,5,4,3,2,1)
#define ETCS_DISPATCH_CONCAT2_(a,b)   a##b
#define ETCS_DISPATCH_CONCAT2(a,b)    ETCS_DISPATCH_CONCAT2_(a,b)
#define ETCS_DISPATCH_CONCAT3_(a,b,c) a##b##c
#define ETCS_DISPATCH_CONCAT3(a,b,c)  ETCS_DISPATCH_CONCAT3_(a,b,c)
// --- Underlying zero-arg / N-arg implementations (not called directly —
// use ETCS_DISPATCH_METHOD / ETCS_DISPATCH_METHOD_CONST below) ---
#define ETCS_DISPATCH_METHOD_0(RetType, Name) \
    virtual RetType Name##Concrete() = 0; \
    RetType Name() { return static_cast<Derived*>(this)->Name##Concrete(); }
#define ETCS_DISPATCH_METHOD_N(RetType, Name, ...) \
    virtual RetType Name##Concrete(FOR_EACH_COMMA(_PARAM, __VA_ARGS__)) = 0; \
    RetType Name(FOR_EACH_COMMA(_PARAM, __VA_ARGS__)) \
    { return static_cast<Derived*>(this)->Name##Concrete(FOR_EACH_COMMA(_ARGNAME, __VA_ARGS__)); }
#define ETCS_DISPATCH_METHOD_0_CONST(RetType, Name) \
    virtual RetType Name##Concrete() const = 0; \
    RetType Name() const { return static_cast<const Derived*>(this)->Name##Concrete(); }
#define ETCS_DISPATCH_METHOD_N_CONST(RetType, Name, ...) \
    virtual RetType Name##Concrete(FOR_EACH_COMMA(_PARAM, __VA_ARGS__)) const = 0; \
    RetType Name(FOR_EACH_COMMA(_PARAM, __VA_ARGS__)) const \
    { return static_cast<const Derived*>(this)->Name##Concrete(FOR_EACH_COMMA(_ARGNAME, __VA_ARGS__)); }
// --- Public entry points: arity is inferred, const-ness is not (it's a
// genuine semantic choice, not derivable from the argument list) ---
#define ETCS_DISPATCH_METHOD(RetType, Name, ...) \
    ETCS_DISPATCH_CONCAT2(ETCS_DISPATCH_METHOD_, ETCS_DISPATCH_SELECT(__VA_ARGS__)) \
        (RetType, Name, ##__VA_ARGS__)
#define ETCS_DISPATCH_METHOD_CONST(RetType, Name, ...) \
    ETCS_DISPATCH_CONCAT3(ETCS_DISPATCH_METHOD_, ETCS_DISPATCH_SELECT(__VA_ARGS__), _CONST) \
        (RetType, Name, ##__VA_ARGS__)
// ---------------------------------------------------------------------------
// ETCS_SUPERTYPE_BASE(Name) / ETCS_MAKE_INSTANCE(Name)
//
// The generic scaffolding every ontology xxx.h/xxxBase.h family pair uses
// to complete the "type provider" surface -- registering the family's own
// aggregate presence and giving each constructed instance a legally-
// resolvable interface pointer, without Entity.h (or this file) ever
// needing to know any specific family (Parser_, Wrapper_, etc.) by name.
//
// ETCS_SUPERTYPE_BASE(Name) opens an xxxBase.h's own CRTP class body,
// publishing an aggregate RIDList<Name_*> for the family under the bare
// tag string #Name (e.g. "Wrapper") into THIS module's own
// EventNode::ridMap. Self-contained per family: including WrapperBase.h
// at all is what makes the "Wrapper" aggregate slot exist in this
// module's own ridMap, regardless of whether this specific module
// implements Wrapper_ itself. Usage:
//
//   ETCS_SUPERTYPE_BASE(Wrapper)
//   {
//       ETCS_MAKE_INSTANCE(Wrapper)
//       ETCS_DISPATCH_METHOD(void, Wrap, (ETCS::MBuffer&, io), (ETCS::SignalContext, ctx));
//       ETCS_DISPATCH_METHOD(void, Unwrap, (ETCS::MBuffer&, io), (ETCS::SignalContext, ctx));
//       ETCS_DISPATCH_METHOD_CONST(ETCS::WireScope, Scope);
//   };
//
// ETCS_MAKE_INSTANCE(Name) supplies the ctor/dtor. The ctor does two
// things, both requiring Entity's own generic surface (Entity.h):
//   1. addTypeTag(#Name) -- a bare, origin-less, upper-case is-a marker
//      in Entity's own tags map. etcs_supertype_fanout (Entity.h) walks
//      this post-construction to add the instance into whichever
//      aggregate RIDList(s) ETCS_SUPERTYPE_BASE above published.
//   2. registerInterfacePointer(#Name, static_cast<void*>(static_cast<Name##_*>(this)))
//      -- static_cast<Name##_*>(this) is ALWAYS a legal, ordinary upcast
//      here (Name##Base<Derived> : public Name##_ is non-virtual at
//      THIS specific link; Name##_'s own virtual inheritance of Entity
//      is one level further up and never touched by this cast). Stored
//      type-erased, keyed by family name, so ANY code holding a bare
//      Entity* can retrieve a real Name##_* via
//      entity->getInterfacePointer(Buffer(#Name)) and cast it back --
//      but only code that already knows what that family IS (e.g.
//      MirrorBuffer.h knowing Wrapper_) ever performs that cast. Entity
//      itself never needs to know any family by name, and no
//      dynamic_cast is ever required anywhere in this path.
//
// NOTE on Wrapper_ specifically: this "Wrapper"-keyed pointer stores a
// Wrapper_* subobject address, which IS also read directly as
// ETCS::IWireWrapper* by MirrorBuffer's own resolveWrapChain/
// buildWrapManifest (DynamicLoader.h) -- no separate key or hand-written
// Wrapper_ constructor needed. This depends on IWireWrapper being
// declared as Wrapper_'s FIRST, non-virtual base (ontology/Wrapper.h):
// under the Itanium C++ ABI, the first non-virtual base subobject sits
// at offset 0, so a Wrapper_* and an IWireWrapper* pointing at the same
// object are bit-identical. Real dependency, not a style choice --
// getting the base order wrong produces a silently mis-adjusted pointer
// at runtime, not a compile error.
// ---------------------------------------------------------------------
#define ETCS_SUPERTYPE_BASE(Name) \
    inline ETCS::RIDList<Name##_*>& _etcs_supertype_list_##Name() { \
        static ETCS::RIDList<Name##_*> list; \
        return list; \
    } \
    inline bool _etcs_supertype_registered_##Name = []() { \
        ETCS::ThreadPool::getInstance(); \
        /* Direct ridMap write, not RegisterRIDRegistry -- that method    \
         * only exists on module-scope EventNode (its own #ifndef         \
         * ETCS_LOADER branch); this macro expands in ontology headers    \
         * included by BOTH the loader (ETCS.h) and every module, so it   \
         * can only rely on members declared unconditionally on           \
         * EventNode, before either branch of that split. ridMap itself   \
         * is exactly such a member. */ \
        ETCS::EventNode::getInstance().ridMap[ETCS::Buffer(#Name)] = \
            _etcs_supertype_list_##Name().handle(#Name); \
        return true; \
    }(); \
    template <typename Derived> class Name##Base : public Name##_

#define ETCS_MAKE_INSTANCE(Name) \
    public: \
        Name##Base() { \
            this->addTypeTag(ETCS::Buffer(#Name)); \
            this->registerInterfacePointer(ETCS::Buffer(#Name), \
                static_cast<void*>(static_cast<Name##_*>(this))); \
        } \
        virtual ~Name##Base() = default;

// --- Pair helpers ---
#define _DECL(pair)         _DECL_ pair
#define _DECL_(T, name)     T name{};
#define _EXTRACT(pair)      _EXTRACT_ pair
#define _EXTRACT_(T, name)  data >> name;
// _PARAM/_ARGNAME — same (Type, name) tuple convention as _DECL/_EXTRACT
// above, consumed by ETCS_DISPATCH_METHOD_N/_N_CONST via FOR_EACH_COMMA.
// _PARAM produces a full parameter declaration ("T name") for the method
// signature; _ARGNAME produces just the bare name, for forwarding into
// the *Concrete call. These don't scale with arity, so they live here
// as static hand-written helpers, not in the generated file.
#define _PARAM_(T, name)      T name
#define _PARAM(pair)          _PARAM_ pair
#define _ARGNAME(pair)        _ARGNAME_ pair
#define _ARGNAME_(T, name)    name
// --- The Core Interface Macros ---
// DEFINE_WORK_FUNC provides self&, SignalContext, and expanded list of types passed (Type, name)(Type, name) as ...
//
// NOTE ON ##Type##_##Name (here and in every DEFINE_*/ETCS_MODULE_EXPORT_*
// macro below): the extra ## splitting out the literal underscore as its
// own pasted token is required, not stylistic. `_implWork_##Type_##Name`
// (single token `Type_` glued to the literal text) does NOT substitute the
// Type parameter at all — `Type_` is a single identifier token that never
// matches the macro parameter named `Type`, so it was silently emitting the
// literal four characters "Type_" regardless of what was actually passed.
// This was invisible for years because it only breaks when two DIFFERENT
// types in the same module share an action name (both would generate the
// identical symbol `_implWork_Type_ActionName`) — a collision the compiler
// only catches as "redefinition", not as "wrong substitution". Writing
// `##Type##_##Name` instead tokenizes as Type, then a separate `_` token,
// then Name — three independently-substitutable pieces — so Type actually
// gets replaced.
// Scoped to DEFINE_WORK_FUNC_TYPED only — forces a reference in the
// _implWork_ signature regardless of the tuple's stored type. This is what
// lets the developer body mutate a field (including an OUT field like
// NBuffer content) in place, without _DECL_ ever declaring a *local of
// reference type* — which is exactly the unbindable case above. The
// tuple's T stays a plain value type; only the call signature gets the &.
#define _PARAM_REF(pair)        _PARAM_REF_ pair
#define _PARAM_REF_(T, name)    T& name
#define DEFINE_WORK_FUNC_TYPED(Type, Name, ...) \
    static_assert(ETCS_COUNT_ARGS(__VA_ARGS__) * 21 <= ETCS::Buffer::bufsize - 1, \
                  #Type "::" #Name " has too many typed fields for Buffer capacity " \
                  "(numeric/RID fields, 21B/field worst case)"); \
 \
    void _implWork_##Type##_##Name(Type& self, ETCS::SignalContext ctx,  \
                                    FOR_EACH_COMMA(_PARAM_REF, __VA_ARGS__)); \
 \
    void _work_##Type##_##Name(ETCS::Entity* handler, ETCS::Buffer& data, \
                      ETCS::SignalContext ctx) { \
        FOR_EACH(_DECL, __VA_ARGS__) \
        FOR_EACH(_EXTRACT, __VA_ARGS__) \
        if(!handler) { \
            ETCS_LOG(#Type, "::" #Name " -- work function reached with null handler " \
                     "(work function outlived its target entity); refusing to dispatch. " \
                     "data=" << data.buf); \
            return; \
        } \
        _implWork_##Type##_##Name(*static_cast<Type*>(handler->getTrueType()), ctx, \
                                    FOR_EACH_COMMA(_ARGNAME, __VA_ARGS__)); \
    } \
 \
    void _implWork_##Type##_##Name(Type& self, ETCS::SignalContext ctx, \
                                    FOR_EACH_COMMA(_PARAM_REF, __VA_ARGS__))
// Difference between produce/consume is non-blocking/blocking pair (when a call is made)
// Consume function always determines lifetime of the stack frame
//
// unpack() now returns bool -- see MirrorBuffer::unpack's own comment
// (MirrorBuffer.h) for why: a wrap chain's unwrap side can fail graceful
// capability negotiation (a required Wrapper module/type isn't loadable
// on this side), and that failure has to stop dispatch BEFORE the
// developer's own _implProduce_/_implConsume_ body ever runs -- a
// corrupted/un-unwrappable stream limping into the developer's own code
// would be strictly worse than simply refusing the call. Both trampolines
// below check the return and bail, logging why, rather than the old
// unconditional `stream.unpack(data);` with no failure path at all.
//
// ScopeTag registration timing (PRODUCE vs CONSUME) — the two trampolines
// below are NOT symmetric, deliberately:
//
//   CONSUME determines the lifetime of the calling stack frame (see this
//   whole macro pair's own top comment) -- it runs the developer's body
//   INLINE, synchronously, on whatever thread is already here. Same
//   thread constructs ScopeTag and runs the body; no gap exists between
//   "registered" and "running" to worry about.
//
//   PRODUCE does not: the developer's body runs LATER, on a pool worker
//   thread, via getThreadPool().enqueue(...). If ScopeTag were
//   constructed INSIDE that enqueued lambda (as it used to be), there
//   would be a real window -- between this function returning (having
//   already handed `self` to the pool by raw pointer) and the pool
//   actually starting that lambda -- where NOTHING is registered to
//   protect self at all. An entity teardown checking for in-flight scopes
//   in exactly that window would see nothing and proceed, and the
//   eventually-started lambda would then dereference an already-
//   dangling self. So ScopeTag is constructed HERE, on the calling
//   thread, BEFORE enqueue() -- registration is complete before this
//   function ever returns -- and then MOVED (see ScopeTag's own move
//   ctor, Bundles.h) into the lambda's own capture, carrying its
//   already-established registration through to wherever it actually
//   runs, rather than constructing a second, later one.
//
// StreamWriteGuard (PRODUCE only) — closing the write end when the
// producer body finishes is a contract of the produce/consume pairing,
// not something each producer author should have to re-implement
// correctly on every branch. It was previously delegated to the body,
// and two producers got it wrong in the same way: HTTPParser::Listen's
// three early returns (socket/bind/listen failure) and
// HTTPParser::ProduceResponse's interrupt branch all returned WITHOUT
// closing, stranding the paired consumer on a producer that had already
// given up. On StrategyLMAX -- which is what a same-type produce/consume
// pair resolves to -- there is no fd-level EOF to fall back on, so the
// consumer simply blocks forever. The guard makes the close happen on
// every path out of the body, including an exception.
//
// This is why `stream` is a REFERENCE parameter rather than by-value: the
// lambda's own captured MirrorBuffer must outlive the developer's body so
// the guard can close it afterwards. A producer that hands ownership
// onward (Listen moving `stream` into an arena-allocated ListenState)
// disarms the guard automatically -- MirrorBuffer's move constructor
// nulls lmax_page_/write_fd_ on the source, so closeWrite() on the husk
// is a no-op and the new owner's own teardown stays authoritative. No
// explicit release call, nothing to forget.
//
// CONSUME gets no such guard: the consumer is the READ side. On Pipe it
// does hold a write_fd_ (the ack channel, see makePair), and closing that
// would be wrong.
#define DEFINE_STREAM_FUNC_PRODUCE(Type, Name) \
    void _implProduce_##Type##_##Name(Type& self, ETCS::MirrorBuffer& stream, ETCS::Buffer data, ETCS::SignalContext ctx); \
    \
    void _stream_##Type##_##Name(ETCS::Entity* handler, ETCS::MBuffer& data, ETCS::SignalContext ctx) { \
        Type* self = static_cast<Type*>(handler->getTrueType()); \
        if(!self) return; \
        ETCS::MirrorBuffer stream; \
        stream.bindContext(ctx); \
        if (!stream.unpack(data, handler)) \
        { \
            ETCS_LOG(#Type, "::" #Name " -- unpack() failed graceful capability " \
                     "negotiation (a required wrap-chain stage could not be " \
                     "bootstrapped on this side); refusing to dispatch."); \
            return; \
        } \
        ETCS::Buffer config = stream.getConfig(); \
        \
        /* Both masks come off the stream, where unpack() just put them. The \
         * TAG one goes to the ScopeTag, whose TagModifyEvent stays in this \
         * module. The MODULE one is published on the BODY's thread, not this \
         * one -- the guard below is moved into the pool lambda, so it \
         * constructs here and runs there. */ \
        ETCS::ScopeTag _auto_scope(self, #Name, ctx, stream.pairTagMask()); \
        stream.bindContext(_auto_scope.ctx()); \
        const ETCS::TagMask _pair_mod = stream.pairModuleMask(); \
        /* Raised HERE, on the trampoline's own thread, not inside the pool \
         * lambda: the enqueue returns immediately and the pair's frame may \
         * reach its own teardown before a worker has even picked the task \
         * up. Set before the enqueue, the flag is already true by then; set \
         * inside, it would be a race the flag exists to remove. Cleared by \
         * ProducerLiveGuard around the body, so an exception leaving counts \
         * as leaving. */ \
        ETCS::SharedPage* _live_token = stream.producerEnter(); \
        try { \
        self->getThreadPool().enqueue(ETCS::Priority::Medium, ctx, [self, stream = std::move(stream), config, _pair_mod, _live_token, _auto_scope = std::move(_auto_scope)]() mutable { \
            ETCS::ProducerLiveGuard _auto_live(_live_token); \
            ETCS::PairScope _pair_scope(_pair_mod); \
            ETCS::StreamWriteGuard _auto_close(stream); \
            _implProduce_##Type##_##Name(*self, stream, config, _auto_scope.ctx()); \
        }); \
        } catch (...) { \
            /* enqueue throws once the pool is stopping. The body will never \
             * run, so release the raise here rather than leaving the pair's \
             * frame waiting out its timeout on a producer that does not \
             * exist. */ \
            ETCS::MirrorBuffer::producerLeave(_live_token); \
            throw; \
        } \
    } \
    void _implProduce_##Type##_##Name(Type& self, ETCS::MirrorBuffer& stream, ETCS::Buffer data, ETCS::SignalContext ctx)

#define DEFINE_STREAM_FUNC_CONSUME(Type, Name) \
    void _implConsume_##Type##_##Name(Type& self, ETCS::MirrorBuffer stream, ETCS::Buffer data, ETCS::SignalContext ctx); \
 \
    void _stream_##Type##_##Name(ETCS::Entity* handler, ETCS::MBuffer& data, ETCS::SignalContext ctx) { \
        if(!handler) { \
            ETCS_LOG(#Type, "::" #Name " -- consume trampoline reached with null " \
                     "handler (work function outlived its target entity); refusing " \
                     "to dispatch."); \
            return; \
        } \
        ETCS::MirrorBuffer stream; \
        stream.bindContext(ctx); \
        if (!stream.unpack(data, handler)) \
        { \
            ETCS_LOG(#Type, "::" #Name " -- unpack() failed graceful capability " \
                     "negotiation (a required wrap-chain stage could not be " \
                     "bootstrapped on this side); refusing to dispatch."); \
            return; \
        } \
        ETCS::Buffer config = stream.getConfig(); \
        Type* self = static_cast<Type*>(handler->getTrueType()); \
        ETCS::ScopeTag _auto_scope(self, #Name, ctx, stream.pairTagMask()); \
        stream.bindContext(_auto_scope.ctx()); \
        /* Runs inline, so this thread IS the body's thread. Read before the \
         * move below; PairScope copies. */ \
        ETCS::PairScope _pair_scope(stream.pairModuleMask()); \
        _implConsume_##Type##_##Name(*self, std::move(stream), config, _auto_scope.ctx()); \
    } \
 \
    void _implConsume_##Type##_##Name(Type& self, ETCS::MirrorBuffer stream, ETCS::Buffer data, ETCS::SignalContext ctx)

// DEFINE_WORK_FUNC provides self&, Buffer&, and SignalContext
#define DEFINE_WORK_FUNC(Type, Name) \
    void _implWork_##Type##_##Name(Type& self, ETCS::Buffer& data, ETCS::SignalContext ctx); \
    \
    void _work_##Type##_##Name(ETCS::Entity* handler, ETCS::Buffer& data, ETCS::SignalContext ctx) { \
        if(!handler) { \
            ETCS_LOG(#Type, "::" #Name " -- consume trampoline reached with null " \
                     "handler (work function outlived its target entity); refusing " \
                     "to dispatch."); \
            return; \
        } \
        _implWork_##Type##_##Name(*static_cast<Type*>(handler->getTrueType()), data, ctx); \
    } \
    \
    void _implWork_##Type##_##Name(Type& self, ETCS::Buffer& data, ETCS::SignalContext ctx)
    // devs { starts here
namespace ETCS
{
    // etcs_count_tags / etcs_tag_index — constexpr tokenization of a
    // module's own Tags string, splitting on the same whitespace class
    // std::stringstream's own operator>> uses (see
    // ETCS_MODULE_EXPORT_MAIN's own static-init loop, which feeds
    // RegisterTagBitIndex). Same string, same tokenizer, same order --
    // so the compile-time bit position and the static-init one agree by
    // construction rather than by inspection.
    //
    // Both are evaluated entirely at compile time at every real call
    // site (Tags is always a string literal), so this costs nothing at
    // runtime and fails the BUILD rather than degrading silently later.
    constexpr bool etcs_is_tag_space(char c)
    {
        return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
    }
    constexpr size_t etcs_count_tags(const char* s)
    {
        size_t count = 0;
        bool in_token = false;
        for (size_t i = 0; s[i] != '\0'; ++i)
        {
            if (etcs_is_tag_space(s[i]))
                in_token = false;
            else if (!in_token)
            {
                in_token = true;
                ++count;
            }
        }
        return count;
    }
    // Sentinel for "this tag is not in that Tags string at all". Distinct
    // from bit 0, which is a perfectly ordinary first tag.
    inline constexpr size_t ETCS_TAG_NOT_FOUND = static_cast<size_t>(-1);
    // Position of `tag` within `tags`, counting tokens from zero -- i.e.
    // exactly the bit RegisterTagBitIndex would assign it, computed at
    // compile time. ETCS_TAG_NOT_FOUND if the tag isn't listed.
    //
    // ETCS_TAG_DECLARE static_asserts on this, which closes a hole that
    // has nothing to do with the tag budget: a type with a tag block but
    // no entry in its module's Tags string used to compile, register a
    // RIDList, spawn, and list correctly -- while silently getting no tag
    // bit at all, so its same-type operations stopped serializing against
    // each other. That was a build-time-knowable mistake diagnosed
    // nowhere.
    constexpr size_t etcs_tag_index(const char* tags, const char* tag)
    {
        size_t index = 0, i = 0;
        while (tags[i] != '\0')
        {
            while (etcs_is_tag_space(tags[i])) ++i;
            if (tags[i] == '\0') break;
            size_t j = 0;
            while (tags[i + j] != '\0' && !etcs_is_tag_space(tags[i + j])
                   && tag[j] != '\0' && tags[i + j] == tag[j]) ++j;
            // A match requires BOTH to have ended at the same point --
            // otherwise "Http" would match the prefix of "HttpServer".
            const bool token_ended = (tags[i + j] == '\0' || etcs_is_tag_space(tags[i + j]));
            if (token_ended && tag[j] == '\0') return index;
            while (tags[i] != '\0' && !etcs_is_tag_space(tags[i])) ++i;
            ++index;
        }
        return ETCS_TAG_NOT_FOUND;
    }
    // Longest single token in a Tags string, for the per-name ceiling
    // ETCS_MAX_TAG_NAME_SIZE expresses. Checked alongside the count,
    // because the byte budget below is derived from BOTH -- a module
    // with few but very long tag names can overrun sizeof(Tags) without
    // ever approaching ETCS_MAX_MODULE_TAGS.
    constexpr size_t etcs_longest_tag(const char* s)
    {
        size_t longest = 0, current = 0;
        for (size_t i = 0; s[i] != '\0'; ++i)
        {
            if (etcs_is_tag_space(s[i])) { current = 0; continue; }
            ++current;
            if (current > longest) longest = current;
        }
        return longest;
    }
    // Looks up the loader's exported manifest getter (ETCS_GetLoaderManifest,
    // DynamicLoader.h) in the process's global symbol scope -- a plain
    // function so ETCS_MODULE_EXPORT_MAIN's macro body doesn't need a raw
    // #ifdef _WIN32 inside its backslash-continued expansion. Null when no
    // loader is in this process (dlsym/GetProcAddress miss), not an error --
    // the module's own manifest check treats that as "nothing to compare
    // against" rather than a mismatch.
    inline void* etcs_find_loader_manifest_getter()
    {
#ifdef _WIN32
        return reinterpret_cast<void*>(
            GetProcAddress(GetModuleHandleA(nullptr), "ETCS_GetLoaderManifest"));
#else
        return dlsym(RTLD_DEFAULT, "ETCS_GetLoaderManifest");
#endif
    }
}
/**
 * ETCS_MODULE_EXPORT_MAIN
 */
#define ETCS_MODULE_EXPORT_MAIN(Name, Tags) \
    static_assert(sizeof(Tags) <= MAX_BOUNDARY_BUFFER_SIZE, #Name " Module Tag list exceeds MAX_BOUNDARY_BUFFER_SIZE!"); \
    static_assert(ETCS::etcs_count_tags(Tags) <= ETCS_MAX_MODULE_TAGS, \
                  #Name " declares more contract tags than ETCS_MAX_MODULE_TAGS. That ceiling " \
                  "is ETCS::TAG_BITS -- one bit per tag in a tag_closure_mask -- so a tag past " \
                  "it would have no bit, and its same-type operations would stop serializing " \
                  "against each other in the reorder buffer. Split the module, or raise " \
                  "ETCS_MAX_MODULE_TAGS (an ABI change: GapSlot grows, so every module must be " \
                  "rebuilt against the same value)."); \
    static_assert(ETCS::etcs_longest_tag(Tags) < ETCS_MAX_TAG_NAME_SIZE, \
                  #Name " has a contract tag name at or over ETCS_MAX_TAG_NAME_SIZE. The " \
                  "boundary buffer is sized as ETCS_MAX_MODULE_TAGS * ETCS_MAX_TAG_NAME_SIZE, " \
                  "so an over-long name spends another tag's worth of budget."); \
    /* The module's own Tags string, in scope for every ETCS_TAG_BLOCK_* \
     * below it -- which is what lets ETCS_TAG_DECLARE resolve a type's \
     * own bit at COMPILE time (ETCS::etcs_tag_index) instead of looking \
     * it up by string at dispatch time. Requires this macro to precede \
     * the tag blocks in the .cc, which was already the convention; the \
     * static_assert in ETCS_TAG_DECLARE turns a reorder into a "not \
     * declared in this scope" rather than anything subtle. */ \
    static constexpr const char* ETCS_MODULE_TAGS_STRING = Tags; \
    /* Tag bit index — built from this EXACT same Tags string, in the \
     * order it declares them, so there's no separate hand-maintained \
     * list to drift out of sync. Runs at static-init time (NOT inside \
     * Name()'s own body below, which only runs if the loader actually \
     * dlsym's-and-calls it — a mere browse might never do that, and this \
     * needs to exist regardless): EventNode::getInstance() here resolves \
     * to THIS module's own per-DSO singleton naturally, the same reason \
     * every other per-module static-init already lands correctly. \
     * \
     * Retained even though dispatch no longer reads it: \
     * DecodeTagClosureMask still needs the reverse mapping to name which \
     * types were actually colliding when diagnosing a stall. */ \
    static const bool Name##_tag_bit_index_registered_ = []() { \
        std::vector<std::string> ordered_tags; \
        std::stringstream ss(Tags); \
        std::string tok; \
        while (ss >> tok) ordered_tags.push_back(tok); \
        ETCS::EventNode::getInstance().RegisterTagBitIndex(ordered_tags); \
        return true; \
    }(); \
    /* This module's own half of the loader/module manifest check -- runs \
     * at static-init time, i.e. during dlopen(), before dlopen() itself \
     * returns to the loader and so before RegisterDynamicLoader (the \
     * loader's first call INTO this module) is ever reached. Reaches the \
     * loader's manifest via dlsym(RTLD_DEFAULT, ...) against \
     * ETCS_GetLoaderManifest (DynamicLoader.h, loader build only, exported \
     * with default visibility and -rdynamic specifically so this lookup \
     * works) rather than waiting for the loader to hand it over -- neither \
     * side's check depends on the other having run, or on which order they \
     * run in; both only need to finish by the time RegisterDynamicLoader \
     * completes, which they structurally do. A dlsym miss (no loader in \
     * the process -- e.g. this module linked into a standalone test \
     * binary) is not itself a mismatch: nothing to compare against. \
     * Depends on ETCS_MODULE_EXPORT_MAIN being invoked after every header \
     * whose HEADER:/ONTOLOGY: hash should be checked has already been \
     * included in this translation unit -- true of every module as \
     * written today (this macro goes last), but now load-bearing rather \
     * than just eventually-true, since this runs mid-static-init instead \
     * of at the loader's first runtime call into Name(). */ \
    static const bool Name##_loader_manifest_checked_ = []() { \
        void* getterAddr = ETCS::etcs_find_loader_manifest_getter(); \
        if (!getterAddr) return true; \
        using LoaderManifestGetter = void* (*)(); \
        void* loaderManifestPtr = reinterpret_cast<LoaderManifestGetter>(getterAddr)(); \
        if (!loaderManifestPtr) return true; \
        auto* loaderManifest = static_cast<ETCS::Manifest*>(loaderManifestPtr); \
        if (ETCS::compareManifests(ETCS::Entity::getManifest(), loaderManifest, #Name)) { \
            std::cerr << "FATAL: module '" #Name "' and the loader disagree on " \
                         "CORE:/HEADER:/ONTOLOGY: hashes -- built for different epochs. " \
                         "Refusing to run." << std::endl; \
            /* TODO(recovery): re-fetch whichever of {this module, the \
             * loader} is older from anticurrententropy.com and retry once \
             * before aborting. A SECURITY boundary as much as a \
             * determinism one -- requires the fetch to be over TLS with \
             * the cert chain signed by the ACE root key on both the \
             * binary and its source, not the plain LetsEncrypt cert the \
             * site uses today. Not wired in yet -- pending that signing \
             * infrastructure and the release-serving protocol -- so this \
             * goes straight to abort() rather than trusting an \
             * unverifiable replacement, or running as one that already \
             * disagrees with the loader. */ \
            std::abort(); \
        } \
        return true; \
    }(); \
    extern "C" ETCS_API HASH_TYPE Name##_GetHash() { \
        return ETCS::GenerateEnvironmentSignature(#Name); \
    } \
    extern "C" ETCS_API ETCS::Manifest* Name(ETCS::BBuffer& buff) { \
        buff.writeString(Tags); \
        SandboxGuard::getInstance(); /* we want to make sure this is cleaned up first*/\
        return &ETCS::Entity::getManifest(); \
    } \
    extern "C" ETCS_API void Name##_Cleanup() { \
        /* Each singleton is guarded separately: they are independent \
         * __cxa_atexit registrations, so on the process-exit path any \
         * subset of them may already be gone. */ \
        if (ETCS::EventNode::alive())  ETCS::EventNode::getInstance().stream.stop(); \
        if (ETCS::ThreadPool::alive()) ETCS::ThreadPool::getInstance().trigger_shutdown_drain(); \
        /* ridMap is an ArenaMap -- its bucket array and nodes physically \
         * live inside this module's own arena. EventNode::~EventNode() is \
         * = default, meaning it destructs ridMap LATER, at dlclose()-time \
         * (ordinary C++ static destruction, triggered by the DSO unload \
         * itself) -- well after memoryTeardown() below has already \
         * munmap'd the pages that bucket array lives in. Clearing it HERE, \
         * while the arena is still fully valid, means every node gets \
         * properly destructed now; by the time ~EventNode() eventually \
         * runs, ridMap is empty and has nothing left to iterate over -- \
         * ArenaAllocator::deallocate() being a no-op is necessary but not \
         * sufficient on its own, since walking an unordered_map's own \
         * bucket structure to FIND what to destruct is a read into that \
         * structure before deallocate is ever reached. */ \
        if (ETCS::EventNode::alive())   ETCS::EventNode::getInstance().ridMap.clear(); \
        if (ETCS::MemoryArena::alive()) ETCS::MemoryArena::getInstance().cleanupTypedEntities(); \
        /* ETCS::MemoryArena::getInstance().memoryTeardown();// we don't do this because it messes up meyers cleanup order for when the static dtor is called later */\
    } /* because threadpool is static as well, we need a proxy from the functions exported by the DLL */ \
    extern "C" ETCS_API ETCS::EventNode* RegisterEventNode() \
    { /* each module is responsible for forwarding its events to the loader via the EventNode static*/ \
        ETCS::ThreadPool::getInstance();\
        return &ETCS::EventNode::getInstance();\
    } \
    /* Name##_GetArena — exposes THIS module's own MemoryArena::getInstance().
     * Under -fvisibility=hidden (this project's actual build flags), an
     * inline function-local static defined in a header is NOT unified
     * across separately-linked DSOs: the loader binary and every module
     * .so each get their OWN private copy of MemoryArena::getInstance()'s
     * static instance. Every entity a module creates (_make_##Name below
     * calls MemoryArena::getInstance().allocate<Name>(), compiled INTO
     * that module) is therefore allocated from — and dtor-chained onto —
     * THAT module's own private arena, never the loader's. This export is
     * the loader's only way to reach it, the same pattern
     * RegisterDynamicLoader/RegisterRootSignalContext already use for
     * crossing this exact boundary. Resolved once per module (see
     * Module::module_arena in DynamicLoader.h) and never re-queried. */ \
    extern "C" ETCS_API ETCS::MemoryArena* Name##_GetArena() \
    { \
        return &ETCS::MemoryArena::getInstance(); \
    }

/**
 * ETCS_MODULE_EXPORT_WORK
 * Internal use by ETCS_TAG_BLOCK
 *
 * Exported SYMBOL names are Type-qualified (Type_ActionName_Work /
 * Type_ActionName_GetHash) to avoid collisions when two different tags in
 * the same module export an action with the same name — Close, GetContent,
 * etc. This is independent of the MANIFEST TOKEN, which stays unqualified
 * (see ETCS_WRITE_WORK_TOKEN below) to avoid overflowing the fixed
 * MAX_TAG_BUFFER_SIZE manifest buffer on tags with several actions —
 * discoverActions (DynamicLoader.h) reconstructs the qualified symbol name
 * itself from the token plus the tag it's already iterating, so nothing
 * downstream needs the token itself to carry the qualification.
 */
#define ETCS_MODULE_EXPORT_WORK(Type, ActionName) \
    extern "C" ETCS_API ETCS::WorkFunc Type##_##ActionName##_Work() { return &_work_##Type##_##ActionName; } \
    extern "C" ETCS_API HASH_TYPE Type##_##ActionName##_GetHash() { \
        return ETCS::GenerateEnvironmentSignature(#Type "." #ActionName); \
    }
/**
 * ETCS_MODULE_EXPORT_STREAM
 * Internal use by ETCS_TAG_BLOCK
 */
#define ETCS_MODULE_EXPORT_STREAM(Type, ActionName) \
    extern "C" ETCS_API ETCS::StreamFunc Type##_##ActionName##_Stream() { return &_stream_##Type##_##ActionName; } \
    extern "C" ETCS_API HASH_TYPE Type##_##ActionName##_GetHash() { \
        return ETCS::GenerateEnvironmentSignature(#Type "." #ActionName); \
    }
/**
 * ETCS_TAG_DECLARE
 * Expands into the internal _make_ factory function.
 * This handles allocation, the myTagInto buffer write, and boundary logging.
 */
#ifdef ETCS_VERBOSE_DISPATCH
    #define ETCS_DLOG(...) ETCS_LOG(__VA_ARGS__)
#else
    #define ETCS_DLOG(...) do {} while(0)
#endif
#define ETCS_TAG_DECLARE(Name) \
    /* This type's own bit position in its module's Tags string, resolved \
     * at COMPILE time from the same string, with the same tokenization, \
     * that RegisterTagBitIndex uses at static-init. Two asserts, two \
     * genuinely different mistakes: the first catches a tag block whose \
     * type was never added to the module's Tags string (which used to \
     * compile fine and silently lose that type's ordering guarantees); \
     * the second is the budget, restated here so it fires at the tag \
     * rather than only at the module. */ \
    static constexpr size_t _tag_bit_##Name = \
        ETCS::etcs_tag_index(ETCS_MODULE_TAGS_STRING, #Name); \
    static_assert(_tag_bit_##Name != ETCS::ETCS_TAG_NOT_FOUND, \
                  #Name " has a tag block but does not appear in this module's " \
                  "ETCS_MODULE_EXPORT_MAIN Tags string. It would spawn, list and " \
                  "dispatch correctly while silently getting no tag bit, so its " \
                  "same-type operations would stop serializing against each other."); \
    static_assert(_tag_bit_##Name < ETCS_MAX_MODULE_TAGS, \
                  #Name " sits past ETCS_MAX_MODULE_TAGS in its module's Tags string."); \
    /* Publishes the compile-time bit onto the TYPE itself (see \
     * WIRE_TYPE_IDENTITY). This is what lets a TagModifyEvent carry its \
     * own mask instead of the handling EventNode looking it up by string \
     * -- which mattered for more than speed: LoaderStream's own copy of \
     * that lookup consulted the LOADER's tag_bit_index, which no module \
     * ever populates, so it returned an empty mask every time. */ \
    static const bool _tag_mask_##Name##_registered = []() { \
        /* Name may be a contract typedef, so both statics below are the \
         * CONCRETE type's -- the point being contract VALUES (#Name and its \
         * bit) in concrete STORAGE, read back through CONTRACT_TAG and \
         * myTagMask(). \
         * \
         * Two tag blocks aliasing one concrete type therefore share one pair, \
         * and whichever init ran second would silently win, the other losing \
         * both its bit and its name indistinguishably. TAG_MASK starts empty, \
         * so this is the only place that collision is detectable. */ \
        if (Name::TAG_MASK.any()) \
        { \
            ETCS_LOG("ETCS_TAG_DECLARE", "FATAL: " #Name "'s concrete type already " \
                     "carries a TAG_MASK (answering to '" << Name::CONTRACT_TAG \
                     << "'). Two contract tags alias one concrete type."); \
            std::abort(); \
        } \
        Name::CONTRACT_TAG = #Name; \
        Name::TAG_MASK     = ETCS::TagMask::bit(_tag_bit_##Name); \
        return true; \
    }(); \
    static ETCS::RIDList<Name*>& _ridlist_##Name() {\
        static ETCS::RIDList<Name*> list;\
        return list;\
    }\
    static bool _ridlist_##Name##_registered = []() {\
        ETCS::ThreadPool::getInstance(); \
        ETCS::EventNode::getInstance().RegisterRIDRegistry(\
            #Name, \
            _ridlist_##Name().handle(#Name) \
        );\
        return true;\
    }(); \
    ETCS::Entity* _make_child_##Name(ETCS::Entity* parent) \
    { \
        if (!parent) return nullptr; \
        return parent->addTag<Name>(); \
    } \
    ETCS::Entity* _make_##Name(ETCS::Buffer& buf) \
    { \
        Name* handler = ETCS::MemoryArena::getInstance().allocate<Name>(); \
        buf.writeString(""); \
        _ridlist_##Name().insert(handler->getRID(), handler);\
        /* Into every family aggregate this type composes, now that it is \
         * fully constructed -- see etcs_supertype_fanout (Entity.h). */ \
        ETCS::etcs_supertype_fanout(handler); \
        bool success = handler->myTagInto(buf); \
        \
        if (success){ \
            ETCS_DLOG(#Name, " success~! Tag crossed DLL boundary: " << handler->myTag()); \
        } else { \
            ETCS_DLOG(#Name, " failed... tag didn't cross DLL boundary: " << handler->myTag()); \
        } \
        return handler; \
    }
/**
 * ETCS_TAG_BLOCK
 * Declares a Tag and automatically maps all its work functions.
 * Converts comma-separated __VA_ARGS__ into a space-separated manifest string.
 */
#define ETCS_TAG_BLOCK_BASIC(TagName, ...) \
    ETCS_TAG_DECLARE(TagName) \
    static_assert(sizeof(#__VA_ARGS__) <= MAX_BOUNDARY_BUFFER_SIZE, "Action list for " #TagName " exceeds MAX_BOUNDARY_BUFFER_SIZE!"); \
    extern "C" ETCS_API ETCS::MakeFunc TagName##_Make() { return &_make_##TagName; } \
    extern "C" ETCS_API ETCS::MakeChildFunc TagName##_MakeChild() { return &_make_child_##TagName; } \
    extern "C" ETCS_API HASH_TYPE TagName##_GetHash() { \
        return ETCS::GenerateEnvironmentSignature(#TagName); \
    } \
    extern "C" ETCS_API ETCS::Manifest* TagName##_List(ETCS::BBuffer& buff) { \
        FOR_EACH_FIXED(ETCS_WRITE_WORK_TOKEN, TagName, __VA_ARGS__) \
        return &ETCS::Entity::getManifest(); \
    }\
    FOR_EACH_FIXED(ETCS_MODULE_EXPORT_WORK, TagName, __VA_ARGS__)
/**
 * ETCS_WRITE_WORK_TOKEN / ETCS_WRITE_STREAM_TOKEN
 *
 * Tokens written into the manifest are deliberately UNQUALIFIED (just
 * "<Action>_Work" / "<Action>_Stream", not "TagName_<Action>_Work") —
 * TagName is accepted (via FOR_EACH_FIXED) but intentionally unused here.
 * Repeating the tag name once per action inside the token was what
 * overflowed the fixed MAX_TAG_BUFFER_SIZE (256-byte) manifest buffer for
 * any tag with more than a handful of actions. discoverActions
 * (DynamicLoader.h) reconstructs the qualified symbol name from the token
 * plus the tag it's already iterating over, so the qualification only
 * needs to live in the EXPORTED SYMBOL name (see ETCS_MODULE_EXPORT_WORK
 * above), not in this token.
 */
#define ETCS_WRITE_WORK_TOKEN(TagName, Name)   buff.write(#Name "_Work ");
#define ETCS_WRITE_STREAM_TOKEN(TagName, Name) buff.write(#Name "_Stream ");
/**
 * ETCS_TAG_BLOCK_HYBRID
 */
#define ETCS_TAG_BLOCK_HYBRID(TagName, WorkActions, StreamActions) \
    ETCS_TAG_DECLARE(TagName) \
    /* MAX_BOUNDARY_BUFFER_SIZE, not MAX_TAG_BUFFER_SIZE. This assert \
     * measured a hybrid tag's action list against the 256-byte per-item \
     * ceiling while _BASIC measured the identical thing against the \
     * boundary-transport ceiling -- two different limits for one \
     * quantity, and the stricter one was on the tag type that declares \
     * MORE actions, not fewer. Both go through TagName##_List into the \
     * same BBuffer, so the boundary size is the one that actually \
     * governs. */ \
    static_assert(sizeof(#WorkActions #StreamActions) <= MAX_BOUNDARY_BUFFER_SIZE, "Module actions list too large!"); \
    \
    extern "C" ETCS_API ETCS::MakeFunc TagName##_Make() { return &_make_##TagName; } \
    extern "C" ETCS_API ETCS::MakeChildFunc TagName##_MakeChild() { return &_make_child_##TagName; } \
    extern "C" ETCS_API HASH_TYPE TagName##_GetHash() { \
        return ETCS::GenerateEnvironmentSignature(#TagName); \
    } \
    \
    extern "C" ETCS_API ETCS::Manifest* TagName##_List(ETCS::BBuffer& buff) { \
        /* Use FOR_EACH to write each name without commas */ \
        FOR_EACH_FIXED(ETCS_WRITE_WORK_TOKEN, TagName, BRACKET_UNWRAP WorkActions) \
        FOR_EACH_FIXED(ETCS_WRITE_STREAM_TOKEN, TagName, BRACKET_UNWRAP StreamActions) \
        return &ETCS::Entity::getManifest(); \
    } \
    \
    /* Symbol Exports */ \
    FOR_EACH_FIXED(ETCS_MODULE_EXPORT_WORK, TagName, BRACKET_UNWRAP WorkActions) \
    FOR_EACH_FIXED(ETCS_MODULE_EXPORT_STREAM, TagName, BRACKET_UNWRAP StreamActions)


#endif