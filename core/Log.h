#ifndef LOG_H__
#define LOG_H__
#include <atomic>
#include <ostream>
#include <sstream>
#include <iostream>
#include <chrono>
#include <cstdio>
#include <ctime>

/*
 * WHERE A LINE GOES IS A RUNTIME PROPERTY, not a compile-time one.
 *
 * ETCS_LOG_TO_FILE decided it at build time, which is the wrong axis: the same
 * binary wants the terminal while you are reading it and the file while you
 * are using the shell, and those alternate minute to minute. It now sets only
 * the DEFAULT.
 *
 * ONE FLAG PER BINARY, and now BY DESIGN rather than by construction. This
 * header is compiled into the loader and into every module, and each of them
 * having its own copy is the whole grain the feature runs on -- getModuleLog()
 * already writes to logs/<ModuleName>.log, so where a module's lines go is a
 * module's own property, separately settable from the loader's.
 *
 * It used to rest on -fvisibility=hidden being passed, which is a BUILD FLAG
 * and therefore not part of this header's meaning: an inline variable has vague
 * linkage, so with default visibility the dynamic linker is entitled to collapse
 * every DSO's copy onto whichever it resolves first, and one `log file` would
 * silently move every module's lines at once again. ETCS_PER_BINARY below says
 * it in the declaration instead, where it cannot be dropped by a build line.
 *
 * NOT `static`, which was the other way to spell it and is a DIFFERENT grain:
 * internal linkage gives one copy per TRANSLATION UNIT, so a loader or module
 * that ever grows a second .cc including this header would carry two flags and
 * the shell would set one of them. Hidden visibility is per-binary exactly,
 * whatever a binary is made of.
 *
 * `log file` in the shell sets the one you are standing in; `log all file`
 * still sets every one it can reach (CommandExecutor.h).
 *
 * The default follows the shell: built WITH ETCS_REPL_SHELL there is a person
 * at that terminal reading it, so the lines go there and `log file` moves them
 * away when they get in the way. Built WITHOUT it nobody is watching stdout,
 * so the files are where the lines are of any use. ETCS_LOG_TO_FILE forces the
 * file default either way, which is what a production build wants.
 *
 * ONLY THE LOADER CAN ANSWER THIS, which is why the shell half of the test is
 * gated on ETCS_LOADER. A module is never compiled with ETCS_REPL_SHELL --
 * that flag describes the executable, not the library -- so a module reading
 * it would conclude "nobody is watching" in the one case where somebody is.
 * Modules therefore start VISIBLE and are told the truth the moment they
 * register (Module::registerLoader). The few lines a module emits before that
 * -- its ThreadPool coming up, its manifest comparison -- follow this default,
 * and defaulting them to the terminal is deliberate: a module cannot know
 * whether anyone is there, and lines nobody asked to hide are worth more on
 * screen than lines nobody can find are worth in a file.
 */
#if defined(ETCS_LOG_TO_FILE) || (defined(ETCS_LOADER) && !defined(ETCS_REPL_SHELL))
    #define ETCS_LOG_TO_FILE_DEFAULT true
#else
    #define ETCS_LOG_TO_FILE_DEFAULT false
#endif

// One copy per DSO, guaranteed here rather than by a build flag -- see above.
// A Windows DLL never shares a data symbol with its host to begin with, so
// there is nothing to say there.
#if defined(_WIN32)
    #define ETCS_PER_BINARY
#else
    #define ETCS_PER_BINARY __attribute__((visibility("hidden")))
#endif

namespace ETCS {
    ETCS_PER_BINARY inline ::std::atomic<bool> log_enabled{true};
    ETCS_PER_BINARY inline ::std::atomic<bool> log_to_file{ ETCS_LOG_TO_FILE_DEFAULT };

    /*
 * THE TIME A LINE LEFT, not the time anything decided to write one.
 *
 * Built here, at the point of emission, and nowhere earlier: a stamp taken
 * when a message was composed and carried to the sink would date the intent
 * rather than the record, and the gap between those is exactly the interval a
 * log is usually being read to measure.
 *
 * A 16-byte struct by value, so a line costs no allocation. The seconds half
 * is cached per thread and recomputed only when the second turns -- localtime_r
 * is the expensive part and a busy log emits hundreds of lines inside one of
 * them.
 *
 * Local wall clock, to the millisecond. Local rather than UTC because the
 * thing being correlated with is usually something else on this machine, and
 * milliseconds because frames are the shortest interval anyone has yet needed
 * to see between two lines.
 */
    struct LogStamp { char text[16]; };

