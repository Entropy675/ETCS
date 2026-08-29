#ifndef LOG_H__
#define LOG_H__
#include <atomic>
#include <ostream>

namespace ETCS {
    inline std::atomic<bool> log_enabled{true};

    // Thread-local ETCS_LOG redirect. Set via LogSinkGuard only -- direct
    // assignment skips the restore that nested guards rely on.
    //
    // Captures output produced SYNCHRONOUSLY on the setting thread only. Work
    // that hops to a ThreadPool worker (a stream produce trampoline) logs to
    // whatever's ambient there, usually null. Threading a sink through every
    // async capture is a much larger change.
    inline thread_local std::ostream* log_sink = nullptr;

    struct LogSinkGuard
    {
        std::ostream* previous;
        explicit LogSinkGuard(std::ostream* sink) : previous(log_sink) { log_sink = sink; }
        ~LogSinkGuard() { log_sink = previous; }
        LogSinkGuard(const LogSinkGuard&)            = delete;
        LogSinkGuard& operator=(const LogSinkGuard&) = delete;
    };
}

// Lives here because TBuffer is the only thing guaranteed included everywhere,
// arena and threadpool included.

#define GET_LOG_MACRO(_1, _2, NAME, ...) NAME

// NO trailing semicolon on ETCS_LOG_1 or ETCS_LOG below. With one, an
// invocation expands to `do{...}while(0);;` -- the extra empty statement ends
// the enclosing `if` body, so a following `else` has nothing to attach to and
// the build fails. That is why call sites had to brace every ETCS_LOG used as
// a branch body; with the semicolons gone, they don't.

#ifdef ETCS_LOG_TO_FILE
#define ETCS_LOG_2(type, msg) \
    do { \
        if (ETCS::log_sink) \
            *ETCS::log_sink << "[" << getCurrentModulePath() << "::" << type << "] " << msg << "\n"; \
        else \
            getModuleLog() << "[" << getCurrentModulePath() << "::" << type << "] " << msg << "\n"; \
    } while (0)
#else
#define ETCS_LOG_2(type, msg) \
    do { \
        if (ETCS::log_sink) \
            *ETCS::log_sink << "[" << getCurrentModulePath() << "::" << type << "] " << msg << "\n"; \
        else \
            std::cout << "[" << getCurrentModulePath() << "::" << type << "] " << msg << std::endl; \
    } while (0)
#endif

#define ETCS_LOG_1(msg) ETCS_LOG_2(this->myTag(), msg)

#define ETCS_LOG(...) GET_LOG_MACRO(__VA_ARGS__, ETCS_LOG_2, ETCS_LOG_1)(__VA_ARGS__)

#endif
