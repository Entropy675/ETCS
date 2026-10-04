#ifndef RECLAIM_H__
#define RECLAIM_H__
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <utility>
#include <vector>
#if defined(__linux__) && !defined(__EMSCRIPTEN__)
#include <linux/membarrier.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

/*
 * READS WITHOUT LOCKS, AND WHEN WHAT THEY READ MAY BE FREED.
 *
 * What an entity shows the world (its flags, values and tags; its children) is
 * written by few -- the ordering thread, mostly, which already puts every write
 * in one order -- and read by many: every walk, every hash, every frame. So the
 * readers take no lock. A writer, still serialized against other writers, builds
 * a new immutable view and publishes it with one pointer store (Entity's
 * surface_view_/child_view_); a reader loads the pointer and reads the view as
 * it is. The only question left is when the view a writer replaced can be
 * freed, and this answers it: once no reader can still be holding it.
 *
 * GRACE PERIODS, with the read side nearly free. A reader announces the period
 * it entered in (its thread's slot) and clears it when it leaves; ReadSection
 * does both, and nests for free. A retired block records the period it was
 * retired in -- taking it is the step that moves the period on -- and is freed
 * once no announced reader entered at or before it. Freeing scans the slots
 * after a process-wide barrier (membarrier, private expedited): that barrier is
 * what lets a reader skip a fence of its own, so the reader's cost is a load
 * and two stores. Where membarrier is not available (a browser, an old kernel)
 * the reader fences instead, and the scan does not need the barrier.
 *
 * ONE PER RUNTIME. An entity made in one image is read in another, so every
 * image must announce into the same slots: a module adopts the loader's
 * instance (adopt_shared, through the loader's node, as ThreadPool does). Each
 * image keeps its own thread-local slot pointer, so a thread reading in two
 * images has two slots -- correct, since the scan takes the oldest of all.
 *
 * WHAT IS RETIRED IS A BLOCK FROM allocate(): plain bytes, no destructor, so a
 * block a module retired can be freed after that module is unloaded without
 * calling into code that is gone. Freed blocks are kept by size (a power of
 * two, up to a budget) and handed out again: a view is rebuilt on every
 * change, and returning each one to malloc had it trimmed and faulted back in.
 */
namespace ETCS
{
class Reclaimer
{
public:
    struct Slot
    {
        ::std::atomic<uint64_t> period{ 0 };   // 0: not reading
        ::std::atomic<bool>     owned{ true };
        Slot*                   next = nullptr;
    };

    static Reclaimer*& shared_slot() { static Reclaimer* p = nullptr; return p; }
    static void adopt_shared(Reclaimer* r) { if (r) shared_slot() = r; }
    // Never destroyed: an arena torn down during static destruction still
    // retires its entities' views, after any static of ours would be gone.
    // What is retired by then is the process's to reclaim.
    static Reclaimer& getInstance()
    {
        if (Reclaimer* r = shared_slot()) return *r;
        static Reclaimer* instance = new Reclaimer;
        return *instance;
    }

    Reclaimer()
    {
#if defined(__linux__) && !defined(__EMSCRIPTEN__) && defined(__x86_64__) && !defined(__SANITIZE_THREAD__)
        const long cmds = ::syscall(__NR_membarrier, MEMBARRIER_CMD_QUERY, 0);
        if (cmds > 0 && (cmds & MEMBARRIER_CMD_PRIVATE_EXPEDITED)
            && ::syscall(__NR_membarrier, MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED, 0) == 0)
            asymmetric_ = true;
#endif
    }
    // This thread's slot in this image, made once. A slot left by a thread that
    // has ended is taken over rather than a new one made.
    Slot* slot()
    {
        struct Holder { Slot* s = nullptr; ~Holder() { if (s) { s->period.store(0); s->owned.store(false); } } };
        static thread_local Holder mine;
        if (mine.s) return mine.s;
        ::std::lock_guard<::std::mutex> lock(mu_);
        for (Slot* s = slots_; s; s = s->next)
        {
            bool free_slot = false;
            if (!s->owned.load() && s->owned.compare_exchange_strong(free_slot, true)) { mine.s = s; return s; }
        }
        Slot* s = new Slot;
        s->next = slots_;
        slots_  = s;
        mine.s  = s;
        return s;
    }

    void enter(Slot* s)
    {
        s->period.store(period_.load(::std::memory_order_acquire), ::std::memory_order_relaxed);
        if (asymmetric_) ::std::atomic_signal_fence(::std::memory_order_seq_cst);
        else             ::std::atomic_thread_fence(::std::memory_order_seq_cst);
    }
    void leave(Slot* s)
    {
        s->period.store(0, ::std::memory_order_release);
    }

