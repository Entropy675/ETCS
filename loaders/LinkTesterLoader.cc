// LinkTesterLoader.cc
//
// A LINK THAT IS SLOW IS NOT A LINK THAT IS GONE. Two ends in one process, a
// Room hosting a record on a LinkHub and a Peer binding a surface to it, with
// a relay between them that can stop passing the host's bytes -- what a busy
// browser tab is to the far side (its socket moves only when its main thread
// runs). What is held to account:
//
//   1. A BIND ANSWERED LATE IS A BIND. The host's bytes held for longer than
//      the 15 s the edge used to wait: the bind waits it out, the surface is
//      bound, and its record reads the host's.
//   2. A VERB ANSWERED LATE IS AN ANSWER. The same for a verb on the surface.
//   3. A LINK THAT CLOSES ENDS THE WAIT. A verb outstanding when its Peer
//      closes comes back at once, unanswered -- the wait has no deadline, but
//      it is not blind to the link going.
//   4. A HOST THAT COMES LATE COMES. A room hosted through the hub (as a
//      browser tab hosts) whose control link is held for 17 s: the hub keeps
//      the guest parked, the guest keeps waiting for its welcome, and when
//      the host comes for it the two are linked.
//
//   ./Run_LinkTesterLoader
//
// No display, no TLS (the relay passes bytes, not records); plain sockets on
// 127.0.0.1.

#include "../ETCS.h"

#include <arpa/inet.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <netinet/in.h>
#include <poll.h>
#include <sstream>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

static int g_fail = 0;
static void check(bool ok, const char* what)
{
    std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++g_fail;
}

static std::string verb(ETCS::Entity* e, const std::string& action, const std::string& args = "")
{
    ETCS::Buffer a(action.c_str()), d(args.c_str());
    e->call(a, d);
    return d.toString();
}
static double since(std::chrono::steady_clock::time_point t0)
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

/*
 * THE RELAY: a TCP listener that passes bytes both ways between whoever
 * connects (as many as do) and the server, and that can HOLD what the server
 * sends -- left unread in the kernel, as a starved reader leaves it -- until
 * let go.
 */
struct Relay
{
    std::atomic<bool> hold{ false }, stop{ false };
    int listener = -1, port = 0, to = 0;
    std::thread th;
    std::vector<std::thread> pipes;

