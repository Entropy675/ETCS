// CryptProviderTesterLoader.cc
//
// THE CRYPT TYPES, AND THAT NOTHING SECRET LEAVES BY ANY WAY BUT THE ONE.
// What is held to account:
//
//   1. ED25519 IS RFC 8032. A Keypair's signature verifies under
//      crypto_ed25519_check and not under crypto_eddsa_check (Monocypher's
//      BLAKE2b EdDSA, the one a habit reaches for); the pinned library
//      reproduces RFC 8032's first test vector.
//   2. PRIVATE IS A CLAIM THE RUNTIME KEEPS. A Keypair carries the tag, no
//      transport will bridge it, its state hash does not move when it takes a
//      key, it reveals nothing and accepts nothing from an edge.
//   3. SHAMIR. A 3-of-5 split rebuilds the same key from every 3 of the 5;
//      2 do not; a share that is wrong but well-formed rebuilds a different
//      key, which Combine refuses and leaves nothing behind.
//   4. CUSTODY. Through an Embassy and a FileRelation: a share lands as a 0600
//      file under a '.private' name and never in a log; custody never
//      overwrites; no name walks out of the root; a '.private' file is read
//      only into a Private entity; a closed Embassy stops everything; the
//      relation is locked once active; a recall rebuilds the key.
//   5. CERTIFICATES. Signature.h round-trips and refuses trailing bytes; a
//      root is its own issuer and nothing else may be; a chain verifies link
//      by link and a flipped bit or a stranger's key does not.
//   6. THE CEREMONY AS WRITTEN. root_ceremony.etcs, with only its paths and
//      keys filled in, leaves every custodian a share of the root and of the
//      active key, and every custodian and the public directory the three-step
//      chain; any three custodians rebuild the root, whose public key is the
//      root certificate's subject; certify.etcs rebuilds the active key from
//      three and certifies one more key under the same root; attest_root.etcs
//      rebuilds the root from three and certifies another root's key.
//   7. A KEY KEPT. A Keypair goes whole only into a Database that claims
//      Private -- DatabaseProvider's Keystore, the OS keystore -- and comes
//      back as the same key; a LocalDatabase is refused; nothing is replaced;
//      the seed is in no log.
//   8. THE ENVELOPE. A Wrapper stage that seals, keyed by its Private child
//      and by no verb: a changed byte, another key, or no key is a refusal.
//      Standing on a Keystore, it makes naming a secret not enough to read it.
//   9. A TEST AUTHORITY AS WRITTEN. test_authority.etcs leaves marketplace,
//      release and registrar certificates for three keys under one active key,
//      and the root's: three registrations the loader accepts, each for its
//      own key and no other, from two keys that do not outlive the script.
//  10. THE REGISTRAR, as a site's route: it takes only a chain that certifies
//      its own key up to a root; a loader's ask, signed and naming an identity
//      and epoch it serves, gets the key certified by its ACE identity and that
//      identity certified for the epoch -- one ACE identity per key, the same in
//      every epoch -- which the loader accepts for that epoch alone; an
//      unsigned ask, an outsider's, or one made of another registrar is refused.
//  11. ACCOUNTS. A key joins with its own word and a member's, in one epoch
//      the registrar serves, never one alone; a key holding a registration
//      moves only at its next one, in a later epoch; one sharing another
//      account does not move; a member retires a key of its own account, which
//      is then registered no more and has no word; a new epoch closes the
//      oldest; the rows name no key, and are read back as left -- epochs too --
//      under the same secret and refused under another.
//  12. THE PORTAL. A browser's session: a code shown and a token kept; only a
//      current member's word on that code signs it in, once; it sees its own
//      account's keys and no other's; with a key's own word to join, it stands
//      for the member's admission, and for its retirement of the account's
//      keys; it ends when closed, when its member is retired, or in time.
//
//   ./Run_CryptProviderTesterLoader
//
// No display, no network: the custodians are directories under /tmp.

#define ETCS_MONOCYPHER_IMPLEMENTATION
#include "../ETCS.h"
#include "../libs/monocypher.h"
#include "../core/Signature.h"
#include "../modules/NetworkProvider/NetworkProvider/RouteRequest.h"

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

static int g_fail = 0;
static void check(bool ok, const char* what)
{
    std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", what);
    std::fflush(stdout);
    if (!ok) ++g_fail;
}

static std::string verb(ETCS::Entity* e, const std::string& action, const std::string& args = "")
{
    ETCS::Buffer a(action.c_str()), d(args.c_str());
    e->call(a, d);
    return d.toString();
}
static bool yes(const std::string& s) { return s.find('1') != std::string::npos; }
// A request route's answer, inline or by reference (NetworkProvider's RouteRef
// frame): a GET, or a POST of `words`.
static std::string route(ETCS::Entity* e, const std::string& path, const std::string& words = "", const char* method = nullptr)
{
    const RouteRequest req = RouteRequest::Parse("/" + path, method ? method : words.empty() ? "GET" : "POST",
                                                 words.data(), words.size());
    const uint64_t at = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(&req));
    ETCS::Buffer a("Registrar.Request"), d;
    d.writeRaw(&at, sizeof at);
    e->call(a, d);
    if (d.written == 64 && std::memcmp(d.buf, "ETCSREF\0", 8) == 0)
    {
        uint64_t p = 0, n = 0;
        std::memcpy(&p, d.buf + 8, 8);
        std::memcpy(&n, d.buf + 16, 8);
        return std::string(reinterpret_cast<const char*>(static_cast<uintptr_t>(p)), static_cast<size_t>(n));
    }
    return d.toString();
}

static std::string hex(const uint8_t* p, size_t n)
{
    static const char* d = "0123456789abcdef";
    std::string o;
    for (size_t i = 0; i < n; ++i) { o += d[p[i] >> 4]; o += d[p[i] & 15]; }
    return o;
}
static bool unhex(const std::string& s, uint8_t* out, size_t n)
{
    if (s.size() != 2 * n) return false;
    for (size_t i = 0; i < n; ++i) out[i] = static_cast<uint8_t>(std::stoul(s.substr(2 * i, 2), nullptr, 16));
    return true;
}
static std::string slurp(const std::string& path)
{
    std::ifstream f(path, std::ios::binary);
    std::stringstream s; s << f.rdbuf();
    return s.str();
}

