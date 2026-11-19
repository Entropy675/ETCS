// ModuleSignatureTesterLoader.cc
//
// A MODULE LOADS ONLY AS BYTES SIGNED BY A KEY THIS LOADER TAKES, UNDER ITS
// NAME, IN THIS EPOCH. What is held to account (core/Signature.h,
// core/Trust.h):
//
//   1. EVERY MODULE IN bin/ IS SIGNED for this loader's key and epoch -- ace
//      signs as it builds -- and this loader attests its own binary.
//   2. WHAT THE DIGEST COVERS. Not the chain, not the section-header fields
//      adding a section moves; any mapped byte. A binary renamed, or carried
//      to an epoch it was not signed in, is refused.
//   3. THE LOCAL HALF, end to end on wasm and PE: the key is made once in the
//      OS keystore and never replaced or written to a file; what it signs
//      carries the key and nothing above, and runs on that loader alone; a
//      second sign is a no-op, a new epoch is carried; a second store is a
//      second loader. A loader binary it signs is passed off: the pass-off
//      names parent and child, and the child attests on that loader; one that
//      passed itself off is a beginning, taken with no identity built in or
//      under its own key's, not under a minted one. A module meets this
//      loader's chain or runs nowhere: signed by another loader's key with
//      nothing above it, it is refused. What meets only this loader's own keys
//      runs local under an identity, and nothing is held local with none; a
//      loader built by a key, or whose identity is that key's, takes it with no
//      keystore.
//   4. THE IDENTITY: a store's is the fold along its registration to its key;
//      three keys under one active key each do their own work and no other's;
//      a session that does not attest signs its local half alone, and leaves
//      what carries its half as it is; the marketplace mints a loader binary
//      regardless, which is where a lineage under its identity begins, and a
//      builder's next build is passed off by the minted loader; a loader passed
//      off by itself, by one that did not attest, or under another identity,
//      does not attest; a release key mints nothing; a registrar certifies a
//      developer's key, whose builds run global on its own identity and on
//      the root's, local on the marketplace's; a download with an empty store
//      takes the marketplace's modules and attests what it minted. A store
//      joins under another root through that root's attestation of its own,
//      and what it signs is global on the identities folded from either. And
//      the table itself, on certificates alone, roots attesting roots among it.
//   5. SEALED: what is checked is a memfd nothing can write; the file on disk
//      changing afterwards changes nothing loaded.
//   6. THE LOADER REFUSES: a signed module under another name, and the same
//      module unsigned, do not attach; nothing is spawned from them. Modules
//      loaded one after another are each themselves.
//   7. THE GATE IS A CHAIN: a Wrapper leaf attached to the gate's entity is
//      handed every load first; refusing, the module does not attach; passing,
//      the loader's own stages still decide.
//   8. THE SITE: only the marketplace publishes, and its list names only what
//      a download would take; a list changed, or of another identity, is not
//      believed; what is behind is fetched, what is the developer's is not;
//      only the listed bytes are put in place. And a module's ask of this
//      loader (TrustEvent) is answered on its ordering thread.
//   9. WITNESSES: two builds that agree are witnessed by the loader among
//      them, and only then; statements agree into a consensus only from
//      distinct witnesses, unaltered, in one epoch, at the quorum, each root
//      directory its own group; a directory is checked against it.
//
//   ./Run_ModuleSignatureTesterLoader

#include "../ETCS.h"

#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace mc = etcs_monocypher;
namespace sig = ETCS::sig;
namespace tr = ETCS::trust;

// A stage a loader adds to its gate: an ordinary Wrapper leaf.
class Veto : public WrapperBase<Veto>
{
public:
    WIRE_TYPE_IDENTITY(Veto)
    bool        refusing = false;
    int         seen = 0;
    std::string last;
    void WrapConcrete(ETCS::MBuffer&, ETCS::SignalContext) override {}
    void UnwrapConcrete(ETCS::MBuffer& io, ETCS::SignalContext) override
    {
        tr::LoadFrame f;
        ++seen;
        if (io.written == sizeof f) { std::memcpy(&f, io.buf, sizeof f); last = f.name; }
        if (refusing) ETCS::wire_refuse(io);
    }
    void CloseConcrete(ETCS::MBuffer&, ETCS::SignalContext) override {}
    ETCS::WireScope ScopeConcrete() const override { return ETCS::WireScope::NetworkOnly; }
};

static int g_fail = 0;
static void check(bool ok, const char* what)
{
    std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", what);
    std::fflush(stdout);
    if (!ok) ++g_fail;
}

static std::vector<uint8_t> bytes_of(const std::string& p)
{
    std::vector<uint8_t> b;
    tr::read_file(p, b);
    return b;
}
static void write_bytes(const std::string& p, const std::vector<uint8_t>& b)
{
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char*>(b.data()), std::streamsize(b.size()));
}
static std::string slurp(const std::string& path)
{
    std::ifstream f(path, std::ios::binary);
    std::stringstream s; s << f.rdbuf();
    return s.str();
}

static bool ok_as(const std::string& name, const std::vector<uint8_t>& b, const sig::Epoch& e,
                  const tr::Identity& A, std::string* why_out = nullptr, bool* local = nullptr)
{
    std::string why;
    const bool ok = tr::verify(name, b.data(), b.size(), e, A, why, false, local);
    if (why_out) *why_out = why;
    return ok;
}

// A small valid wasm module: preamble, a type section, a custom "name".
static std::vector<uint8_t> tiny_wasm()
{
    return { 0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
             0x01, 0x04, 0x01, 0x60, 0x00, 0x00,
             0x00, 0x05, 0x04, 'n', 'a', 'm', 'e' };
}
static void put_uleb(std::vector<uint8_t>& o, size_t n)
{
    do { uint8_t c = n & 0x7f; n >>= 7; o.push_back(c | (n ? 0x80 : 0)); } while (n);
}
static std::vector<uint8_t> wasm_signed(std::vector<uint8_t> w, const std::string& chain)
{
    std::vector<uint8_t> body;
    put_uleb(body, 8);
    body.insert(body.end(), { 'e', 't', 'c', 's', '.', 's', 'i', 'g' });
    body.insert(body.end(), chain.begin(), chain.end());
    w.push_back(0);
    put_uleb(w, body.size());
    w.insert(w.end(), body.begin(), body.end());
    return w;
}
static std::vector<uint8_t> tiny_pe()
{
    std::vector<uint8_t> p(0x100, 0x11);
    p[0] = 'M'; p[1] = 'Z';
    p[0x3C] = 0x40; p[0x3D] = p[0x3E] = p[0x3F] = 0;
    p[0x40] = 'P'; p[0x41] = 'E'; p[0x42] = 0; p[0x43] = 0;
    return p;
}
static std::vector<uint8_t> pe_signed(std::vector<uint8_t> p, const std::string& chain)
{
    p.insert(p.end(), chain.begin(), chain.end());
    const uint32_t n = uint32_t(chain.size());
    for (int i = 0; i < 4; ++i) p.push_back(uint8_t(n >> (8 * i)));
    p.insert(p.end(), sig::kPeTrailer, sig::kPeTrailer + 8);
    return p;
}

// What `who` signs with `secret`: the gate, wrapped, for a session that is
// not this binary's -- attested under an identity this binary was not built with.
// "already" when the bytes carry it; empty when refused.
static std::string sign_as(const tr::Identity& who, const uint8_t* secret, const std::string& name,
                           const std::vector<uint8_t>& b, const sig::Epoch& e, bool kernel, std::string* why = nullptr)
{
    tr::Gate g;
    tr::LoadFrame f;
    std::string chain, w;
    g.use(std::make_shared<const tr::Identity>(who));
    g.signature.signer = [secret](const uint8_t* m, size_t n, uint8_t* s64) { mc::crypto_ed25519_sign(s64, secret, m, n); };
    tr::load_frame(name, b.data(), b.size(), e, f, kernel);
    f.chain = &chain;
    const bool ok = g.wrap(f, w);
    if (why) *why = w;
    return !ok ? std::string() : f.already ? std::string("already") : chain;
}

static sig::Key digest_of(const std::string& name, const std::vector<uint8_t>& b)
{
    sig::Layout L; sig::Key d{}; std::string w;
    sig::layout(b.data(), b.size(), L, w);
    tr::digest(name, b.data(), b.size(), L, d);
    return d;
}

static bool pass_of(const std::vector<uint8_t>& b, sig::PassOff& p)
{
    sig::Layout L; std::string w;
    return sig::layout(b.data(), b.size(), L, w) && L.has_pass && sig::decode(b.data() + L.pass.at, L.pass.len, p);
}

// The identity folded along these keys, from the top.
static sig::Key head_of(std::initializer_list<std::pair<sig::Role, sig::Key>> steps)
{
    sig::Key h = tr::fold_start();
    for (const auto& st : steps) h = tr::fold(h, st.first, st.second);
    return h;
}

struct Keys { uint8_t secret[64]; sig::Key pub; };
static Keys fresh_key()
{
    Keys k{};
    uint8_t seed[32];
    tr::random_bytes(seed, 32);
    mc::crypto_ed25519_key_pair(k.secret, k.pub.data(), seed);
    return k;
}
static sig::Certificate issue(const Keys& by, sig::Role role, const sig::Key& subject, std::vector<sig::Epoch> epochs)
{
    sig::Certificate c;
    c.role = role; c.subject = subject; c.issuer = by.pub; c.epochs = std::move(epochs);
    const std::string m = sig::signed_part(c);
    mc::crypto_ed25519_sign(c.signature, by.secret, reinterpret_cast<const uint8_t*>(m.data()), m.size());
    return c;
}