    // A block for a view: at least `bytes`, 16-aligned.
    void* allocate(size_t bytes)
    {
        const unsigned c = classOf(bytes + kHead);
        char* raw = nullptr;
        {
            ::std::lock_guard<::std::mutex> lock(mu_);
            if (!pool_[c].empty()) { raw = pool_[c].back(); pool_[c].pop_back(); pooled_ -= size_t(1) << c; }
        }
        if (!raw) raw = static_cast<char*>(::std::malloc(size_t(1) << c));
        *reinterpret_cast<uint32_t*>(raw) = c;
        return raw + kHead;
    }

    // A block no new reader can reach (its replacement is already published):
    // freed once every reader that might hold it has left.
    void retire(void* block)
    {
        if (!block) return;
        const uint64_t at = period_.fetch_add(1, ::std::memory_order_acq_rel);
        bool collect_now = false;
        {
            ::std::lock_guard<::std::mutex> lock(mu_);
            retired_.emplace_back(block, at);
            retired_bytes_ += size_t(1) << *reinterpret_cast<uint32_t*>(static_cast<char*>(block) - kHead);
            collect_now = retired_.size() >= kBatch || retired_bytes_ >= kBatchBytes;
        }
        if (collect_now) collect();
    }

    void collect()
    {
        // The other half of the reader's fence: publish, then look at the slots
        // (a reader announces, then looks at the pointer). Asymmetric, every
        // running thread is fenced here instead of on its own read path.
#if defined(__linux__) && !defined(__EMSCRIPTEN__) && defined(__x86_64__)
        if (asymmetric_) ::syscall(__NR_membarrier, MEMBARRIER_CMD_PRIVATE_EXPEDITED, 0);
        else
#endif
        ::std::atomic_thread_fence(::std::memory_order_seq_cst);
        ::std::vector<void*> to_free;
        {
            ::std::lock_guard<::std::mutex> lock(mu_);
            uint64_t oldest = UINT64_MAX;
            for (Slot* s = slots_; s; s = s->next)
            {
                const uint64_t p = s->period.load(::std::memory_order_acquire);
                if (p && p < oldest) oldest = p;
            }
            size_t kept = 0;
            for (auto& r : retired_)
            {
                if (r.second < oldest) to_free.push_back(r.first);
                else retired_[kept++] = r;
            }
            retired_.resize(kept);
            retired_bytes_ = 0;
            for (auto& r : retired_) retired_bytes_ += size_t(1) << *reinterpret_cast<uint32_t*>(static_cast<char*>(r.first) - kHead);
            for (void*& p : to_free)
            {
                char* raw = static_cast<char*>(p) - kHead;
                const unsigned c = *reinterpret_cast<uint32_t*>(raw);
                if (pooled_ + (size_t(1) << c) > kPoolBudget) { p = raw; continue; }   // freed below
                pool_[c].push_back(raw);
                pooled_ += size_t(1) << c;
                p = nullptr;
            }
        }
        for (void* p : to_free) if (p) ::std::free(p);
    }

    bool asymmetric() const { return asymmetric_; }

private:
    // A collect costs a membarrier (~20 us here: an interrupt to every core
    // running one of our threads), so it waits for a batch: by count for the
    // many small surface views, by bytes for the large child views.
    static constexpr size_t   kBatch      = 1024;
    static constexpr size_t   kBatchBytes = size_t(1) << 20;
    static constexpr size_t   kHead       = 16;                  // the size class, kept ahead of the block
#if defined(__SANITIZE_ADDRESS__)
    static constexpr size_t   kPoolBudget = 0;                   // every block freed: a late reader is a report
#else
    static constexpr size_t   kPoolBudget = size_t(16) << 20;    // bytes kept for reuse
#endif
    static constexpr unsigned kClasses    = 48;
    static unsigned classOf(size_t bytes) { unsigned c = 6; while ((size_t(1) << c) < bytes) ++c; return c; }
    ::std::vector<char*>    pool_[kClasses];
    size_t                  pooled_ = 0;
    // What every reader loads, on a line of their own; the writers' state
    // (the lock, the pool, the retired list) on others.
    alignas(64) ::std::atomic<uint64_t> period_{ 1 };
    bool                    asymmetric_ = false;
    alignas(64) size_t      retired_bytes_ = 0;
    ::std::mutex            mu_;
    Slot*                   slots_ = nullptr;
    ::std::vector<::std::pair<void*, uint64_t>> retired_;
};

/*
 * A read of published views. Nests: only the outermost announces, so a walk
 * that opens one around itself pays once however many reads it makes inside.
 */
struct ReadSection
{
    ReadSection()
    {
        if (depth()++ == 0)
        {
            Reclaimer& r = Reclaimer::getInstance();
            s_ = r.slot();
            r.enter(s_);
        }
    }
    ~ReadSection()
    {
        if (--depth() == 0) Reclaimer::getInstance().leave(s_);
    }
    ReadSection(const ReadSection&) = delete;
    ReadSection& operator=(const ReadSection&) = delete;
private:
    static unsigned& depth() { static thread_local unsigned d = 0; return d; }
    Reclaimer::Slot* s_ = nullptr;
};
} // namespace ETCS
#endif // RECLAIM_H__
