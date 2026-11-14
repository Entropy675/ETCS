// MirrorMessageTesterLoader.cc
//
// A MESSAGE IS ONE UNIT, WHATEVER ITS SIZE, ON EVERY STRATEGY.
//
// MirrorBuffer::writeMessage / readMessage (core/MirrorBuffer.h): a run of
// frames joined on the far side (Pipe, Socket), or one handed-over block
// (LMAX). Checked on raw pairs, no entities:
//
//   - sizes either side of every boundary (one Buffer, one frame, megabytes)
//     arrive byte for byte, in order
//   - a message that fits one frame is a plain frame, and a plain frame is a
//     message: either end can move to messages first
//   - over the reader's cap: refused, and the NEXT message still arrives
//   - LMAX: a writer with no reader withdraws on a signal instead of hanging
//   - a staging page smaller than a frame (a module arena's 4 KiB chunk, as
//     a link's edge has) still carries a message whole
//
//   ./Run_MirrorMessageTesterLoader

#include "../ETCS.h"

#include <sys/socket.h>
#include <atomic>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

static int g_fail = 0;
static void check(bool ok, const std::string& what)
{
    std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", what.c_str());
    if (!ok) ++g_fail;
}

static std::string pattern(size_t n, unsigned seed)
{
    std::string s(n, '\0');
    uint32_t x = seed * 2654435761u + 1;
    for (size_t i = 0; i < n; ++i) { x ^= x << 13; x ^= x >> 17; x ^= x << 5; s[i] = static_cast<char>(x); }
    return s;
}

enum class Kind { LMAX, Pipe, Socket, SocketSmallPage };
static const char* name(Kind k)
{
    return k == Kind::LMAX ? "LMAX" : k == Kind::Pipe ? "Pipe" : k == Kind::Socket ? "Socket" : "Socket, 4 KiB page";
}
// A sub-arena of OS pages, the way a module's own arena is sized.
static ETCS::MemoryArena& small_arena()
{
    static ETCS::MemoryArena a(4096, /* performance */ false);
    return a;
}

// One pair of the given strategy, torn down with it.
struct Pair
{
    Kind kind;
    ETCS::SignalFlag   flag{ 0 };
    ETCS::SignalContext ctx;
    ETCS::MirrorBuffer prod, cons;
    ETCS::LMAXSequentialSharedPage* ring = nullptr;
    ETCS::SharedPage*  page = nullptr;
    int sv[2] = { -1, -1 };

    explicit Pair(Kind k) : kind(k)
    {
        ctx.interrupt = &flag;
        prod.bindContext(ctx);
        cons.bindContext(ctx);
        auto& arena = ETCS::MemoryArena::getInstance();
        if (k == Kind::LMAX)
        {
            ring = ETCS::LMAXSequentialSharedPage::allocate(arena, 2, 64);
            ETCS::MirrorBuffer::makePair<ETCS::StrategyLMAX, ETCS::LMAXSequentialSharedPage>(prod, cons, ring, 1, 2);
        }
        else if (k == Kind::Pipe)
        {
            page = ETCS::SharedPage::allocate(arena, 2);
            ETCS::MirrorBuffer::makePair<ETCS::StrategyPipe, ETCS::SharedPage>(prod, cons, page, 1, 2);
        }
        else
        {
            ::socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
            page = ETCS::SharedPage::allocate(k == Kind::SocketSmallPage ? small_arena() : arena, 2);
            ETCS::MirrorBuffer::makePair<ETCS::StrategySocket, ETCS::SharedPage>(
                prod, cons, page, static_cast<uint64_t>(sv[0]), static_cast<uint64_t>(sv[1]));
        }
    }
    ~Pair()
    {
        if (kind == Kind::LMAX)
            ETCS::MirrorBuffer::teardownPair<ETCS::StrategyLMAX, ETCS::LMAXSequentialSharedPage>(prod, cons, ring);
        else if (kind == Kind::Pipe)
            ETCS::MirrorBuffer::teardownPair<ETCS::StrategyPipe, ETCS::SharedPage>(prod, cons, page);
        else
        {
            ETCS::MirrorBuffer::teardownPair<ETCS::StrategySocket, ETCS::SharedPage>(prod, cons, page);
            if (sv[0] >= 0) ::close(sv[0]);
            if (sv[1] >= 0) ::close(sv[1]);
        }
    }
};

