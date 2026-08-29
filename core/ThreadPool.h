#ifndef THREADPOOL_H__
#define THREADPOOL_H__

#include <vector>
#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <future>
#include <functional>
#include <atomic>
#include <unordered_map>
#include <chrono>
#include <iostream>
#include <string>

#ifdef __linux__
#include <liburing.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#elif defined(_WIN32)
#include <winsock2.h>
#include <windows.h>
#endif

#include "Buffer.h"
#include "MemoryArena.h"

namespace ETCS
{

enum class Priority : int
{
    Low     = 10,
    Medium  = 50,
    High    = 100
};

enum class IOOp : uint8_t
{
    Accept,
    Recv,
    Send,
    Timeout,
    Close,
    Cancel      // cancel all pending ops on a given fd via io_uring_prep_cancel_fd
};

struct IOCompletion
{
    IOOp        op;
    int         fd;
    int         result;
    void*       buffer;
    size_t      buffer_len;
    ETCS::SignalContext ctx;
    std::function<void(IOCompletion)> callback;
};

struct IOSubmission
{
#ifdef _WIN32
    OVERLAPPED  overlapped = {};
#endif

    IOOp        op;
    int         fd;
    void*       buffer     = nullptr;
    size_t      buffer_len = 0;
    int         priority;
    ETCS::SignalContext ctx;
    std::function<void(IOCompletion)> callback;

    uint64_t    timeout_ns = 0;
#ifdef __linux__
    __kernel_timespec ts = {};
#endif
};

class IOSubmissionPool
{
public:
    IOSubmission* acquire()
    {
        auto& arena = ETCS::MemoryArena::getInstance();
        // Free list FIRST -- these are all identically sized by construction
        // (one concrete type, never an array), so the bin always matches and
        // a released submission is always reusable by the next acquire.
        void* mem = arena.tryAcquireFromFreeList(
            static_cast<long long>(sizeof(IOSubmission)),
            static_cast<long long>(alignof(IOSubmission)));
        if (!mem)
            mem = arena.allocateRaw(sizeof(IOSubmission), alignof(IOSubmission));
        return new (mem) IOSubmission{};
    }

    void release(IOSubmission* p)
    {
        // Return the bytes, not just run the destructor. Without this the
        // "pool" pools nothing: every submission permanently consumed
        // sizeof(IOSubmission) from the module root arena, which at three
        // submissions per HTTP request (accept + recv + send) is ~2KB per
        // request -- invisible per request, tens of MB over a day of polling,
        // and entirely independent of entity lifetime, which is why it
        // survived every entity-teardown fix.
        //
        // Destructor first, THEN release: releaseToFreeList memsets the
        // block, and the SignalContext/std::function members must have
        // destructed before their storage is overwritten.
        auto& arena = ETCS::MemoryArena::getInstance();
        p->~IOSubmission();
        arena.releaseToFreeList(p,
            static_cast<long long>(sizeof(IOSubmission)),
            static_cast<long long>(alignof(IOSubmission)));
    }
};

class ThreadPool
{
private:
    std::atomic<bool> is_drained_{false};

    struct TaskMetadata
    {
        ETCS::SignalContext ctx;
        std::chrono::steady_clock::time_point signal_detected_at;
        bool is_terminating = false;
        bool dump_requested = false;
        bool cancel_requested = false;
        std::thread::native_handle_type native_handle;
    };

    void watchdog_loop()
    {
        while (!watchdog_stop_)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));

            std::unique_lock<std::mutex> lock(active_tasks_mutex_);
            auto now = std::chrono::steady_clock::now();

            for (auto it = active_tasks_.begin(); it != active_tasks_.end(); )
            {
                TaskMetadata& meta = it->second;
                bool signal_active = meta.ctx.isInterrupted() || meta.ctx.isTerminated();

                if (!signal_active) { ++it; continue; }

                if (!meta.is_terminating)
                {
                    ETCS_LOG("ThreadPool:WATCHDOG", "Active signal detected for: " << meta.ctx.tag);
                    meta.is_terminating = true;
                    meta.signal_detected_at = now;
                    ++it;
                    continue;
                }

                auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
                    now - meta.signal_detected_at);
                long long ms = duration.count();

                if (ms > 400 && ms <= 500 && meta.ctx.user1 && !meta.dump_requested)
                {
                    meta.ctx.user1->store(1, std::memory_order_release);
                    meta.dump_requested = true;
                    ETCS_LOG("ThreadPool:WATCHDOG", "Task '" << meta.ctx.tag << "' is hanging. Requested state dump (User1).");
                }

