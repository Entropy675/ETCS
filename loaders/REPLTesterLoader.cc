// ===========================================================================
// REPLTesterLoader.cc
//
// Black-box process test for the interactive `etcs` REPL binary's own
// shutdown behavior under SIGINT -- specifically the class of bug this
// whole session's Root/Module/SignalContext arc was about: a signal
// arriving while nav_root (a per-navigation Root, repl_shell_loop's own
// local) is live and holding a module's lifetime token must lead to a
// clean process exit, never a SIGSEGV/SIGABRT racing dlclose() against
// still-running worker threads, and never a hang.
//
// Deliberately NOT built against ETCS.h at all, unlike every other
// tester in this suite -- this isn't testing an internal API, it's
// testing the actual compiled `etcs` binary's own observable behavior
// from outside the process, the same way a person testing this by hand
// in a terminal would: spawn it, feed it keystrokes over its own stdin,
// watch its own stdout for the prompts it's expected to print, deliver a
// real SIGINT via kill(), and check how (and whether) it actually exits.
// Plain POSIX + STL only -- fork/exec/pipe/waitpid/kill -- so the build
// is a single, ordinary g++ invocation with no ETCS-specific flags:
//
//   g++ -std=c++17 -Wall -Wextra -O2 -pthread -o Run_REPLTesterLoader REPLTesterLoader.cc
//
// Run from the same directory `etcs` itself lives in (matching every
// other Run_*TesterLoader in this bin/), so the child's inherited CWD
// lets it find NetworkProvider.so exactly as an interactive session
// would. Exits non-zero if any scenario fails, so this can be wired into
// a CI-style check later without extra plumbing.
//
// A non-TTY stdin is deliberately NOT a PTY here -- repl_shell_get_char_unix
// (ShellREPL.h) already has a plain, non-raw-mode fallback specifically
// for !isatty(STDIN_FILENO): an ordinary blocking single-byte read(). A
// ready-made plain pipe drives that path correctly with no termios setup
// needed on this side at all.
// ===========================================================================

#include <unistd.h>
#include <sys/wait.h>
#include <signal.h>
#include <cstring>
#include <string>
#include <thread>
#include <mutex>
#include <chrono>
#include <sstream>
#include <iostream>
#include <vector>
#include <atomic>

using namespace std::chrono_literals;

static const char* signal_name(int sig);

// ---------------------------------------------------------------------------
// ChildProcess -- forks + execs a target binary with its own stdin/stdout
// redirected to pipes this process controls, and runs a background thread
// that continuously drains the child's stdout into a shared, mutex-
// protected buffer. Every prompt this REPL prints ends with an explicit
// std::flush (repl_shell_get_input, ShellREPL.h), so what actually lands
// in that buffer is never stuck behind libstdc++'s own full-buffering-
// when-not-a-tty behavior -- no special flushing/line-buffering coercion
// needed on this side.
// ---------------------------------------------------------------------------
struct ChildProcess
{
    pid_t pid = -1;
    int   stdin_write_fd  = -1;
    int   stdout_read_fd  = -1;

    std::thread        reader_thread;
    std::mutex         output_mutex;
    std::string        output;
    std::atomic<bool>  reader_stop{false};

    ChildProcess() = default;
    ChildProcess(const ChildProcess&)            = delete;
    ChildProcess& operator=(const ChildProcess&) = delete;

    bool spawn(const std::vector<std::string>& argv)
    {
        int in_pipe[2];  // parent writes in_pipe[1]  -> child reads in_pipe[0]  (child's stdin)
        int out_pipe[2]; // child writes out_pipe[1]   -> parent reads out_pipe[0] (child's stdout)

        if (pipe(in_pipe) != 0 || pipe(out_pipe) != 0)
        {
            std::cerr << "ChildProcess::spawn: pipe() failed: " << std::strerror(errno) << "\n";
            return false;
        }

        pid = fork();
        if (pid < 0)
        {
            std::cerr << "ChildProcess::spawn: fork() failed: " << std::strerror(errno) << "\n";
            return false;
        }

        if (pid == 0)
        {
            // Child: wire pipes to stdin/stdout, close everything else,
            // exec the real target. Stderr is left alone (inherited) so
            // any crash output from the child still shows up directly in
            // this test's own terminal for a human to read afterward.
            dup2(in_pipe[0],  STDIN_FILENO);
            dup2(out_pipe[1], STDOUT_FILENO);
            close(in_pipe[0]);  close(in_pipe[1]);
            close(out_pipe[0]); close(out_pipe[1]);

            std::vector<char*> cargv;
            for (auto& s : argv) cargv.push_back(const_cast<char*>(s.c_str()));
            cargv.push_back(nullptr);

            execvp(cargv[0], cargv.data());
            // execvp only returns on failure.
            std::cerr << "ChildProcess::spawn: execvp('" << argv[0]
                       << "') failed: " << std::strerror(errno) << "\n";
            _exit(127);
        }

        // Parent
        close(in_pipe[0]);
        close(out_pipe[1]);
        stdin_write_fd = in_pipe[1];
        stdout_read_fd = out_pipe[0];

        reader_thread = std::thread([this]()
        {
            char buf[4096];
            while (!reader_stop.load(std::memory_order_relaxed))
            {
                ssize_t n = ::read(stdout_read_fd, buf, sizeof(buf));
                if (n <= 0) break; // EOF (child exited/closed) or real error
                std::lock_guard<std::mutex> lock(output_mutex);
                output.append(buf, static_cast<size_t>(n));
            }
        });

        return true;
    }

