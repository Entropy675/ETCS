#include "../ETCS.h"
#include "../core/LMAXSequentialSharedPage.h"
#include "../modules/NetworkProvider/NetworkProvider/SocketConnectionState.h"
#include <thread>
#include <vector>
#include <deque>
#include <atomic>
#include <mutex>
#include <memory>
#include <iostream>
#include <iomanip>
#include <sstream>
#include <string>
#include <chrono>
#include <random>
#include <algorithm>
#include <cstdlib>
#include <cerrno>
#include <dirent.h>
#include <fcntl.h>
#include <dlfcn.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/stat.h>

// ═════════════════════════════════════════════════════════════════════════════
// scsrealtest — the SAME discipline as scstest, driven against the REAL module.
//
// scstest models SocketConnectionState and runs in a third of a second. This
// one instantiates the actual class out of NetworkProvider, with real file
// descriptors, real mbedtls, real certificates and a real TLS handshake. It is
// slower and it has real dependencies. Keep both: the model is what you run on
// every save, this is what you run before you trust a change.
//
// WHAT "REAL" MEANS HERE, EXACTLY
//
// The module's headers are compiled INTO this loader rather than dlopened and
// driven through the tag surface. That is deliberate. The discipline under
// test -- TryClaim / NoteSubmit / NoteComplete / RecycleForNextRequest /
// ResetConcrete / finalizeIfDraining -- is a C++ protocol between the accept
// chain and the completion callbacks. It is not reachable from the work-
// function surface at all (the tag block exposes Reset/Close/IsOpen/SetPage/
// GetPage and nothing else), so dlopening the .so and issuing work would test
// dispatch, not this. The ABI section at the end does dlopen the built .so, so
// an exports.map regression still gets caught -- it just isn't the vehicle for
// the lifecycle tests.
//
// Consequences of compiling it in, stated so nobody is surprised:
//   - the arena and thread pool are THIS loader's, not the module's. Per-DSO
//     Meyers singletons mean the module's own copies are untouched.
//   - ResetConcrete's cancel therefore lands in this loader's io_uring, naming
//     an fd that io_uring never saw. That is harmless and is what production
//     does verbatim; it is not the thing being asserted.
//   - a SocketConnectionState here is constructed directly rather than via
//     addTag<> from a ConnectionManager, so its arena is the root's rather
//     than a manager's child. Nothing in the protocol reads that.
//
// WHAT THIS CATCHES THAT THE MODEL CANNOT
//
//   1. Real fds. finalizeIfDraining's close is a real close(2), so the suite
//      can assert the descriptor was closed EXACTLY once -- and, via a bank of
//      sentinel descriptors held open across the churn, that no path ever
//      closed a number it no longer owned. That specific failure is the one
//      CloseConnection's exchange(-1) exists to prevent, and it is invisible
//      to a model because a bool cannot be handed to a newer connection.
//   2. Real descriptor accounting. /proc/self/fd is counted before and after,
//      so a connection that drains without closing shows up as a leak.
//   3. Real TLS. A real handshake is driven through the real staging buffers
//      against a real mbedtls client -- which is the code path that actually
//      broke this cycle (ciphertext staged in CipherOut but never flushed,
//      because the flush was keyed on WANT_WRITE, which bioSend never
//      returns). A model cannot fail that way; this can, and does if the
//      keying regresses, by exhausting its step budget.
//   4. Real keep-alive. The recycle-vs-finalize split is asserted on an
//      ESTABLISHED session by sending a SECOND request over it after
//      RecycleForNextRequest and requiring it to decrypt. "The session
//      survived" stops being a flag and becomes a decryption.
//   5. Real cert rotation. The refcount that makes ReloadCerts zero-downtime
//      is checked with a weak_ptr: the superseded config must stay alive for
//      exactly as long as the session holding it, and must die on its Free().
//
// BUILD
//   This target needs the module's own include paths and its two static
//   dependencies. `make` in modules/NetworkProvider first -- that is what
//   builds picohttpparser.o and the vendored mbedtls.
//
//   THE VENDORED HEADERS MUST WIN. If /usr/include/mbedtls exists it is very
//   likely 3.x, and the module is written against 4.x: the first symptom is
//   "too few arguments to mbedtls_pk_parse_keyfile" out of TLSServerConfig.h,
//   which is the 4.x three-argument form being checked against the 3.x
//   five-argument declaration. The module's Makefile gets this right by
//   putting -I$(MBEDTLS_DIR)/include ahead of the system path; this target
//   has to do the same.
//
//     NP := ../modules/NetworkProvider
//     MB := $(NP)/mbedtls
//     SCSREAL_INC := -I$(NP)/picohttpparser
//                    -I$(MB)/include
//                    -I$(MB)/tf-psa-crypto/include
//                    -I$(MB)/tf-psa-crypto/drivers/builtin/include
//                    -I$(MB)/tf-psa-crypto/drivers/everest/include
//                    -I$(MB)/tf-psa-crypto/drivers/pqcp/include
//                    -I$(MB)/tf-psa-crypto/drivers/p256-m
//     SCSREAL_LIB := $(NP)/picohttpparser.o
//                    $(MB)/build/library/libmbedtls.a
//                    $(MB)/build/library/libmbedx509.a
//                    $(MB)/build/library/libmbedcrypto.a
//
//   (Written one flag per line for readability -- join them onto one line, or
//   use trailing backslashes, in the actual Makefile.)
//
//   LINK ORDER MATTERS. The archives must come AFTER this translation unit on
//   the command line, since gold does not rescan an archive it has already
//   passed. If the only injection point available puts them before the source
//   -- which is where EXTRADEFINES lands -- wrap them in
//   -Wl,--whole-archive ... -Wl,--no-whole-archive so demand order stops
//   mattering. That works and costs a fatter binary; a proper target-specific
//   LDLIBS is the real fix.
//
// CERTIFICATE
//   Order: $ETCS_TEST_CERT / $ETCS_TEST_KEY, then argv[2] / argv[3], then a
//   throwaway self-signed pair generated into /tmp with openssl. If none of
//   those work the TLS sections SKIP rather than fail -- a missing openssl is
//   not a defect in the module, and reporting it as one trains people to
//   ignore red.
//
// USAGE
//   ./Run_SCSRealTesterLoader [iters] [cert.pem] [key.pem]
//   ./Run_SCSRealTesterLoader --no-dlopen     (skip the ABI section)
// ═════════════════════════════════════════════════════════════════════════════