                if (ms > 500 && !meta.cancel_requested)
                {
                    {
                        std::lock_guard<std::mutex> err_lock(error_mutex_);
                        last_error_tag = meta.ctx.tag.toString();
                        has_error = true;
                    }

                    ETCS_LOG("ThreadPool:WATCHDOG", "CRITICAL: Task '" << meta.ctx.tag
                              << "' ignored signals for 500ms. Parent: " << meta.ctx.parent
                              << ". Requesting cancellation of thread " << it->first << " (one-shot).");
    #ifdef __linux__
                    pthread_cancel(meta.native_handle);
    #elif defined(_WIN32)
                    TerminateThread(meta.native_handle, 0);
    #endif
                    meta.cancel_requested = true;
                }

                ++it;
            }
        }
    }

#ifdef __linux__
    static constexpr unsigned RING_ENTRIES = 256;
    struct io_uring ring_;

    void io_completion_loop()
    {
        while (!io_stop_)
        {
            struct io_uring_cqe* cqe = nullptr;
            struct __kernel_timespec ts { .tv_sec = 0, .tv_nsec = 100'000'000 };
            int ret = io_uring_wait_cqe_timeout(&ring_, &cqe, &ts);

            if (ret == -ETIME || ret == -EINTR)
                continue;

            if (ret < 0)
            {
                ETCS_LOG("ThreadPool:IO", "io_uring_wait_cqe_timeout error: " << ret);
                continue;
            }

            if (!cqe) continue;

            IOSubmission* sub = reinterpret_cast<IOSubmission*>(io_uring_cqe_get_data(cqe));
            int result = cqe->res;
            io_uring_cqe_seen(&ring_, cqe);

            if (!sub) continue;

            // Cancel completions have no meaningful callback — just release the sub
            if (sub->op == IOOp::Cancel)
            {
                std::lock_guard<std::mutex> lock(submit_mutex_);
                sub_pool_.release(sub);
                continue;
            }

            IOCompletion completion
            {
                sub->op, sub->fd, result,
                sub->buffer, sub->buffer_len,
                sub->ctx, sub->callback
            };

            auto cb  = sub->callback;
            int  pri = sub->priority;
            auto ctx = sub->ctx;
            {
                std::lock_guard<std::mutex> lock(submit_mutex_);
                sub_pool_.release(sub);
            }

            if (!cb) continue;
            // enqueue throws once stop_ is set; uncaught it unwinds the loop
            // into std::terminate during shutdown.
            try { enqueue(pri, ctx, [cb, completion]() mutable { cb(completion); }); }
            catch (const std::exception& e)
            { ETCS_LOG("ThreadPool:IO", "completion dropped: " << e.what()); }
        }
    }

#elif defined(_WIN32)
    HANDLE iocp_        = INVALID_HANDLE_VALUE;
    bool   iocp_closed_ = false;

    void io_completion_loop()
    {
        while (!io_stop_)
        {
            DWORD     bytes_transferred = 0;
            ULONG_PTR completion_key    = 0;
            OVERLAPPED* overlapped      = nullptr;

            BOOL ok = GetQueuedCompletionStatus(
                iocp_, &bytes_transferred, &completion_key, &overlapped, 100);

            if (!ok && !overlapped) continue;

            IOSubmission* sub = reinterpret_cast<IOSubmission*>(overlapped);
            if (!sub) continue;

            IOCompletion completion
            {
                sub->op,
                static_cast<int>(completion_key),
                static_cast<int>(bytes_transferred),
                sub->buffer, sub->buffer_len,
                sub->ctx, sub->callback
            };

            auto cb  = sub->callback;
            int  pri = sub->priority;
            auto ctx = sub->ctx;
            {
                std::lock_guard<std::mutex> lock(submit_mutex_);
                sub_pool_.release(sub);
            }

            if (!cb) continue;
            try { enqueue(pri, ctx, [cb, completion]() mutable { cb(completion); }); }
            catch (const std::exception& e)
            { ETCS_LOG("ThreadPool:IO", "completion dropped: " << e.what()); }
        }
    }
