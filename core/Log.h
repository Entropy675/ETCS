#ifndef LOG_H__
#define LOG_H__
#include <atomic>
#include <ostream>
#include <sstream>
#include <iostream>

/*
 * WHERE A LINE GOES IS A RUNTIME PROPERTY, not a compile-time one.
 *
 * ETCS_LOG_TO_FILE decided it at build time, which is the wrong axis: the same
 * binary wants the terminal while you are reading it and the file while you
 * are using the shell, and those alternate minute to minute. It now sets only
 * the DEFAULT.
 *
 * ONE FLAG PER MODULE, by construction rather than by design: this header is
 * compiled into the loader and into every module, and an inline variable under
 * -fvisibility=hidden gives each DSO its own copy. That is the right grain --
 * getModuleLog() already writes to logs/<ModuleName>.log, so a module's
 * destination is a module's own property. `log file` in the shell sets every
 * one it can reach (CommandExecutor.h).
 *
 * The default follows the shell: built with ETCS_REPL_SHELL, a terminal that
 * is also a prompt is a terminal you cannot read, so lines go to the files and
 * the prompt stays legible. Without it, stdout is the whole interface and the
 * lines belong there. ETCS_LOG_TO_FILE still forces the file default, which is
 * what a production build wants.
 */
#if defined(ETCS_LOG_TO_FILE) || defined(ETCS_REPL_SHELL)
    #define ETCS_LOG_TO_FILE_DEFAULT true
#else
    #define ETCS_LOG_TO_FILE_DEFAULT false
#endif

namespace ETCS {
    inline std::atomic<bool> log_enabled{true};
    inline std::atomic<bool> log_to_file{ ETCS_LOG_TO_FILE_DEFAULT };

    // Set THIS DSO's destination. The loader reaches a module's copy through
    // that module's own EventNode (EventNode::SetLogToFile), which is compiled
    // into the module and so writes the module's variable, not the loader's.
    inline void set_log_to_file(bool on) { log_to_file.store(on, std::memory_order_relaxed); }
    inline bool get_log_to_file()        { return log_to_file.load(std::memory_order_relaxed); }

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

// ---------------------------------------------------------------------------
// ONE LINE IS ONE INSERTION, which is why the message is built in a local
// stream before anything is emitted.
//
// It used to be inserted piecewise -- prefix, then module path, then type,
// then the caller's `msg` (itself usually several more `<<`), then the
// newline. Every one of those is a separate call into a shared streambuf, and
// threads interleave BETWEEN them, so two threads logging at once produced
// lines spliced through each other:
//
//   [/path/ETCS[/path/ETCS/bin/WindowProvider.so::ConsumeEvents] MOTION: ...
//
// which reads as corrupted data and is nothing of the kind -- the data was
// fine and the transcript of it was not. That distinction matters more than
// the tidiness: a log that can garble itself cannot be used to diagnose
// anything else, so every bug looked at through it inherits this one.
//
// Building the line first collapses it to a SINGLE insertion of a single
// string. That is not a lock and does not pretend to be one; it makes the
// window in which a splice can happen one call wide instead of a dozen, which
// for a line-oriented log is the difference between never and constantly.
// ---------------------------------------------------------------------------
#define ETCS_LOG_LINE(type, msg, out) \
    do { \
        std::ostringstream etcs_log_ss_; \
        etcs_log_ss_ << "[" << getCurrentModulePath() << "::" << type << "] " << msg << "\n"; \
        (out) << etcs_log_ss_.str(); \
    } while (0)

// THE FILE PATH FLUSHES PER LINE, and that is what actually makes it atomic
// rather than merely likely. getModuleLog() is a THREAD-LOCAL ofstream, so
// every thread has its own buffer over the same appended file, and an
// unflushed buffer is emitted when it happens to fill -- in the middle of
// whatever line was being written. Flushing here turns one line into one
// write() against an O_APPEND descriptor, which the kernel does not split.
// The cost is a syscall per line, paid on a path that was already doing file
// I/O, to buy a transcript that can be trusted.
// One branch now, on a relaxed atomic load. A thread-local sink still wins
// over both -- it is a capture, and a capture that silently went to a file
// would not be one.
#define ETCS_LOG_2(type, msg) \
    do { \
        if (ETCS::log_sink) ETCS_LOG_LINE(type, msg, *ETCS::log_sink); \
        else if (ETCS::log_to_file.load(std::memory_order_relaxed)) \
                          { ETCS_LOG_LINE(type, msg, getModuleLog()); getModuleLog().flush(); } \
        else              { ETCS_LOG_LINE(type, msg, std::cout); std::cout.flush(); } \
    } while (0)

#define ETCS_LOG_1(msg) ETCS_LOG_2(this->myTag(), msg)

#define ETCS_LOG(...) GET_LOG_MACRO(__VA_ARGS__, ETCS_LOG_2, ETCS_LOG_1)(__VA_ARGS__)

#endif