int main(int, char**)
{
    WIRE_CONTEXT();
    std::cout << "=== Module signatures ===\n";
    ETCS::ExecutionContext s(&root, &ctx);
    auto run = [&](const std::string& text) {
        std::istringstream in(text);
        return ETCS::run_script(in, std::string(ETCS_ACE_ROOT) + "/loaders/sig_test.etcs", s);
    };

    const std::string bin = std::string(ETCS_ACE_ROOT) + "/bin/";
    const sig::Epoch& E = tr::loader_epoch();
    const tr::Identity& A = *tr::gate().signature.id;      // this session's, after its handshake
    sig::Epoch other{}; other[0] = 0x5a;

    // -- 1. every module in bin/ is signed for this loader ---------------------------
    std::cout << "-- 1. bin/ --\n";
    {
        const char* mods[] = { "ShellProvider", "WindowProvider", "ChessProvider", "CryptProvider",
                               "NetworkProvider", "DatabaseProvider", "PaintProvider", "KartProvider" };
        int seen = 0, good = 0;
        for (const char* m : mods)
        {
            const auto b = bytes_of(bin + m + ".so");
            if (b.empty()) continue;
            ++seen;
            std::string why;
            if (ok_as(m, b, E, A, &why)) ++good;
            else std::printf("       %s: %s\n", m, why.c_str());
        }
        check(seen > 0 && good == seen, "every module present verifies under this loader's key and epoch");
        check(A.proven && !tr::zero(A.pub), "this loader proved its key from the keystore");
        if (!A.proven) std::printf("       %s\n", A.unproven.c_str());
        check(A.attested && A.self_name == "Run_ModuleSignatureTesterLoader" && !tr::zero(A.self),
              "this loader attests itself: its own binary passes its own stages");
        if (!A.attested) std::printf("       %s\n", A.unattested.c_str());
    }

    const std::vector<uint8_t> chess = bytes_of(bin + "ChessProvider.so");
    sig::Layout L;
    std::string why;
    check(sig::layout(chess.data(), chess.size(), L, why) && L.format == sig::Format::Elf && L.has_chain,
          "ChessProvider.so is ELF and carries a chain");
    {
        std::vector<sig::Certificate> c;
        sig::decode_chain(chess.data() + L.chain.at, L.chain.len, c);
        check(c.size() == 1 + A.reg.size() && c[0].issuer == A.pub
              && sig::encode_chain({ c.begin() + 1, c.end() }) == sig::encode_chain(A.reg),
              A.reg.empty() ? "its chain is the local half alone: module <- this loader's key"
                            : "its chain is module <- this loader's key <- ACE's half");
    }

    // -- 2. what the digest covers ---------------------------------------------------
    std::cout << "-- 2. digest --\n";
    {
        sig::Key d0{}, d1{};
        tr::digest("ChessProvider", chess.data(), chess.size(), L, d0);
        auto b = chess;
        for (size_t i = 0; i < L.chain.len; ++i) b[L.chain.at + i] ^= 0xff;
        for (size_t i = 0x28; i < 0x30; ++i) b[i] ^= 0x01;
        b[0x3C] ^= 0x01; b[0x3E] ^= 0x01;
        sig::Layout L2 = L;                         // the header now points nowhere sane
        tr::digest("ChessProvider", b.data(), b.size(), L2, d1);
        check(d0 == d1, "the chain and e_shoff/e_shnum/e_shstrndx are outside the digest");

        auto t = chess;
        const sig::Span& load = L.covered[2];       // first PT_LOAD
        t[load.at + load.len / 2] ^= 0x01;
        check(!ok_as("ChessProvider", t, E, A, &why) && why == "signed for other bytes, or another name",
              "one flipped mapped byte is refused");
        check(!ok_as("WindowProvider", chess, E, A, &why) && why == "signed for other bytes, or another name",
              "the same bytes under another name are refused");
        check(!ok_as("ChessProvider", chess, other, A, &why) && why == "not witnessed under this epoch",
              "another epoch is refused");
        tr::Identity other_head = A; other_head.head[0] ^= 0x01; other_head.reg.clear();   // as establish leaves it
        {
            bool local = false;
            check(ok_as("ChessProvider", chess, E, other_head, nullptr, &local) && local,
                  "this loader's own build runs under another identity too, and runs local: it meets at this loader's key");
        }
        tr::Identity not_me = A; not_me.pub[0] ^= 0x01; not_me.builder = sig::Key{};
        tr::Identity unproven = A; unproven.proven = false; unproven.unproven = "no keystore"; unproven.builder = sig::Key{};
        if (A.role == sig::Role::Marketplace && !A.reg.empty())
            check(ok_as("ChessProvider", chess, E, not_me) && ok_as("ChessProvider", chess, E, unproven),
                  "the marketplace signed it: any loader of its identity takes it, with or without a key");
        else
        {
            not_me.head = unproven.head = sig::Key{};
            check(!ok_as("ChessProvider", chess, E, not_me, &why) && why == "signed by another loader's key, and nothing above it",
                  "a loader that is not this one refuses it: another loader's key, nothing above it");
            check(!ok_as("ChessProvider", chess, E, unproven, &why)
                  && why == "signed by a loader, and this one has not proven a key: no keystore",
                  "and so does this one, in a session where it could not prove its key");
            tr::Identity built = unproven; built.builder = A.pub;
            check(A.builder == A.pub && ok_as("ChessProvider", chess, E, built),
                  "unless the key is the one this loader was built by: compiled in, it needs no proof");
            tr::Identity of_it; of_it.head = head_of({ { sig::Role::Loader, A.pub } });
            bool local = true;
            check(A.reg.empty() && ok_as("ChessProvider", chess, E, of_it, nullptr, &local) && !local,
                  "or whose identity is that key's alone: there it is global, with no key at all");
        }
    }

    // -- 3. the local half: the keystore, and nothing above the key ----------------------
    std::cout << "-- 3. local half --\n";
    char tmpl[] = "/tmp/etcs-sigtest-XXXXXX";
    const std::string dir = ::mkdtemp(tmpl) ? tmpl : "";
    const std::string store = dir + "/store", store2 = dir + "/other";
    namespace ks = ETCS::keystore;
    std::string held, kwhy;
    {
        const sig::Key cfg{};                       // no identity built in, whatever this binary has
        auto sign_cfg = [&](const std::string& st, const char* name, const std::string& file, const sig::Epoch& e,
                            const std::string& out) { return tr::sign_module(st, name, file, e, out, why, nullptr, cfg); };
        const ks::Status k0 = ks::load(store, "loader-key", held, kwhy);
        check(k0 == ks::Status::Absent, "the keystore answers, and holds nothing for a new store");
        if (k0 == ks::Status::Unavailable) std::printf("       %s\n", kwhy.c_str());

        write_bytes(dir + "/Probe.wasm", tiny_wasm());
        check(sign_cfg(store, "Probe", dir + "/Probe.wasm", E, dir + "/c1") == 0, "a wasm module signs");
        check(ks::load(store, "loader-key", held, kwhy) == ks::Status::Ok && held.size() == 64,
              "the key is made on first use, in the keystore");
        const std::string seed_before = held;
        struct stat st{};
        check(::stat((store + "/loader.sign").c_str(), &st) != 0 && slurp(store + "/loader.sign.pub").size() == 65,
              "no file holds it; its public half is beside the store");
        check(ks::store_new(store, "loader-key", std::string(64, '0'), kwhy) == ks::Status::Refused,
              "the keystore never replaces a key");

        sig::Key pub{};
        std::vector<sig::Certificate> c1;
        const std::string c1b = slurp(dir + "/c1");
        check(tr::load_public(store, pub) && sig::decode_chain(c1b.data(), c1b.size(), c1)
              && c1.size() == 1 && c1[0].issuer == pub && ::stat((store + "/loader.chain").c_str(), &st) != 0,
              "what it signs carries its key and nothing above it; no chain is made");
        tr::Identity mine; mine.pub = pub; mine.proven = true;

        const auto w1 = wasm_signed(tiny_wasm(), c1b);
        check(ok_as("Probe", w1, E, mine), "signed wasm runs on this loader");
        write_bytes(dir + "/Probe.wasm", w1);
        check(sign_cfg(store, "Probe", dir + "/Probe.wasm", E, dir + "/c2") == 3,
              "signing again in the same epoch is a no-op");
        check(sign_cfg(store, "Probe", dir + "/Probe.wasm", other, dir + "/c3") == 0,
              "a new epoch signs again");
        const auto w2 = wasm_signed(tiny_wasm(), slurp(dir + "/c3"));
        check(ok_as("Probe", w2, E, mine) && ok_as("Probe", w2, other, mine),
              "and carries the old epoch with the new");
        check(ks::load(store, "loader-key", held, kwhy) == ks::Status::Ok && held == seed_before,
              "the key is the same key");

        sig::Layout PL; sig::Key pd{};
        const auto tw = tiny_wasm();
        sig::layout(tw.data(), tw.size(), PL, why);
        tr::digest("Probe", tw.data(), tw.size(), PL, pd);
        const Keys someone = fresh_key();
        check(!ok_as("Probe", wasm_signed(tw, sig::encode_chain({ issue(someone, sig::Role::Module, pd, { E }) })), E, mine, &why)
              && why == "signed by another loader's key, and nothing above it", "a lone certificate by another key meets nothing here");

        write_bytes(dir + "/Other.wasm", tiny_wasm());
        check(sign_cfg(store2, "Probe", dir + "/Other.wasm", E, dir + "/o1") == 0,
              "a second store is a second loader");
        sig::Key pub2{};
        tr::load_public(store2, pub2);
        tr::Identity theirs; theirs.pub = pub2; theirs.proven = true;
        const auto o1 = wasm_signed(tiny_wasm(), slurp(dir + "/o1"));
        check(pub2 != pub && ok_as("Probe", o1, E, theirs), "its module runs on it");
        check(!ok_as("Probe", o1, E, mine, &why) && why == "signed by another loader's key, and nothing above it",
              "and not on a loader that is not it");
        tr::Identity headed = theirs; headed.head = fresh_key().pub;
        bool local = false;
        check(ok_as("Probe", o1, E, headed, nullptr, &local) && local,
              "with an identity built in that is not its key's, its own local half still runs on it, marked local");
        check(!ok_as("Probe", o1, E, mine, &why) && why == "signed by another loader's key, and nothing above it" && (mine.head = headed.head, !ok_as("Probe", o1, E, mine)),
              "and another loader's, with nothing above it, runs in neither");
        mine.head = sig::Key{};
        tr::Identity built; built.builder = pub2;
        check(ok_as("Probe", o1, E, built, nullptr, &local) && !local,
              "a loader built by that key takes it, with no key or keystore of its own; with no identity, nothing is held local");
        built.head = headed.head;
        check(ok_as("Probe", o1, E, built, nullptr, &local) && local, "under another identity, as local");
        tr::Identity its; its.head = head_of({ { sig::Role::Loader, pub2 } });
        check(ok_as("Probe", o1, E, its, nullptr, &local) && !local,
              "and a loader whose identity is that key's alone takes it as global, with no key at all");

        // The lineage: this loader passes off the next, and says so.
        std::string line;
        std::vector<tr::LineageEdge> edges;
        write_bytes(dir + "/next", tiny_wasm());
        check(tr::succeed(store2, "etcs", dir + "/next", E, dir + "/n1", why, &line, cfg) == 0 && !line.empty(),
              "signing the next loader binary records an edge");
        sig::Key child{};
        tr::digest("etcs", tw.data(), tw.size(), PL, child);
        check(line.rfind("etcs-lineage 3 etcs head=none ", 0) == 0
              && tr::lineage_read(store2, edges, why) && edges.size() == 1 && edges[0].child == child && tr::zero(edges[0].head)
              && edges[0].key == pub2 && edges[0].to == E && edges[0].from == E && !edges[0].genesis
              && edges[0].parent == A.self && !edges[0].host.empty(),
              "the edge names parent, child, both epochs, the signer and the host, and verifies");
        check(tr::succeed(store2, "etcs", dir + "/next", E, dir + "/n2", why, nullptr, cfg) == 0
              && (edges.clear(), tr::lineage_read(store2, edges, why)) && edges.size() == 1,
              "the same child in the same epoch is not recorded twice");
        const auto n1 = wasm_signed(tiny_wasm(), slurp(dir + "/n1"));
        sig::PassOff po;
        check(pass_of(n1, po) && po.parent == A.self && po.child == child && po.key == pub2 && tr::zero(po.head)
              && po.to == E && po.attested == tr::establish(store2, true, cfg)->attested,
              "the child carries its pass-off: this binary -> it, under no identity, by that key, attested as this session is");
        uint8_t sk2[64]; sig::Key k2{};
        tr::loader_key(store2, sk2, k2, why);
        tr::Identity as2 = *tr::establish(store2, true, cfg);
        as2.attested = true;                        // as a signed build of no identity would be
        const std::string a1b = sign_as(as2, sk2, "etcs", tiny_wasm(), E, true);
        const auto a1 = wasm_signed(tiny_wasm(), a1b);
        std::string kw;
        check(tr::verify("etcs", a1.data(), a1.size(), E, theirs, kw, true), "the loader an attesting one passed off attests on it");
        check(!tr::verify("etcs", a1.data(), a1.size(), E, mine, kw, true) && kw == "signed by another loader's key, and nothing above it",
              "and on no other loader");
        {
            const auto n0 = wasm_signed(tiny_wasm(), a1b.substr(0, a1b.size() - sig::kPassBytes));
            check(!tr::verify("etcs", n0.data(), n0.size(), E, theirs, kw, true) && kw == "no signed build passed this loader off",
                  "the same chain with no pass-off does not attest: an unsigned build");
            auto n2 = a1; n2[n2.size() - 1] ^= 0x01;
            check(!tr::verify("etcs", n2.data(), n2.size(), E, theirs, kw, true) && kw == "bad signature on the pass-off",
                  "a pass-off altered after signing is refused");
            check(ok_as("etcs", a1, E, theirs), "loaded as a module, the same bytes are taken: a pass-off is a loader binary's question");
            tr::Identity self2 = *tr::establish(store2, true, cfg);
            self2.self = child;
            const auto s1 = wasm_signed(tiny_wasm(), sign_as(self2, sk2, "etcs", tiny_wasm(), E, true));
            check(pass_of(s1, po) && po.parent == po.child && tr::verify("etcs", s1.data(), s1.size(), E, theirs, kw, true),
                  "a loader binary that passed itself off is a beginning, and an experimental epoch takes it");
            self2.head = head_of({ { sig::Role::Loader, pub2 } });
            tr::Identity own = theirs; own.head = self2.head;
            const auto s3 = wasm_signed(tiny_wasm(), sign_as(self2, sk2, "etcs", tiny_wasm(), E, true));
            check(tr::verify("etcs", s3.data(), s3.size(), E, own, kw, true),
                  "and so does a loader whose identity is its own key's: nothing above it to begin from");
            self2.head = fresh_key().pub;
            tr::Identity minted = theirs; minted.head = self2.head;
            const auto s2 = wasm_signed(tiny_wasm(), sign_as(self2, sk2, "etcs", tiny_wasm(), E, true));
            check(!tr::verify("etcs", s2.data(), s2.size(), E, minted, kw, true)
                  && kw == "it passed itself off: under a minted identity a lineage begins at a mint",
                  "a minted identity does not: there a lineage begins at a mint");
            check(!tr::verify("etcs", n1.data(), n1.size(), E, minted, kw, true)
                  && kw == "passed off under another identity: a loader enters an identity from a mint",
                  "and a binary passed off under no identity does not attest under one");
            mc::crypto_wipe(sk2, 64);
        }
        {
            std::ofstream f(store2 + "/lineage.log", std::ios::app);
            std::string forged = line;
            forged[forged.find("host=") + 5] ^= 0x01;
            f << forged << "\n";
        }
        check((edges.clear(), !tr::lineage_read(store2, edges, why)) && why.find("line 2") != std::string::npos,
              "an edge altered after signing is found");

        auto w3 = w2; w3[11] ^= 0x01;               // inside the type section
        check(!ok_as("Probe", w3, E, mine), "a changed wasm section is refused");
        const auto w4 = wasm_signed(w1, c1b);
        check(!ok_as("Probe", w4, E, mine, &why) && why == "two signature sections", "two chains are malformed");
        check(!ok_as("Probe", tiny_wasm(), E, mine, &why) && why == "unsigned", "no chain is unsigned");

        write_bytes(dir + "/Probe.dll", tiny_pe());
        check(sign_cfg(store, "Probe", dir + "/Probe.dll", E, dir + "/p1") == 0, "a PE image signs");
        const auto p1 = pe_signed(tiny_pe(), slurp(dir + "/p1"));
        check(ok_as("Probe", p1, E, mine), "the PE trailer verifies");
        auto p2 = p1; p2[0x80] ^= 0x01;
        check(!ok_as("Probe", p2, E, mine), "a changed PE byte is refused");
        auto p3 = p1; p3[p3.size() - 12] = 0xff; p3[p3.size() - 9] = 0x7f;
        check(!ok_as("Probe", p3, E, mine, &why) && why == "signature trailer longer than the file",
              "a trailer longer than the file is malformed");

        {
            std::ofstream f(store + "/loader.sign.pub", std::ios::trunc);
            f << std::string(64, 'a') << "\n";
        }
        check(sign_cfg(store, "Probe", dir + "/Probe.dll", other, dir + "/p4") == 2
              && why.find("is not this key's") != std::string::npos,
              "a pub file that is not the key's is refused, not rewritten");

        // The handshake: the claim is the files', the proof is the keystore's.
        const auto id2 = tr::establish(store2, true, cfg);
        check(id2->proven && id2->pub == pub2 && id2->reg.empty(),
              "a session proves the store's key against the keystore, and unregistered has nothing above it");
        check(!tr::establish(store, true, cfg)->proven
              && tr::establish(store, true, cfg)->unproven == "the keystore's key is not loader.sign.pub's",
              "and a store whose pub file was swapped proves nothing");
        check(!tr::establish(dir + "/none", true, cfg)->proven && !tr::establish(store2, false, cfg)->proven,
              "a store with no key, or a session that did not ask, is unproven");

        check(ks::erase(store, "loader-key", kwhy) == ks::Status::Ok && ks::erase(store2, "loader-key", kwhy) == ks::Status::Ok
              && ks::load(store, "loader-key", held, kwhy) == ks::Status::Absent,
              "erase is the only way a key goes");
    }

    // -- 4. the identity: the join, one role per key, minting, and the table ------------
    std::cout << "-- 4. identity --\n";
    {
        const Keys sroot = fresh_key(), active = fresh_key();
        const sig::Key R = sroot.pub;
        const std::string mstore = dir + "/marketplace", rstore = dir + "/release",
                          gstore = dir + "/registrar", dstore = dir + "/developer";
        auto key_of = [&](const std::string& st) {
            uint8_t sk[64]; sig::Key k{};
            tr::loader_key(st, sk, k, why);
            mc::crypto_wipe(sk, 64);
            return k;
        };
        const sig::Key mk = key_of(mstore), rk = key_of(rstore), gk = key_of(gstore), dk = key_of(dstore);
        const auto ic = issue(sroot, sig::Role::Intermediate, active.pub, {});
        const auto rc = issue(sroot, sig::Role::Root, R, {});
        auto join = [&](const std::string& st, const std::vector<sig::Certificate>& reg) {
            return tr::install_registration(st, reg, why);
        };
        auto identity_of = [&](const std::string& st) { sig::Key h{}; tr::store_identity(st, h, why); return h; };

        check(identity_of(dstore) == head_of({ { sig::Role::Loader, dk } }),
              "an unregistered store's identity is its key's alone");
        check(!join(mstore, { issue(active, sig::Role::Marketplace, rk, {}), ic, rc })
              && why == "not a registration of this loader's key", "a registration for another loader's key is not installed");
        check(!join(mstore, { issue(active, sig::Role::Marketplace, mk, {}), ic })
              && why == "the chain does not end at a root certificate", "nor one that does not end at a root");
        check(!join(dstore, { issue(active, sig::Role::Loader, dk, {}), ic, rc })
              && why == "not a registration of this loader's key", "the active key certifies no loader: a registrar does");

        check(join(mstore, { issue(active, sig::Role::Marketplace, mk, {}), ic, rc })
              && join(rstore, { issue(active, sig::Role::Release, rk, {}), ic, rc })
              && join(gstore, { issue(active, sig::Role::Registrar, gk, {}), ic, rc }),
              "three loaders' keys sharing one active key are joined, one role each, under any root");
        const sig::Key MI = head_of({ { sig::Role::Root, R }, { sig::Role::Intermediate, active.pub }, { sig::Role::Marketplace, mk } });
        check(identity_of(mstore) == MI, "the marketplace's identity is the fold root -> active -> its key: what the minting server builds with");
        check(identity_of(rstore) != MI && identity_of(gstore) != MI, "each key's is its own");
        const auto M = tr::establish(mstore, true, MI), Rl = tr::establish(rstore, true, MI), G = tr::establish(gstore, true, MI);
        check(M->role == sig::Role::Marketplace && Rl->role == sig::Role::Release && G->role == sig::Role::Registrar
              && M->proven && Rl->proven && G->proven && M->head == MI,
              "each session is its role's, under the identity built in");
        check(!M->attested && !Rl->attested && !G->attested, "none attests: this binary is no build of this identity");
        // The same sessions, as they are once a signed build runs them.
        struct Session { tr::Identity id; uint8_t secret[64]; };
        auto attested = [&](const std::string& st, const sig::Key& head) {
            Session ss; sig::Key k{};
            tr::loader_key(st, ss.secret, k, why);
            ss.id = *tr::establish(st, true, head);
            ss.id.attested = true; ss.id.unattested.clear();
            return ss;
        };

        write_bytes(dir + "/Mod.wasm", tiny_wasm());
        {
            std::vector<sig::Certificate> u;
            const std::string ub = (tr::sign_module(dstore, "Probe", dir + "/Mod.wasm", E, dir + "/u1", why, nullptr, MI) == 0)
                                 ? slurp(dir + "/u1") : std::string();
            check(sig::decode_chain(ub.data(), ub.size(), u) && u.size() == 1,
                  "under a minted identity an unregistered loader signs its local half: what runs local");
        }
        {
            std::vector<sig::Certificate> u;
            const std::string ub = (tr::sign_module(mstore, "Probe", dir + "/Mod.wasm", E, dir + "/u2", why, nullptr, MI) == 0)
                                 ? slurp(dir + "/u2") : std::string();
            check(sig::decode_chain(ub.data(), ub.size(), u) && u.size() == 1 && why.rfind("its local half alone", 0) == 0,
                  "a marketplace whose loader does not attest signs a module with its local half alone, and says so");
        }
        Session Mt = attested(mstore, MI);
        const std::string m1b = sign_as(Mt.id, Mt.secret, "Probe", tiny_wasm(), E, false);
        const auto m1 = wasm_signed(tiny_wasm(), m1b);
        std::vector<sig::Certificate> mc1;
        check(sig::decode_chain(m1b.data(), m1b.size(), mc1) && mc1.size() == 4 && mc1[1].role == sig::Role::Marketplace,
              "a marketplace whose loader attests signs a module with its half");
        check(tr::sign_module(rstore, "Probe", dir + "/Mod.wasm", E, dir + "/x1", why, nullptr, MI) == 2
              && why == "a release key signs loaders, not modules", "a release key, which no one is given now, signs no module");
        check(tr::sign_module(gstore, "Probe", dir + "/Mod.wasm", E, dir + "/x2", why, nullptr, MI) == 2
              && why == "a registrar certifies loaders; it signs no binary", "a registrar signs no binary");

        // Minting: the marketplace signs a loader binary with its half, attested or not.
        write_bytes(dir + "/etcs", tiny_wasm());
        check(!M->attested && (why.clear(), tr::succeed(mstore, "etcs", dir + "/etcs", E, dir + "/mint", why, nullptr, MI)) == 0 && why.empty(),
              "the marketplace mints a loader binary with its half, attested or not: a mint is where a lineage begins");
        const auto minted = wasm_signed(tiny_wasm(), slurp(dir + "/mint"));
        const auto bare = tr::establish(dir + "/none", true, MI);
        auto as_kernel = [&](const std::vector<uint8_t>& b, const tr::Identity& who) {
            return tr::verify("etcs", b.data(), b.size(), E, who, why, true);
        };
        check(tr::zero(bare->pub) && !bare->proven && ok_as("Probe", m1, E, *bare),
              "a download with an empty store takes the marketplace's module: the identity it was built with is enough");
        check(as_kernel(minted, *bare), "and attests a loader binary the marketplace minted");
        tr::Identity other_mint = *bare; other_mint.head = head_of({ { sig::Role::Root, R }, { sig::Role::Intermediate, active.pub },
                                                                    { sig::Role::Marketplace, fresh_key().pub } });
        check(!as_kernel(minted, other_mint) && why == "does not come to this loader's identity",
              "a loader of another marketplace's identity does not");
        Session Rt = attested(rstore, MI);
        const auto rel = wasm_signed(tiny_wasm(), sign_as(Rt.id, Rt.secret, "etcs", tiny_wasm(), E, true));
        check(!as_kernel(rel, *bare) && why == "does not come to this loader's identity",
              "a release key mints nothing: its chain does not come to the marketplace's identity");
        tr::Identity rooted; rooted.head = head_of({ { sig::Role::Root, R } });
        check(!as_kernel(rel, rooted) && why == "a loader binary is taken from this loader's identity, or from its own key",
              "nor under the root's own identity, which it does come to: only a marketplace mints");
        auto next = tiny_wasm(); next.back() = 'x';
        write_bytes(dir + "/next", next);
        check(tr::succeed(mstore, "etcs", dir + "/next", E, dir + "/mk1", why, nullptr, MI) == 0
              && as_kernel(wasm_signed(next, slurp(dir + "/mk1")), *bare),
              "the minting server's own successor is minted the same way");

        // A builder under the minted identity: its own key, unregistered.
        const auto B = tr::establish(dstore, true, MI);
        check(tr::succeed(dstore, "etcs", dir + "/next", E, dir + "/b1", why, nullptr, MI) == 0,
              "a builder under the minted identity passes off its successor with its own key");
        const auto b1 = wasm_signed(next, slurp(dir + "/b1"));
        check(!as_kernel(b1, *B) && why == "passed off by a loader that did not attest",
              "which, passed off by a session that does not attest, does not attest either");
        Session Bm = attested(dstore, MI);
        Bm.id.self = digest_of("etcs", minted);
        const auto bmint = wasm_signed(next, sign_as(Bm.id, Bm.secret, "etcs", next, E, true));
        sig::PassOff po;
        check(pass_of(bmint, po) && po.parent == Bm.id.self && po.attested && po.head == MI && as_kernel(bmint, *B),
              "the build after a mint, passed off by the minted loader, attests on that builder");
        check(!as_kernel(bmint, *bare) && why == "signed by another loader's key, and nothing above it",
              "and nowhere else");
        Session Bs = attested(dstore, MI);
        Bs.id.self = digest_of("etcs", next);
        const auto bself = wasm_signed(next, sign_as(Bs.id, Bs.secret, "etcs", next, E, true));
        check(!as_kernel(bself, *B) && why == "it passed itself off: under a minted identity a lineage begins at a mint",
              "a builder that passes itself off begins nothing under a minted identity");
        check(sign_as(Bs.id, Bs.secret, "etcs", bmint, E, true) == "already",
              "and, signing the build the mint passed off, leaves that pass-off as it is");

        check(tr::issue(gstore, sig::Role::Loader, dk, dir + "/dev.chain", why, MI) == 2
              && why.rfind("only a signed build certifies", 0) == 0,
              "a registrar whose loader does not attest certifies no one");
        {
            Keys gkeys{};
            tr::loader_key(gstore, gkeys.secret, gkeys.pub, why);
            std::vector<sig::Certificate> chain{ issue(gkeys, sig::Role::Loader, dk, {}) };
            chain.insert(chain.end(), G->reg.begin(), G->reg.end());
            const std::string bytes = sig::encode_chain(chain);
            std::ofstream(dir + "/dev.chain", std::ios::binary) << bytes;
            mc::crypto_wipe(gkeys.secret, 64);
        }
        check(tr::issue(gstore, sig::Role::Marketplace, dk, dir + "/no.chain", why, MI) == 2
              && why == "a registrar does not certify a marketplace", "and nothing but a loader");
        check(tr::issue(mstore, sig::Role::Loader, dk, dir + "/no.chain", why, MI) == 2
              && why == "a marketplace does not certify a loader", "a marketplace certifies no one");
        std::vector<sig::Certificate> dreg;
        check(tr::read_certificates(dir + "/dev.chain", dreg) && dreg.size() == 4 && dreg[0].role == sig::Role::Loader
              && dreg[1].role == sig::Role::Registrar && join(dstore, dreg),
              "the developer installs it: its key <- registrar <- active <- root");
        const sig::Key DI = head_of({ { sig::Role::Root, R }, { sig::Role::Intermediate, active.pub },
                                      { sig::Role::Registrar, gk }, { sig::Role::Loader, dk } });
        check(identity_of(dstore) == DI, "and its identity is the fold along it, down to its key");
        const auto D = tr::establish(dstore, true, DI);
        check(D->role == sig::Role::Loader && D->proven && D->reg.size() == 4, "its loader is joined to the half above it");
        write_bytes(dir + "/Dev.wasm", tiny_wasm());
        Session Dt = attested(dstore, DI);
        const std::string d1b = sign_as(Dt.id, Dt.secret, "Probe", tiny_wasm(), E, false);
        check(!d1b.empty(), "its loader, attesting, signs its own build");
        const auto d1 = wasm_signed(tiny_wasm(), d1b);
        write_bytes(dir + "/Dev.wasm", d1);
        check(tr::sign_module(dstore, "Probe", dir + "/Dev.wasm", E, dir + "/d2", why, nullptr, DI) == 3,
              "and a session of it that does not attest leaves what carries its half as it is");
        std::vector<sig::Certificate> dc;
        check(sig::decode_chain(d1b.data(), d1b.size(), dc) && dc.size() == 5 && dc[1].role == sig::Role::Loader
              && dc[2].role == sig::Role::Registrar && dc[4].role == sig::Role::Root,
              "which carries both halves: module <- its key <- registrar <- active <- root");
        bool local = true;
        check(ok_as("Probe", d1, E, *D, nullptr, &local) && !local, "it runs on its own identity's loaders, on the global surface");
        local = true;
        check(ok_as("Probe", d1, E, rooted, nullptr, &local) && !local, "and on the root's: its chain comes to that head");
        check(!ok_as("Probe", d1, E, *bare, &why) && why == "does not come to this loader's identity",
              "but not on a download of the marketplace's: that requires the marketplace at its head");
        local = false;
        check(ok_as("Probe", d1, E, *M, nullptr, &local) && local,
              "and on the minting server itself it meets that server's own half below the head: local");

        // Under an identity, the half goes on only from a signed build.
        auto signs = [&](const tr::Identity& who) {
            std::vector<sig::Certificate> c;
            const std::string chain = sign_as(who, Dt.secret, "Probe", tiny_wasm(), other, false);
            return sig::decode_chain(chain.data(), chain.size(), c) ? c.size() : size_t(0);
        };
        tr::Identity canon = Dt.id; canon.attested = false; canon.unattested = "no signed build passed this loader off";
        check(signs(canon) == 1, "under an identity, a loader that does not attest signs its local half alone");
        canon.attested = true;
        check(signs(canon) == 5, "and one that attests, both halves");

        const auto fresh = tr::establish(dir + "/none", true, tr::zero(A.head) ? MI : A.head);
        tr::Identity unbuilt = *fresh; unbuilt.builder = sig::Key{};
        std::vector<uint8_t> me;
        std::string my_name, w;
        if (A.role == sig::Role::Marketplace && !A.reg.empty() && !tr::zero(A.head))
            check(tr::own_binary(my_name, me) && tr::verify(my_name, me.data(), me.size(), E, unbuilt, w, true),
                  "this binary, which the marketplace that built it minted, attests on an empty store of its identity");
        else
            check(tr::own_binary(my_name, me) && !tr::verify(my_name, me.data(), me.size(), E, unbuilt, w, true),
                  "this binary, which no marketplace minted, attests on an empty store only by the builder compiled into it");

        {
            // A store joined under another root, through that root's attestation of this one.
            const Keys root2 = fresh_key();
            const std::string cstore = dir + "/continued";
            const sig::Key ck = key_of(cstore);
            const auto cross = issue(root2, sig::Role::Root, R, {});
            const auto r2c   = issue(root2, sig::Role::Root, root2.pub, {});
            check(join(cstore, { issue(active, sig::Role::Marketplace, ck, {}), ic, cross, r2c }),
                  "a store joins a half continued by another root's attestation");
            const sig::Key CI = head_of({ { sig::Role::Root, root2.pub }, { sig::Role::Root, R },
                                          { sig::Role::Intermediate, active.pub }, { sig::Role::Marketplace, ck } });
            check(identity_of(cstore) == CI, "its identity folds from the attesting root down");
            uint8_t sk[64]; sig::Key k{};
            tr::loader_key(cstore, sk, k, why);
            tr::Identity C = *tr::establish(cstore, true, CI);
            C.attested = true;
            check(C.reg.size() == 4 && C.role == sig::Role::Marketplace, "its session reads the continued half");
            const auto cm = wasm_signed(tiny_wasm(), sign_as(C, sk, "Probe", tiny_wasm(), E, false));
            mc::crypto_wipe(sk, 64);
            tr::Identity on2; on2.head = CI;
            tr::Identity on1; on1.head = head_of({ { sig::Role::Root, R }, { sig::Role::Intermediate, active.pub },
                                                   { sig::Role::Marketplace, ck } });
            tr::Identity onr2; onr2.head = head_of({ { sig::Role::Root, root2.pub } });
            bool lc = true;
            check(ok_as("Probe", cm, E, on2, nullptr, &lc) && !lc, "what it signs is global on its identity's loaders");
            lc = true;
            check(ok_as("Probe", cm, E, on1, nullptr, &lc) && !lc,
                  "and on the identity folded from its own root: the fold from either root comes to it");
            lc = true;
            check(ok_as("Probe", cm, E, onr2, nullptr, &lc) && !lc, "and on the other root's own");
            check(!ok_as("Probe", m1, E, onr2), "which takes nothing the continuation does not carry");
            ks::erase(cstore, "loader-key", kwhy);
        }
        for (uint8_t* sk : { Mt.secret, Rt.secret, Bm.secret, Bs.secret, Dt.secret }) mc::crypto_wipe(sk, 64);
        for (const std::string& st : { mstore, rstore, gstore, dstore }) ks::erase(st, "loader-key", kwhy);
        check(ks::load(dstore, "loader-key", held, kwhy) == ks::Status::Absent, "(their keys are erased)");
    }

    // -- 4b. the table, on certificates alone --------------------------------------------
    {
        const Keys root = fresh_key(), inter = fresh_key(), market = fresh_key(), stranger = fresh_key(),
                   rel = fresh_key(), reg = fresh_key();
        sig::Layout WL;
        const auto w = tiny_wasm();
        sig::layout(w.data(), w.size(), WL, why);
        sig::Key d{};
        tr::digest("Probe", w.data(), w.size(), WL, d);

        const auto leaf   = issue(market, sig::Role::Module, d, { E });
        const auto mcert  = issue(inter, sig::Role::Marketplace, market.pub, { E });
        const auto icert  = issue(root, sig::Role::Intermediate, inter.pub, {});
        const auto rcert  = issue(root, sig::Role::Root, root.pub, {});
        tr::Identity ace; ace.head = head_of({ { sig::Role::Root, root.pub } }); ace.proven = true;
        tr::Identity site; site.head = head_of({ { sig::Role::Root, root.pub }, { sig::Role::Intermediate, inter.pub },
                                                 { sig::Role::Marketplace, market.pub } });
        auto as = [&](std::vector<sig::Certificate> chain) { return wasm_signed(w, sig::encode_chain(chain)); };

        check(ok_as("Probe", as({ leaf, mcert, icert, rcert }), E, ace), "module <- marketplace <- intermediate <- root, on the root's identity");
        check(ok_as("Probe", as({ leaf, mcert, icert, rcert }), E, site), "and on the marketplace's");
        check(ok_as("Probe", as({ leaf, mcert, icert }), E, ace) && ok_as("Probe", as({ leaf, mcert, icert }), E, site),
              "the root's own certificate may be left off");
        check(!ok_as("Probe", as({ leaf, issue(root, sig::Role::Marketplace, market.pub, {}) }), E, ace)
              && !ok_as("Probe", as({ leaf, issue(root, sig::Role::Marketplace, market.pub, {}), rcert }), E, ace, &why)
              && why == "chain is broken",
              "but the root certifies only the active key, with its certificate or without");
        check(!ok_as("Probe", as({ leaf, mcert, icert, rcert }), E, tr::Identity{}, &why)
              && why == "signed under a root, and no identity is built into this loader",
              "a loader with no identity built in takes nothing from a half");
        check(!ok_as("Probe", as({ leaf, mcert, icert, rcert }), other, ace), "not in another epoch");

        const auto lcert = issue(inter, sig::Role::Loader, market.pub, {});
        check(!ok_as("Probe", as({ leaf, lcert, icert, rcert }), E, ace, &why) && why == "chain is broken",
              "a loader the active key certified is no loader: only a registrar certifies one");
        const auto gcert = issue(inter, sig::Role::Registrar, reg.pub, {});
        const auto lreg  = issue(reg, sig::Role::Loader, market.pub, {});
        check(ok_as("Probe", as({ leaf, lreg, gcert, icert, rcert }), E, ace),
              "a loader the registrar certified signs for any loader of the root's identity");
        check(!ok_as("Probe", as({ leaf, lreg, gcert, icert, rcert }), E, site, &why) && why == "does not come to this loader's identity",
              "and for none of the marketplace's: its chain does not come to that head");
        check(!ok_as("Probe", as({ leaf, lreg, gcert }), E, ace, &why) && why == "does not come to this loader's identity",
              "a chain that stops short of anything this loader holds meets nothing");
        tr::Identity both = ace; both.pub = market.pub; both.proven = true;
        bool local = true;
        check(ok_as("Probe", as({ leaf }), E, both, nullptr, &local) && local,
              "and that loader's key alone is its own: local");
        check(!ok_as("Probe", as({ issue(reg, sig::Role::Module, d, { E }), gcert, icert, rcert }), E, ace, &why)
              && why == "a key certified as registrar signs no binary", "a registrar's key signs no binary");
        check(!ok_as("Probe", as({ leaf, issue(root, sig::Role::Intermediate, market.pub, {}), rcert }), E, ace, &why)
              && why == "a key certified as intermediate signs no binary", "nor does the active key");

        const auto relcert = issue(inter, sig::Role::Release, rel.pub, {});
        const auto rleaf   = issue(rel, sig::Role::Module, d, { E });
        const auto rb = as({ rleaf, relcert, icert, rcert });
        std::string kw;
        check(!ok_as("Probe", rb, E, ace, &why) && why == "a release key signs loaders, not modules"
              && !tr::verify("Probe", rb.data(), rb.size(), E, ace, kw, true)
              && kw == "a loader binary is taken from this loader's identity, or from its own key",
              "a release key's signature is no module's, and mints no loader binary");
        const auto mb = as({ leaf, mcert, icert, rcert });
        check(tr::verify("Probe", mb.data(), mb.size(), E, site, kw, true) && tr::verify("Probe", mb.data(), mb.size(), E, ace, kw, true),
              "the marketplace's is a module's and a loader binary's alike: a mint");

        const auto mold = issue(inter, sig::Role::Marketplace, market.pub, { other });
        check(!ok_as("Probe", as({ leaf, mold, icert, rcert }), E, ace, &why) && why == "witness not valid in this epoch",
              "a marketplace certificate outside this epoch is refused");
        const auto ibad = issue(stranger, sig::Role::Intermediate, inter.pub, {});
        check(!ok_as("Probe", as({ leaf, mcert, ibad }), E, ace), "an intermediate the root did not certify is refused");
        check(!ok_as("Probe", as({ leaf, mcert, rcert, icert }), E, ace), "a root that is not last is refused");
        check(!ok_as("Probe", as({ issue(root, sig::Role::Module, d, { E }) }), E, ace),
              "a module the root's key signed directly is refused: the root certifies only the active key");
        // Roots attest each other: a second root certifies the first, and a chain
        // carrying that continuation comes to an identity folded from either.
        const Keys root2 = fresh_key();
        const auto cross  = issue(root2, sig::Role::Root, root.pub, {});
        const auto r2cert = issue(root2, sig::Role::Root, root2.pub, {});
        tr::Identity ace2; ace2.head = head_of({ { sig::Role::Root, root2.pub } }); ace2.proven = true;
        const auto carried = as({ leaf, mcert, icert, cross, r2cert });
        local = true;
        check(ok_as("Probe", carried, E, ace2, nullptr, &local) && !local,
              "a chain continued by another root's attestation comes to that root's identity: global on its loaders");
        local = true;
        check(ok_as("Probe", carried, E, ace, nullptr, &local) && !local && ok_as("Probe", carried, E, site),
              "and still to its own root's, and its marketplace's, folded from below the continuation");
        tr::Identity site2; site2.head = head_of({ { sig::Role::Root, root2.pub }, { sig::Role::Root, root.pub },
                                                   { sig::Role::Intermediate, inter.pub }, { sig::Role::Marketplace, market.pub } });
        check(ok_as("Probe", carried, E, site2) && !ok_as("Probe", mb, E, site2),
              "the marketplace's identity folded through the continuation takes it, and not the chain without it");
        check(ok_as("Probe", as({ leaf, mcert, icert, cross }), E, ace2), "the attesting root's own certificate may be left off");
        check(!ok_as("Probe", as({ leaf, mcert, icert, rcert }), E, ace2, &why) && why == "does not come to this loader's identity",
              "without the continuation the other root's loaders take nothing");
        tr::Identity third; third.head = head_of({ { sig::Role::Root, fresh_key().pub } });
        check(!ok_as("Probe", carried, E, third), "nor does a loader of a root that attested neither");
        check(!ok_as("Probe", as({ leaf, mcert, icert, rcert, cross, r2cert }), E, ace2, &why)
              && why == "a root's own certificate is not the last", "a root's own certificate is still the last");
        check(!ok_as("Probe", as({ leaf, mcert, icert, issue(inter, sig::Role::Root, root.pub, {}) }), E, ace2),
              "a root certificate signed by a key that is no root of this loader's attests nothing here");

        tr::Identity reg_loader = ace; reg_loader.reg = { lreg, gcert, icert, rcert }; reg_loader.pub = market.pub;
        check(ok_as("Probe", as({ issue(reg, sig::Role::Loader, stranger.pub, {}), }), E, reg_loader, &why) == false,
              "a certificate is no module");
        const auto sleaf = issue(stranger, sig::Role::Module, d, { E });
        local = false;
        check(ok_as("Probe", as({ sleaf, issue(reg, sig::Role::Loader, stranger.pub, {}) }), E, reg_loader, nullptr, &local) && local,
              "a chain may end on any key of this loader's own chain -- its registrar's here -- and runs local");
    }

    // -- 5. sealed --------------------------------------------------------------------
    std::cout << "-- 5. sealed --\n";
    {
        const std::string copy = dir + "/ChessProvider.so";
        write_bytes(copy, chess);
        tr::Opened o;
        check(tr::open_verified("ChessProvider", copy, o, why), "a copy of a signed module opens verified");
        check(o.path.rfind("/proc/self/fd/", 0) == 0, "as a descriptor, not as the file");
        const char x = 'x';
        check(::pwrite(o.fd, &x, 1, 0) < 0, "the memfd refuses a write");
        auto changed = chess; changed[100] ^= 0xff;
        write_bytes(copy, changed);
        check(bytes_of(o.path) == chess, "the file changing on disk does not change what is opened");
        tr::Opened o2;
        check(!tr::open_verified("ChessProvider", copy, o2, why), "the changed file itself is refused");
    }

    // -- 6. the loader refuses --------------------------------------------------------
    std::cout << "-- 6. attach --\n";
    {
        write_bytes(bin + "SigProbe.so", chess);
        const std::string strip = "objcopy --remove-section .etcs.sig '" + bin + "ChessProvider.so' '"
                                + bin + "SigProbeU.so' 2>/dev/null";
        const bool stripped = std::system(strip.c_str()) == 0;
        check(!run("spawn SigProbe::ChessBoard p\n") && !s.names.count("p"),
              "a signed module copied to another name does not attach");
        check(stripped && !run("spawn SigProbeU::ChessBoard q\n") && !s.names.count("q"),
              "an unsigned module does not attach");
        std::remove((bin + "SigProbe.so").c_str());
        std::remove((bin + "SigProbeU.so").c_str());
        check(run("spawn ChessProvider::ChessBoard board\n") && s.names.count("board"),
              "the signed module under its own name does");
        // Each load is its own /proc/self/fd/N for the life of the process: a
        // descriptor closed after one load and reused by the next was answered
        // by the dynamic linker with the module already loaded under that name.
        check(run("spawn CryptProvider::Keypair key\n") && s.names.count("key"),
              "and a second module after it is that module, not the first again");
        run("board.Delete()\nkey.Delete()\n");
    }

    // -- 7. the gate is a chain ---------------------------------------------------------
    std::cout << "-- 7. gate --\n";
    {
        Veto* veto = tr::gate_entity()->addTag<Veto>();
        check(veto && veto->hasTag(ETCS::Buffer("Wrapper")), "a Wrapper leaf attached to the gate's entity");
        veto->refusing = true;
        check(!run("spawn DatabaseProvider::LocalDatabase db\n") && !s.names.count("db")
              && veto->seen > 0 && veto->last == "DatabaseProvider",
              "it is handed the load before the loader's own stages, and its refusal is a failed attach");
        veto->refusing = false;
        const int before = veto->seen;
        check(run("spawn DatabaseProvider::LocalDatabase db\n") && s.names.count("db") && veto->seen > before,
              "passing, the same module goes on through Signature and Own, and loads");
        run("db.Delete()\n");
        veto->getOwningArena().deleteEntity(veto, true);
    }

    // -- 8. the site: its list, what it has moved past, and what is fetched -----------------
    std::cout << "-- 8. site --\n";
    {
        namespace ks = ETCS::keystore;
        const Keys sroot = fresh_key(), active = fresh_key();
        const sig::Key R = sroot.pub;
        const std::string ms = dir + "/s-market", rs = dir + "/s-release", ds = dir + "/s-dev",
                          dist = dir + "/dist", here = dir + "/here/";
        ::mkdir(dist.c_str(), 0700);
        ::mkdir(here.c_str(), 0700);
        auto key_of = [&](const std::string& st) {
            uint8_t sk[64]; sig::Key k{};
            tr::loader_key(st, sk, k, why);
            mc::crypto_wipe(sk, 64);
            return k;
        };
        const sig::Key mk = key_of(ms), rk = key_of(rs), dk = key_of(ds);
        const auto ic = issue(sroot, sig::Role::Intermediate, active.pub, {});
        const auto rc = issue(sroot, sig::Role::Root, R, {});
        tr::install_registration(ms, { issue(active, sig::Role::Marketplace, mk, {}), ic, rc }, why);
        tr::install_registration(rs, { issue(active, sig::Role::Release, rk, {}), ic, rc }, why);
        sig::Key MI{};
        check(tr::store_identity(ms, MI, why) && MI == head_of({ { sig::Role::Root, R }, { sig::Role::Intermediate, active.pub },
                                                                  { sig::Role::Marketplace, mk } }),
              "the minting server's identity: what its downloads are built with");

        // Two builds of one module, a minted loader, and a developer's own build.
        auto variant = [](uint8_t v) { auto w = tiny_wasm(); w.back() = v; return w; };
        auto signed_by = [&](const std::string& st, const char* name, const std::vector<uint8_t>& w, bool kernel,
                             const sig::Key& head) {
            write_bytes(dir + "/unsigned", w);
            const std::string out = dir + "/chain";
            std::remove(out.c_str());
            if (kernel) tr::succeed(st, name, dir + "/unsigned", E, out, why, nullptr, head);
            else if (tr::zero(head)) tr::sign_module(st, name, dir + "/unsigned", E, out, why, nullptr, head);
            else                                    // as a signed build of this identity would
            {
                uint8_t sk[64]; sig::Key k{};
                tr::loader_key(st, sk, k, why);
                tr::Identity who = *tr::establish(st, true, head);
                who.attested = true;
                const std::string chain = sign_as(who, sk, name, w, E, false);
                mc::crypto_wipe(sk, 64);
                return wasm_signed(w, chain);
            }
            return wasm_signed(w, slurp(out));
        };
        const auto probe_new = signed_by(ms, "Probe", variant('1'), false, MI);
        const auto probe_old = signed_by(ms, "Probe", variant('2'), false, MI);
        const auto minted    = signed_by(ms, "etcs", variant('3'), true, MI);
        const auto mine      = signed_by(ds, "Mine", variant('4'), false, sig::Key{});   // a developer's, experimental
        write_bytes(dist + "/Probe.wasm", probe_new);
        write_bytes(dist + "/etcs", minted);
        write_bytes(dist + "/Mine.wasm", mine);

        std::string list, w2;
        std::vector<std::string> left;
        check(!tr::publish(rs, dist, E, list, why, nullptr, MI) && why == "only the marketplace publishes, and this loader is not registered as it",
              "only the marketplace publishes");
        check(tr::publish(ms, dist, E, list, why, &left, MI) && left.size() == 1 && left[0].rfind("Mine.wasm", 0) == 0,
              "the marketplace publishes what a download would take, and leaves off a developer's own build");
        tr::Dist d;
        check(tr::dist_read(list, d, why, MI) && d.epoch == E && d.entries.size() == 2
              && d.entries[0].name == "Probe" && d.entries[1].name == "etcs" && d.entries[1].file == "etcs",
              "the list reads back: its epoch, a module and a minted loader");
        std::string forged = list;
        forged[forged.find("Probe.wasm ") + 11] ^= 0x01;
        check(!tr::dist_read(forged, d, w2, MI) && w2 == "the list's signature does not verify", "a list changed after signing is refused");
        check(!tr::dist_read(list, d, w2, fresh_key().pub) && w2 == "the list is not signed by the marketplace of this loader's identity",
              "a list of another identity is refused");
        check(!tr::dist_read(list, d, w2, head_of({ { sig::Role::Root, R } })),
              "even the root's: a list is believed of the marketplace whose identity this loader is");
        check(!tr::dist_read(list, d, w2, sig::Key{}), "with no identity built in, no list is believed");

        // The marketplace key retired: an identity with another key at the third link.
        {
            const std::string ms2 = dir + "/s-market2";
            const sig::Key mk2 = key_of(ms2);
            tr::install_registration(ms2, { issue(active, sig::Role::Marketplace, mk2, {}), ic, rc }, why);
            sig::Key MI2{};
            tr::store_identity(ms2, MI2, why);
            tr::Identity healed; healed.head = MI2;
            check(MI2 != MI && !tr::dist_read(list, d, w2, MI2)
                  && w2 == "the list is not signed by the marketplace of this loader's identity",
                  "a loader built with the next marketplace key at the third link refuses the retired key's list");
            check(!ok_as("Probe", probe_new, E, healed, &w2) && w2 == "does not come to this loader's identity"
                  && !tr::verify("etcs", minted.data(), minted.size(), E, healed, w2, true),
                  "and its modules, and the loader it minted: the identity pins the key, not only the root");
            ks::erase(ms2, "loader-key", kwhy);
        }

        // The order lists come in: the ABI's version, never older than what was taken.
        check(tr::dist_read(list, d, why, MI) && d.kernel == tr::kernel_version() && !d.kernel.empty(),
              "the list names the ABI's version it was published under");
        // A list as `store`'s marketplace would sign it under `kernel`.
        auto relist_of = [&](const std::string& base, const std::string& store, const std::string& kernel) {
            const size_t eol = base.find('\n'), sig_at = base.rfind("\n" + std::string(sig::kDistSig));
            const std::string header = base.substr(0, eol), keep = header.substr(0, header.find(" kernel="));
            const std::string body = (kernel.empty() ? keep : keep + " kernel=" + kernel) + base.substr(eol, sig_at + 1 - eol);
            uint8_t sk[64]; sig::Key k{};
            std::vector<sig::Certificate> reg;
            tr::identity(store, sk, k, reg, why);
            uint8_t sg[sig::kSigBytes];
            const std::string m = "ETCSDST1" + body;
            mc::crypto_ed25519_sign(sg, sk, reinterpret_cast<const uint8_t*>(m.data()), m.size());
            mc::crypto_wipe(sk, 64);
            const std::string chain = sig::encode_chain(reg);
            return body + sig::kDistSig + tr::hex(reinterpret_cast<const uint8_t*>(chain.data()), chain.size())
                 + " " + tr::hex(sg, sizeof sg) + "\n";
        };
        auto relist = [&](const std::string& kernel) { return relist_of(list, ms, kernel); };
        std::vector<uint32_t> up = tr::kernel_version(), down = tr::kernel_version();
        up.push_back(1);
        down.back() == 0 ? (void)down.pop_back() : (void)--down.back();
        const std::string newer = relist(tr::kernel_text(up)), older = relist(tr::kernel_text(down));
        check(tr::dist_read(newer, d, w2, MI) && d.kernel == up, "a list of a newer ABI is taken");
        check(!tr::dist_read(older, d, w2, MI) && w2.find("a rollback") != std::string::npos,
              "a list older than the loader itself is refused: a rollback");
        check(!tr::dist_read(relist(""), d, w2, MI) && w2 == "the list names no kernel version",
              "and a list that names no version is no list");
        {
            const std::string seen = dir + "/s-seen", shere = dir + "/seen-here/";
            ::mkdir(seen.c_str(), 0700);
            ::mkdir(shere.c_str(), 0700);
            tr::Identity dl; dl.head = MI;
            std::string rep2, put2;
            check(tr::survey(list, rep2, w2, dl, E, shere, shere + "etcs", seen) && ::access((seen + "/kernel.seen").c_str(), F_OK) != 0,
                  "a list of the loader's own version is taken, and moves nothing");
            check(tr::survey(newer, rep2, w2, dl, E, shere, shere + "etcs", seen) && slurp(seen + "/kernel.seen") == tr::kernel_text(up) + "\n",
                  "a newer one is taken, and the store keeps its version");
            const std::string nb(probe_new.begin(), probe_new.end());
            check(!tr::survey(list, rep2, w2, dl, E, shere, shere + "etcs", seen) && w2.find("a rollback") != std::string::npos
                  && !tr::replace(list, "Probe", nb, put2, w2, dl, shere, shere + "etcs", seen),
                  "after which the list from before is refused, surveyed or fetched from: it cannot be replayed");
        }

        // RE-SYNC: the marketplace key retired. A sibling marketplace under the
        // same active key mints the successor, a release key under that same
        // active key countersigns it, and a loader of the old identity takes
        // it -- told its identity's parent, since a fold is followed only down.
        {
            sig::Key head{}, P{};
            check(tr::store_identity(ms, head, why, &P) && head == MI
                  && P == head_of({ { sig::Role::Root, R }, { sig::Role::Intermediate, active.pub } }),
                  "the minting server's parent: the fold root -> active, one step short of its key");
            sig::Key dh{}, dp{};
            check(tr::store_identity(ds, dh, why, &dp) && tr::zero(dp), "an unregistered store's identity has no parent");

            const std::string ms2 = dir + "/r-market2", ms3 = dir + "/r-market3", rs2 = dir + "/r-release2",
                              st = dir + "/r-store", rhere = dir + "/r-here/";
            ::mkdir(st.c_str(), 0700);
            ::mkdir(rhere.c_str(), 0700);
            const Keys active2 = fresh_key();
            const auto ic2 = issue(sroot, sig::Role::Intermediate, active2.pub, {});
            tr::install_registration(ms2, { issue(active, sig::Role::Marketplace, key_of(ms2), {}), ic, rc }, why);
            tr::install_registration(ms3, { issue(active2, sig::Role::Marketplace, key_of(ms3), {}), ic2, rc }, why);
            tr::install_registration(rs2, { issue(active2, sig::Role::Release, key_of(rs2), {}), ic2, rc }, why);
            sig::Key MI2{}, MI3{};
            tr::store_identity(ms2, MI2, why);
            tr::store_identity(ms3, MI3, why);

            // A successor minted by `market`, countersigned by `release` (or not).
            auto mint_as = [&](const std::string& market, const sig::Key& mhead, const std::vector<uint8_t>& w,
                               const std::string& release) {
                uint8_t sk[64]; sig::Key k{};
                tr::loader_key(market, sk, k, why);
                tr::Identity who = *tr::establish(market, true, mhead);
                who.attested = true;
                const auto minted_bytes = wasm_signed(w, sign_as(who, sk, "etcs", w, E, true));
                mc::crypto_wipe(sk, 64);
                if (release.empty()) return minted_bytes;
                write_bytes(dir + "/r-unsigned", minted_bytes);
                if (tr::countersign(release, "etcs", dir + "/r-unsigned", E, dir + "/r-section", why) != 0) return std::vector<uint8_t>();
                return wasm_signed(w, slurp(dir + "/r-section"));
            };
            const auto succ = variant('6');
            const auto bare_succ = mint_as(ms2, MI2, succ, ""), counted = mint_as(ms2, MI2, succ, rs);
            tr::Identity old; old.head = MI; old.head_parent = P;
            old.self = digest_of("etcs", variant('7'));
            std::string kw;
            auto as_old = [&](const std::vector<uint8_t>& b, const tr::Identity& who) {
                return tr::verify("etcs", b.data(), b.size(), E, who, kw, true);
            };
            check(!counted.empty() && !as_old(bare_succ, old)
                  && kw == "a successor under another marketplace of this identity's parent needs a release key's countersignature",
                  "a successor minted by a sibling marketplace, without a release key's countersignature, is refused");
            check(as_old(counted, old), "countersigned by a release key under the same active key, it bridges: two keys, neither the retired one");
            tr::Identity orphan = old; orphan.head_parent = sig::Key{};
            check(!as_old(counted, orphan) && kw == "does not come to this loader's identity",
                  "a loader that was not told its identity's parent cannot recognise it");
            check(!ok_as("etcs", counted, E, old, &kw), "and the same bytes as a module are not taken: the bridge is a loader binary's");
            check(!as_old(mint_as(ms2, MI2, succ, rs2), old)
                  && kw == "the countersignature is not a release key's under the same active key",
                  "a release key under another active key countersigns nothing here");
            check(!as_old(mint_as(ms3, MI3, succ, rs), old) && kw == "does not come to this loader's identity",
                  "a marketplace another active key certified is no sibling");
            auto torn = counted; torn[torn.size() - 1] ^= 0x01;
            check(!as_old(torn, old), "an altered countersignature is refused");

            // The sibling's list, at a newer kernel.
            const std::string dist2 = dir + "/r-dist";
            ::mkdir(dist2.c_str(), 0700);
            write_bytes(dist2 + "/etcs", counted);
            write_bytes(dist2 + "/Probe.wasm", signed_by(ms2, "Probe", variant('8'), false, MI2));
            std::string list2;
            check(tr::publish(ms2, dist2, E, list2, why, nullptr, MI2), "the sibling publishes its list, the countersigned successor in it");
            const std::string list2up = relist_of(list2, ms2, tr::kernel_text(up));
            check(tr::dist_read(list2up, d, w2, MI, tr::kernel_version(), P) && d.sibling && d.head == MI2,
                  "a loader of the old identity, told its parent, reads the sibling's list as the identity it moves to");
            check(!tr::dist_read(list2, d, w2, MI, tr::kernel_version(), P) && w2.find("only at a kernel newer") != std::string::npos,
                  "only at a kernel newer than its own");
            check(!tr::dist_read(list2up, d, w2, MI), "and not without its parent");

            const std::string self = rhere + "etcs";
            write_bytes(self, variant('7'));
            std::string rep3, put3;
            check(tr::survey(list2up, rep3, w2, old, E, rhere, self, st) && rep3.find("resync etcs etcs\n") != std::string::npos,
                  "surveyed, the sibling's list names one thing to do: re-sync to the successor");
            const std::string pb(probe_new.begin(), probe_new.end()), cb(counted.begin(), counted.end());
            check(!tr::replace(list2up, "Probe", pb, put3, w2, old, rhere, self, st)
                  && w2 == "from a list of the identity this loader moves to, it takes only its successor",
                  "and nothing else of it");
            check(tr::replace(list2up, "etcs", cb, put3, w2, old, rhere, self, st) && bytes_of(self) == counted
                  && tr::head_retired(st, MI) && slurp(st + "/kernel.seen") == tr::kernel_text(up) + "\n",
                  "the successor is put in place; the old identity is retired in the store, and the newer kernel kept");
            check(tr::resync_state(st, old, self) == tr::Resync::Placed, "the re-sync waits for the restart");
            check(!tr::survey(relist(tr::kernel_text(up)), rep3, w2, old, E, rhere, self, st)
                  && w2.rfind("this loader's identity was retired", 0) == 0,
                  "the retired marketplace's own list is no longer taken, whatever kernel it claims");
            check(tr::survey(list2up, rep3, w2, old, E, rhere, self, st) && rep3.find("re-sync placed: restart\n") != std::string::npos
                  && rep3.find("resync etcs") == std::string::npos,
                  "seen again before the restart, it asks for the restart, not another fetch");
            write_bytes(self, variant('7'));
            check(tr::survey(list2up, rep3, w2, old, E, rhere, self, st) && rep3.find("reinstall from the site") != std::string::npos
                  && rep3.find("resync etcs") == std::string::npos,
                  "a successor that never took is not fetched again: no loop, a reinstall");
            tr::Identity next = old; next.self = digest_of("etcs", counted);
            check(tr::resync_state(st, next, self) == tr::Resync::Done, "the successor, running, sees the re-sync done");
            for (const std::string& k : { ms2, ms3, rs2 }) ks::erase(k, "loader-key", kwhy);
        }

        tr::Identity download; download.head = MI;
        tr::Identity developer = download; developer.pub = dk; developer.proven = true;
        const std::string self = here + "etcs";
        std::string report;
        check(tr::survey(list, report, why, download, E, here, self)
              && report.find("Probe not here\n") != std::string::npos && report.find("fetch") == std::string::npos,
              "what is not here is not fetched");
        write_bytes(here + "Probe.wasm", probe_old);
        write_bytes(self, minted);
        check(tr::survey(list, report, why, download, E, here, self)
              && report.find("Probe differs\nfetch Probe Probe.wasm\n") != std::string::npos
              && report.find("etcs current\n") != std::string::npos,
              "an older build of a module is fetched; the minted loader that is here is current");
        check(tr::survey(list, report, why, download, other, here, self)
              && report.find("this loader is in " + tr::hex(other.data(), 32)) != std::string::npos
              && report.find("Probe behind\n") != std::string::npos,
              "a site in another epoch is said to be, and what is not current is behind");
        write_bytes(here + "Probe.wasm", signed_by(ds, "Probe", variant('5'), false, sig::Key{}));
        check(tr::survey(list, report, why, developer, E, here, self)
              && report.find("Probe yours, not fetched\n") != std::string::npos,
              "a module this loader's own key signed is the developer's, and is not fetched over");

        std::string put;
        write_bytes(here + "Probe.wasm", probe_old);
        const std::string old_bytes(probe_old.begin(), probe_old.end()), new_bytes(probe_new.begin(), probe_new.end());
        check(!tr::replace(list, "Probe", old_bytes, put, why, download, here, self) && why == "these are not the bytes the list names",
              "bytes the list does not name are not put in place");
        check(!tr::replace(list, "Nothing", new_bytes, put, why, download, here, self) && why == "the list has no 'Nothing'",
              "nor a name it does not list");
        check(tr::replace(list, "Probe", new_bytes, put, why, download, here, self) && put == here + "Probe.wasm"
              && bytes_of(here + "Probe.wasm") == probe_new, "the listed bytes are put where the loader reads them");
        check(tr::survey(list, report, why, download, E, here, self) && report.find("Probe current\n") != std::string::npos,
              "and are then current");
        const std::string rel_bytes(minted.begin(), minted.end());
        std::remove(self.c_str());
        check(tr::replace(list, "etcs", rel_bytes, put, why, download, here, self) && bytes_of(self) == minted,
              "the minted loader is put in place of the loader's own binary");
        tr::Identity wrong = download; wrong.head = fresh_key().pub;
        check(!tr::replace(list, "Probe", new_bytes, put, why, wrong, here, self),
              "a loader of another identity takes nothing from this list");

        // The same questions, asked of this loader through its ordering thread.
        sig::Key reg{}; reg[0] = 7;
        ETCS::TrustEvent q(ETCS::TrustEvent::Op::Request);
        q.a = tr::hex(reg.data(), 32);
        sig::Key asked{};
        uint8_t qs[64];
        const bool answered = q() && q.out.size() == 64 + 1 + 128 && tr::unhex(q.out.substr(0, 64), asked.data(), 32)
                              && tr::unhex(q.out.substr(65), qs, 64);
        const std::string stmt = sig::registration_statement(asked, reg);
        check(answered && asked == A.pub
              && mc::crypto_ed25519_check(qs, asked.data(), reinterpret_cast<const uint8_t*>(stmt.data()), stmt.size()) == 0,
              "a module's ask (TrustEvent) is signed by this loader's key, in the registration's form, on the ordering thread");
        ETCS::TrustEvent sv(ETCS::TrustEvent::Op::Survey);
        sv.a = list;
        check(!sv() && (sv.out.rfind("no identity is built into", 0) == 0 || sv.out == "the list is not signed by the marketplace of this loader's identity"),
              "and a list of an identity this loader was not built with is refused there too");
        ETCS::TrustEvent bogus(ETCS::TrustEvent::Op::Install);
        bogus.a = "not a chain";
        check(!bogus() && bogus.out == "not a chain of certificates", "an install of something that is not a chain is refused");

        for (const std::string& st : { ms, rs, ds }) ks::erase(st, "loader-key", kwhy);
    }

    // -- 9. witnesses ------------------------------------------------------------------
    std::cout << "-- 9. witnesses --\n";
    {
        std::vector<uint8_t> me;
        std::string my_name;
        tr::own_binary(my_name, me);
        const std::string a = dir + "/wa", b = dir + "/wb", c = dir + "/wc";
        for (const std::string& d : { a, b, c })
        {
            ::mkdir(d.c_str(), 0700);
            write_bytes(d + "/etcs", me);
            write_bytes(d + "/Probe.wasm", tiny_wasm());
            write_bytes(d + "/notes.txt", { 'h', 'i' });
        }
        auto other_wasm = tiny_wasm(); other_wasm.back() = 'x';
        write_bytes(c + "/Probe.wasm", other_wasm);
        std::string report, w1, w2;
        check(!tr::witness_write(a, c, {}, dir + "/no.txt", report, why) && why.find("came out differently") != std::string::npos
              && report.find("Probe.wasm differs") != std::string::npos,
              "two builds that differ are not witnessed, and the binary that differs is named");
        write_bytes(c + "/etcs", tiny_wasm());
        check(!tr::witness_write(c, c, {}, dir + "/no.txt", report, why) && why.rfind("this loader is not the build", 0) == 0,
              "a loader witnesses only a build it is part of");
        check(tr::witness_write(a, b, { { "commit", "abc" }, { "path", "/x" } }, dir + "/w1.txt", report, why),
              "two builds that agree are witnessed by the loader among them");
        check(tr::witness_write(a, b, { { "commit", "abc" }, { "path", "/x" } }, dir + "/w2.txt", report, why), "(a second witness)");
        w1 = slurp(dir + "/w1.txt"); w2 = slurp(dir + "/w2.txt");
        tr::Witness ws;
        check(tr::witness_read(w1, ws, why) && ws.files.size() == 2 && ws.files.count("etcs") && ws.files.count("Probe")
              && ws.epoch == E && ws.arch == tr::build_arch() && ws.about["commit"] == "abc",
              "the statement names each binary by digest, the epoch, the arch and what it was built from; not what is no binary");
        std::string r;
        check(tr::consensus({ w1, w2 }, { "w1", "w2" }, 2, "", r) && r.find(" at /x: 2 witnesses agree") != std::string::npos,
              "two witnesses of one build, at one root, agree");
        check(!tr::consensus({ w1, w2 }, { "w1", "w2" }, 3, "", r) && r.find("fewer than 3") != std::string::npos,
              "but not to a quorum of three");
        check(!tr::consensus({ w1, w1 }, { "w1", "w1'" }, 1, "", r) && r.find("are one witness") != std::string::npos,
              "a statement twice is one witness");
        std::string forged = w1;
        forged[forged.find("Probe ") + 30] ^= 0x01;
        check(!tr::consensus({ forged, w2 }, { "forged", "w2" }, 2, "", r) && r.find("does not verify") != std::string::npos,
              "a statement altered after signing is refused");
        write_bytes(c + "/etcs", me);
        check(tr::witness_write(c, c, { { "commit", "abc" }, { "path", "/z" } }, dir + "/w3.txt", report, why), "(a witness at another root)");
        const std::string w3 = slurp(dir + "/w3.txt");
        check(!tr::consensus({ w1, w2, w3 }, { "w1", "w2", "w3" }, 2, "", r) && r.find(" at /x: 2 witnesses agree") != std::string::npos
              && r.find(" at /z: 1 witness, fewer than 2") != std::string::npos,
              "a witness at another root is a group of its own, held to the quorum on its own");
        check(tr::consensus({ w1, w2, w3 }, { "w1", "w2", "w3" }, 1, "", r), "and each root with its quorum is its own consensus");
        check(tr::witness_write(c, c, { { "commit", "abc" }, { "path", "/x" }, { "cc", "other" } }, dir + "/w4.txt", report, why),
              "(a witness of another build at the first root)");
        check(!tr::consensus({ w1, slurp(dir + "/w4.txt") }, { "w1", "w4" }, 2, "", r)
              && r.find("Probe differs") != std::string::npos && r.find("they differ in cc: - / other") != std::string::npos,
              "witnesses at one root that disagree are no consensus, and what their accounts differ in is named");
        check(tr::consensus({ w1, w2 }, { "w1", "w2" }, 2, a, r) && r.find("check: Probe.wasm is the consensus build at /x") != std::string::npos,
              "a directory of the agreed binaries checks against the consensus, and says whose root");
        check(!tr::consensus({ w1, w2 }, { "w1", "w2" }, 2, c, r) && r.find("check: Probe.wasm is not the consensus build") != std::string::npos,
              "and one of other binaries does not");
    }

    if (std::system(("rm -rf '" + dir + "'").c_str()) != 0) std::printf("  (could not remove %s)\n", dir.c_str());
    std::cout << (g_fail ? "FAILED: " : "PASSED") << (g_fail ? std::to_string(g_fail) : "") << "\n";
    return g_fail ? 1 : 0;
}
