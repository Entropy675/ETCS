// ModuleSignatureTesterLoader.cc
//
// A MODULE LOADS ONLY AS THE BYTES SOMEONE THIS LOADER TRUSTS SIGNED, UNDER
// ITS NAME, IN THIS EPOCH. What is held to account (core/Signature.h,
// core/Trust.h):
//
//   1. EVERY MODULE IN bin/ IS SIGNED for this loader's key and epoch -- ace
//      signs as it builds.
//   2. WHAT THE DIGEST COVERS. Not the chain, not the section-header fields
//      adding a section moves; any mapped byte. A binary renamed, or carried
//      to an epoch it was not signed in, is refused.
//   3. IDENTITY, end to end on wasm and PE: the key is made once in the OS
//      keystore and never replaced or written to a file; a temporary root
//      registers it; a second sign is a no-op, a new epoch is carried; a
//      second store is a second loader whose modules this one refuses; a leaf
//      alone, a pub file that is not the key's, a second chain are refused.
//      And with THE ROOT BUILT IN: nothing registers itself, the key is still
//      made so that it can be registered, only a chain ending at that root is
//      installed, and a loader holding nothing but the root takes what the
//      marketplace signed -- its own binary first.
//   4. THE ACE CHAIN: marketplace -> intermediate -> root verifies; a zero
//      root, another loader's Loader-role signer, any other signing role, a
//      broken link, a root not last, a module signed by the root directly, a
//      witness outside the epoch do not.
//   5. SEALED: what is checked is a memfd nothing can write; the file on disk
//      changing afterwards changes nothing loaded.
//   6. THE LOADER REFUSES: a signed module under another name, and the same
//      module unsigned, do not attach; nothing is spawned from them. Modules
//      loaded one after another are each themselves.
//   7. THE GATE IS A CHAIN: a Wrapper leaf attached to the gate's entity is
//      handed every load first; refusing, the module does not attach; passing,
//      the loader's own stages still decide.
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
                  const tr::Identity& A, std::string* why_out = nullptr)
{
    std::string why;
    const bool ok = tr::verify(name, b.data(), b.size(), e, A, why);
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
        check(A.proven && !tr::zero(A.pub) && !tr::zero(A.root),
              "this loader proved its key from the keystore, and has a root");
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
        check(c.size() == 4 && c[1].role == A.role && c[1].subject == A.pub && c[2].role == sig::Role::Intermediate
              && c[3].role == sig::Role::Root && c[3].subject == A.root,
              "its chain is module <- this loader's key <- intermediate <- root");
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

        sig::Key d2{};
        tr::digest("ChessProvider", chess.data(), chess.size(), L, d2);
        auto t = chess;
        const sig::Span& load = L.covered[2];       // first PT_LOAD
        t[load.at + load.len / 2] ^= 0x01;
        check(!ok_as("ChessProvider", t, E, A, &why) && why == "signed for other bytes, or another name",
              "one flipped mapped byte is refused");
        check(!ok_as("WindowProvider", chess, E, A, &why) && why == "signed for other bytes, or another name",
              "the same bytes under another name are refused");
        check(!ok_as("ChessProvider", chess, other, A, &why) && why == "not witnessed under this epoch",
              "another epoch is refused");
        tr::Identity no_root = A; no_root.root = sig::Key{};
        check(!ok_as("ChessProvider", chess, E, no_root, &why) && why == "does not chain to this loader's root",
              "a loader with another root refuses it");
        tr::Identity not_me = A; not_me.pub[0] ^= 0x01;
        tr::Identity unproven = A; unproven.proven = false; unproven.unproven = "no keystore";
        if (A.role == sig::Role::Marketplace && !A.temporary)
            check(ok_as("ChessProvider", chess, E, not_me) && ok_as("ChessProvider", chess, E, unproven),
                  "the marketplace signed it under ACE's root: any loader takes it, with or without a key");
        else
        {
            check(!ok_as("ChessProvider", chess, E, not_me, &why)
                  && why == (A.role == sig::Role::Loader ? "signed by another loader"
                                                         : "a temporary root has no marketplace but this loader's own"),
                  "a loader that is not this one refuses it");
            check(!ok_as("ChessProvider", chess, E, unproven, &why)
                  && why == "signed by a loader, and this one has not proven a key: no keystore",
                  "and so does this one, in a session where it could not prove its key");
        }
    }

    // -- 3. this loader's identity: the keystore, a temporary root, a registration ----
    std::cout << "-- 3. identity --\n";
    char tmpl[] = "/tmp/etcs-sigtest-XXXXXX";
    const std::string dir = ::mkdtemp(tmpl) ? tmpl : "";
    const std::string store = dir + "/store", store2 = dir + "/other";
    {
        namespace ks = ETCS::keystore;
        const sig::Key cfg{};                       // no root built in: the configuring path, whatever this binary has
        auto sign_cfg = [&](const std::string& st, const char* name, const std::string& file, const sig::Epoch& e,
                            const std::string& out) { return tr::sign_module(st, name, file, e, out, why, nullptr, cfg); };
        std::string held, kwhy;
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

        sig::Key pub{}, root{};
        std::vector<uint8_t> raw;
        std::vector<sig::Certificate> reg;
        check(tr::load_public(store, pub) && tr::load_root(store, root, cfg) && tr::read_file(store + "/loader.chain", raw)
              && sig::decode_chain(raw.data(), raw.size(), reg) && tr::registration_valid(reg, pub, root),
              "a temporary root registered it: Loader <- intermediate <- root");
        tr::Identity mine; mine.root = root; mine.pub = pub; mine.proven = true;

        const auto w1 = wasm_signed(tiny_wasm(), slurp(dir + "/c1"));
        check(ok_as("Probe", w1, E, mine), "signed wasm verifies under that root, as this loader's");
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

        std::vector<sig::Certificate> c1;
        const std::string c1b = slurp(dir + "/c1");
        sig::decode_chain(c1b.data(), c1b.size(), c1);
        check(!ok_as("Probe", wasm_signed(tiny_wasm(), sig::encode_chain({ c1[0] })), E, mine, &why)
              && why == "the signer has no certificate", "a leaf alone is refused");

        write_bytes(dir + "/Other.wasm", tiny_wasm());
        check(sign_cfg(store2, "Probe", dir + "/Other.wasm", E, dir + "/o1") == 0,
              "a second store is a second loader");
        sig::Key pub2{}, root2{};
        tr::load_public(store2, pub2); tr::load_root(store2, root2, cfg);
        tr::Identity theirs; theirs.root = root2; theirs.pub = pub2; theirs.proven = true;
        const auto o1 = wasm_signed(tiny_wasm(), slurp(dir + "/o1"));
        check(pub2 != pub && ok_as("Probe", o1, E, theirs), "its module loads on it");
        tr::Identity same_root_not_me = theirs; same_root_not_me.pub = pub;
        check(!ok_as("Probe", o1, E, same_root_not_me, &why) && why == "signed by another loader",
              "and not on a loader that is not it");
        check(!ok_as("Probe", o1, E, mine, &why), "nor under another root");

        // A registration issued elsewhere, injected: the second loader becomes
        // a marketplace's, under a root an authority made.
        {
            const Keys sroot = fresh_key(), active = fresh_key();
            const auto mk = issue(active, sig::Role::Marketplace, pub2, {});
            const auto ic = issue(sroot, sig::Role::Intermediate, active.pub, {});
            const auto rc = issue(sroot, sig::Role::Root, sroot.pub, {});
            check(!tr::install_registration(store2, { issue(active, sig::Role::Marketplace, pub, {}), ic, rc }, why, cfg)
                  && why == "not a registration of this loader's key",
                  "a registration for another loader's key is not installed");
            check(!tr::install_registration(store2, { mk, ic }, why, cfg)
                  && why == "the chain does not end at a root certificate",
                  "nor one that does not end at a root");
            check(tr::install_registration(store2, { mk, ic, rc }, why, cfg), "an injected registration is installed");
            const auto id2 = tr::establish(store2, true, cfg);
            check(id2->proven && id2->role == sig::Role::Marketplace && id2->root == sroot.pub,
                  "the loader is now a marketplace under that root");
            write_bytes(dir + "/Market.wasm", tiny_wasm());
            check(sign_cfg(store2, "Probe", dir + "/Market.wasm", E, dir + "/m1") == 0, "and signs as one");
            const auto m1 = wasm_signed(tiny_wasm(), slurp(dir + "/m1"));
            tr::Identity anyone; anyone.root = sroot.pub; anyone.temporary = false;
            check(ok_as("Probe", m1, E, anyone), "under a canonical root, any loader takes what it signed");
            anyone.temporary = true; anyone.proven = true; anyone.pub = pub;
            check(!ok_as("Probe", m1, E, anyone, &why) && why == "a temporary root has no marketplace but this loader's own",
                  "under a temporary root, only the loader that is that marketplace does");
            check(ok_as("Probe", m1, E, *id2), "which it is");

            // The lineage: this loader signs the next, and says so.
            std::string line;
            std::vector<tr::LineageEdge> edges;
            write_bytes(dir + "/next", tiny_wasm());
            check(id2->genesis && tr::succeed(store2, "etcs", dir + "/next", E, dir + "/n1", why, &line, cfg) == 0 && !line.empty(),
                  "signing the next loader binary records an edge");
            sig::Layout NL; sig::Key child{};
            const auto nb = tiny_wasm();
            sig::layout(nb.data(), nb.size(), NL, why);
            tr::digest("etcs", nb.data(), nb.size(), NL, child);
            check(tr::lineage_read(store2, edges, why) && edges.size() == 1 && edges[0].child == child
                  && edges[0].key == pub2 && edges[0].to == E && edges[0].from == E && !edges[0].genesis
                  && edges[0].parent == A.self && !edges[0].host.empty(),
                  "the edge names parent, child, both epochs, the signer and the host, and verifies");
            check(tr::succeed(store2, "etcs", dir + "/next", E, dir + "/n2", why, nullptr, cfg) == 0
                  && (edges.clear(), tr::lineage_read(store2, edges, why)) && edges.size() == 1,
                  "the same child in the same epoch is not recorded twice");
            check(!tr::establish(store2, true, cfg)->genesis, "a store with an edge is past its genesis");
            {
                std::ofstream f(store2 + "/lineage.log", std::ios::app);
                std::string forged = line;
                forged[forged.find("host=") + 5] ^= 0x01;
                f << forged << "\n";
            }
            check((edges.clear(), !tr::lineage_read(store2, edges, why)) && why.find("line 2") != std::string::npos,
                  "an edge altered after signing is found");

            // Once the root is canonical, only a loader that attests signs.
            tr::Identity canon = *id2; canon.temporary = false; canon.attested = false; canon.genesis = false;
            canon.unattested = "not in the lineage";
            auto signs = [&](const tr::Identity& who) {
                tr::Gate g; tr::LoadFrame f; std::string chain, w;
                g.use(std::make_shared<const tr::Identity>(who));
                g.signature.signer = [](const uint8_t*, size_t, uint8_t* s64) { std::memset(s64, 0, 64); };
                tr::load_frame("Probe", nb.data(), nb.size(), other, f);
                f.chain = &chain;
                return g.wrap(f, w);
            };
            check(!signs(canon), "under a canonical root, a loader that does not attest signs nothing");
            canon.genesis = true;
            check(signs(canon), "unless the store has no lineage yet: one beginning");
            canon.genesis = false; canon.attested = true;
            check(signs(canon), "and a loader that attests signs");
        }

        // THE ROOT BUILT IN (ace_root(), given here): what a loader is when
        // the root came with its binary instead of with its store.
        {
            const Keys sroot = fresh_key(), active = fresh_key(), stray = fresh_key();
            const std::string store3 = dir + "/pinned";
            uint8_t secret[64];
            sig::Key pub3{};
            std::vector<sig::Certificate> reg3;
            struct stat st3{};
            write_bytes(dir + "/Pinned.wasm", tiny_wasm());
            check(tr::sign_module(store3, "Probe", dir + "/Pinned.wasm", E, dir + "/b0", why, nullptr, sroot.pub) == 2
                  && why.find("not registered under ACE's root") != std::string::npos,
                  "with a root built in, a loader registers nothing for itself and signs nothing");
            check(tr::loader_key(store3, secret, pub3, why) && !tr::zero(pub3)
                  && ::stat((store3 + "/root.pub").c_str(), &st3) != 0 && ::stat((store3 + "/loader.chain").c_str(), &st3) != 0,
                  "its key is made all the same, to be registered; the store is given no root");
            mc::crypto_wipe(secret, 64);

            const auto ic = issue(sroot, sig::Role::Intermediate, active.pub, {});
            const auto rc = issue(sroot, sig::Role::Root, sroot.pub, {});
            const auto mk = issue(active, sig::Role::Marketplace, pub3, {});
            check(!tr::install_registration(store3, { issue(stray, sig::Role::Marketplace, pub3, {}),
                                                      issue(stray, sig::Role::Root, stray.pub, {}) }, why, sroot.pub)
                  && why == "the chain's root is not ACE's",
                  "a registration under any other root is not installed");
            check(tr::install_registration(store3, { mk, ic, rc }, why, sroot.pub)
                  && ::stat((store3 + "/root.pub").c_str(), &st3) != 0,
                  "one under the built-in root is, and still no root file is written");
            const auto id3 = tr::establish(store3, true, sroot.pub);
            check(!id3->temporary && id3->root == sroot.pub && id3->proven && id3->role == sig::Role::Marketplace,
                  "the session is the marketplace's, under ACE's root");
            check(tr::sign_module(store3, "Probe", dir + "/Pinned.wasm", E, dir + "/b1", why, nullptr, sroot.pub) == 0,
                  "and it signs");
            const auto b1 = wasm_signed(tiny_wasm(), slurp(dir + "/b1"));
            const auto bare = tr::establish(dir + "/none", true, sroot.pub);
            check(tr::zero(bare->pub) && !bare->proven && ok_as("Probe", b1, E, *bare),
                  "a loader with an empty store takes it: the root it was built with is enough");

            // This binary, as a download would meet it: nothing in the store,
            // the root it is signed under built in.
            const auto fresh = tr::establish(dir + "/none", true, A.root);
            check(A.role == sig::Role::Marketplace
                      ? fresh->attested
                      : !fresh->attested && fresh->unattested.rfind("signed by a loader", 0) == 0,
                  A.role == sig::Role::Marketplace
                      ? "this binary attests with an empty store, by the root alone: the marketplace signed it"
                      : "this binary is a loader's own build: by the root alone it does not attest");
            check(ks::erase(store3, "loader-key", kwhy) == ks::Status::Ok, "(its key is erased)");
        }

        auto w3 = w2; w3[11] ^= 0x01;               // inside the type section
        check(!ok_as("Probe", w3, E, mine), "a changed wasm section is refused");
        const auto w4 = wasm_signed(w1, slurp(dir + "/c1"));
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
        tr::load_public(store2, pub2);
        check(tr::establish(store2, true, cfg)->proven && tr::establish(store2, true, cfg)->pub == pub2 && !tr::establish(store2, true, cfg)->reg.empty(),
              "a session proves the store's key against the keystore");
        check(!tr::establish(store, true, cfg)->proven
              && tr::establish(store, true, cfg)->unproven == "the keystore's key is not loader.sign.pub's",
              "and a store whose pub file was swapped proves nothing");
        check(!tr::establish(dir + "/none", true, cfg)->proven && !tr::establish(store2, false, cfg)->proven,
              "a store with no key, or a session that did not ask, is unproven");

        check(ks::erase(store, "loader-key", kwhy) == ks::Status::Ok && ks::erase(store2, "loader-key", kwhy) == ks::Status::Ok
              && ks::load(store, "loader-key", held, kwhy) == ks::Status::Absent,
              "erase is the only way a key goes");
        if (!held.empty()) mc::crypto_wipe(&held[0], held.size());
    }

    // -- 4. the ACE chain ---------------------------------------------------------------
    std::cout << "-- 4. ACE chain --\n";
    {
        const Keys root = fresh_key(), inter = fresh_key(), market = fresh_key(), stranger = fresh_key();
        sig::Layout WL;
        const auto w = tiny_wasm();
        sig::layout(w.data(), w.size(), WL, why);
        sig::Key d{};
        tr::digest("Probe", w.data(), w.size(), WL, d);

        const auto leaf   = issue(market, sig::Role::Module, d, { E });
        const auto mcert  = issue(inter, sig::Role::Marketplace, market.pub, { E });
        const auto icert  = issue(root, sig::Role::Intermediate, inter.pub, {});
        const auto rcert  = issue(root, sig::Role::Root, root.pub, {});
        tr::Identity ace; ace.root = root.pub; ace.temporary = false; ace.proven = true;
        auto as = [&](std::vector<sig::Certificate> chain) { return wasm_signed(w, sig::encode_chain(chain)); };

        check(ok_as("Probe", as({ leaf, mcert, icert, rcert }), E, ace), "module <- marketplace <- intermediate <- root");
        check(ok_as("Probe", as({ leaf, mcert, icert }), E, ace), "the root's own certificate may be left off");
        check(!ok_as("Probe", as({ leaf, mcert, icert, rcert }), E, tr::Identity{}), "a zero root anchors nothing");
        check(!ok_as("Probe", as({ leaf, mcert, icert, rcert }), other, ace), "not in another epoch");

        const auto lcert = issue(inter, sig::Role::Loader, market.pub, {});
        check(!ok_as("Probe", as({ leaf, lcert, icert, rcert }), E, ace, &why) && why == "signed by another loader",
              "a Loader-role signer that is not this loader is refused");
        const auto icert_as_signer = issue(root, sig::Role::Intermediate, market.pub, {});
        check(!ok_as("Probe", as({ leaf, icert_as_signer, rcert }), E, ace, &why)
              && why == "modules are signed by the marketplace or by a loader",
              "no other role signs modules");
        const auto mold = issue(inter, sig::Role::Marketplace, market.pub, { other });
        check(!ok_as("Probe", as({ leaf, mold, icert, rcert }), E, ace, &why) && why == "witness not valid in this epoch",
              "a marketplace certificate outside this epoch is refused");
        const auto ibad = issue(stranger, sig::Role::Intermediate, inter.pub, {});
        check(!ok_as("Probe", as({ leaf, mcert, ibad }), E, ace), "an intermediate the root did not certify is refused");
        check(!ok_as("Probe", as({ leaf, mcert, rcert, icert }), E, ace), "a root that is not last is refused");
        const auto byroot = issue(root, sig::Role::Module, d, { E });
        check(!ok_as("Probe", as({ byroot }), E, ace), "a module signed by the root directly is refused");
        tr::Identity temp = ace; temp.temporary = true;
        check(!ok_as("Probe", as({ leaf, mcert, icert, rcert }), E, temp, &why)
              && why == "a temporary root has no marketplace but this loader's own",
              "a temporary root has no marketplace but this loader's own");
        temp.pub = market.pub;
        check(ok_as("Probe", as({ leaf, mcert, icert, rcert }), E, temp), "which this loader may be");
        tr::Identity both = ace; both.pub = market.pub;
        check(ok_as("Probe", as({ leaf, lcert, icert }), E, both),
              "a Loader-role signer is taken only as this loader's own");
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

    if (std::system(("rm -rf '" + dir + "'").c_str()) != 0) std::printf("  (could not remove %s)\n", dir.c_str());
    std::cout << (g_fail ? "FAILED: " : "PASSED") << (g_fail ? std::to_string(g_fail) : "") << "\n";
    return g_fail ? 1 : 0;
}
