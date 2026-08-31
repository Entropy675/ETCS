// NetworkProviderStressTestLoader.cc
//
// dlopen's the real NetworkProvider.so and drives HTTPParser::Listen ->
// HTTPParser::ServeTree under concurrent HTTP load until it survives or
// crashes under a debugger.
//
// Two things about this file are load-bearing and NOT obvious from the ETCS
// headers alone -- both cost a real debugging pass to find, so don't "clean
// up" past them:
//
//   1. WIRE_CONTEXT() (ETCS_API.h) must supply a root explicitly. LoadEvent's
//      vacant-registry bootstrap now requires one; older test loaders that
//      predate this requirement get a "no bootstrap_root was supplied" error.
//
//   2. Listen->ServeTree is a produce/consume PAIR, not a plain work-func --
//      Entity::call()'s ordinary overloads refuse stream actions outright.
//      The correct surface is the third, non-templated, ETCS_LOADER-only
//      call() overload (Entity.h): runtime-string, always StrategyPipe since
//      it crosses the DSO boundary. This is what a plain unwrapped .etcs
//      script (e.g. file_server.etcs's "Listen server 8080 ./www" /
//      "ServeTree server") compiles down to -- no leading name token, since
//      that stripping is CommandExecutor.h's concern for script-level name
//      resolution, not call()'s.
//
// ServeTree runs synchronously on the calling thread for the server's
// entire lifetime, so the call lives on its own thread while main() runs
// the load generator concurrently, then stops the server with an ordinary
// HTTPParser.Delete work-func call.
//
// Deliberately does not include NetworkProvider.h / Contract_NetworkProvider.h /
// PicoHTTPParser.h / SocketConnectionState.h -- NetworkProvider.so is
// dlopen'd at runtime, same as the real loader; this binary only needs core
// headers plus POSIX sockets for the load generator.
//
// Build (best-effort reconstruction of the loader's own build rule -- not
// verified against the actual Makefile target):
//
//   g++ -std=c++17 -fvisibility=hidden -Wall -Wextra -O0 -g
//       -DETCS_LOADER -DETCS_MODULE_NAME=\"NetworkProviderStressTester\"
//       -I. -I.. -pipe -pthread -fuse-ld=gold -ldl
//       NetworkProviderStressTestLoader.cc -o NetworkProviderStressTestLoader
//
// Run from bin/ (module path resolution is relative to cwd):
//
//   lldb ./NetworkProviderStressTestLoader
//   (lldb) run 18080 16 200
//   (lldb) bt all          # if it crashes

#include "../ETCS.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {

struct StressConfig
{
    int port           = 18080;
    int num_threads     = 16;
    int iterations_each = 200;
};

bool parse_args(int argc, char** argv, StressConfig& cfg)
{
    if (argc > 1) cfg.port           = std::atoi(argv[1]);
    if (argc > 2) cfg.num_threads     = std::atoi(argv[2]);
    if (argc > 3) cfg.iterations_each = std::atoi(argv[3]);

    if (cfg.port <= 0 || cfg.port > 65535)
    {
        std::cerr << "bad port: " << cfg.port << "\n";
        return false;
    }
    if (cfg.num_threads <= 0 || cfg.iterations_each <= 0)
    {
        std::cerr << "num_threads and iterations_each must be positive "
                     "(got " << cfg.num_threads << ", " << cfg.iterations_each << ")\n";
        return false;
    }
    return true;
}

// Minimal ./www tree -- ServeTree/FileHtmlPage needs real content to
// resolve. Filenames match asset paths seen in production logs, so a visual
// diff against real server logs stays easy.
void write_test_www_tree()
{
    if (std::system("mkdir -p www/static/js www/static/css") != 0)
        std::cerr << "warning: mkdir -p www/static/... failed; ServeTree "
                     "requests will probably 404\n";

    auto write = [](const char* path, const char* content)
    {
        std::ofstream f(path);
        if (!f) { std::cerr << "warning: couldn't write " << path << "\n"; return; }
        f << content;
    };
    write("www/static/index.html",   "<html><body>stress test</body></html>");
    write("www/static/js/socket.js", "console.log('socket.js');");
    write("www/static/js/ui.js",     "console.log('ui.js');");
    write("www/static/js/main.js",   "console.log('main.js');");
    write("www/static/js/state.js",  "console.log('state.js');");
    write("www/static/js/utils.js",  "console.log('utils.js');");
    write("www/static/css/style.css", "body{color:red}");
}

// --- Raw HTTP load generator -- plain POSIX sockets, no ETCS API involved.
// Two traffic shapes concurrently: ordinary request/response, and abrupt
// mid-request disconnect (the path every production crash this session
// traced back to).

std::atomic<long> g_requests_sent{0};
std::atomic<long> g_requests_failed{0};

int connect_to_port(int port)
{
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(static_cast<uint16_t>(port));
    if (::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr) != 1)
    {
        ::close(fd);
        return -1;
    }
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0)
    {
        ::close(fd);
        return -1;
    }
    return fd;
}

void http_hammer_thread(int port, int iterations, std::vector<std::string> paths)
{
    for (int i = 0; i < iterations; ++i)
    {
        int fd = connect_to_port(port);
        if (fd < 0) { ++g_requests_failed; continue; }

        const std::string& path = paths[static_cast<size_t>(i) % paths.size()];
        std::string req = "GET " + path + " HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
        ::send(fd, req.data(), req.size(), 0);

        char buf[4096];
        while (::recv(fd, buf, sizeof(buf), 0) > 0) {}
        ::close(fd);
        ++g_requests_sent;
    }
}