    inline LogStamp log_stamp()
    {
        const auto now  = ::std::chrono::system_clock::now();
        const auto secs = ::std::chrono::time_point_cast<::std::chrono::seconds>(now);
        const long ms   = static_cast<long>(
            ::std::chrono::duration_cast<::std::chrono::milliseconds>(now - secs).count());

        static thread_local ::std::time_t cached = 0;
        static thread_local char hms[9] = "00:00:00";

        const ::std::time_t t = ::std::chrono::system_clock::to_time_t(secs);
        if (t != cached)
        {
            cached = t;
            ::std::tm tmv{};
#if defined(_WIN32)
            localtime_s(&tmv, &t);
#else
            localtime_r(&t, &tmv);
#endif
            ::std::snprintf(hms, sizeof(hms), "%02d:%02d:%02d",
                          tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
        }

        LogStamp s{};
        ::std::snprintf(s.text, sizeof(s.text), "%s.%03ld", hms, ms);
        return s;
    }

    // Set THIS DSO's destination. The loader reaches a module's copy through
    // that module's own EventNode::set_log_to_file trampoline (EventNode.h),
    // which is compiled into the module and so writes the module's variable,
    // not the loader's.
    inline void set_log_to_file(bool on) { log_to_file.store(on, ::std::memory_order_relaxed); }
    inline bool get_log_to_file()        { return log_to_file.load(::std::memory_order_relaxed); }

    // Thread-local ETCS_LOG redirect. Set via LogSinkGuard only -- direct
    // assignment skips the restore that nested guards rely on.
    //
    // Captures output produced SYNCHRONOUSLY on the setting thread only. Work
    // that hops to a ThreadPool worker (a stream produce trampoline) logs to
    // whatever's ambient there, usually null. Threading a sink through every
    // async capture is a much larger change.
    inline thread_local ::std::ostream* log_sink = nullptr;

    struct LogSinkGuard
    {
        ::std::ostream* previous;
        explicit LogSinkGuard(::std::ostream* sink) : previous(log_sink) { log_sink = sink; }
        ~LogSinkGuard() { log_sink = previous; }
        LogSinkGuard(const LogSinkGuard&)            = delete;
        LogSinkGuard& operator=(const LogSinkGuard&) = delete;
    };
}

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
//
// `stamp` is a literal at every call site, so the branch folds away and an
// unstamped line pays nothing. WHO PASSES TRUE is the whole of the policy: the
// log FILE does, and nothing else. A terminal line is read in the moment it is
// produced, with the frame it belongs to still on screen, so the clock beside
// it is noise on the one output where noise was already the problem -- `log
// file` exists because that terminal fills up. A file is read afterwards, with
// none of that context left, and there the time IS the context. A captured
// sink (LogSinkGuard) is a transcript handed back to a caller -- a session's
// own output, a command's captured result -- and dating those would put wall
// clock into strings that get compared and displayed.
#define ETCS_LOG_LINE(type, msg, out, stamp) \
    do { \
        ::std::ostringstream etcs_log_ss_; \
        if (stamp) etcs_log_ss_ << "[" << ETCS::log_stamp().text << "] "; \
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
        if (ETCS::log_sink) ETCS_LOG_LINE(type, msg, *ETCS::log_sink, false); \
        else if (ETCS::log_to_file.load(::std::memory_order_relaxed)) \
                          { ETCS_LOG_LINE(type, msg, getModuleLog(), true); getModuleLog().flush(); } \
        else              { ETCS_LOG_LINE(type, msg, ::std::cout, false); ::std::cout.flush(); } \
    } while (0)

#define ETCS_LOG_1(msg) ETCS_LOG_2(this->myTag(), msg)

#define ETCS_LOG(...) GET_LOG_MACRO(__VA_ARGS__, ETCS_LOG_2, ETCS_LOG_1)(__VA_ARGS__)

#endif