    // Writes a line (newline appended) to the child's own stdin, exactly
    // as if a person had typed it and pressed Enter -- repl_shell_get_input
    // (ShellREPL.h) reads one byte at a time regardless of whether the
    // source is a real keyboard or a pipe, so a whole line written in one
    // write() call is consumed identically either way.
    void write_line(const std::string& line)
    {
        std::string with_nl = line + "\n";
        ssize_t off = 0;
        while (off < static_cast<ssize_t>(with_nl.size()))
        {
            ssize_t n = ::write(stdin_write_fd, with_nl.data() + off,
                                 with_nl.size() - static_cast<size_t>(off));
            if (n <= 0) return; // child already gone -- nothing more to do
            off += n;
        }
    }

    // Polls the shared output buffer for `needle`, up to `timeout`. Returns
    // true the moment it appears; false if the deadline passes first --
    // the caller decides whether that's itself a test failure (a prompt
    // that should have appeared but didn't is just as real a regression
    // as a crash is).
    bool wait_for_output(const std::string& needle, std::chrono::milliseconds timeout)
    {
        auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline)
        {
            {
                std::lock_guard<std::mutex> lock(output_mutex);
                if (output.find(needle) != std::string::npos) return true;
            }
            std::this_thread::sleep_for(10ms);
        }
        return false;
    }

    void send_signal(int sig)
    {
        if (pid > 0) kill(pid, sig);
    }

    // Polls waitpid(WNOHANG) up to `timeout` rather than blocking
    // indefinitely -- a future regression that turns this bug into a
    // deadlock instead of a crash is a DIFFERENT failure mode than a
    // SIGSEGV, and just as real; a plain blocking waitpid() would hang
    // this test itself forever instead of reporting that failure.
    // Force-kills (SIGKILL) and reaps if the deadline passes, so a timed-
    // out test never leaves an orphaned process behind.
    enum class ExitOutcome { Exited, Signaled, TimedOut };
    ExitOutcome wait_for_exit(std::chrono::milliseconds timeout, int& out_code_or_signal)
    {
        auto deadline = std::chrono::steady_clock::now() + timeout;
        int status = 0;
        while (std::chrono::steady_clock::now() < deadline)
        {
            pid_t r = waitpid(pid, &status, WNOHANG);
            if (r == pid)
            {
                if (WIFEXITED(status))   { out_code_or_signal = WEXITSTATUS(status); return ExitOutcome::Exited; }
                if (WIFSIGNALED(status)) { out_code_or_signal = WTERMSIG(status);     return ExitOutcome::Signaled; }
            }
            std::this_thread::sleep_for(10ms);
        }
        // Timed out -- force-kill and reap so nothing is left orphaned.
        kill(pid, SIGKILL);
        waitpid(pid, &status, 0);
        return ExitOutcome::TimedOut;
    }

    // Called whenever a wait_for_output() call comes back false -- the
    // single most important diagnostic this harness can offer, and the
    // thing its first version was missing entirely. "Never saw the
    // prompt" is ambiguous between two completely different bugs: the
    // child crashed or exited before ever printing it (a real bug in the
    // target), versus the child is alive and well but this HARNESS
    // failed to actually observe its output (a bug in the harness's own
    // process/pipe plumbing). This distinguishes them directly: a
    // non-blocking waitpid() reveals whether the child is even still
    // alive, and if it already exited, exactly how; either way, whatever
    // partial output WAS captured is printed verbatim, so "nothing at
    // all came through" and "some output arrived but not the expected
    // string" are visibly different outcomes too.
    std::string diagnose_failure()
    {
        std::ostringstream oss;
        int status = 0;
        pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid)
        {
            if (WIFEXITED(status))
                oss << "  -> child already EXITED with code " << WEXITSTATUS(status) << ".\n";
            else if (WIFSIGNALED(status))
                oss << "  -> child was KILLED by signal " << WTERMSIG(status)
                    << " (" << signal_name(WTERMSIG(status)) << ").\n";
            pid = -1; // already reaped -- destructor must not touch it again
        }
        else if (r == 0)
        {
            oss << "  -> child is still RUNNING (pid " << pid << ") -- it simply "
                   "hasn't printed the expected text yet.\n";
        }
        else
        {
            oss << "  -> waitpid() itself failed: " << std::strerror(errno) << "\n";
        }

        {
            std::lock_guard<std::mutex> lock(output_mutex);
            oss << "  -> captured output so far (" << output.size() << " bytes):\n"
                << "-----------------------------------------------\n"
                << (output.empty() ? std::string("(nothing at all)\n") : output)
                << "\n-----------------------------------------------\n";
        }
        return oss.str();
    }

    ~ChildProcess()
    {
        reader_stop.store(true, std::memory_order_relaxed);
        if (stdin_write_fd  >= 0) close(stdin_write_fd);
        if (stdout_read_fd  >= 0) close(stdout_read_fd);
        if (reader_thread.joinable()) reader_thread.join();
        // Best-effort: if the child is somehow still alive (a prior step
        // already timed out and force-killed it, or a test returned
        // early), make sure nothing outlives this object.
        if (pid > 0)
        {
            int status;
            if (waitpid(pid, &status, WNOHANG) == 0) { kill(pid, SIGKILL); waitpid(pid, &status, 0); }
        }
    }
};

