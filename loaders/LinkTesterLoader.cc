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
//   5. A LINK IS BETWEEN TWO PROVEN KEYS, SEALED. Each end knows the other's
//      key as its loader checked it (here one loader, so its own key, a
//      local identity: the fold along its key alone). A loader's word on one
//      link is no word on another, nor on the other side's part of the same.
//      A byte altered on the way closes the link. The same key coming back
//      under its name takes the place of its old link. A room that admits
//      registered identities only turns a local one away at the hello. (An
//      untrusted build vouches with a key made for the session instead of
//      its own: the same checks hold, on that key.)
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
    std::atomic<int>  flip{ 0 };     // alter the last byte of the server's next chunks
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
                if (n > 0 && flip.load() > 0) { --flip; buf[n - 1] ^= 0x5a; }
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

    // -- 5. proven, and sealed --------------------------------------------------
    std::cout << "\n-- 5. proven, and sealed --\n";
    {
        ETCS::sig::Key key = ETCS::trust::gate().signature.id->pub;
        if (ETCS::trust::untrusted_build)
        {
            uint8_t secret[64]; std::string w;
            ETCS::trust::session_link_key(secret, key, w);
            ETCS::trust::mc::crypto_wipe(secret, 64);
        }
        const ETCS::sig::Key ident = ETCS::trust::identity_of(ETCS::trust::path_of({}, key));
        const std::string mine = ETCS::trust::hex(key.data(), 32) + " " + ETCS::trust::hex(ident.data(), 32) + " 0 0";
        ETCS::ExecutionContext third(&root, &ctx);
        std::istringstream in(
            "spawn NetworkProvider::Peer peer3\n"
            "peer3.SetName(third)\n"
            "peer3.Connect(ws://127.0.0.1:" + std::to_string(relay.port) + "/link/t1)\n"
            "spawn NetworkProvider::Ledger mirror3\n"
            "mirror3.spawn(NetworkProvider::Remote rm3)\n"
            "rm3.Bind(book @peer3)\n");
        ETCS::run_script(in, std::string(ETCS_ACE_ROOT) + "/loaders/link_third.etcs", third);
        ETCS::Entity* peer3 = entity(third, "peer3");
        ETCS::Entity* mirror3 = entity(third, "mirror3");
        ETCS::Entity* room = entity(host, "room");
        const std::string seen_by_guest = peer3 ? verb(peer3, "Peer.Who") : std::string();
        const std::string seen_by_host = verb(room, "Room.Who", "third");
        std::printf("        (the guest sees %s; the host sees %s)\n", seen_by_guest.substr(0, 16).c_str(), seen_by_host.substr(0, 16).c_str());
        check(seen_by_guest == mine && seen_by_host == mine,
              "each end knows the other's key as its loader proved it: a local identity, its key alone");

        // A word is for one link, and for one side of it.
        uint8_t t[32], u[32];
        for (int i = 0; i < 32; ++i) { t[i] = uint8_t(i); u[i] = uint8_t(i ^ 1); }
        const std::string th = ETCS::trust::hex(t, 32), uh = ETCS::trust::hex(u, 32);
        std::string word, out, why;
        const bool vouched = ETCS::trust::vouch('d', th, word, why);
        check(vouched && ETCS::trust::recognize('d', word, th, out, why) && out.compare(0, 64, mine.substr(65, 64)) == 0
              && !ETCS::trust::recognize('a', word, th, out, why) && !ETCS::trust::recognize('d', word, uh, out, why),
              "a loader's word on a link is taken for that link and that side, not for another or the other side");
        ETCS::sig::Epoch elsewhere = ETCS::trust::loader_epoch();
        elsewhere[0] ^= 1;
        check(vouched && !ETCS::trust::recognize_in('d', word, th, out, why, elsewhere, ETCS::trust::ace_identity(),
                                                    ETCS::trust::ace_identity_parent())
              && why.compare(0, 13, "another epoch") == 0,
              "and for its own epoch: a loader running another refuses the link");

        // Altered on the way: the guest's answer does not open, the link closes.
        check(mirror3 && verb(mirror3, "Ledger.Head") == head, "...and the surface over it reads the host's record");
        relay.flip = 1;
        const std::string altered = mirror3 ? verb(mirror3, "Ledger.Head") : std::string("x");
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        check(altered.empty() && peer3 && verb(peer3, "Peer.Who") == "-", "a byte altered on the way closes the link");

        // The same key back under the same name takes its place. (A Peer holds
        // its address until closed, so the second dials the host directly.)
        if (peer3) verb(peer3, "Peer.Close");
        ETCS::ExecutionContext again(&root, &ctx);
        std::istringstream in2(
            "spawn NetworkProvider::Peer peer4\n"
            "peer4.SetName(fourth)\n"
            "peer4.Connect(ws://127.0.0.1:" + std::to_string(relay.port) + "/link/t1)\n"
            "spawn NetworkProvider::Peer peer5\n"
            "peer5.SetName(fourth)\n"
            "peer5.Connect(ws://127.0.0.1:18471/link/t1)\n");
        ETCS::run_script(in2, std::string(ETCS_ACE_ROOT) + "/loaders/link_again.etcs", again);
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        ETCS::Entity* peer4 = entity(again, "peer4");
        ETCS::Entity* peer5 = entity(again, "peer5");
        check(peer4 && peer5 && verb(peer4, "Peer.Who") == "-" && verb(peer5, "Peer.Who") == mine
              && verb(room, "Room.Who", "fourth") == mine,
              "the same key coming back under its name takes the place of its old link");
        for (ETCS::Entity* p : { peer4, peer5 }) if (p) verb(p, "Peer.Close");

        // A room that admits registered identities only.
        check(verb(room, "Room.Admit", "registered").find('1') != std::string::npos, "a room set to admit registered identities only");
        ETCS::ExecutionContext barred(&root, &ctx);
        std::istringstream in3(
            "spawn NetworkProvider::Peer peer6\n"
            "peer6.SetName(sixth)\n"
            "peer6.Connect(ws://127.0.0.1:18471/link/t1)\n");
        ETCS::run_script(in3, std::string(ETCS_ACE_ROOT) + "/loaders/link_barred.etcs", barred);
        ETCS::Entity* peer6 = entity(barred, "peer6");
        check(peer6 && verb(peer6, "Peer.Who") == "-" && verb(room, "Room.Who", "sixth") == "-",
              "...turns a local identity away at the hello");
        verb(room, "Room.Admit", "anyone");
    }

    relay.close();
    verb(entity(host, "room"), "Room.Delete");
    verb(entity(host, "web"), "HttpServer.Stop");
    ETCS::PendingUnloadRegistry::getInstance().join_all();
    std::printf("\n%s (%d failure%s)\n", g_fail ? "FAILED" : "PASSED", g_fail, g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}