#endif

    struct Task
    {
        int priority;
        ETCS::SignalContext ctx;
        std::function<void()> func;

        Task() : priority(0), ctx({}), func(nullptr) {}
        Task(int p, ETCS::SignalContext c, std::function<void()> f)
            : priority(p), ctx(c), func(std::move(f)) {}
    };

    struct CompareTask
    {
        bool operator()(const Task& a, const Task& b) const
        {
            return a.priority < b.priority;
        }
    };

    std::vector<std::thread> workers_;
    std::priority_queue<Task, std::vector<Task>, CompareTask> task_queue_;

    std::mutex              queue_mutex_;
    std::mutex              active_tasks_mutex_;
    std::mutex              error_mutex_;
    std::condition_variable condition_;

    std::atomic<bool> stop_;
    std::atomic<bool> watchdog_stop_;
    std::atomic<bool> io_stop_;
    std::atomic<bool> io_cleaned_up_{false};

    std::unordered_map<std::thread::id, TaskMetadata> active_tasks_;
    std::thread watchdog_thread_;
    std::thread io_thread_;

    std::atomic<bool> has_error{false};
    std::string       last_error_tag{"none"};

    // io_uring_get_sqe bumps the SQ tail non-atomically and liburing gives no
    // thread-safety without SQPOLL. Every pool worker calls submit().
    std::mutex       submit_mutex_;
    IOSubmissionPool sub_pool_;

    void cleanup_io()
    {
        bool expected = false;
        if (!io_cleaned_up_.compare_exchange_strong(expected, true))
            return;
#ifdef __linux__
        io_uring_queue_exit(&ring_);
#elif defined(_WIN32)
        if (iocp_ != INVALID_HANDLE_VALUE)
        {
            CloseHandle(iocp_);
            iocp_ = INVALID_HANDLE_VALUE;
        }
#endif
    }

public:
    explicit ThreadPool(size_t threads = DEFAULT_THREAD_POOL_THREADS)
        : stop_(false), watchdog_stop_(false), io_stop_(false)
    {
        // ---------------------------------------------------------------
        // Static-destruction-order guard. MemoryArena::getInstance() and
        // ThreadPool::getInstance() are two independent Meyers-singleton
        // function-local statics with no ordering relationship between
        // them otherwise -- C++ only guarantees teardown happens in the
        // REVERSE of first-construction order, and nothing enforces which
        // of the two the rest of the codebase happens to touch first.
        //
        // That's not a theoretical concern -- it's what a real, reproduced
        // SIGSEGV traced back to: at process exit, if MemoryArena ever
        // gets first-constructed AFTER ThreadPool (entirely possible,
        // depending on which singleton some unrelated static initializer
        // happens to touch first), MemoryArena is torn down FIRST at exit
        // -- while ThreadPool's own worker threads are still alive and
        // still capable of running a queued task that touches entity
        // memory, since ~ThreadPool() (the thing that actually stops and
        // joins them) hasn't run yet. The crash traced to exactly that: a
        // worker thread's own do_recv/send callback still executing,
        // concurrently, while ~MemoryArena()'s static destructor was
        // already mid-walk through the same arena's dtor chain.
        //
        // This line forces MemoryArena to always be constructed (and, by
        // the reverse rule, always destructed AFTER) before ThreadPool's
        // own constructor finishes -- the standard "construct-on-first-
        // use dependency" idiom for pinning relative order between two
        // otherwise-unrelated Meyers singletons. Whichever caller first
        // triggers ThreadPool::getInstance() now transitively guarantees
        // MemoryArena::getInstance() already exists first, regardless of
        // what either singleton's own constructor body actually does.
        // Safe regardless of how early this runs: Meyers singletons are
        // designed to be first-constructed at any point, by any caller.
        ETCS::MemoryArena::getInstance();

#ifdef __linux__
        if (io_uring_queue_init(RING_ENTRIES, &ring_, 0) < 0)
            throw std::runtime_error("[IO] Failed to init io_uring");
#elif defined(_WIN32)
        iocp_ = CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 1);
        if (!iocp_ || iocp_ == INVALID_HANDLE_VALUE)
            throw std::runtime_error("[IO] Failed to create IOCP");