static const char* signal_name(int sig)
{
    switch (sig)
    {
        case SIGSEGV: return "SIGSEGV";
        case SIGABRT: return "SIGABRT";
        case SIGBUS:  return "SIGBUS";
        case SIGILL:  return "SIGILL";
        case SIGKILL: return "SIGKILL";
        default:      return "?";
    }
}

// Shared pass/fail judgment: after SIGINT is delivered, the ONLY
// acceptable outcome is a clean process exit (exit code 0, from
// drive_main_loop_then_exit's own normal return path) within a
// generous-but-bounded window. Anything else -- crash or hang -- fails
// the scenario with a specific, actionable message.
static bool judge_clean_shutdown(ChildProcess& child, const char* scenario_name)
{
    int code_or_sig = -1;
    auto outcome = child.wait_for_exit(5000ms, code_or_sig);

    switch (outcome)
    {
        case ChildProcess::ExitOutcome::Exited:
            if (code_or_sig == 0)
            {
                std::cout << "[PASS] " << scenario_name
                          << " -- clean exit (code 0) after SIGINT.\n";
                return true;
            }
            std::cout << "[FAIL] " << scenario_name
                      << " -- exited, but with non-zero code " << code_or_sig << ".\n";
            return false;

        case ChildProcess::ExitOutcome::Signaled:
            std::cout << "[FAIL] " << scenario_name
                      << " -- process was killed by signal "
                      << code_or_sig << " (" << signal_name(code_or_sig)
                      << ") instead of exiting cleanly. This is the exact "
                         "failure class this test exists to catch.\n";
            return false;

        case ChildProcess::ExitOutcome::TimedOut:
        default:
            std::cout << "[FAIL] " << scenario_name
                      << " -- process did not exit within the timeout "
                         "(hung instead of crashing or exiting -- force-"
                         "killed and reaped).\n";
            return false;
    }
}

// ---------------------------------------------------------------------------
// Scenario 1 -- baseline: SIGINT at the bare Root> prompt, no module ever
// touched. nav_root never exists in this scenario at all (it's only
// constructed once a module name is actually entered), so this doesn't
// exercise the Root/Module fix directly -- it's a cheap sanity check that
// ordinary Ctrl+C-to-quit still works at all after these changes, not a
// regression test for the specific bug.
// ---------------------------------------------------------------------------
static bool test_sigint_at_root_prompt(const std::string& etcs_path)
{
    const char* name = "Scenario 1: SIGINT at bare Root> prompt";
    std::cout << "\n--- " << name << " ---\n";

    ChildProcess child;
    if (!child.spawn({etcs_path})) { std::cout << "[FAIL] " << name << " -- spawn failed.\n"; return false; }

    if (!child.wait_for_output("Root> ", 3000ms))
    {
        std::cout << "[FAIL] " << name << " -- never saw the Root> prompt.\n"
                   << child.diagnose_failure();
        return false;
    }

    child.send_signal(SIGINT);
    return judge_clean_shutdown(child, name);
}