// stdout to a file for a stretch, so what the runtime logged can be searched.
struct Capture
{
    int saved = -1; std::string path;
    explicit Capture(const std::string& p) : path(p)
    {
        std::fflush(stdout); std::cout.flush();
        saved = ::dup(1);
        const int fd = ::open(p.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
        ::dup2(fd, 1); ::close(fd);
    }
    std::string done()
    {
        std::fflush(stdout); std::cout.flush();
        ::dup2(saved, 1); ::close(saved);
        return slurp(path);
    }
};

static Private_*  priv(ETCS::Entity* e) { return e ? static_cast<Private_*>(e->getInterfacePointer(ETCS::Buffer("Private"))) : nullptr; }
static Foreign_*  foreign(ETCS::Entity* e) { return e ? static_cast<Foreign_*>(e->getInterfacePointer(ETCS::Buffer("Foreign"))) : nullptr; }

int main(int, char**)
{
    WIRE_CONTEXT();
    std::cout << "=== CryptProvider ===\n";

    ETCS::ExecutionContext s(&root, &ctx);
    auto entity = [&](const char* n) { return ETCS::etcs_resolve_rid_anywhere(&ETCS::getLoader(), s.names[n].rid); };
    auto run = [&](const std::string& text) {
        std::istringstream in(text);
        return ETCS::run_script(in, std::string(ETCS_ACE_ROOT) + "/loaders/crypt_test.etcs", s);
    };

    // -- 1. ed25519 is RFC 8032 ------------------------------------------------------
    std::cout << "\n-- 1. ed25519 is RFC 8032 --\n";
    {
        // RFC 8032 7.1, TEST 1.
        uint8_t seed[32], sk[64], pk[32], sig[64], want_pk[32], want_sig[64];
        unhex("9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60", seed, 32);
        unhex("d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a", want_pk, 32);
        unhex("e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e06522490155"
              "5fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b", want_sig, 64);
        mc::crypto_ed25519_key_pair(sk, pk, seed);
        mc::crypto_ed25519_sign(sig, sk, nullptr, 0);
        check(std::memcmp(pk, want_pk, 32) == 0 && std::memcmp(sig, want_sig, 64) == 0,
              "the pinned Monocypher reproduces RFC 8032's first test vector");
    }
    check(run("spawn CryptProvider::Keypair k\n"), "a Keypair spawns");
    ETCS::Entity* k = entity("k");
    const uint64_t before = k ? k->getHash() : 0;
    check(k && yes(verb(k, "Keypair.Generate")) && !yes(verb(k, "Keypair.Generate")),
          "Generate takes a key from the OS, and refuses while one is held");
    const std::string pub = k ? verb(k, "Keypair.PublicKey") : "";
    const std::string msg = "6574637320726f6f74";   // "etcs root"
    const std::string sighex = k ? verb(k, "Keypair.Sign", msg) : "";
    {
        uint8_t p[32] = {}, sg[64] = {}, m[9];
        unhex(msg, m, 9);
        const bool parsed = unhex(pub, p, 32) && unhex(sighex, sg, 64);
        check(parsed && mc::crypto_ed25519_check(sg, p, m, sizeof m) == 0,
              "a Keypair's signature verifies as RFC 8032 Ed25519");
        check(parsed && mc::crypto_eddsa_check(sg, p, m, sizeof m) != 0,
              "...and not as Monocypher's BLAKE2b EdDSA");
    }

    // -- 2. private is a claim the runtime keeps --------------------------------------
    std::cout << "\n-- 2. private is a claim the runtime keeps --\n";
    check(k && k->hasTag(ETCS::Buffer("Private")) && priv(k), "a Keypair claims Private");
    check(k && ETCS::MirrorBuffer::localFrame(k), "...and no transport will bridge it");
    check(k && k->getHash() == before, "its state hash did not move when it took a key: nothing on the value surface");
    {
        std::string out = "x";
        check(priv(k) && !priv(k)->Reveal(out) && out.empty() && !priv(k)->Receive("anything"),
              "a Keypair reveals nothing to an edge and accepts nothing from one");
    }
    verb(k, "Keypair.Wipe");
    check(!yes(verb(k, "Keypair.Holds")) && verb(k, "Keypair.PublicKey").empty(), "Wipe forgets it");

    // -- 3. shamir -----------------------------------------------------------------------
    std::cout << "\n-- 3. shamir --\n";
    check(run("spawn CryptProvider::Keypair key\n"
              "key.Generate()\n"
              "spawn CryptProvider::Shamir split\n"
              "split.Threshold(3)\n"
              "split.spawn(CryptProvider::Shard s1)\n"
              "split.spawn(CryptProvider::Shard s2)\n"
              "split.spawn(CryptProvider::Shard s3)\n"
              "split.spawn(CryptProvider::Shard s4)\n"
              "split.spawn(CryptProvider::Shard s5)\n"
              "split.Split(@key)\n"), "a key split 3 of 5 by script");
    const std::string keypub = verb(entity("key"), "Keypair.PublicKey");
    std::vector<std::string> shares;
    bool shaped = true;
    for (int i = 1; i <= 5; ++i)
    {
        ETCS::Entity* sh = entity(("s" + std::to_string(i)).c_str());
        std::string r;
        shaped = shaped && sh && priv(sh) && priv(sh)->Reveal(r)
              && verb(sh, "Shard.Index").find(std::to_string(i)) == 0
              && verb(sh, "Shard.Threshold").find('3') == 0 && verb(sh, "Shard.Set") == keypub;
        shares.push_back(r);
    }
    check(shaped, "five shards: indices 1..5, threshold 3, the key's public key as their set");
    check(!yes(verb(entity("split"), "Shamir.Split", std::to_string(entity("key")->getRID()))),
          "a second Split over shards that hold is refused");

    // Every 3 of the 5, each into a fresh Shamir by Receive, rebuilds the key.
    int rebuilt = 0, tried = 0;
    for (int a = 0; a < 5; ++a) for (int b = a + 1; b < 5; ++b) for (int c = b + 1; c < 5; ++c)
    {
        ++tried;
        const std::string n = std::to_string(tried);
        run("spawn CryptProvider::Keypair back" + n + "\n"
            "spawn CryptProvider::Shamir join" + n + "\n"
            "join" + n + ".spawn(CryptProvider::Shard ja" + n + ")\n"
            "join" + n + ".spawn(CryptProvider::Shard jb" + n + ")\n"
            "join" + n + ".spawn(CryptProvider::Shard jc" + n + ")\n");
        priv(entity(("ja" + n).c_str()))->Receive(shares[a]);
        priv(entity(("jb" + n).c_str()))->Receive(shares[b]);
        priv(entity(("jc" + n).c_str()))->Receive(shares[c]);
        ETCS::Entity* back = entity(("back" + n).c_str());
        if (yes(verb(entity(("join" + n).c_str()), "Shamir.Combine", std::to_string(back->getRID())))
            && verb(back, "Keypair.PublicKey") == keypub) ++rebuilt;
    }
    std::printf("        (%d of %d subsets rebuilt the key)\n", rebuilt, tried);
    check(tried == 10 && rebuilt == 10, "every 3 of the 5 rebuilds the same key");

    run("spawn CryptProvider::Keypair two\n"
        "spawn CryptProvider::Shamir duo\n"
        "duo.spawn(CryptProvider::Shard t1)\n"
        "duo.spawn(CryptProvider::Shard t2)\n");
    priv(entity("t1"))->Receive(shares[0]);
    priv(entity("t2"))->Receive(shares[1]);
    check(!yes(verb(entity("duo"), "Shamir.Combine", std::to_string(entity("two")->getRID())))
          && !yes(verb(entity("two"), "Keypair.Holds")), "two of them do not");

    {
        // A share that is wrong but well-formed: one byte changed, the check redone.
        std::string bad = shares[2];
        const size_t at = bad.find("share ") + 6;
        bad[at] = bad[at] == '0' ? '1' : '0';
        const std::string body = bad.substr(0, bad.find("check "));
        uint8_t d[32];
        picohash_ctx_t hc; picohash_init_sha256(&hc); picohash_update(&hc, body.data(), body.size()); picohash_final(&hc, d);
        bad = body + "check " + hex(d, 8) + "\n";
        std::string miscopied = shares[2];
        miscopied[at] = miscopied[at] == '0' ? '1' : '0';
        run("spawn CryptProvider::Keypair wrong\n"
            "spawn CryptProvider::Shamir trio_bad\n"
            "trio_bad.spawn(CryptProvider::Shard w1)\n"
            "trio_bad.spawn(CryptProvider::Shard w2)\n"
            "trio_bad.spawn(CryptProvider::Shard w3)\n"
            "trio_bad.spawn(CryptProvider::Shard w4)\n");
        check(!priv(entity("w4"))->Receive(miscopied), "a share copied wrong fails its check");
        std::string crlf;
        for (char ch : shares[3]) { if (ch == '\n') crlf += '\r'; crlf += ch; }
        run("spawn CryptProvider::Shard via_crlf\n");
        check(priv(entity("via_crlf"))->Receive(crlf) && verb(entity("via_crlf"), "Shard.Index").find('4') == 0,
              "a share copied through a CRLF editor still reads");
        priv(entity("w1"))->Receive(shares[0]);
        priv(entity("w2"))->Receive(shares[1]);
        priv(entity("w3"))->Receive(bad);
        check(!yes(verb(entity("trio_bad"), "Shamir.Combine", std::to_string(entity("wrong")->getRID())))
              && !yes(verb(entity("wrong"), "Keypair.Holds")),
              "a wrong share that passes its check rebuilds a different key: refused, nothing left behind");
    }

    // -- 4. custody ---------------------------------------------------------------------
    std::cout << "\n-- 4. custody --\n";
    char tmpl[] = "/tmp/etcs_custody_XXXXXX";
    const std::string dir = ::mkdtemp(tmpl) ? std::string(tmpl) : std::string();
    check(!dir.empty() && run(
        "spawn NetworkProvider::Embassy embassy\n"
        "spawn CryptProvider::Custody a\n"
        "spawn NetworkProvider::FileRelation to_a\n"
        "to_a.Root(" + dir + ")\n"
        "to_a.Target(Directory)\n"
        "to_a.Activate(@a)\n"
        "embassy.Advertise(custodian-a file @to_a)\n"
        "a.SetEmbassy(@embassy)\n"
        "a.SetRelation(custodian-a)\n"), "a custodian: a FileRelation on a directory, landing on a Custody, in an Embassy");
    ETCS::Entity* a = entity("a");
    ETCS::Entity* to_a = entity("to_a");
    const std::string s1 = std::to_string(entity("s1")->getRID());
    check(!yes(verb(a, "Custody.Advertise", "root shard " + s1)), "while the Embassy is closed, nothing leaves");
    verb(entity("embassy"), "Embassy.Open");

    Capture cap("/tmp/etcs_custody_log.txt");
    // By a script line, so the executor's own log of the line is in the capture.
    run("a.Advertise(root shard @s1)\n");
    const bool put = true;
    const bool again = yes(verb(a, "Custody.Advertise", "root shard " + std::to_string(entity("s2")->getRID())));
    const bool walk = yes(verb(a, "Custody.Advertise", "../root shard " + s1));
    std::string ans;
    const bool raw_walk = foreign(to_a)->Exchange("PUT ../escape\nx", ans, ctx);
    const bool raw_peek = foreign(to_a)->Exchange("GET root.shard.private", ans, ctx);
    const bool raw_public_private = foreign(to_a)->Exchange("PUT plain.cert\n{private:" + s1 + "}", ans, ctx);
    const std::string log = cap.done();

    struct stat st{};
    const std::string file = dir + "/root.shard.private";
    const bool exists = ::stat(file.c_str(), &st) == 0;
    check(put && exists && (st.st_mode & 0777) == 0600, "a share lands as a 0600 file under a '.private' name");
    check(slurp(file) == shares[0], "...in its custody form, exactly");
    const std::string share_hex = shares[0].substr(shares[0].find("share ") + 6, 64);
    check(log.find(share_hex) == std::string::npos && log.find("a.Advertise(root shard " + s1 + ")") != std::string::npos,
          "...and the share is nowhere in the log; the line names the shard");
    check(!again, "custody never overwrites: a second share under the same name is refused");
    check(!walk && !raw_walk && ::access("/tmp/escape", F_OK) != 0, "no name walks out of the root");
    check(!raw_peek, "a '.private' file is read only into a Private entity");
    check(!raw_public_private, "...and private material goes only to a '.private' name");
    check(!yes(verb(to_a, "FileRelation.Root", "/tmp")) && !yes(verb(to_a, "FileRelation.Activate", std::to_string(a->getRID()))),
          "the relation is locked once active");
    {
        const std::string listed = verb(to_a, "FileRelation.Ask", "LIST");
        const std::string peeked = verb(to_a, "FileRelation.Ask", "GET root.shard.private");
        check(listed.find("root.shard.private") != std::string::npos && peeked.empty(),
              "Ask from a line: LIST answers, and a '.private' file is not read into a log");
    }

    for (int i = 2; i <= 3; ++i)
        verb(a, "Custody.Advertise", "root" + std::to_string(i) + " shard " + std::to_string(entity(("s" + std::to_string(i)).c_str())->getRID()));
    const std::string listing = verb(a, "Custody.List", "shard");
    std::printf("        (custody lists: %s)\n", listing.substr(0, listing.find('\n')).c_str());
    check(listing.find("root shard custodian-a") != std::string::npos && listing.find("root3 shard custodian-a") != std::string::npos,
          "List is what the custodian holds now");

    run("spawn CryptProvider::Keypair recalled\n"
        "spawn CryptProvider::Shamir rejoin\n"
        "rejoin.spawn(CryptProvider::Shard q1)\n"
        "rejoin.spawn(CryptProvider::Shard q2)\n"
        "rejoin.spawn(CryptProvider::Shard q3)\n"
        "a.Recall(root @q1)\n"
        "a.Recall(root2 @q2)\n"
        "a.Recall(root3 @q3)\n"
        "rejoin.Combine(@recalled)\n");
    check(verb(entity("recalled"), "Keypair.PublicKey") == keypub, "three recalled shares rebuild the key");
    check(!yes(verb(a, "Custody.Recall", "root " + std::to_string(entity("q1")->getRID()))),
          "a recall into a shard that holds is refused");
    check(yes(verb(a, "Custody.Withdraw", "root3")) && ::access((dir + "/root3.shard.private").c_str(), F_OK) != 0,
          "Withdraw drops the custodian's copy");

    // -- 5. certificates -----------------------------------------------------------------
    std::cout << "\n-- 5. certificates --\n";
    {
        ETCS::sig::Certificate c;
        c.role = ETCS::sig::Role::Marketplace;
        for (int i = 0; i < 32; ++i) { c.subject[i] = static_cast<uint8_t>(i); c.issuer[i] = static_cast<uint8_t>(255 - i); }
        c.epochs.push_back(ETCS::sig::Epoch{});
        c.epochs[0][0] = 7;
        for (int i = 0; i < 64; ++i) c.signature[i] = static_cast<uint8_t>(i * 3);
        const std::string b = ETCS::sig::encode(c);
        ETCS::sig::Certificate d;
        size_t used = 0;
        const bool rt = ETCS::sig::decode(b.data(), b.size(), d, &used) && used == b.size()
            && d.role == c.role && d.subject == c.subject && d.issuer == c.issuer && d.epochs == c.epochs
            && std::memcmp(d.signature, c.signature, 64) == 0;
        std::vector<ETCS::sig::Certificate> chain{ c, c }, back;
        const std::string cb = ETCS::sig::encode_chain(chain);
        check(rt && ETCS::sig::decode_chain(cb.data(), cb.size(), back) && back.size() == 2,
              "Signature.h: a certificate and a chain round-trip");
        const std::string trailing = cb + "x";
        check(!ETCS::sig::decode_chain(trailing.data(), trailing.size(), back) && !ETCS::sig::decode(b.data(), b.size() - 1, d),
              "...and a chain with a trailing byte, or a short certificate, is refused");
    }
    check(run("spawn CryptProvider::Keypair r\n"
              "r.Generate()\n"
              "spawn CryptProvider::Keypair im\n"
              "im.Generate()\n"
              "spawn CryptProvider::Keypair stranger\n"
              "stranger.Generate()\n"
              "spawn CryptProvider::Certificate rc\n"
              "spawn CryptProvider::Certificate ic\n"
              "spawn CryptProvider::Certificate fake\n"
              "spawn CryptProvider::Certificate attests\n"
              "spawn CryptProvider::Certificate self_im\n"), "keys and certificates spawn");
    auto rid = [&](const char* n) { return std::to_string(entity(n)->getRID()); };
    check(yes(verb(entity("rc"), "Certificate.Issue", rid("r") + " root " + rid("r")))
          && yes(verb(entity("rc"), "Certificate.Verify")), "a root is its own issuer, and verifies");
    check(yes(verb(entity("attests"), "Certificate.Issue", rid("r") + " root " + rid("im")))
          && yes(verb(entity("attests"), "Certificate.Verify", rid("rc"))),
          "a root naming another key as a root is its attestation, and verifies under the root's own certificate");
    check(!yes(verb(entity("self_im"), "Certificate.Issue", rid("im") + " intermediate " + rid("im"))),
          "a non-root of its own is refused");
    verb(entity("ic"), "Certificate.AddEpoch", std::string(64, 'a'));
    check(yes(verb(entity("ic"), "Certificate.Issue", rid("r") + " intermediate " + rid("im")))
          && yes(verb(entity("ic"), "Certificate.Verify", rid("rc"))), "an intermediate by the root verifies against the root's certificate");
    verb(entity("fake"), "Certificate.Issue", rid("stranger") + " intermediate " + rid("im"));
    check(yes(verb(entity("fake"), "Certificate.Verify")) && !yes(verb(entity("fake"), "Certificate.Verify", rid("rc"))),
          "one by a stranger verifies alone, and not against the root");

    check(yes(verb(a, "Custody.Advertise", "intermediate cert " + rid("ic"))), "a certificate goes to custody");
    run("spawn CryptProvider::Certificate ic_back\n"
        "a.Recall(intermediate @ic_back)\n");
    check(verb(entity("ic_back"), "Certificate.Digest") == verb(entity("ic"), "Certificate.Digest")
          && yes(verb(entity("ic_back"), "Certificate.Verify", rid("rc"))), "...and comes back the same, still verifying");
    {
        // One bit of the custodian's copy flipped: it still loads, and no longer verifies.
        std::string h = slurp(dir + "/intermediate.cert");
        h[140] = h[140] == '0' ? '1' : '0';
        { std::ofstream o(dir + "/flipped.cert"); o << h; }
        run("spawn CryptProvider::Certificate ic_flip\n"
            "a.Recall(flipped @ic_flip)\n");
        check(!yes(verb(entity("ic_flip"), "Certificate.Verify", rid("rc"))), "a flipped bit does not verify");
    }

    // -- 6. the ceremony as written -------------------------------------------------------
    std::cout << "\n-- 6. the ceremony as written --\n";
    {
        // Section 4's custodian goes first: the ceremony is a root script whose
        // names (embassy, a, to_a) are globals, and a spawn never clobbers one.
        verb(entity("to_a"), "FileRelation.Delete");
        verb(entity("a"), "Custody.Delete");
        verb(entity("embassy"), "Embassy.Delete");
        namespace sig = ETCS::sig;
        namespace tr = ETCS::trust;
        std::string script = slurp(std::string(ETCS_ACE_ROOT) + "/modules/CryptProvider/scripts/root_ceremony.etcs");
        auto put = [](std::string& text, const std::string& what, const std::string& with) {
            for (size_t at = text.find(what); at != std::string::npos; at = text.find(what, at + with.size()))
                text.replace(at, what.size(), with);
        };
        auto fresh_dir = [] { char t[] = "/tmp/etcs_ceremony_XXXXXX"; return ::mkdtemp(t) ? std::string(t) : std::string(); };
        std::vector<std::string> dirs;
        for (const char* who : { "a", "b", "c", "d", "e" })
        {
            dirs.push_back(fresh_dir());
            put(script, std::string("/media/custodian-") + who, dirs.back());
        }
        const std::string pub_dir = fresh_dir();
        uint8_t seed[32], secret[64];
        sig::Key role_keys[3] = {};
        for (sig::Key& k : role_keys) { tr::random_bytes(seed, 32); mc::crypto_ed25519_key_pair(secret, k.data(), seed); }
        put(script, "PUBLIC_DIR", pub_dir);
        put(script, "MARKETPLACE_KEY", hex(role_keys[0].data(), 32));
        put(script, "RELEASE_KEY", hex(role_keys[1].data(), 32));
        put(script, "REGISTRAR_KEY", hex(role_keys[2].data(), 32));
        ETCS::ExecutionContext c2(&root, &ctx);
        std::istringstream in(script);
        const bool ran = ETCS::run_script(in, std::string(ETCS_ACE_ROOT) + "/modules/CryptProvider/scripts/root_ceremony.etcs", c2);
        const char* certs[] = { "root.cert", "active.cert", "marketplace.cert", "release.cert", "registrar.cert" };
        bool all = ran;
        for (const std::string& d : dirs)
            for (const char* f : { "root.shard.private", "active.shard.private" })
            {
                struct stat fs{};
                all = all && ::stat((d + "/" + f).c_str(), &fs) == 0;
            }
        for (const std::string& d : dirs)
            for (const char* f : certs)
            {
                struct stat fs{};
                all = all && ::stat((d + "/" + f).c_str(), &fs) == 0;
            }
        check(all, "the ceremony runs as written: every custodian holds a share of the root and of the active key, and the chain");
        const char* roles[] = { "marketplace.cert", "release.cert", "registrar.cert" };
        bool chained = true;
        sig::Key root_pub{};
        for (int i = 0; i < 3; ++i)
        {
            std::vector<sig::Certificate> reg;
            chained = chained && tr::read_certificates(pub_dir + "/" + roles[i], reg)
                      && tr::read_certificates(pub_dir + "/active.cert", reg) && tr::read_certificates(pub_dir + "/root.cert", reg)
                      && reg.size() == 3 && tr::registration_valid(reg, role_keys[i]);
            if (chained) root_pub = reg[2].subject;
        }
        check(chained, "the public directory holds the three-step chain: each role's key <- active <- root");

        // Three custodians, after the fact: a fresh runtime's view of their media.
        std::string recall =
            "spawn NetworkProvider::Embassy back_embassy\n"
            "spawn CryptProvider::Keypair back_root\n"
            "spawn CryptProvider::Shamir back_join\n"
            "spawn CryptProvider::Certificate back_rc\n";
        for (int i = 0; i < 3; ++i)
        {
            const std::string n = std::to_string(i), cu = "back_c" + n, re = "back_r" + n;
            recall += "spawn CryptProvider::Custody " + cu + "\n"
                      "spawn NetworkProvider::FileRelation " + re + "\n" +
                      re + ".Root(" + dirs[static_cast<size_t>(i) + 2] + ")\n" +
                      re + ".Target(Directory)\n" +
                      re + ".Activate(@" + cu + ")\n"
                      "back_embassy.Advertise(back-" + n + " file @" + re + ")\n" +
                      cu + ".SetEmbassy(@back_embassy)\n" +
                      cu + ".SetRelation(back-" + n + ")\n"
                      "back_join.spawn(CryptProvider::Shard back_s" + n + ")\n";
        }
        recall += "back_embassy.Open()\n";
        for (int i = 0; i < 3; ++i)
            recall += "back_c" + std::to_string(i) + ".Recall(root @back_s" + std::to_string(i) + ")\n";
        recall += "back_c0.Recall(root @back_rc)\n"
                  "back_join.Combine(@back_root)\n";
        run(recall);
        const std::string rebuilt_pub = verb(entity("back_root"), "Keypair.PublicKey");
        check(!rebuilt_pub.empty() && rebuilt_pub == verb(entity("back_rc"), "Certificate.Subject")
              && yes(verb(entity("back_rc"), "Certificate.Verify")),
              "three custodians rebuild the root, and it is the root certificate's subject");
        // The ceremony's names are globals; certify.etcs spawns some of the same.
        auto gone = [&](const char* name, const char* tag) {
            if (c2.names.count(name)) verb(ETCS::etcs_resolve_rid_anywhere(&ETCS::getLoader(), c2.names[name].rid), std::string(tag) + ".Delete");
        };
        for (const char* n : { "a", "b", "c", "d", "e", "out" }) gone(n, "Custody");
        for (const char* n : { "to_a", "to_b", "to_c", "to_d", "to_e", "to_out" }) gone(n, "FileRelation");
        for (const char* n : { "root_cert", "active_cert", "marketplace_cert", "release_cert", "registrar_cert" }) gone(n, "Certificate");
        gone("embassy", "Embassy");

        // Later: one more key, certified by the active key rebuilt from three custodians.
        std::string certify = slurp(std::string(ETCS_ACE_ROOT) + "/modules/CryptProvider/scripts/certify.etcs");
        const std::string pub2 = fresh_dir();
        sig::Key later{};
        tr::random_bytes(seed, 32);
        mc::crypto_ed25519_key_pair(secret, later.data(), seed);
        mc::crypto_wipe(secret, 64);
        put(certify, "/media/custodian-a", dirs[1]);
        put(certify, "/media/custodian-b", dirs[3]);
        put(certify, "/media/custodian-c", dirs[4]);
        put(certify, "PUBLIC_DIR", pub2);
        put(certify, "ROLE", "registrar");
        put(certify, "SUBJECT_KEY", hex(later.data(), 32));
        ETCS::ExecutionContext c3(&root, &ctx);
        std::istringstream cin_(certify);
        const bool certified = ETCS::run_script(cin_, std::string(ETCS_ACE_ROOT) + "/modules/CryptProvider/scripts/certify.etcs", c3);
        std::vector<sig::Certificate> reg;
        check(certified && tr::read_certificates(pub2 + "/registrar.cert", reg)
              && tr::read_certificates(pub_dir + "/active.cert", reg) && tr::read_certificates(pub_dir + "/root.cert", reg)
              && tr::registration_valid(reg, later) && reg.back().subject == root_pub,
              "certify.etcs: three custodians rebuild the active key, which certifies one more key under the same root");
        auto gone3 = [&](const char* name, const char* tag) {
            if (c3.names.count(name)) verb(ETCS::etcs_resolve_rid_anywhere(&ETCS::getLoader(), c3.names[name].rid), std::string(tag) + ".Delete");
        };
        for (const char* n : { "a", "b", "c", "out" }) gone3(n, "Custody");
        for (const char* n : { "to_a", "to_b", "to_c", "to_out" }) gone3(n, "FileRelation");
        for (const char* n : { "active_cert", "cert" }) gone3(n, "Certificate");
        gone3("embassy", "Embassy");

        // Another root, attested by this one: rebuilt from three custodians, it
        // certifies the other's key as a root -- the continuation the other's
        // chains carry so a loader built with this root takes them.
        std::string attest = slurp(std::string(ETCS_ACE_ROOT) + "/modules/CryptProvider/scripts/attest_root.etcs");
        const std::string pub3 = fresh_dir();
        sig::Key other_root{};
        tr::random_bytes(seed, 32);
        mc::crypto_ed25519_key_pair(secret, other_root.data(), seed);
        mc::crypto_wipe(secret, 64);
        put(attest, "/media/custodian-a", dirs[0]);
        put(attest, "/media/custodian-b", dirs[2]);
        put(attest, "/media/custodian-c", dirs[4]);
        put(attest, "PUBLIC_DIR", pub3);
        put(attest, "OTHER_ROOT_KEY", hex(other_root.data(), 32));
        ETCS::ExecutionContext c4(&root, &ctx);
        std::istringstream ain(attest);
        const bool attested = ETCS::run_script(ain, std::string(ETCS_ACE_ROOT) + "/modules/CryptProvider/scripts/attest_root.etcs", c4);
        std::vector<sig::Certificate> cross;
        check(attested && tr::read_certificates(pub3 + "/attested.cert", cross) && cross.size() == 1
              && cross[0].role == sig::Role::Root && cross[0].subject == other_root && cross[0].issuer == root_pub
              && tr::check(cross[0]),
              "attest_root.etcs: three custodians rebuild the root, which certifies another root's key as a root");
        auto gone4 = [&](const char* name, const char* tag) {
            if (c4.names.count(name)) verb(ETCS::etcs_resolve_rid_anywhere(&ETCS::getLoader(), c4.names[name].rid), std::string(tag) + ".Delete");
        };
        for (const char* n : { "a", "b", "c", "out" }) gone4(n, "Custody");
        for (const char* n : { "to_a", "to_b", "to_c", "to_out" }) gone4(n, "FileRelation");
        for (const char* n : { "root_cert", "attested" }) gone4(n, "Certificate");
        gone4("embassy", "Embassy");
    }

    // -- 7. a key kept ------------------------------------------------------------------
    std::cout << "\n-- 7. a key kept --\n";
    {
        Capture cap(dir + "/kept.log");
        const bool made = run("spawn DatabaseProvider::Keystore vault\n"
                              "vault.Connect(crypt-test)\n"
                              "vault.Forget()\n"
                              "spawn DatabaseProvider::LocalDatabase plain\n"
                              "spawn CryptProvider::Keypair kept\n"
                              "kept.Generate()\n"
                              "spawn CryptProvider::Keypair again\n");
        ETCS::Entity* vault = entity("vault"); ETCS::Entity* kept = entity("kept"); ETCS::Entity* again = entity("again");
        const std::string vrid = vault ? std::to_string(vault->getRID()) : "0";
        const std::string prid = entity("plain") ? std::to_string(entity("plain")->getRID()) : "0";
        const bool is_private = made && vault && vault->hasTag(ETCS::Buffer("Private"))
                             && vault->getInterfacePointer(ETCS::Buffer("Database")) && priv(vault);
        const bool empty_first = !yes(verb(vault, "Keystore.Holds"));
        const bool plain_refused = !yes(verb(kept, "Keypair.StoreTo", prid));
        const bool stored = yes(verb(kept, "Keypair.StoreTo", vrid)) && yes(verb(vault, "Keystore.Holds"));
        const bool not_twice = !yes(verb(kept, "Keypair.StoreTo", vrid));
        const bool loaded = yes(verb(again, "Keypair.LoadFrom", vrid))
                         && verb(again, "Keypair.PublicKey") == verb(kept, "Keypair.PublicKey");
        const bool held_refused = !yes(verb(again, "Keypair.LoadFrom", vrid));
        std::string seed;
        priv(vault)->Reveal(seed);
        ETCS::Buffer sql("CREATE TABLE t(x);");
        const bool no_sql = !static_cast<Database_*>(vault->getInterfacePointer(ETCS::Buffer("Database")))->ExecuteRaw(sql);
        const bool forgot = yes(verb(vault, "Keystore.Forget")) && !yes(verb(vault, "Keystore.Holds"));
        const std::string log = cap.done();
        check(is_private, "a Keystore is a Database that claims Private");
        check(empty_first && plain_refused, "a key is not kept in a Database that does not");
        check(stored, "a Keypair is kept in the Keystore");
        check(not_twice, "a kept secret is never replaced");
        check(loaded, "another Keypair takes it back: the same key");
        check(held_refused, "a Keypair that holds a key takes none");
        check(seed.size() == 64 && log.find(seed) == std::string::npos, "the seed is in no log");
        check(no_sql, "a Keystore runs no SQL");
        check(forgot, "Forget erases it");
        run("vault.Delete()\nplain.Delete()\nagain.Delete()\n");
    }

    // -- 8. the envelope ----------------------------------------------------------------
    std::cout << "\n-- 8. the envelope --\n";
    {
        namespace ks = ETCS::keystore;
        const std::string store = ETCS::etcs_store_dir();
        std::string kw, raw, nothing;
        ks::erase(store, "script/crypt-sealed", kw);
        ks::erase(store, "script/crypt-nokey", kw);
        check(run("spawn DatabaseProvider::Keystore sealed\n"
                  "sealed.Connect(crypt-sealed)\n"
                  "sealed.spawn(CryptProvider::Envelope env)\n"
                  "env.spawn(CryptProvider::Keypair envkey)\n"
                  "envkey.Generate()\n"
                  "spawn DatabaseProvider::Keystore bare\n"
                  "bare.Connect(crypt-sealed)\n"
                  "spawn DatabaseProvider::Keystore other\n"
                  "other.Connect(crypt-sealed)\n"
                  "other.spawn(CryptProvider::Envelope env2)\n"
                  "env2.spawn(CryptProvider::Keypair envkey2)\n"
                  "envkey2.Generate()\n"
                  "spawn DatabaseProvider::Keystore nokey\n"
                  "nokey.Connect(crypt-nokey)\n"
                  "nokey.spawn(CryptProvider::Envelope env3)\n"
                  "spawn CryptProvider::Keypair opened\n"
                  "spawn CryptProvider::Keypair shut\n"),
              "a Keystore under an Envelope whose key is its Keypair child; no line gives a key");
        ETCS::Entity* kept = entity("kept");
        auto rid = [&](const char* n) { return entity(n) ? std::to_string(entity(n)->getRID()) : std::string("0"); };

        // The stage itself, on a frame: what Edge and MirrorBuffer would hand it.
        ETCS::IWireWrapper* env = static_cast<ETCS::IWireWrapper*>(entity("env")->getInterfacePointer(ETCS::Buffer("Wrapper")));
        ETCS::IWireWrapper* env2 = static_cast<ETCS::IWireWrapper*>(entity("env2")->getInterfacePointer(ETCS::Buffer("Wrapper")));
        ETCS::IWireWrapper* env3 = static_cast<ETCS::IWireWrapper*>(entity("env3")->getInterfacePointer(ETCS::Buffer("Wrapper")));
        const std::string plain = "a frame that leaves the process";
        ETCS::MBuffer io, io2, io3;
        io.writeRaw(plain.data(), plain.size());
        env->Wrap(io, ETCS::SignalContext{});
        check(!ETCS::wire_refused(io) && io.written == plain.size() + 40
              && std::string(io.buf, io.written).find(plain) == std::string::npos,
              "Wrap seals: 40 bytes longer, and the payload is not in it");
        io2 = io; io3 = io;
        env->Unwrap(io, ETCS::SignalContext{});
        check(!ETCS::wire_refused(io) && std::string(io.buf, io.written) == plain, "Unwrap opens it");
        io2.buf[45] ^= 0x01;
        env->Unwrap(io2, ETCS::SignalContext{});
        check(ETCS::wire_refused(io2), "one changed byte is refused");
        env2->Unwrap(io3, ETCS::SignalContext{});
        check(ETCS::wire_refused(io3), "an Envelope with another key refuses it");
        ETCS::MBuffer io4;
        io4.writeRaw(plain.data(), plain.size());
        env3->Wrap(io4, ETCS::SignalContext{});
        check(ETCS::wire_refused(io4), "an Envelope with no key refuses to wrap: nothing leaves in the clear");
        check(yes(verb(entity("env"), "Envelope.Holds")) && !yes(verb(entity("env3"), "Envelope.Holds")),
              "the handshake happened where there was a key, and only there");

        // Guarding a Keystore: its stages stand between it and the OS.
        Capture cap(dir + "/sealed.log");
        const bool stored = yes(verb(kept, "Keypair.StoreTo", rid("sealed")));
        const bool wrapped = ks::load(store, "script/crypt-sealed", raw, kw) == ks::Status::Ok && raw.compare(0, 3, "w1:") == 0;
        const bool bare_refused = !yes(verb(entity("shut"), "Keypair.LoadFrom", rid("bare")));
        const bool other_refused = !yes(verb(entity("shut"), "Keypair.LoadFrom", rid("other")));
        const bool same_opens = yes(verb(entity("opened"), "Keypair.LoadFrom", rid("sealed")))
                             && verb(entity("opened"), "Keypair.PublicKey") == verb(kept, "Keypair.PublicKey");
        const bool nokey_refused = !yes(verb(kept, "Keypair.StoreTo", rid("nokey")))
                                && ks::load(store, "script/crypt-nokey", nothing, kw) == ks::Status::Absent;
        const std::string log = cap.done();
        check(stored && wrapped, "a key kept there is in the OS keystore sealed, not bare");
        check(bare_refused, "an entity that names the same secret without the Envelope is refused");
        check(other_refused, "and so is one whose Envelope has another key");
        check(same_opens, "the entity that carries it takes the same key back");
        check(nokey_refused, "under an Envelope with no key, nothing is kept at all");
        check(log.find(raw.substr(3)) == std::string::npos, "the sealed form is in no log either");
        verb(entity("sealed"), "Keystore.Forget");
        run("sealed.Delete()\nbare.Delete()\nother.Delete()\nnokey.Delete()\nopened.Delete()\nshut.Delete()\nkept.Delete()\n");
    }

    // -- 9. a test authority, as written -----------------------------------------------
    std::cout << "\n-- 9. a test authority --\n";
    {
        namespace tr = ETCS::trust;
        uint8_t seed[32], secret[64];
        ETCS::sig::Key keys[3] = {};                            // marketplace, release, registrar: three loaders
        for (ETCS::sig::Key& k : keys) { tr::random_bytes(seed, 32); mc::crypto_ed25519_key_pair(secret, k.data(), seed); }
        const std::string out = dir + "/authority";
        ::mkdir(out.c_str(), 0700);
        std::string script = slurp(std::string(ETCS_ACE_ROOT) + "/modules/CryptProvider/scripts/test_authority.etcs");
        auto put = [&](const std::string& what, const std::string& with) {
            for (size_t at = script.find(what); at != std::string::npos; at = script.find(what, at + with.size()))
                script.replace(at, what.size(), with);
        };
        put("AUTHORITY_DIR", out);
        put("MARKETPLACE_KEY", hex(keys[0].data(), 32));
        put("RELEASE_KEY", hex(keys[1].data(), 32));
        put("REGISTRAR_KEY", hex(keys[2].data(), 32));
        ETCS::ExecutionContext c3(&root, &ctx);                 // a root script, as it will be run
        std::istringstream in(script);
        check(ETCS::run_script(in, std::string(ETCS_ACE_ROOT) + "/modules/CryptProvider/scripts/test_authority.etcs", c3),
              "test_authority.etcs runs as written, with its placeholders filled");
        const char* names[3] = { "marketplace", "release", "registrar" };
        const ETCS::sig::Role roles[3] = { ETCS::sig::Role::Marketplace, ETCS::sig::Role::Release, ETCS::sig::Role::Registrar };
        std::vector<ETCS::sig::Certificate> reg[3];
        bool read = true, shaped = true, valid = true, crossed = false;
        for (int i = 0; i < 3; ++i)
        {
            read = read && tr::read_certificates(out + "/" + names[i] + ".cert", reg[i])
                        && tr::read_certificates(out + "/active.cert", reg[i])
                        && tr::read_certificates(out + "/root.cert", reg[i]);
            shaped = shaped && reg[i].size() == 3 && reg[i][0].role == roles[i]
                     && reg[i][1].role == ETCS::sig::Role::Intermediate && reg[i][2].role == ETCS::sig::Role::Root;
        }
        check(read && shaped && reg[0][1].subject == reg[1][1].subject && reg[1][1].subject == reg[2][1].subject,
              "it leaves marketplace, release and registrar certificates under one active key, and the root's");
        for (int i = 0; read && shaped && i < 3; ++i)
        {
            valid = valid && tr::registration_valid(reg[i], keys[i]);
            crossed = crossed || tr::registration_valid(reg[i], keys[(i + 1) % 3]);
        }
        check(read && shaped && valid && !crossed,
              "each is a registration of its own loader's key and of no other, as the loader will check it");
        auto forgotten = [&](const char* n) {
            ETCS::Entity* e = c3.names.count(n) ? ETCS::etcs_resolve_rid_anywhere(&ETCS::getLoader(), c3.names[n].rid) : nullptr;
            return !e || !yes(verb(e, "Keypair.Holds"));
        };
        check(forgotten("super_root") && forgotten("active"), "and neither key outlives it");
    }

    // -- 10. the registrar -------------------------------------------------------------
    std::cout << "\n-- 10. the registrar --\n";
    {
        namespace sig = ETCS::sig;
        namespace tr = ETCS::trust;
        check(run("spawn CryptProvider::Keypair reg_root\nreg_root.Generate()\n"
                  "spawn CryptProvider::Keypair reg_active\nreg_active.Generate()\n"
                  "spawn CryptProvider::Keypair reg_key\nreg_key.Generate()\n"
                  "spawn CryptProvider::Keypair reg_secret\nreg_secret.Generate()\n"
                  "spawn CryptProvider::Keypair reg_other_secret\nreg_other_secret.Generate()\n"
                  "spawn CryptProvider::Certificate reg_rc\nreg_rc.Issue(@reg_root root @reg_root)\n"
                  "spawn CryptProvider::Certificate reg_ic\nreg_ic.Issue(@reg_root intermediate @reg_active)\n"
                  "spawn CryptProvider::Certificate reg_gc\nreg_gc.Issue(@reg_active registrar @reg_key)\n"
                  "spawn CryptProvider::Certificate reg_mc\nreg_mc.Issue(@reg_active marketplace @reg_key)\n"
                  "spawn DatabaseProvider::LocalDatabase reg_db\nreg_db.Connect(" + dir + "/accounts.db)\n"
                  "spawn CryptProvider::Registrar pki\n"), "a registrar, its key, its secret, its accounts, and an authority above it");
        ETCS::Entity* pki = entity("pki");
        auto rid = [&](const char* n) { return entity(n) ? std::to_string(entity(n)->getRID()) : std::string("0"); };
        check(pki && !yes(verb(pki, "Registrar.Above", rid("reg_gc"))), "nothing goes above it before it uses a key");
        check(pki && yes(verb(pki, "Registrar.Use", rid("reg_key"))), "it uses a Keypair");
        check(pki && !yes(verb(pki, "Registrar.Above", rid("reg_mc"))) && !yes(verb(pki, "Registrar.Above", rid("reg_ic"))),
              "a marketplace certificate for its key, or one for another key, does not go above it");
        check(pki && route(pki, "pki/registrar").find("not set up") != std::string::npos,
              "until the chain above reaches a root, it is not set up");
        check(pki && yes(verb(pki, "Registrar.Above", rid("reg_gc"))) && yes(verb(pki, "Registrar.Above", rid("reg_ic")))
              && yes(verb(pki, "Registrar.Above", rid("reg_rc"))), "registrar <- active <- root go above it, in order");
        const std::string key = pki ? verb(pki, "Registrar.Key") : "";
        check(pki && key.size() == 64 && route(pki, "pki/registrar") == key + "\n"
              && key == verb(entity("reg_key"), "Keypair.PublicKey"), "it says its key");

        struct Loader { uint8_t secret[64]; sig::Key pub{}; };
        auto fresh = [&] { Loader l; uint8_t seed[32]; tr::random_bytes(seed, 32); mc::crypto_ed25519_key_pair(l.secret, l.pub.data(), seed); return l; };
        auto sign = [&](const Loader& l, const std::string& m) {
            uint8_t sg[64];
            mc::crypto_ed25519_sign(sg, l.secret, reinterpret_cast<const uint8_t*>(m.data()), m.size());
            return hex(sg, 64);
        };
        Loader A = fresh();
        sig::Key registrar{}, root{}, other{}, head{}, stranger{};
        sig::Epoch E = tr::loader_epoch(), E2{};
        unhex(key, registrar.data(), 32);
        unhex(verb(entity("reg_root"), "Keypair.PublicKey"), root.data(), 32);
        head[0] = 0x4a; stranger[0] = 0x5b; E2[0] = 0x6c;
        auto ask = [&](const Loader& l, const sig::Key& of, const sig::Key& h, const sig::Epoch& e) {
            return hex(l.pub.data(), 32) + " " + hex(h.data(), 32) + " " + hex(e.data(), 32) + " "
                   + sign(l, sig::registration_statement(l.pub, of, h, e));
        };
        auto reg = [&](const std::string& words) { return route(pki, "pki/register", words); };
        const std::string words = ask(A, registrar, head, E);
        check(pki && reg(words).find("not set up for accounts") != std::string::npos,
              "it registers no one before it has a secret, accounts, and the identity and epoch it serves");
        check(pki && !yes(verb(pki, "Registrar.Accounts", rid("reg_db"))), "accounts are kept only under a secret");
        check(pki && !yes(verb(pki, "Registrar.Derive", rid("reg_key"))) && yes(verb(pki, "Registrar.Derive", rid("reg_secret"))),
              "the secret is another key than the one it signs with");
        check(pki && yes(verb(pki, "Registrar.Accounts", rid("reg_db"))), "its accounts are kept in a database");
        const bool built_in = tr::ace_identity() != sig::Key{};
        check(pki && yes(verb(pki, "Registrar.Current")) == built_in,
              built_in ? "Current takes this loader's own identity and epoch"
                       : "Current is refused by a loader with no identity built in");
        check(pki && yes(verb(pki, "Registrar.Head", hex(head.data(), 32))) && yes(verb(pki, "Registrar.Epoch", hex(E.data(), 32)))
              && !yes(verb(pki, "Registrar.Epoch", "nothing")), "it takes the identity and epoch it serves");

        const std::string answer = pki ? reg(words) : "";
        std::vector<sig::Certificate> chain;
        check(tr::parse_certificates(answer, chain) && chain.size() == 5
              && chain[0].role == sig::Role::Loader && chain[0].subject == A.pub
              && chain[1].role == sig::Role::Identity && chain[0].issuer == chain[1].subject && chain[1].issuer == registrar
              && chain[1].epochs.size() == 1 && chain[1].epochs[0] == E && chain[2].role == sig::Role::Registrar,
              "a loader's signed ask gets its key certified by its ACE identity, and that identity certified for the epoch");
        const sig::Key K = chain.size() == 5 ? chain[1].subject : sig::Key{};
        check(chain.size() == 5 && tr::registration_valid(chain, A.pub) && chain.back().subject == root,
              "which the loader takes as its registration under that root");
        check(chain.size() == 5 && tr::registration_current(chain, E) && !tr::registration_current(chain, E2),
              "and holds in that epoch, and in no other");
        check(pki && reg(words) == answer, "the same ask gets the same answer");
        check(pki && reg(ask(A, registrar, stranger, E)) == "refused: a loader of an identity or epoch this registrar does not serve\n"
              && reg(ask(A, registrar, head, E2)).rfind("refused: a loader of an identity", 0) == 0,
              "a loader of an identity or an epoch it does not serve is an outsider, and refused");
        std::vector<sig::Certificate> again;
        verb(pki, "Registrar.Epoch", hex(E2.data(), 32));
        const std::string answer2 = pki ? reg(ask(A, registrar, head, E2)) : "";
        check(pki && tr::parse_certificates(answer2, again) && again.size() == 5
              && again[1].subject == K && again[1].epochs[0] == E2,
              "the same key, in another epoch it serves: the same ACE identity, certified for that epoch");
        Loader B = fresh();
        std::vector<sig::Certificate> theirs;
        check(pki && tr::parse_certificates(reg(ask(B, registrar, head, E)), theirs) && theirs.size() == 5
              && theirs[1].subject != K, "another key: another ACE identity -- one for one");
        std::string bad = words;
        bad[bad.size() - 1] = bad.back() == '0' ? '1' : '0';
        check(pki && reg(bad).rfind("refused", 0) == 0, "an ask the key did not sign is refused");
        other[0] = 1;
        check(pki && reg(ask(A, other, head, E)).rfind("refused", 0) == 0, "an ask made of another registrar is refused here");
        check(pki && route(pki, "pki/register/" + hex(A.pub.data(), 32) + "/" + std::string(128, '0')) == "refused: a word is POSTed to pki/register, as the loader said it\n"
              && reg(hex(A.pub.data(), 32) + " " + std::string(128, '0')) == "refused: not the words register takes\n",
              "an old loader's GET, or an ask without its identity and epoch, is refused and told why");
        ETCS::Buffer pa("Registrar.Request"), pd("pki/registrar");
        if (pki) pki->call(pa, pd);
        check(pki && pd.toString() == "refused: a registrar is added with HttpServer.AddRequestRoute\n",
              "added as a path route, it says how it is added");
        check(pki && route(pki, "pki/nothing") == "no such path\n" && route(pki, "chess/registrar") == "no such path\n",
              "a path it does not serve is answered as one");
        ETCS::Buffer fa("Registrar.Filter"), f1("pki/register/x"), f2("chess/pki");
        if (pki) { pki->call(fa, f1); pki->call(fa, f2); }
        check(pki && f1.toString() == "pki" && f2.written == 0, "its filter claims the paths under its mount and no others");

        // -- 11. accounts --
        std::cout << "\n-- 11. accounts --\n";
        auto admit = [&](const Loader& m, const Loader& k, bool member_word = true, bool joining_word = true,
                         const sig::Epoch* me = nullptr, const sig::Epoch* ke = nullptr) {
            const std::string ms = member_word ? sign(m, sig::admit_statement(m.pub, k.pub, registrar, me ? *me : E)) : std::string(128, '0');
            const std::string ks = joining_word ? sign(k, sig::join_statement(k.pub, m.pub, registrar, ke ? *ke : E)) : std::string(128, '0');
            return route(pki, "pki/admit", hex(k.pub.data(), 32) + " " + ks + " " + hex(m.pub.data(), 32) + " " + ms);
        };
        auto retire = [&](const Loader& m, const Loader& k) {
            return route(pki, "pki/retire", hex(k.pub.data(), 32) + " " + hex(m.pub.data(), 32) + " "
                                            + sign(m, sig::retire_statement(m.pub, k.pub, registrar)));
        };
        sig::Epoch E3{};
        E3[0] = 0x7d;
        Loader P = fresh();                                             // a paper key: never a loader, only a member
        check(pki && admit(A, P, true, false).rfind("refused: the joining key's word", 0) == 0,
              "a member alone admits no key: every key is public, and a key agrees to join");
        check(pki && admit(A, P, false, true).rfind("refused: the member's word does not verify", 0) == 0,
              "nor does a key's wish to join admit it");
        check(pki && admit(A, P, true, true, &E3, &E3).rfind("refused: the member's word does not verify", 0) == 0
              && admit(A, P, true, true, &E, &E2).rfind("refused: the joining key's word", 0) == 0,
              "words of an epoch it does not serve, or of two epochs, admit no one");
        check(pki && admit(P, A).rfind("refused: the admitting key is not a current member", 0) == 0, "only a member admits");
        check(pki && admit(A, P) == "admitted\n" && admit(A, P, true, true, &E2, &E2) == "admitted\n",
              "with both words in one epoch it serves, a key holding no registration joins at once -- a paper key as well as a loader's");
        check(pki && admit(A, B) == "admitted at its next registration\n",
              "a key holding a registration moves only at its next one: an account holds still under a live certificate");
        check(pki && retire(P, B) == "refused: not keys of one account\n" && reg(ask(B, registrar, head, E)).rfind("refused", 0) != 0
              && retire(P, B) == "refused: not keys of one account\n",
              "so until then it is not the account's, and registering again in the same epoch moves nothing");
        check(pki && reg(ask(B, registrar, head, E2)).rfind("refused", 0) != 0 && retire(P, B) == "retired\n",
              "its registration in a later epoch, on its own word, is where it moves");
        Loader C = fresh(), D = fresh();
        reg(ask(C, registrar, head, E));
        check(pki && admit(C, D) == "admitted\n" && admit(A, D) == "refused: that key shares another account; a member there retires it first\n",
              "a key that shares an account with another does not move");
        check(pki && retire(C, A) == "refused: not keys of one account\n", "a member retires only within its own account");
        check(pki && retire(P, B) == "retired\n", "retiring twice changes nothing");
        check(pki && reg(ask(B, registrar, head, E2)) == "refused: this key was retired from its account\n",
              "a retired key is registered no more");
        check(pki && retire(B, A) == "refused: the retiring key is not a current member\n" && admit(B, C).rfind("refused", 0) == 0,
              "and has no word in its account");
        check(pki && admit(A, B) == "refused: that key was retired, and comes back to no account\n", "nor comes back to it");
        check(pki && reg(words) == answer, "the members still current are registered as before");
        check(pki && yes(verb(pki, "Registrar.Epoch", hex(E3.data(), 32))) && reg(ask(C, registrar, head, E)).rfind("refused: a loader of an identity", 0) == 0
              && reg(ask(C, registrar, head, E3)).rfind("refused", 0) != 0,
              "given a new epoch it serves that one and the last: the oldest closes");

        // -- 12. the portal --
        std::cout << "\n-- 12. the portal --\n";
        auto portal = [&](const std::string& verb, const std::string& words) { return route(pki, "pki/portal/" + verb, words, "POST"); };
        auto open = [&](std::string& code, std::string& token) {
            const std::string o = portal("open", "");
            const size_t sp = o.find(' ');
            if (o.size() != 64 + 1 + 64 + 1 || sp != 64) return false;
            code = o.substr(0, 64); token = o.substr(65, 64);
            return true;
        };
        auto word = [&](const Loader& m, const std::string& code) {
            sig::Key c{};
            unhex(code, c.data(), 32);
            return hex(m.pub.data(), 32) + " " + sign(m, sig::portal_statement(m.pub, registrar, c));
        };
        std::string code, token, code2, token2;
        check(pki && open(code, token) && open(code2, token2) && code != code2 && token != token2,
              "a portal opens with a code to show and a token to keep, new each time");
        check(portal("state", token) == "pending\n" && portal("members", token) == "refused: the session is not signed in\n",
              "until a member's word comes, it is pending and sees nothing");
        Loader X = fresh();
        check(portal("signin", code + " " + word(X, code)) == "refused: that key is not a current member of any account\n"
              && portal("signin", code + " " + word(B, code)) == "refused: that key is not a current member of any account\n",
              "no word but a current member's signs it in: a stranger's or a retired key's");
        check(portal("signin", code + " " + word(A, code2)).rfind("refused: the word does not verify", 0) == 0,
              "a member's word on another code does not");
        check(portal("signin", code + " " + word(A, code)) == "signed in\n"
              && portal("state", token) == "signed in " + hex(A.pub.data(), 32) + "\n"
              && portal("signin", code + " " + word(A, code)) == "refused: no portal is waiting on that code\n",
              "a member's word on its code signs it in, once");
        const std::string listed = portal("members", token);
        auto line_of = [&](const sig::Key& k) {
            const std::string h = hex(k.data(), 32);
            const size_t at = listed.find(h);
            return at == std::string::npos ? std::string() : listed.substr(at, listed.find('\n', at) - at);
        };
        check(line_of(A.pub) == hex(A.pub.data(), 32) + " current " + hex(E2.data(), 32) + " you"
              && line_of(P.pub) == hex(P.pub.data(), 32) + " current -"
              && line_of(B.pub).rfind(hex(B.pub.data(), 32) + " retired ", 0) == 0
              && line_of(C.pub).empty() && line_of(D.pub).empty(),
              "it sees its account's keys -- current, retired, where each was last registered -- and no other account's");
        Loader N = fresh();
        auto joins = [&](const Loader& k, const Loader& m, const sig::Epoch& e) {
            return hex(k.pub.data(), 32) + " " + sign(k, sig::join_statement(k.pub, m.pub, registrar, e));
        };
        check(portal("admit", token + " " + joins(N, P, E3)).rfind("refused: that key's word does not name this member", 0) == 0
              && portal("admit", token + " " + joins(N, A, E)).rfind("refused: that key's word does not name this member", 0) == 0,
              "a key's word to join someone else, or in an epoch no longer served, admits nothing");
        check(portal("admit", token + " " + joins(N, A, E3)) == "admitted\n"
              && portal("members", token).find(hex(N.pub.data(), 32) + " current -") != std::string::npos,
              "with the key's own word naming this member, the session stands for the member's: the key joins");
        check(portal("retire", token + " " + hex(N.pub.data(), 32)) == "retired\n"
              && portal("members", token).find(hex(N.pub.data(), 32) + " retired") != std::string::npos
              && portal("retire", token + " " + hex(D.pub.data(), 32)) == "refused: not keys of one account\n",
              "and for its word to retire one of the account's keys, and no other's");
        Loader F = fresh();
        std::string code3, token3;
        reg(ask(F, registrar, head, E3));
        check(open(code3, token3) && portal("signin", code3 + " " + word(F, code3)) == "signed in\n"
              && portal("retire", token3 + " " + hex(F.pub.data(), 32)) == "retired\n"
              && portal("state", token3) == "refused: no such session\n",
              "a session that retires its own member ends with it");
        check(portal("close", token) == "closed\n" && portal("members", token) == "refused: no such session\n"
              && portal("state", std::string(64, '0')) == "refused: no such session\n",
              "closed, a session is gone; a token never opened is none");
        check(yes(verb(pki, "Registrar.SessionLife", "1")) && open(code3, token3)
              && (::usleep(1200 * 1000), portal("state", token3) == "refused: no such session\n")
              && yes(verb(pki, "Registrar.SessionLife", "900")),
              "a session runs out after its life");
        {
            // A member's ninth session drops its first; a full table gives way rather than shut.
            Loader S = fresh();
            reg(ask(S, registrar, head, E3));
            std::string c[9], t[9];
            bool in = true;
            for (int i = 0; i < 9; ++i) in = in && open(c[i], t[i]) && portal("signin", c[i] + " " + word(S, c[i])) == "signed in\n";
            check(in && portal("state", t[0]) == "refused: no such session\n" && portal("state", t[1]).rfind("signed in ", 0) == 0
                  && portal("state", t[8]).rfind("signed in ", 0) == 0,
                  "a member holds eight sessions: a ninth takes the place of its first");
            std::string first_code, first_token, cx, tx;
            bool opened = open(first_code, first_token);
            for (int i = 0; i < 1100 && opened; ++i) opened = open(cx, tx);
            check(opened && portal("state", first_token) == "refused: no such session\n" && portal("state", tx) == "pending\n"
                  && portal("state", t[8]).rfind("signed in ", 0) == 0,
                  "opening sessions without end shuts no one out: the oldest still waiting goes first, and one signed in stays");
            check(reg(hex(B.pub.data(), 32) + " " + hex(head.data(), 32) + " " + hex(E3.data(), 32) + " " + std::string(128, '0'))
                  == "refused: not a loader key's registration with this registrar\n",
                  "whether a key was retired is told to its own word only");
            mc::crypto_wipe(S.secret, 64);
        }
        Loader U = fresh(), V = fresh();
        reg(ask(U, registrar, head, E2)); reg(ask(U, registrar, head, E3)); reg(ask(V, registrar, head, E3));
        check(pki && admit(V, U, true, true, &E3, &E3) == "admitted at its next registration\n",
              "a key registered in the newest epoch, admitted elsewhere, waits for a later one");
        mc::crypto_wipe(X.secret, 64); mc::crypto_wipe(N.secret, 64); mc::crypto_wipe(F.secret, 64);

        const std::string rows = slurp(dir + "/accounts.db");
        const auto has = [&](const sig::Key& k) { return rows.find(std::string(reinterpret_cast<const char*>(k.data()), 32)) != std::string::npos; };
        check(!rows.empty() && !has(A.pub) && !has(B.pub) && !has(P.pub) && !has(K),
              "the rows hold no loader's key and no ACE identity -- the keys the portal lists are sealed: "
              "nothing in them says whose keys share an account");

        check(run("spawn DatabaseProvider::LocalDatabase reg_db2\nreg_db2.Connect(" + dir + "/accounts.db)\n"
                  "spawn DatabaseProvider::LocalDatabase reg_db3\nreg_db3.Connect(" + dir + "/accounts.db)\n"
                  "spawn CryptProvider::Registrar pki2\npki2.Use(@reg_key)\n"
                  "pki2.Above(@reg_gc)\npki2.Above(@reg_ic)\npki2.Above(@reg_rc)\n"
                  "spawn CryptProvider::Registrar pki3\npki3.Use(@reg_key)\n"), "another registrar on the same accounts");
        ETCS::Entity* pki2 = entity("pki2");
        ETCS::Entity* pki3 = entity("pki3");
        check(pki3 && yes(verb(pki3, "Registrar.Derive", rid("reg_other_secret"))) && !yes(verb(pki3, "Registrar.Accounts", rid("reg_db3"))),
              "accounts kept under one secret are not read under another");
        check(pki2 && yes(verb(pki2, "Registrar.Derive", rid("reg_secret"))) && yes(verb(pki2, "Registrar.Accounts", rid("reg_db2")))
              && yes(verb(pki2, "Registrar.Head", hex(head.data(), 32))),
              "under the same secret they are read back");
        std::swap(pki, pki2);
        check(pki && reg(ask(A, registrar, head, E2)) == answer2 && reg(ask(B, registrar, head, E2)) == "refused: this key was retired from its account\n"
              && admit(A, P, true, true, &E2, &E2) == "admitted\n" && admit(A, D, true, true, &E3, &E3).rfind("refused: that key shares", 0) == 0
              && reg(words).rfind("refused: a loader of an identity", 0) == 0,
              "as they were left: the same ACE identities, members, retirements, and epochs served");
        sig::Epoch E4{};
        E4[0] = 0x7e;
        check(pki && reg(ask(U, registrar, head, E2)).rfind("refused", 0) != 0 && retire(V, U) == "refused: not keys of one account\n",
              "asked again in an earlier epoch still served -- of a registrar that has answered nothing yet -- its registration moves nothing");
        check(pki && yes(verb(pki, "Registrar.Epoch", hex(E4.data(), 32))) && reg(ask(U, registrar, head, E4)).rfind("refused", 0) != 0
              && retire(V, U) == "retired\n",
              "the later epoch's is where it moves");
        mc::crypto_wipe(U.secret, 64); mc::crypto_wipe(V.secret, 64);
        std::swap(pki, pki2);
        mc::crypto_wipe(A.secret, 64); mc::crypto_wipe(B.secret, 64); mc::crypto_wipe(P.secret, 64);
        mc::crypto_wipe(C.secret, 64); mc::crypto_wipe(D.secret, 64);
        run("pki.Delete()\npki2.Delete()\npki3.Delete()\nreg_db.Delete()\nreg_db2.Delete()\nreg_db3.Delete()\n"
            "reg_root.Delete()\nreg_active.Delete()\nreg_key.Delete()\nreg_secret.Delete()\nreg_other_secret.Delete()\n");
    }

    ETCS::PendingUnloadRegistry::getInstance().join_all();
    std::printf("\n%s (%d failure%s)\n", g_fail ? "FAILED" : "PASSED", g_fail, g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}