#endif

        ETCS_LOG("ThreadPool", "Initializing with hardware threads: " << threads);
        workers_.reserve(threads);
        for (size_t i = 0; i < threads; ++i)
        {
            workers_.emplace_back([this]()
            {
                while (true)
                {
                    Task task;
                    {
                        std::unique_lock<std::mutex> lock(queue_mutex_);
                        condition_.wait(lock, [this] { return stop_ || !task_queue_.empty(); });
                        if (stop_ && task_queue_.empty()) return;
                        task = std::move(task_queue_.top());
                        task_queue_.pop();
                    }

                    {
                        std::lock_guard<std::mutex> lock(active_tasks_mutex_);
                        TaskMetadata meta;
                        meta.ctx                = task.ctx;
                        meta.signal_detected_at  = std::chrono::steady_clock::now();
#ifdef _WIN32
                        meta.native_handle = GetCurrentThread();
#else
                        meta.native_handle = pthread_self();
#endif
                        active_tasks_[std::this_thread::get_id()] = std::move(meta);
                    }
                    
                    try { task.func(); }
                    catch (const std::exception& e)
                    {
                        ETCS_LOG("ThreadPool", "Worker caught exception in '"
                                 << task.ctx.tag << "': " << e.what());
                    }
                    catch (...) {}

                    {
                        std::lock_guard<std::mutex> lock(active_tasks_mutex_);
                        active_tasks_.erase(std::this_thread::get_id());
                    }
                }
            });
        }

        watchdog_thread_ = std::thread(&ThreadPool::watchdog_loop, this);
        io_thread_       = std::thread(&ThreadPool::io_completion_loop, this);
        s_alive.store(true, std::memory_order_release);
    }

    // Liveness, readable AFTER this object has been destroyed. Same hazard
    // and same shape as EventNode::s_alive -- see there for the full
    // reasoning. Constant-initialized and trivially destructible, so it has
    // no destructor of its own to be ordered against and stays valid for as
    // long as this DSO is mapped.
    //
    // This is the CROSS-DSO half of the ordering problem the constructor
    // below already solves intra-DSO by pinning MemoryArena ahead of itself.
    // That trick works because both singletons are ours; it cannot reach the
    // loader's statics, which register with __cxa_atexit before this module
    // was ever dlopen'd and are therefore torn down after it.
    inline static std::atomic<bool> s_alive{false};
    static bool alive() { return s_alive.load(std::memory_order_acquire); }

    ~ThreadPool()
    {
        s_alive.store(false, std::memory_order_release);
        ETCS_LOG("ThreadPool", "dtor called, trigger_shutdown_drain...");
        trigger_shutdown_drain();
    }
    static ThreadPool& getInstance()
    {
        static ThreadPool instance;
        return instance;
    }

    bool submit(IOSubmission&& sub)
    {
#ifdef __linux__
        std::lock_guard<std::mutex> lock(submit_mutex_);

        IOSubmission* heap_sub = sub_pool_.acquire();
        *heap_sub = std::move(sub);

        struct io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
        if (!sqe)
        {
            ETCS_LOG("ThreadPool:IO", "SQE ring full, submission REFUSED -- caller must retry");
            sub_pool_.release(heap_sub);
            return false;
        }

        switch (heap_sub->op)
        {
            case IOOp::Accept:
                io_uring_prep_accept(sqe, heap_sub->fd, nullptr, nullptr, 0);
                break;

            case IOOp::Recv:
                io_uring_prep_recv(sqe, heap_sub->fd, heap_sub->buffer, heap_sub->buffer_len, 0);
                break;

            case IOOp::Send:
                io_uring_prep_send(sqe, heap_sub->fd, heap_sub->buffer, heap_sub->buffer_len, 0);
                break;

            case IOOp::Timeout:
                heap_sub->ts =
                {
                    .tv_sec  = static_cast<long long>(heap_sub->timeout_ns / 1'000'000'000),
                    .tv_nsec = static_cast<long long>(heap_sub->timeout_ns % 1'000'000'000)
                };
                io_uring_prep_timeout(sqe, &heap_sub->ts, 0, 0);
                break;

            case IOOp::Close:
                io_uring_prep_close(sqe, heap_sub->fd);
                break;

            case IOOp::Cancel:
                // Cancel EVERY pending io_uring operation on this fd, not just
                // the first match -- plain flags=0 only retires one, which is
                // fine for SocketConnectionState (at most one op in flight per
                // connection) but silently left the rest of a multi-deep accept
                // window (ConnectionManager::CloseConcrete, kAcceptWindow > 1)
                // permanently un-retired: inflight_ decremented by exactly one
                // and then never moved again, hanging the drain wait forever
                // with both io_completion_loop threads parked on cqes that
                // were never coming. IORING_ASYNC_CANCEL_ALL is what the
                // comment above always claimed this did.
                io_uring_prep_cancel_fd(sqe, heap_sub->fd, IORING_ASYNC_CANCEL_ALL);
                break;
        }

        io_uring_sqe_set_data(sqe, heap_sub);
        io_uring_submit(&ring_);

#elif defined(_WIN32)
        std::lock_guard<std::mutex> lock(submit_mutex_);
        IOSubmission* heap_sub = sub_pool_.acquire();
        *heap_sub = std::move(sub);
        heap_sub->overlapped = {};

        switch (heap_sub->op)
        {
            case IOOp::Recv:
            {
                WSABUF buf { static_cast<ULONG>(heap_sub->buffer_len), static_cast<char*>(heap_sub->buffer) };
                DWORD flags = 0, received = 0;
                WSARecv(static_cast<SOCKET>(heap_sub->fd), &buf, 1,
                    &received, &flags,
                    reinterpret_cast<OVERLAPPED*>(heap_sub), nullptr);
                break;
            }
            case IOOp::Send:
            {
                WSABUF buf { static_cast<ULONG>(heap_sub->buffer_len), static_cast<char*>(heap_sub->buffer) };
                DWORD sent = 0;
                WSASend(static_cast<SOCKET>(heap_sub->fd), &buf, 1,
                    &sent, 0,
                    reinterpret_cast<OVERLAPPED*>(heap_sub), nullptr);
                break;
            }
            case IOOp::Accept:
            case IOOp::Close:
            case IOOp::Timeout:
            case IOOp::Cancel:
                break;
        }
#endif
        return true;
    }

#ifdef _WIN32
    void associateHandle(HANDLE handle, ULONG_PTR completion_key)
    {
        CreateIoCompletionPort(handle, iocp_, completion_key, 0);
    }
#endif

    template<class F, class... Args>
    auto enqueue(ETCS::Priority priority, ETCS::SignalContext ctx, F&& f, Args&&... args)
        -> std::future<std::invoke_result_t<F, Args...>>
    {
        return enqueue(static_cast<int>(priority), std::move(ctx),
                       std::forward<F>(f), std::forward<Args>(args)...);
    }

    template<class F, class... Args>
    auto enqueue(int priority, ETCS::SignalContext ctx, F&& f, Args&&... args)
        -> std::future<std::invoke_result_t<F, Args...>>
    {
        using return_type = std::invoke_result_t<F, Args...>;

        auto packaged = std::make_shared<std::packaged_task<return_type()>>(
            [
                f_func = std::forward<F>(f),
                f_args = std::make_tuple(std::forward<Args>(args)...)
            ]() mutable -> return_type
            {
                return std::apply(std::move(f_func), std::move(f_args));
            }
        );

        std::future<return_type> res = packaged->get_future();
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            if (stop_.load(std::memory_order_relaxed))
                throw std::runtime_error("ThreadPool stopped");
            task_queue_.emplace(priority, std::move(ctx), [packaged]() { (*packaged)(); });
        }
        condition_.notify_one();
        return res;
    }

    void trigger_shutdown_drain()
    {
        bool expected = false;
        if (!is_drained_.compare_exchange_strong(expected, true))
        {
            ETCS_LOG("ThreadPool", "static drain triggered - but already drained, skipping.");
            return;
        }

        ETCS_LOG("ThreadPool", "static drain triggered...");
        io_stop_ = true;
        if (io_thread_.joinable()) io_thread_.join();
        cleanup_io();

        watchdog_stop_ = true;
        if (watchdog_thread_.joinable()) watchdog_thread_.join();

        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            stop_ = true;
        }
        condition_.notify_all();

        ETCS_LOG("ThreadPool", "cushion wait for threads...");
        std::this_thread::sleep_for(std::chrono::milliseconds(101)); // intentional delay so this doesn't go too fast... 
        // this sleep also allows any unload event in transit to resolve in worst case within 100ms
        ETCS_LOG("ThreadPool", "done cushion waiting for threads...");

        for (std::thread& worker : workers_)
            if (worker.joinable()) worker.join();
    }
    
    bool isDrained() const { return is_drained_.load(std::memory_order_acquire); }

    bool getLastError(std::string& out_tag)
    {
        std::lock_guard<std::mutex> lock(error_mutex_);
        if (!has_error) return false;
        out_tag = last_error_tag;
        has_error = false;
        return true;
    }
};

} // namespace ETCS

#endif
