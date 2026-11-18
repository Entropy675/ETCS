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
//   6. THE CEREMONY AS WRITTEN. root_ceremony.etcs, with only its paths
//      changed, leaves every custodian a share of each key and both
//      certificates; any three custodians rebuild the root, whose public key
//      is the root certificate's subject.
//   7. A KEY KEPT. A Keypair goes whole only into a Database that claims
//      Private -- DatabaseProvider's Keystore, the OS keystore -- and comes
//      back as the same key; a LocalDatabase is refused; nothing is replaced;
//      the seed is in no log.
//   8. THE ENVELOPE. A Wrapper stage that seals, keyed by its Private child
//      and by no verb: a changed byte, another key, or no key is a refusal.
//      Standing on a Keystore, it makes naming a secret not enough to read it.
//   9. A TEST AUTHORITY AS WRITTEN. test_authority.etcs leaves a marketplace
//      certificate, its active key's and a root's: a registration the loader
//      accepts for that key, from two keys that do not outlive the script.
//
//   ./Run_CryptProviderTesterLoader
//
// No display, no network: the custodians are directories under /tmp.

#define ETCS_MONOCYPHER_IMPLEMENTATION
#include "../ETCS.h"
#include "../libs/monocypher.h"
#include "../core/Signature.h"

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
              "spawn CryptProvider::Certificate bad_root\n"
              "spawn CryptProvider::Certificate self_im\n"), "keys and certificates spawn");
    auto rid = [&](const char* n) { return std::to_string(entity(n)->getRID()); };
    check(yes(verb(entity("rc"), "Certificate.Issue", rid("r") + " root " + rid("r")))
          && yes(verb(entity("rc"), "Certificate.Verify")), "a root is its own issuer, and verifies");
    check(!yes(verb(entity("bad_root"), "Certificate.Issue", rid("r") + " root " + rid("im")))
          && !yes(verb(entity("self_im"), "Certificate.Issue", rid("im") + " intermediate " + rid("im"))),
          "a root of another key, or a non-root of its own, is refused");
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
        std::string script = slurp(std::string(ETCS_ACE_ROOT) + "/modules/CryptProvider/scripts/root_ceremony.etcs");
        std::vector<std::string> dirs;
        for (const char* who : { "a", "b", "c", "d", "e" })
        {
            char t[] = "/tmp/etcs_ceremony_XXXXXX";
            dirs.push_back(::mkdtemp(t) ? std::string(t) : std::string());
            const std::string from = std::string("/media/custodian-") + who;
            const size_t at = script.find(from);
            if (at != std::string::npos) script.replace(at, from.size(), dirs.back());
        }
        ETCS::ExecutionContext c2(&root, &ctx);
        std::istringstream in(script);
        const bool ran = ETCS::run_script(in, std::string(ETCS_ACE_ROOT) + "/modules/CryptProvider/scripts/root_ceremony.etcs", c2);
        bool all = ran;
        for (const std::string& d : dirs)
            for (const char* f : { "root.shard.private", "intermediate.shard.private", "root.cert", "intermediate.cert" })
            {
                struct stat fs{};
                all = all && ::stat((d + "/" + f).c_str(), &fs) == 0;
            }
        check(all, "the ceremony runs as written: every custodian holds both shares and both certificates");

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
        // The ceremony's Embassy is a global; the next root script names its own.
        verb(ETCS::etcs_resolve_rid_anywhere(&ETCS::getLoader(), c2.names["embassy"].rid), "Embassy.Delete");
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
        ETCS::sig::Key loader{};
        tr::random_bytes(seed, 32);
        mc::crypto_ed25519_key_pair(secret, loader.data(), seed);
        const std::string out = dir + "/authority";
        ::mkdir(out.c_str(), 0700);
        std::string script = slurp(std::string(ETCS_ACE_ROOT) + "/modules/CryptProvider/scripts/test_authority.etcs");
        auto put = [&](const std::string& what, const std::string& with) {
            for (size_t at = script.find(what); at != std::string::npos; at = script.find(what, at + with.size()))
                script.replace(at, what.size(), with);
        };
        put("AUTHORITY_DIR", out);
        put("LOADER_KEY", hex(loader.data(), 32));
        ETCS::ExecutionContext c3(&root, &ctx);                 // a root script, as it will be run
        std::istringstream in(script);
        check(ETCS::run_script(in, std::string(ETCS_ACE_ROOT) + "/modules/CryptProvider/scripts/test_authority.etcs", c3),
              "test_authority.etcs runs as written, with its two placeholders filled");
        std::vector<ETCS::sig::Certificate> reg;
        const bool read = tr::read_certificates(out + "/marketplace.cert", reg)
                       && tr::read_certificates(out + "/active.cert", reg)
                       && tr::read_certificates(out + "/root.cert", reg);
        check(read && reg.size() == 3 && reg[0].role == ETCS::sig::Role::Marketplace
              && reg[1].role == ETCS::sig::Role::Intermediate && reg[2].role == ETCS::sig::Role::Root,
              "it leaves three certificates: marketplace, active, root");
        check(read && tr::registration_valid(reg, loader, reg[2].subject),
              "which are a registration of that loader's key, as the loader will check it");
        auto forgotten = [&](const char* n) {
            ETCS::Entity* e = c3.names.count(n) ? ETCS::etcs_resolve_rid_anywhere(&ETCS::getLoader(), c3.names[n].rid) : nullptr;
            return !e || !yes(verb(e, "Keypair.Holds"));
        };
        check(forgotten("super_root") && forgotten("active"), "and neither key outlives it");
    }

    ETCS::PendingUnloadRegistry::getInstance().join_all();
    std::printf("\n%s (%d failure%s)\n", g_fail ? "FAILED" : "PASSED", g_fail, g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}