// ---------------------------------------------------------------------------
// Scenario 2 -- THE targeted case this test exists for: navigate into
// NetworkProvider (constructing nav_root, which claims the module's
// lifetime token since nothing else has touched it yet), land at its Tag>
// prompt with zero live instances, then SIGINT. This is the exact
// sequence from every crash trace this session's Root/Module arc chased:
// ~Root()'s own synchronous vacate, ~Module()'s independent "Root going
// out of scope" branch, and PendingUnloadRegistry's tracked recheck
// thread all have to cooperate correctly here.
// ---------------------------------------------------------------------------
static bool test_sigint_during_module_navigation(const std::string& etcs_path)
{
    const char* name = "Scenario 2: SIGINT at module Tag> prompt (the targeted case)";
    std::cout << "\n--- " << name << " ---\n";

    ChildProcess child;
    if (!child.spawn({etcs_path})) { std::cout << "[FAIL] " << name << " -- spawn failed.\n"; return false; }

    if (!child.wait_for_output("Root> ", 3000ms))
    {
        std::cout << "[FAIL] " << name << " -- never saw the Root> prompt.\n"
                   << child.diagnose_failure();
        return false;
    }

    child.write_line("NetworkProvider");

    if (!child.wait_for_output("NetworkProvider Tag> ", 3000ms))
    {
        std::cout << "[FAIL] " << name
                  << " -- never reached the NetworkProvider Tag> prompt "
                     "(is NetworkProvider.so present in this directory?).\n"
                  << child.diagnose_failure();
        return false;
    }

    child.send_signal(SIGINT);
    return judge_clean_shutdown(child, name);
}

// ---------------------------------------------------------------------------
// Scenario 3 -- deeper variant: also select a tag and spawn a real,
// live instance before SIGINT. This changes the shape of what's alive at
// signal time (a genuine Entity now exists under the module, not just
// the navigation-only nav_root), which is worth covering separately from
// scenario 2 rather than assumed to be equivalent.
// ---------------------------------------------------------------------------
static bool test_sigint_after_spawning_instance(const std::string& etcs_path)
{
    const char* name = "Scenario 3: SIGINT after spawning a live instance";
    std::cout << "\n--- " << name << " ---\n";

    ChildProcess child;
    if (!child.spawn({etcs_path})) { std::cout << "[FAIL] " << name << " -- spawn failed.\n"; return false; }

    if (!child.wait_for_output("Root> ", 3000ms))
    {
        std::cout << "[FAIL] " << name << " -- never saw the Root> prompt.\n"
                   << child.diagnose_failure();
        return false;
    }

    child.write_line("NetworkProvider");
    if (!child.wait_for_output("NetworkProvider Tag> ", 3000ms))
    {
        std::cout << "[FAIL] " << name << " -- never reached the NetworkProvider Tag> prompt.\n"
                   << child.diagnose_failure();
        return false;
    }

    child.write_line("HTTPParser");
    if (!child.wait_for_output("HTTPParser Inst> ", 3000ms))
    {
        std::cout << "[FAIL] " << name << " -- never reached the HTTPParser Inst> prompt.\n"
                   << child.diagnose_failure();
        return false;
    }

    child.write_line("spawn");
    if (!child.wait_for_output("HTTPParser Act> ", 3000ms))
    {
        std::cout << "[FAIL] " << name
                  << " -- spawn didn't land at an HTTPParser Act> prompt.\n"
                  << child.diagnose_failure();
        return false;
    }

    child.send_signal(SIGINT);
    return judge_clean_shutdown(child, name);
}

int main(int argc, char** argv)
{
    std::string etcs_path = (argc > 1) ? argv[1] : "./etcs";

    std::cout << "REPLTesterLoader -- driving " << etcs_path
              << " as a child process, exercising SIGINT-during-shutdown "
                 "scenarios.\n";

    int passed = 0, total = 0;
    auto run = [&](bool (*fn)(const std::string&))
    {
        ++total;
        if (fn(etcs_path)) ++passed;
    };

    run(test_sigint_at_root_prompt);
    run(test_sigint_during_module_navigation);
    run(test_sigint_after_spawning_instance);

    std::cout << "\n=== " << passed << "/" << total << " scenarios passed ===\n";
    return (passed == total) ? 0 : 1;
}