static void sizes(Kind k)
{
    const size_t F = ETCS::MirrorBuffer::MAX_FRAME_PAYLOAD, B = ETCS::Buffer::bufsize;
    const std::vector<size_t> ns = { 0, 1, B - 2, B - 1, B, F - 1, F, F + 1, 3 * F + 7, 100000, 5u << 20 };
    Pair p(k);
    std::vector<std::string> sent;
    for (size_t i = 0; i < ns.size(); ++i) sent.push_back(pattern(ns[i], static_cast<unsigned>(i + 1)));
    std::atomic<int> wrote{ 0 };
    std::thread w([&] { for (auto& s : sent) { if (!p.prod.writeMessage(s)) break; ++wrote; } p.prod.closeWrite(); });
    size_t same = 0;
    for (size_t i = 0; i < sent.size(); ++i)
    {
        std::string got;
        if (!p.cons.readMessage(got)) break;
        if (got == sent[i]) ++same;
        else std::printf("       #%zu: %zu bytes sent, %zu read\n", i, sent[i].size(), got.size());
    }
    w.join();
    check(wrote.load() == static_cast<int>(sent.size()) && same == sent.size(),
          std::string(name(k)) + ": " + std::to_string(sent.size()) + " messages, 0 B to 5 MiB, arrive whole and in order");
}

static void interop(Kind k)
{
    Pair p(k);
    std::thread w([&]
    {
        ETCS::Buffer b; b.writeString("a plain frame");
        p.prod.writeRaw(b);
        p.prod.writeMessage(std::string("a short message"));
        p.prod.closeWrite();
    });
    std::string m;
    const bool a = p.cons.readMessage(m) && m == "a plain frame";
    ETCS::Buffer b;
    const bool c = p.cons.readRaw(b) && b.toString() == "a short message";
    w.join();
    check(a, std::string(name(k)) + ": a plain frame reads as a message");
    check(c, std::string(name(k)) + ": a message that fits a Buffer reads as a plain frame");
}

static void cap(Kind k)
{
    Pair p(k);
    const std::string big = pattern(100000, 9);
    std::thread w([&] { p.prod.writeMessage(big); p.prod.writeMessage(std::string("after")); p.prod.closeWrite(); });
    std::string got;
    const bool refused = !p.cons.readMessage(got, 1000);
    const bool next    = p.cons.readMessage(got) && got == "after";
    w.join();
    check(refused && next, std::string(name(k)) + ": over the reader's cap is refused, and the next message still arrives");
}

static void withdraw()
{
    Pair p(Kind::LMAX);
    std::atomic<int> result{ -1 };
    std::thread w([&] { result = p.prod.writeMessage(pattern(10000, 3)) ? 1 : 0; });
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    const bool waiting = result.load() == -1;
    p.flag.store(1);
    w.join();
    check(waiting && result.load() == 0, "LMAX: a writer waits for its reader, and withdraws on a signal");
}

static void too_wide_for_readraw()
{
    Pair p(Kind::LMAX);
    std::atomic<int> result{ -1 };
    std::thread w([&] { result = p.prod.writeMessage(pattern(1000, 4)) ? 1 : 0; });
    ETCS::Buffer b;
    const bool read = p.cons.readRaw(b);
    w.join();
    check(!read && result.load() == 1, "LMAX: readRaw refuses a message wider than a Buffer, and its writer is released");
}

int main(int, char**)
{
    WIRE_CONTEXT();
    std::printf("\n-- messages ------------------------------------------------------\n");
    for (Kind k : { Kind::LMAX, Kind::Pipe, Kind::Socket, Kind::SocketSmallPage }) sizes(k);
    for (Kind k : { Kind::LMAX, Kind::Pipe, Kind::Socket }) interop(k);
    for (Kind k : { Kind::LMAX, Kind::Pipe, Kind::Socket }) cap(k);
    withdraw();
    too_wide_for_readraw();
    std::printf("\n%s (%d failed)\n", g_fail ? "FAILED" : "all passed", g_fail);
    return g_fail ? 1 : 0;
}