// ── helpers ───────────────────────────────────────────────────────────────────

static int s_passed  = 0;
static int s_failed  = 0;
static int s_skipped = 0;

#define TEST(name, expr) \
    do { \
        if (expr) { \
            std::cout << "  [PASS] " name "\n"; \
            ++s_passed; \
        } else { \
            std::cout << "  [FAIL] " name "\n"; \
            ++s_failed; \
        } \
    } while(0)

static void section(const char* name)
{
    std::cout << "\n=== " << name << " ===\n";
}

static void skip(const char* name, const std::string& why)
{
    std::cout << "  [SKIP] " << name << " -- " << why << "\n";
    ++s_skipped;
}

static int get_core_count()
{
    int n = static_cast<int>(std::thread::hardware_concurrency());
    return n > 0 ? n : 1;
}

static void pin_thread(int core_id)
{
    if (core_id < 0) return;
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core_id, &cpuset);
    pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
}

static std::string formatWithCommas(long long n)
{
    std::string s = std::to_string(n);
    int pos   = static_cast<int>(s.length()) - 3;
    int limit = (n < 0) ? 1 : 0;
    while (pos > limit) { s.insert(pos, ","); pos -= 3; }
    return s;
}

static std::string formatDuration(double ms)
{
    std::ostringstream o;
    o << std::fixed << std::setprecision(1);
    if (ms < 1000.0) { o << ms << "ms"; return o.str(); }
    o << (ms / 1000.0) << "s";
    return o.str();
}

static const std::string RESET      = "\033[0m";
static const std::string FG_GREEN   = "\033[1;32m";
static const std::string FG_CYAN    = "\033[1;36m";
static const std::string FG_RED     = "\033[31m";
static const std::string FG_YELLOW  = "\033[1;33m";

// ── violation recording ───────────────────────────────────────────────────────
// Counted rather than asserted, same as scstest: an abort on the first hit
// tells you nothing about whether it is constant or one in a million.

struct Violations
{
    std::atomic<long long> count{0};
    std::mutex             first_mutex;
    std::string            first;

    void record(const std::string& what)
    {
        if (count.fetch_add(1, std::memory_order_relaxed) == 0)
        {
            std::lock_guard<std::mutex> lk(first_mutex);
            if (first.empty()) first = what;
        }
    }
    void reset() { count.store(0); std::lock_guard<std::mutex> lk(first_mutex); first.clear(); }
};

static Violations g_violations;

// ── descriptor instrumentation ────────────────────────────────────────────────
//
// The whole reason this suite exists rather than only the model. A closed fd
// number goes straight back to the kernel's free pool, so a double close is
// not a crash -- it is somebody ELSE's descriptor disappearing. That is
// unobservable in a model and trivially observable here.

static int count_open_fds()
{
    DIR* d = opendir("/proc/self/fd");
    if (!d) return -1;
    int n = 0;
    while (readdir(d)) ++n;
    closedir(d);
    return n - 3; // ., .., and the dirfd itself
}

static bool fd_is_valid(int fd)
{
    return fd >= 0 && ::fcntl(fd, F_GETFD) != -1;
}

// A bank of descriptors opened once and never touched again. If any of them
// stops being valid, something closed a number it did not own.
struct SentinelBank
{
    std::vector<int> fds;

    void open(int n)
    {
        for (int i = 0; i < n; ++i)
        {
            int fd = ::open("/dev/null", O_RDONLY);
            if (fd >= 0) fds.push_back(fd);
        }
    }
    int casualties() const
    {
        int dead = 0;
        for (int fd : fds) if (!fd_is_valid(fd)) ++dead;
        return dead;
    }
    void close_all() { for (int fd : fds) ::close(fd); fds.clear(); }
};

// ── certificate acquisition ───────────────────────────────────────────────────

struct CertPaths
{
    std::string cert;
    std::string key;
    std::string why_missing;
    bool ok() const { return !cert.empty() && !key.empty(); }
};