void abrupt_disconnect_thread(int port, int iterations)
{
    for (int i = 0; i < iterations; ++i)
    {
        int fd = connect_to_port(port);
        if (fd < 0) { ++g_requests_failed; continue; }

        const char* partial = "GET /static/index.html HTTP/1.1\r\n";
        ::send(fd, partial, std::strlen(partial), 0);
        // No recv() -- close immediately, before the request is even
        // complete and before any response could arrive.
        ::close(fd);
        ++g_requests_sent;
    }
}

bool wait_for_port(int port, int timeout_ms)
{
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline)
    {
        int fd = connect_to_port(port);
        if (fd >= 0) { ::close(fd); return true; }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return false;
}

void run_load_generation(const StressConfig& cfg)
{
    static const std::vector<std::string> paths = {
        "/static/index.html", "/static/js/socket.js", "/static/js/ui.js",
        "/static/js/main.js", "/static/js/state.js", "/static/js/utils.js",
        "/static/css/style.css", "/nonexistent",
        "/socket.io/?EIO=4&transport=polling", // matches real client traffic
                                                 // seen in production logs
    };

    std::vector<std::thread> threads;
    threads.reserve(static_cast<size_t>(cfg.num_threads));

    int half = cfg.num_threads / 2;
    for (int t = 0; t < half; ++t)
        threads.emplace_back(http_hammer_thread, cfg.port, cfg.iterations_each, paths);
    for (int t = half; t < cfg.num_threads; ++t)
        threads.emplace_back(abrupt_disconnect_thread, cfg.port, cfg.iterations_each);

    for (auto& t : threads) t.join();

    std::cout << "\nLoad generation complete: " << g_requests_sent.load()
              << " requests sent, " << g_requests_failed.load()
              << " connection failures.\n";
    std::cout << "Process still alive -- no crash during "
              << (cfg.num_threads * cfg.iterations_each) << " total connection attempts.\n";
}



} // namespace


int main(int argc, char** argv)
{
    StressConfig cfg;
    if (!parse_args(argc, argv, cfg))
    {
        std::cerr << "usage: " << argv[0] << " [port] [num_threads] [iterations_each]\n";
        return 1;
    }
    std::cout << "NetworkProviderStressTestLoader -- port=" << cfg.port
              << " threads=" << cfg.num_threads
              << " iterations_each=" << cfg.iterations_each << "\n";

    write_test_www_tree();

    // See this file's header comment for why WIRE_CONTEXT()/spawn_entity/
    // the two http->call() sites below are exactly this shape and not the
    // more "obvious" alternatives.
    WIRE_CONTEXT();

    // Root of the served site (see HttpServer.h's own class comment: "a bag of
    // configuration with Start/Stop attached"). Not HTTPParser -- HTTPParser's
    // own actions are ConsumeRequest/ProduceResponse, a Stream:1 pair meant to
    // be driven BY a ConnectionManager subscriber, not called directly.
    ETCS::Entity* httpserver = ETCS::spawn_entity("NetworkProvider", "HttpServer", env, loader);
    if (!httpserver)
    {
        std::cerr << "Failed to load NetworkProvider:HttpServer\n";
        return 1;
    }
    std::cout << "HttpServer loaded, RID:" << httpserver->getRID() << "\n";

    // Page tree, attached the same way `web.add(FileHtmlPage tree)` does in
    // run_website.etcs -- BEFORE Start, per HttpServer.h: "Added by a script
    // BEFORE Start ... Serve walks them in attach order."
    // make_typed_child, not addTag<FileHtmlPage>: the concrete type is not
    // compiled into this binary at all, so the child is built through the
    // module's own dlsym'd "<Tag>_MakeChild" export. See its comment in
    // CommandExecutor.h.
    ETCS::Entity* pages = ETCS::make_typed_child("NetworkProvider", "FileHtmlPage", httpserver, loader);
    if (!pages)
    {
        std::cerr << "Failed to attach FileHtmlPage to HttpServer\n";
        httpserver->call("HttpServer.Delete", "", ctx);
        return 1;
    }
    pages->call("FileHtmlPage.LoadFromDisk", "./www", ctx);

    std::string port_str = std::to_string(cfg.port);
    httpserver->call("HttpServer.SetPort", port_str.c_str(), ctx);

    std::cout << "Starting HttpServer (real dlopen, real io_uring)...\n";
    httpserver->call("HttpServer.Start", "", ctx);
    // Start() is a plain work func -- binds/listens/submits the initial accept
    // window synchronously before returning, so this line does not block for
    // the server's lifetime the way the old Listen->ServeTree call did.
    // No background thread needed to keep main() free for load generation.

    std::cout << "Waiting for port " << cfg.port << " to accept connections...\n";
    if (!wait_for_port(cfg.port, 5000))
    {
        std::cerr << "FAILED: port " << cfg.port << " never started accepting "
                     "connections within 5s -- check log output above for "
                     "[HttpServer]/[ConnectionManager] Start/Open errors.\n";
        httpserver->call("HttpServer.Delete", "", ctx);
        return 1;
    }
    std::cout << "Port is live -- starting load generation.\n";

    run_load_generation(cfg);

    std::cout << "Stopping server...\n";
    httpserver->call("HttpServer.Delete", "", ctx);
    ETCS::PendingUnloadRegistry::getInstance().join_all();   // wait out the module's unload-recheck thread

    std::cout << "Server stopped cleanly. Test complete.\n";
    return 0;
}