    bool open(int server_port)
    {
        to = server_port;
        listener = ::socket(AF_INET, SOCK_STREAM, 0);
        int one = 1;
        ::setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        sockaddr_in a{};
        a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(0x7F000001); a.sin_port = 0;
        if (::bind(listener, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0 || ::listen(listener, 4) != 0) return false;
        socklen_t n = sizeof a;
        ::getsockname(listener, reinterpret_cast<sockaddr*>(&a), &n);
        port = ntohs(a.sin_port);
        th = std::thread([this]() { run(); });
        return true;
    }
    void run()
    {
        while (!stop)
        {
            pollfd lp{ listener, POLLIN, 0 };
            if (::poll(&lp, 1, 100) <= 0) continue;
            const int c = ::accept(listener, nullptr, nullptr);
            if (c >= 0) pipes.emplace_back([this, c]() { pipe(c); });
        }
        for (std::thread& t : pipes) t.join();
    }
    void pipe(int c)
    {
        const int s = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in a{};
        a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(0x7F000001); a.sin_port = htons(static_cast<uint16_t>(to));
        if (::connect(s, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) { ::close(c); ::close(s); return; }
        char buf[16384];
        while (!stop)
        {
            pollfd p[2] = { { c, POLLIN, 0 }, { s, static_cast<short>(hold ? 0 : POLLIN), 0 } };
            if (::poll(p, 2, 50) <= 0) continue;
            if (p[0].revents & (POLLIN | POLLHUP))
            {
                const ssize_t n = ::recv(c, buf, sizeof buf, 0);
                if (n <= 0 || ::send(s, buf, static_cast<size_t>(n), MSG_NOSIGNAL) != n) break;
            }
            if (!hold && (p[1].revents & (POLLIN | POLLHUP)))
            {
                const ssize_t n = ::recv(s, buf, sizeof buf, 0);
                if (n <= 0 || ::send(c, buf, static_cast<size_t>(n), MSG_NOSIGNAL) != n) break;
            }
        }
        ::close(c); ::close(s);
    }
    void close()
    {
        stop = true;
        if (th.joinable()) th.join();
        if (listener >= 0) ::close(listener);
    }
};

int main(int, char**)
{
    WIRE_CONTEXT();
    std::cout << "=== A slow link, not a gone one ===\n";

    // The host: a server with a hub, a record with a few thousand lines on
    // it, and a room publishing the record on the hub.
    ETCS::ExecutionContext host(&root, &ctx);
    {
        std::istringstream in(
            "spawn NetworkProvider::HttpServer web\n"
            "web.SetPort(18471)\n"
            "web.spawn(NetworkProvider::LinkHub links)\n"
            "web.Start()\n"
            "spawn NetworkProvider::Ledger book\n"
            "book.Author(host)\n"
            "spawn NetworkProvider::Room room\n"
            "room.SetName(host)\n"
            "room.Publish(book @book Head Since Follow)\n"
            "room.Host(@links t1)\n");
        if (!ETCS::run_script(in, std::string(ETCS_ACE_ROOT) + "/loaders/link_host.etcs", host))
        { std::printf("FAILED (the host script)\n"); return 1; }
    }
    auto entity = [](ETCS::ExecutionContext& c, const char* n) {
        return ETCS::etcs_resolve_rid_anywhere(&ETCS::getLoader(), c.names[n].rid);
    };
    ETCS::Entity* book = entity(host, "book");
    for (int i = 0; i < 4000; ++i) verb(book, "Ledger.Append", "tick " + std::to_string(2 * i + 2));
    const std::string head = verb(book, "Ledger.Head");

    Relay relay;
    if (!relay.open(18471)) { std::printf("FAILED (the relay)\n"); return 1; }

    // The guest: a peer over the relay, and a ledger to make a surface of.
    ETCS::ExecutionContext guest(&root, &ctx);
    {
        std::istringstream in(
            "spawn NetworkProvider::Peer peer\n"
            "peer.SetName(guest)\n"
            "peer.Connect(ws://127.0.0.1:" + std::to_string(relay.port) + "/link/t1)\n"
            "spawn NetworkProvider::Ledger mirror\n"
            "mirror.spawn(NetworkProvider::Remote rm)\n");
        if (!ETCS::run_script(in, std::string(ETCS_ACE_ROOT) + "/loaders/link_guest.etcs", guest))
        { std::printf("FAILED (the guest script)\n"); return 1; }
    }
    ETCS::Entity* peer = entity(guest, "peer");
    ETCS::Entity* mirror = entity(guest, "mirror");
    ETCS::Entity* rm = entity(guest, "rm");
    check(peer && mirror && rm && !head.empty(), "a host with a 4000-line record, a guest linked to it through the relay");

    // Holds the host's bytes for `secs`, from now.
    auto holdFor = [&](double secs) {
        relay.hold = true;
        return std::thread([&relay, secs]() {
            std::this_thread::sleep_for(std::chrono::duration<double>(secs));
            relay.hold = false;
        });
    };

    // -- 1. a bind answered late ------------------------------------------------
    std::cout << "\n-- 1. a bind answered late --\n";
    {
        std::thread let_go = holdFor(17.0);
        const auto t0 = std::chrono::steady_clock::now();
        const std::string bound = verb(rm, "Remote.Bind", "book " + std::to_string(peer->getRID()));
        const double took = since(t0);
        let_go.join();
        std::printf("        (bound after %.1f s: \"%s\")\n", took, bound.c_str());
        check(took > 16.0 && bound.find('1') != std::string::npos, "the bind waits out a host 17 s late, and binds");
    }
    check(verb(mirror, "Ledger.Head") == head, "...and its record reads the host's");

    // -- 2. a verb answered late ------------------------------------------------
    std::cout << "\n-- 2. a verb answered late --\n";
    {
        std::thread let_go = holdFor(17.0);
        const auto t0 = std::chrono::steady_clock::now();
        const std::string h = verb(mirror, "Ledger.Head");
        const double took = since(t0);
        let_go.join();
        std::printf("        (answered after %.1f s)\n", took);
        check(took > 16.0 && h == head, "a verb on the surface waits out a host 17 s late, and has its answer");
    }

    // -- 3. a link that closes ends the wait -----------------------------------
    std::cout << "\n-- 3. a link that closes ends the wait --\n";
    {
        relay.hold = true;
        std::thread closer([&]() {
            std::this_thread::sleep_for(std::chrono::seconds(2));
            verb(peer, "Peer.Close");
        });
        const auto t0 = std::chrono::steady_clock::now();
        const std::string h = verb(mirror, "Ledger.Head");
        const double took = since(t0);
        closer.join();
        std::printf("        (back after %.1f s)\n", took);
        check(took < 6.0 && h.empty(), "a verb outstanding when its peer closes comes back at once, unanswered");
        relay.hold = false;
    }

    // -- 4. a host that comes late comes ----------------------------------------
    std::cout << "\n-- 4. a host that comes late comes --\n";
    Relay far;
    if (!far.open(18471)) { std::printf("FAILED (the second relay)\n"); return 1; }
    ETCS::ExecutionContext elsewhere(&root, &ctx);
    {
        std::istringstream in(
            "spawn NetworkProvider::Ledger book2\n"
            "book2.Author(far)\n"
            "spawn NetworkProvider::Room room2\n"
            "room2.SetName(far)\n"
            "room2.Publish(book2 @book2 Head)\n"
            "room2.HostVia(ws://127.0.0.1:" + std::to_string(far.port) + "/link/t2)\n");
        check(ETCS::run_script(in, std::string(ETCS_ACE_ROOT) + "/loaders/link_far.etcs", elsewhere),
              "a room hosted through the hub, its links passing the relay");
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    {
        far.hold = true;
        std::thread let_go([&far]() { std::this_thread::sleep_for(std::chrono::seconds(17)); far.hold = false; });
        ETCS::ExecutionContext late(&root, &ctx);
        const auto t0 = std::chrono::steady_clock::now();
        std::istringstream in(
            "spawn NetworkProvider::Peer peer2\n"
            "peer2.SetName(late)\n"
            "peer2.Connect(ws://127.0.0.1:18471/link/t2)\n"
            "spawn NetworkProvider::Ledger copy2\n"
            "copy2.spawn(NetworkProvider::Remote rm2)\n"
            "rm2.Bind(book2 @peer2)\n");
        ETCS::run_script(in, std::string(ETCS_ACE_ROOT) + "/loaders/link_late.etcs", late);
        const double took = since(t0);
        let_go.join();
        ETCS::Entity* copy2 = entity(late, "copy2");
        const std::string h2 = copy2 ? verb(copy2, "Ledger.Head") : std::string();
        std::printf("        (linked and bound after %.1f s)\n", took);
        check(took > 16.0 && copy2 && h2 == verb(entity(elsewhere, "book2"), "Ledger.Head"),
              "the guest waits, parked, until the host comes for it 17 s later -- and they are linked");
        if (ETCS::Entity* p2 = entity(late, "peer2")) verb(p2, "Peer.Close");
    }
    verb(entity(elsewhere, "room2"), "Room.Delete");
    far.close();

    relay.close();
    verb(entity(host, "room"), "Room.Delete");
    verb(entity(host, "web"), "HttpServer.Stop");
    ETCS::PendingUnloadRegistry::getInstance().join_all();
    std::printf("\n%s (%d failure%s)\n", g_fail ? "FAILED" : "PASSED", g_fail, g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}