static bool file_exists(const std::string& p)
{
    struct stat st;
    return !p.empty() && ::stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

static CertPaths acquire_cert(const char* argv_cert, const char* argv_key)
{
    CertPaths cp;

    const char* ec = std::getenv("ETCS_TEST_CERT");
    const char* ek = std::getenv("ETCS_TEST_KEY");
    if (ec && ek && file_exists(ec) && file_exists(ek)) { cp.cert = ec; cp.key = ek; return cp; }

    if (argv_cert && argv_key && file_exists(argv_cert) && file_exists(argv_key))
    { cp.cert = argv_cert; cp.key = argv_key; return cp; }

    // Throwaway pair, regenerated only if absent. Two days of validity, since
    // nothing here should ever be reachable by anything but this process.
    const std::string c = "/tmp/etcs_scsreal_cert.pem";
    const std::string k = "/tmp/etcs_scsreal_key.pem";
    if (!file_exists(c) || !file_exists(k))
    {
        const std::string cmd =
            "openssl req -x509 -newkey rsa:2048 -keyout " + k + " -out " + c +
            " -days 2 -nodes -subj /CN=localhost >/dev/null 2>&1";
        if (std::system(cmd.c_str()) != 0)
        {
            cp.why_missing = "no cert given and openssl generation failed "
                             "(set ETCS_TEST_CERT / ETCS_TEST_KEY)";
            return cp;
        }
    }
    if (!file_exists(c) || !file_exists(k))
    {
        cp.why_missing = "generated certificate did not appear at " + c;
        return cp;
    }
    cp.cert = c;
    cp.key  = k;
    return cp;
}

// ═════════════════════════════════════════════════════════════════════════════
// Real-fd lifecycle
// ═════════════════════════════════════════════════════════════════════════════

// A connection needs a live fd to be worth testing; socketpair gives one whose
// peer we own, so nothing here depends on a listener, a port or a peer process.
struct FdPair
{
    int server = -1;
    int peer   = -1;
    bool make()
    {
        int sp[2];
        if (::socketpair(AF_UNIX, SOCK_STREAM, 0, sp) != 0) return false;
        server = sp[0];
        peer   = sp[1];
        return true;
    }
    void close_peer() { if (peer != -1) { ::close(peer); peer = -1; } }
};

static void test_real_fd_lifecycle()
{
    section("Real SocketConnectionState — fd lifecycle");

    SocketConnectionState conn;
    std::atomic<int> pool_counter{0};
    conn.SetPoolCounter(&pool_counter);

    TEST("a fresh connection is not active", !conn.IsActiveConcrete());
    TEST("a fresh connection has no fd", conn.GetClientFdConcrete() == -1);

    FdPair fds;
    if (!fds.make()) { skip("socketpair", "socketpair(2) failed"); return; }

    pool_counter.fetch_add(1);
    TEST("claim succeeds on a free connection", conn.TryClaim(fds.server));
    TEST("the claimed fd is the one we handed it", conn.GetClientFdConcrete() == fds.server);
    TEST("a claimed connection is active", conn.IsActiveConcrete());
    TEST("a claimed connection reports open", conn.IsConnectionOpenConcrete());
    TEST("a second claim is refused", !conn.TryClaim(fds.server));

    // One outstanding submission on top of the seeded dispatch reference.
    conn.NoteSubmit();
    conn.ResetConcrete();
    TEST("reset with work outstanding does not close the fd yet", fd_is_valid(fds.server));
    TEST("reset with work outstanding leaves it active", conn.IsActiveConcrete());

    conn.NoteComplete();               // the submission
    TEST("still draining while the dispatch reference is held", conn.IsActiveConcrete());

    conn.NoteComplete();               // the dispatch reference -- last one out
    TEST("the last release finalizes", !conn.IsActiveConcrete());
    TEST("finalize really closed the descriptor", !fd_is_valid(fds.server));
    TEST("finalize cleared the fd", conn.GetClientFdConcrete() == -1);
    TEST("finalize decremented the pool counter", pool_counter.load() == 0);

    fds.close_peer();

    // Reclaimable, and the dispatch reference was re-seeded -- if it were not,
    // this claim would be immediately finalizable by any passing NoteComplete.
    FdPair again;
    if (!again.make()) { skip("socketpair (reclaim)", "socketpair(2) failed"); return; }
    pool_counter.fetch_add(1);
    TEST("the connection is claimable again", conn.TryClaim(again.server));
    conn.ResetConcrete();
    conn.NoteComplete();
    TEST("second cycle finalizes cleanly", !conn.IsActiveConcrete());
    TEST("second cycle closed its descriptor", !fd_is_valid(again.server));
    again.close_peer();
}

static void test_close_is_exactly_once()
{
    section("Real SocketConnectionState — close is exactly once");

    // CloseConnection uses exchange(-1) so a second call cannot close a number
    // the kernel has since handed to someone else. Proving that needs a victim
    // to steal: a sentinel opened AFTER the connection's own fd was closed is
    // very likely to be handed that exact number.
    FdPair fds;
    if (!fds.make()) { skip("socketpair", "socketpair(2) failed"); return; }
    const int watched = fds.server;

    SocketConnectionState conn;
    conn.TryClaim(fds.server);
    conn.ResetConcrete();
    conn.NoteComplete();
    TEST("the descriptor is closed after the drain", !fd_is_valid(watched));

    // Almost certainly the same number, since it was just returned.
    int squatter = ::open("/dev/null", O_RDONLY);
    TEST("a squatter took the freed number", squatter == watched);

    // Every path that could plausibly close again.
    conn.CloseConnection();
    conn.ResetConcrete();
    conn.NoteComplete();
    conn.NoteComplete();

    TEST("no later path closed the squatter's descriptor", fd_is_valid(squatter));
    if (squatter >= 0) ::close(squatter);
    fds.close_peer();
}

// ═════════════════════════════════════════════════════════════════════════════
// Real TLS — a genuine handshake through the real staging buffers
// ═════════════════════════════════════════════════════════════════════════════

// Two byte queues standing in for the socket. Nothing here is a shortcut past
// the code under test: the SERVER side goes through TLSServerContext's own
// CipherIn/CipherOut spans exactly as ConnectionManager's handshake driver
// does, and this wire is only what a recv/send completion would have moved.
struct Wire
{
    std::string c2s;   // client -> server ciphertext
    std::string s2c;   // server -> client ciphertext
};

// A plain mbedtls client. Its BIO talks to the wire directly -- it is the
// peer, not the thing under test.
struct TestClient
{
    mbedtls_ssl_context ssl{};
    mbedtls_ssl_config  conf{};
    Wire*               wire = nullptr;
    bool                up   = false;

    static int cSend(void* p, const unsigned char* b, size_t l)
    {
        auto* self = static_cast<TestClient*>(p);
        self->wire->c2s.append(reinterpret_cast<const char*>(b), l);
        return static_cast<int>(l);
    }
    static int cRecv(void* p, unsigned char* b, size_t l)
    {
        auto* self = static_cast<TestClient*>(p);
        if (self->wire->s2c.empty()) return MBEDTLS_ERR_SSL_WANT_READ;
        size_t n = std::min(l, self->wire->s2c.size());
        std::memcpy(b, self->wire->s2c.data(), n);
        self->wire->s2c.erase(0, n);
        return static_cast<int>(n);
    }

    bool Init(Wire* w)
    {
        wire = w;
        if (psa_crypto_init() != PSA_SUCCESS) return false;
        mbedtls_ssl_init(&ssl);
        mbedtls_ssl_config_init(&conf);
        if (mbedtls_ssl_config_defaults(&conf, MBEDTLS_SSL_IS_CLIENT,
                                        MBEDTLS_SSL_TRANSPORT_STREAM,
                                        MBEDTLS_SSL_PRESET_DEFAULT) != 0) return false;
        // The server presents a self-signed cert and does not ask for one
        // back; verification is not what is under test here.
        mbedtls_ssl_conf_authmode(&conf, MBEDTLS_SSL_VERIFY_NONE);
        if (mbedtls_ssl_setup(&ssl, &conf) != 0) return false;
        mbedtls_ssl_set_hostname(&ssl, "localhost");
        mbedtls_ssl_set_bio(&ssl, this, &TestClient::cSend, &TestClient::cRecv, nullptr);
        up = true;
        return true;
    }
    void Free()
    {
        if (!up) return;
        mbedtls_ssl_free(&ssl);
        mbedtls_ssl_config_free(&conf);
        up = false;
    }
    ~TestClient() { Free(); }
};

// Move whatever the wire holds into the server's staging buffer, and whatever
// the server staged out onto the wire. This is the ONLY thing a real recv/send
// completion contributes -- which is precisely why keying the flush wrongly
// deadlocks: no completion ever arrives to unstick it.
static void feed_server(SocketConnectionState& conn, Wire& w)
{
    auto& in = conn.GetTLS().CipherIn();
    in.compact();
    if (w.c2s.empty() || in.writable() == 0) return;
    size_t n = std::min(w.c2s.size(), in.writable());
    std::memcpy(in.ptr + in.len, w.c2s.data(), n);
    in.len += n;
    w.c2s.erase(0, n);
}

static void drain_server(SocketConnectionState& conn, Wire& w)
{
    auto& out = conn.GetTLS().CipherOut();
    if (out.unread() == 0) { out.reset(); return; }
    w.s2c.append(out.ptr + out.off, out.unread());
    out.reset();
}

static bool is_fatal(int ret)
{
    return ret != 0
        && ret != MBEDTLS_ERR_SSL_WANT_READ
        && ret != MBEDTLS_ERR_SSL_WANT_WRITE;
}

// Drives both sides to Established. Budgeted rather than unbounded: a flush
// regression shows up as "made no progress", and a test that hangs forever is
// a test nobody runs.
static bool drive_handshake(SocketConnectionState& conn, TestClient& cli, Wire& w,
                            int budget, std::string& err)
{
    bool server_done = false;
    bool client_done = false;

    for (int step = 0; step < budget; ++step)
    {
        if (!server_done)
        {
            feed_server(conn, w);
            int sret = conn.GetTLS().DriveHandshake();
            drain_server(conn, w);
            if (sret == 0) server_done = true;
            else if (is_fatal(sret))
            {
                err = "server handshake failed: " + std::to_string(sret);
                return false;
            }
        }
        if (!client_done)
        {
            int cret = mbedtls_ssl_handshake(&cli.ssl);
            if (cret == 0) client_done = true;
            else if (is_fatal(cret))
            {
                err = "client handshake failed: " + std::to_string(cret);
                return false;
            }
        }
        if (server_done && client_done) return true;
    }
    err = "handshake made no progress within " + std::to_string(budget) +
          " steps -- staged ciphertext is not reaching the wire";
    return false;
}

// One request in, one response out, over the established session. The point is
// that this DECRYPTS: it is the only way to tell a live session from a flag.
static bool exchange(SocketConnectionState& conn, TestClient& cli, Wire& w,
                     const std::string& request, const std::string& response,
                     std::string& got_request, std::string& got_response,
                     int budget, std::string& err)
{
    got_request.clear();
    got_response.clear();

    int n = mbedtls_ssl_write(&cli.ssl,
                              reinterpret_cast<const unsigned char*>(request.data()),
                              request.size());
    if (n != static_cast<int>(request.size()))
    {
        err = "client write returned " + std::to_string(n);
        return false;
    }

    unsigned char buf[4096];
    for (int step = 0; step < budget && got_request.empty(); ++step)
    {
        feed_server(conn, w);
        int r = conn.GetTLS().ReadPlain(buf, sizeof(buf));
        drain_server(conn, w);
        if (r > 0) { got_request.assign(reinterpret_cast<char*>(buf), static_cast<size_t>(r)); break; }
        if (is_fatal(r)) { err = "server ReadPlain failed: " + std::to_string(r); return false; }
    }
    if (got_request.empty()) { err = "server never decrypted the request"; return false; }

    for (size_t off = 0; off < response.size(); )
    {
        int wn = conn.GetTLS().WritePlain(
            reinterpret_cast<const unsigned char*>(response.data() + off),
            response.size() - off);
        drain_server(conn, w);
        if (wn > 0) { off += static_cast<size_t>(wn); continue; }
        if (is_fatal(wn)) { err = "server WritePlain failed: " + std::to_string(wn); return false; }
    }

    for (int step = 0; step < budget && got_response.size() < response.size(); ++step)
    {
        int r = mbedtls_ssl_read(&cli.ssl, buf, sizeof(buf));
        if (r > 0) { got_response.append(reinterpret_cast<char*>(buf), static_cast<size_t>(r)); continue; }
        if (is_fatal(r)) { err = "client read failed: " + std::to_string(r); return false; }
    }
    if (got_response != response) { err = "client did not receive the full response"; return false; }
    return true;
}

static void test_tls_config(const CertPaths& cp)
{
    section("Real TLSServerConfig");

    if (!cp.ok()) { skip("TLSServerConfig", cp.why_missing); return; }

    auto conf = std::make_shared<TLSServerConfig>();
    TEST("a fresh config is not ready", !conf->IsReady());
    TEST("loading a real certificate and key succeeds", conf->LoadCertAndKey(cp.cert, cp.key));
    TEST("the config reports ready", conf->IsReady());

    // The guard that matters: x509_crt_parse_file APPENDS, so a second load
    // would leave the old and new certificates stapled together rather than
    // replacing one with the other. Reload must build a new config.
    std::cout << "  (the refusal below is expected -- it is the assertion)\n";
    TEST("a second load on the same config is refused", !conf->LoadCertAndKey(cp.cert, cp.key));
    TEST("the refusal left the config usable", conf->IsReady());

    auto bad = std::make_shared<TLSServerConfig>();
    std::cout << "  (the parse error below is expected -- it is the assertion)\n";
    TEST("a missing certificate fails the load",
         !bad->LoadCertAndKey("/nonexistent/cert.pem", "/nonexistent/key.pem"));
    TEST("a failed load leaves the config not ready", !bad->IsReady());
}

static void test_real_handshake(const CertPaths& cp)
{
    section("Real TLS handshake through the real staging buffers");

    if (!cp.ok()) { skip("handshake", cp.why_missing); return; }

    auto conf = std::make_shared<TLSServerConfig>();
    if (!conf->LoadCertAndKey(cp.cert, cp.key)) { skip("handshake", "config would not load"); return; }

    SocketConnectionState conn;
    FdPair fds;
    if (!fds.make()) { skip("handshake", "socketpair(2) failed"); return; }
    conn.TryClaim(fds.server);

    TEST("TLS starts idle on a fresh connection", !conn.GetTLS().IsActive());
    TEST("Init against a ready config succeeds", conn.GetTLS().Init(conf));
    TEST("Init moves the session to handshaking",
         conn.GetTLS().GetPhase() == TLSServerContext::Phase::Handshaking);

    Wire wire;
    TestClient cli;
    if (!cli.Init(&wire)) { skip("handshake", "could not stand up an mbedtls client"); return; }

    std::string err;
    auto t0 = std::chrono::steady_clock::now();
    const bool shook = drive_handshake(conn, cli, wire, 512, err);
    auto t1 = std::chrono::steady_clock::now();
    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

    if (!shook) std::cout << "    " << FG_RED << err << RESET << "\n";
    else        std::cout << "    handshake completed in " << formatDuration(ms) << "\n";

    TEST("a real handshake completes", shook);
    TEST("the server session reports established", conn.GetTLS().IsEstablished());

    if (shook)
    {
        // ── keep-alive, for real ─────────────────────────────────────────────
        // RecycleForNextRequest is the between-requests path. If it ever
        // reached TLSServerContext::Free, this second exchange would fail to
        // decrypt -- which is the whole reason the split exists.
        std::string got_req, got_resp;
        const bool first = exchange(conn, cli, wire,
                                    "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n",
                                    "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nhi",
                                    got_req, got_resp, 512, err);
        if (!first) std::cout << "    " << FG_RED << err << RESET << "\n";
        TEST("the first request decrypts server-side", first && got_req.rfind("GET /", 0) == 0);
        TEST("the first response decrypts client-side", first);

        const unsigned served_before = conn.Served();
        conn.RecycleForNextRequest();
        TEST("recycle counted a served request", conn.Served() == served_before + 1);
        TEST("recycle left the TLS session established", conn.GetTLS().IsEstablished());
        TEST("recycle kept the descriptor", conn.GetClientFdConcrete() == fds.server);

        const bool second = exchange(conn, cli, wire,
                                     "GET /second HTTP/1.1\r\nHost: localhost\r\n\r\n",
                                     "HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\nbye",
                                     got_req, got_resp, 512, err);
        if (!second) std::cout << "    " << FG_RED << err << RESET << "\n";
        // The load-bearing assertion of this whole file: a SECOND request
        // decrypted on the SAME session after a recycle. No re-handshake
        // happened, because none was driven.
        TEST("a second request decrypts on the SAME session after recycle",
             second && got_req.rfind("GET /second", 0) == 0);
    }

    // ── and finalize DOES tear it down ───────────────────────────────────────
    conn.ResetConcrete();
    conn.NoteComplete();
    TEST("the connection finalized", !conn.IsActiveConcrete());
    TEST("finalize freed the TLS session", !conn.GetTLS().IsActive());
    TEST("finalize closed the descriptor", !fd_is_valid(fds.server));
    fds.close_peer();
}

static void test_cert_rotation_refcount(const CertPaths& cp)
{
    section("Real cert rotation — the refcount that makes it zero-downtime");

    if (!cp.ok()) { skip("cert rotation", cp.why_missing); return; }

    auto old_conf = std::make_shared<TLSServerConfig>();
    if (!old_conf->LoadCertAndKey(cp.cert, cp.key)) { skip("cert rotation", "config would not load"); return; }
    std::weak_ptr<TLSServerConfig> watch = old_conf;

    SocketConnectionState conn;
    FdPair fds;
    if (!fds.make()) { skip("cert rotation", "socketpair(2) failed"); return; }
    conn.TryClaim(fds.server);
    TEST("a session is set up against the old config", conn.GetTLS().Init(old_conf));

    // ReloadCerts, modelled exactly: a NEW config is built and installed, and
    // the manager drops its reference to the old one. Nothing tells the live
    // session about it.
    auto new_conf = std::make_shared<TLSServerConfig>();
    TEST("a replacement config loads", new_conf->LoadCertAndKey(cp.cert, cp.key));
    old_conf.reset();

    TEST("the superseded config is still alive -- a session holds it", !watch.expired());

    // And it is still USABLE: the point of the refcount is that a connection
    // mid-handshake when the reload lands still completes against the config
    // its ssl_ context points into.
    Wire wire;
    TestClient cli;
    if (cli.Init(&wire))
    {
        std::string err;
        const bool shook = drive_handshake(conn, cli, wire, 512, err);
        if (!shook) std::cout << "    " << FG_RED << err << RESET << "\n";
        TEST("a session handshakes to completion against a superseded config", shook);
        TEST("the superseded config outlived the reload", !watch.expired());
    }
    else skip("handshake after rotation", "could not stand up an mbedtls client");

    conn.ResetConcrete();
    conn.NoteComplete();
    // finalizeIfDraining -> tls_.Free() -> conf_.reset(), the last reference.
    TEST("the superseded config is destroyed by the last session's finalize", watch.expired());
    fds.close_peer();
}

// ═════════════════════════════════════════════════════════════════════════════
// Churn — real objects, real descriptors, several threads
// ═════════════════════════════════════════════════════════════════════════════

struct Slot
{
    SocketConnectionState conn;
    // Who believes they hold this slot. -1 is nobody. Republishing a
    // connection under a live owner shows up here and nowhere else.
    std::atomic<int> holder{-1};
};

struct CompletionQueue
{
    std::mutex               m;
    std::deque<Slot*>        q;
    std::atomic<bool>        stop{false};
    std::atomic<long long>   drained{0};

    void push(Slot* s) { std::lock_guard<std::mutex> lk(m); q.push_back(s); }
    Slot* pop()
    {
        std::lock_guard<std::mutex> lk(m);
        if (q.empty()) return nullptr;
        Slot* s = q.front();
        q.pop_front();
        return s;
    }
    bool empty() { std::lock_guard<std::mutex> lk(m); return q.empty(); }
};

struct ChurnReport
{
    long long claims       = 0;
    long long finalizes    = 0;
    long long fd_leaks     = 0;
    long long violations   = 0;
    int       sentinel_dead = 0;
    int       fd_delta     = 0;
    int       pool_counter = 0;
    double    ms           = 0.0;
    std::string first;
};

static ChurnReport run_churn(int pool_size, int owner_threads, int completion_threads,
                             int iters, SentinelBank& sentinels)
{
    g_violations.reset();

    std::vector<std::unique_ptr<Slot>> pool;
    pool.reserve(static_cast<size_t>(pool_size));
    std::atomic<int> pool_counter{0};
    for (int i = 0; i < pool_size; ++i)
    {
        pool.emplace_back(new Slot());
        pool.back()->conn.SetPoolCounter(&pool_counter);
    }

    CompletionQueue cq;
    std::atomic<long long> claims{0};
    std::atomic<long long> finalizes{0};
    std::atomic<long long> fd_leaks{0};

    const int fd_baseline = count_open_fds();
    auto t0 = std::chrono::steady_clock::now();

    std::vector<std::thread> completers;
    for (int t = 0; t < completion_threads; ++t)
    {
        completers.emplace_back([&cq, t]()
        {
            pin_thread((t + 8) % get_core_count());
            int idle = 0;
            while (true)
            {
                Slot* s = cq.pop();
                if (!s)
                {
                    if (cq.stop.load(std::memory_order_acquire)) return;
                    ETCS::LMAXSequentialSharedPage::progressiveYield(idle);
                    continue;
                }
                idle = 0;
                cq.drained.fetch_add(1, std::memory_order_relaxed);
                // LAST thing touched: NoteComplete can publish this object as
                // reusable, so anything after it races the next claimer.
                s->conn.NoteComplete();
            }
        });
    }

    std::vector<std::thread> owners;
    for (int t = 0; t < owner_threads; ++t)
    {
        owners.emplace_back([&, t]()
        {
            pin_thread(t % get_core_count());
            std::mt19937 rng(static_cast<unsigned>(0x5eed + t));
            const int my_id = t;

            for (int i = 0; i < iters; ++i)
            {
                FdPair fds;
                if (!fds.make()) { fd_leaks.fetch_add(1); break; }

                // Find a slot. Contested on purpose -- two accept completions
                // reaching TryClaim at once is the case the CAS exists for.
                Slot* slot = nullptr;
                int spins = 0;
                while (!slot)
                {
                    for (int k = 0; k < pool_size; ++k)
                    {
                        Slot* cand = pool[static_cast<size_t>(k)].get();
                        if (cand->conn.TryClaim(fds.server)) { slot = cand; break; }
                    }
                    if (!slot) ETCS::LMAXSequentialSharedPage::progressiveYield(spins);
                }
                pool_counter.fetch_add(1, std::memory_order_acq_rel);
                claims.fetch_add(1, std::memory_order_relaxed);

                const int prev = slot->holder.exchange(my_id, std::memory_order_acq_rel);
                if (prev != -1)
                    g_violations.record("claimed a connection still held by owner "
                                        + std::to_string(prev));

                // The connection is ours; it must agree.
                if (slot->conn.GetClientFdConcrete() != fds.server)
                    g_violations.record("a claimed connection reports someone else's fd");
                if (!slot->conn.IsActiveConcrete())
                    g_violations.record("a claimed connection reports inactive");

                // Some outstanding I/O, retired by the completion threads.
                const int submissions = 1 + static_cast<int>(rng() % 3);
                for (int s = 0; s < submissions; ++s)
                {
                    slot->conn.NoteSubmit();
                    cq.push(slot);
                }

                // Some connections serve a second request first.
                if (rng() % 4 == 0)
                {
                    slot->conn.RecycleForNextRequest();
                    if (slot->conn.GetClientFdConcrete() != fds.server)
                        g_violations.record("recycle changed the descriptor");
                    if (!slot->conn.IsActiveConcrete())
                        g_violations.record("recycle deactivated a live connection");
                }

                slot->conn.ResetConcrete();

                // Released before the last reference goes, not after: once
                // io_inflight can reach zero this slot may be republished, and
                // touching it then is the race being tested for.
                slot->holder.store(-1, std::memory_order_release);
                slot->conn.NoteComplete();   // the dispatch reference

                // Wait for the drain rather than assuming it. The fd is not
                // ours to reason about until the connection says it is done.
                int wait = 0;
                while (slot->conn.IsActiveConcrete())
                    ETCS::LMAXSequentialSharedPage::progressiveYield(wait);

                finalizes.fetch_add(1, std::memory_order_relaxed);

                // NOT fd_is_valid(fds.server) -- that is wrong here, and
                // wrong in a way worth naming because the first draft of this
                // test failed on it. The kernel hands a freed descriptor
                // NUMBER straight back out, so by the time this line runs
                // another owner thread's socketpair may already be holding
                // that same number: the check would report a live descriptor
                // and call it a leak, at a rate that rises with thread count.
                //
                // The peer is identity, not a number. It is ours, it was never
                // handed to the connection, and it reads EOF exactly when the
                // server end is genuinely closed -- regardless of who holds
                // the number now.
                char probe;
                ssize_t n = ::recv(fds.peer, &probe, 1, MSG_DONTWAIT);
                if (n != 0)
                {
                    fd_leaks.fetch_add(1, std::memory_order_relaxed);
                    ::close(fds.server);
                }
                fds.close_peer();
            }
        });
    }

    for (auto& th : owners) th.join();
    cq.stop.store(true, std::memory_order_release);
    for (auto& th : completers) th.join();

    auto t1 = std::chrono::steady_clock::now();

    ChurnReport r;
    r.claims        = claims.load();
    r.finalizes     = finalizes.load();
    r.fd_leaks      = fd_leaks.load();
    r.violations    = g_violations.count.load();
    r.first         = g_violations.first;
    r.sentinel_dead = sentinels.casualties();
    r.pool_counter  = pool_counter.load();
    r.ms            = std::chrono::duration<double, std::milli>(t1 - t0).count();
    r.fd_delta      = count_open_fds() - fd_baseline;
    return r;
}

static void test_churn(int owner_threads, int completion_threads, int pool_size,
                       int iters, SentinelBank& sentinels)
{
    section("Churn — real connections, real descriptors");

    ChurnReport r = run_churn(pool_size, owner_threads, completion_threads, iters, sentinels);

    std::cout << "    claims " << std::setw(10) << formatWithCommas(r.claims)
              << "  finalizes " << std::setw(10) << formatWithCommas(r.finalizes)
              << "  fd_leaks " << r.fd_leaks
              << "  fd_delta " << r.fd_delta
              << "  violations " << formatWithCommas(r.violations)
              << "  (" << formatDuration(r.ms) << ")\n";
    if (!r.first.empty())
        std::cout << "    " << FG_RED << "first: " << r.first << RESET << "\n";

    TEST("every claim finalized", r.claims == r.finalizes && r.claims > 0);
    TEST("no ownership violations", r.violations == 0);
    TEST("every connection closed its own descriptor", r.fd_leaks == 0);
    TEST("no descriptors leaked overall", r.fd_delta <= 0);
    TEST("no unrelated descriptor was closed", r.sentinel_dead == 0);
    TEST("the pool counter returned to zero", r.pool_counter == 0);
}

// ═════════════════════════════════════════════════════════════════════════════
// ABI — the built module actually loads
// ═════════════════════════════════════════════════════════════════════════════
//
// Deliberately LAST. dlopening a module runs its static initialisers, which
// touch its own per-DSO singletons; running it before the lifecycle tests
// would put that in the blast radius of everything above for no benefit. It is
// not dlclosed either -- tearing an ETCS module back down mid-process is not
// something this suite should be the first to try.
static void test_module_abi()
{
    section("Module ABI — the built .so loads and exports its entry point");

    const char* candidates[] = {
        "../ETCS-Commons/NetworkProvider/NetworkProvider.so",
        "./NetworkProvider.so",
        "../modules/NetworkProvider.so",
    };

    const char* found = nullptr;
    for (const char* p : candidates) if (file_exists(p)) { found = p; break; }
    if (!found)
    {
        skip("dlopen NetworkProvider.so",
             "not found next to the loader -- run make in NetworkProvider first");
        return;
    }

    void* h = ::dlopen(found, RTLD_NOW | RTLD_LOCAL);
    if (!h)
    {
        std::cout << "    " << FG_RED << ::dlerror() << RESET << "\n";
        TEST("the module dlopens", false);
        return;
    }
    TEST("the module dlopens", true);

    // exports.map names this one exactly, no wildcard -- if it stops being
    // visible the runtime cannot load the module at all.
    ::dlerror();
    void* sym = ::dlsym(h, "RegisterDynamicLoader");
    TEST("RegisterDynamicLoader is exported", sym != nullptr && ::dlerror() == nullptr);

    // And the version script must still be hiding everything else.
    // SocketConnectionState::TryClaim(int), Itanium-mangled: 21 characters of
    // class name, 8 of method. Spelled out because a typo here would make this
    // assertion vacuously true -- a symbol that does not exist is absent for
    // the wrong reason.
    ::dlerror();
    void* leaked = ::dlsym(h, "_ZN21SocketConnectionState8TryClaimEi");
    TEST("module internals stay hidden by the version script", leaked == nullptr);
    ::dlerror();
}

// ═════════════════════════════════════════════════════════════════════════════

int main(int argc, char* argv[])
{
    WIRE_CONTEXT();

    int  iters      = 1000;
    bool want_dlopen = true;
    const char* argv_cert = nullptr;
    const char* argv_key  = nullptr;

    for (int i = 1; i < argc; ++i)
    {
        std::string a = argv[i];
        if (a == "--no-dlopen") { want_dlopen = false; continue; }
        if (!a.empty() && a[0] >= '0' && a[0] <= '9') { iters = std::atoi(a.c_str()); continue; }
        if (!argv_cert) argv_cert = argv[i];
        else if (!argv_key) argv_key = argv[i];
    }

    const int owner_threads      = 4;
    const int completion_threads = 3;
    const int pool_size          = 8;

    CertPaths cp = acquire_cert(argv_cert, argv_key);

    std::cout << "SocketConnectionState — real module suite\n";
    std::cout << "cores:              " << get_core_count() << "\n";
    std::cout << "pool size:          " << pool_size << "\n";
    std::cout << "owner threads:      " << owner_threads << "\n";
    std::cout << "completion threads: " << completion_threads << "\n";
    std::cout << "iterations/owner:   " << formatWithCommas(iters) << "\n";
    std::cout << "certificate:        " << (cp.ok() ? cp.cert : std::string("(none -- TLS sections skip)")) << "\n";

    // Opened before anything else touches a descriptor, so every close that
    // happens from here on is inside the window they are watching.
    SentinelBank sentinels;
    sentinels.open(16);

    test_real_fd_lifecycle();
    test_close_is_exactly_once();
    test_tls_config(cp);
    test_real_handshake(cp);
    test_cert_rotation_refcount(cp);
    test_churn(owner_threads, completion_threads, pool_size, iters, sentinels);

    TEST("no sentinel descriptor was closed by anything above", sentinels.casualties() == 0);
    sentinels.close_all();

    if (want_dlopen) test_module_abi();
    else             skip("module ABI", "--no-dlopen");

    std::cout << "\n────────────────────────────────────\n";
    std::cout << (s_failed == 0 ? FG_GREEN : FG_RED)
              << "passed: " << s_passed << "\n"
              << "failed: " << s_failed << RESET << "\n";
    if (s_skipped > 0)
        std::cout << FG_YELLOW << "skipped: " << s_skipped << RESET << "\n";

    if (s_failed == 0)
        std::cout << FG_CYAN
                  << "\n  This exercised the real class with real descriptors and a real\n"
                     "  handshake. It does NOT cover the accept chain, io_uring completion\n"
                     "  ordering, or ConnectionManager's drain -- those need a live server.\n"
                     "  Run scstest for the fast accounting sweep and this before a release.\n"
                  << RESET;

    return s_failed == 0 ? 0 : 1;
}