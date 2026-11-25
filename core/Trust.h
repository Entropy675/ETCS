#ifndef TRUST_H__
#define TRUST_H__

/*
 * THE LOADER'S GATE: what a module's bytes pass before any of them is mapped.
 * Loader-only (DynamicLoader.h includes it under ETCS_LOADER); the byte
 * layouts are core/Signature.h, the key's medium core/Keystore.h.
 *
 * A WRAP CHAIN, the one the Wrapper family already is (ontology/Wrapper.h): a
 * load is a frame, a stage transforms it in place or refuses it, and a
 * refusal is a failed attach. Signing is the same chain walked the other way.
 * The loader's own stages are wired here, early -- IWireWrapper is core's, and
 * these exist before any module can provide a type -- and any Wrapper child of
 * the gate's entity (gate_entity) wraps outside them, like any chain.
 *
 *   Signature  the chain names these bytes, under this name, in this epoch,
 *              and climbs through strictly higher roles to a root. Passing,
 *              it says who signed.
 *   Own        what a signature may stand for: a loader binary is minted --
 *              the identity's marketplace key signed it -- or is this loader's
 *              own or its builder's, and passed off.
 *
 * THE LOADER IS THE ONE BINARY NOTHING ELSE CHECKS, so it attests itself: at
 * the handshake it puts its own bytes through its own two stages. A loader
 * binary signed by a loader's key attests only if the loader before it PASSED
 * IT OFF (core/Signature.h): the parent signs the one fragment it can sign for
 * into the child, and the lineage behind that is the marketplace's to attest.
 * Under a minted identity a lineage begins at a mint. A loader passed off by
 * itself there, or by one that did not attest, runs with a warning, and what
 * it signs carries its local half alone: local authority is any loader's,
 * global authority a signed build's. lineage.log is the signer's own record of
 * what it passed off.
 *
 * A CHAIN IS TWO HALVES (core/Signature.h). The local half -- this loader's key
 * and what it signs -- is taken by nothing but that loader. The half above is
 * joined on by a registration, and is what lets anyone else take what the key
 * signs: through this key's ACE identity, certified for the epoch it was
 * registered in, so a registration is this epoch's and is asked for again in
 * the next (registration_current). A root attesting another root continues a
 * chain the same way: adding to the chain is trusting it as a remote.
 *
 * THE IDENTITY is built in (ace: -DETCS_IDENTITY): the checksum folded along a
 * chain from a root down to the key that built this loader -- the minting
 * server's marketplace key, for what the site serves -- or that key alone, for
 * a builder that is not registered. It is no header, so it is in no epoch:
 * every identity's loaders share one, and differ only in what they require.
 *
 * A MODULE'S CHAIN MUST COME TO THIS LOADER'S IDENTITY, or meet its own keys:
 * the fold from one of its roots coming to the identity at some key along it
 * is the global surface. Short of that it meets this loader's key, proven at
 * the handshake; the key it was built by (ETCS_BUILDER_KEY); a certificate of
 * its registration -- and runs LOCAL: no transport bridges it
 * (MirrorBuffer::localFrame). A module signed by another loader's key with
 * nothing above it meets nothing, and is the case refused by default. With no
 * identity built in a loader is EXPERIMENTAL: nothing is held local, and
 * nothing places it among anyone else's.
 *
 * AS TLS IS HERE (NetworkProvider): the identity is the config -- built once
 * per session, read-only after, what every stage was set up against. Proving
 * it is the handshake, before anything is dispatched. A stage does no I/O: the
 * caller reads the file and seals it, the stage is handed the bytes. And a
 * gate that cannot prove what it was asked to be refuses; it does not fall
 * back to being something weaker.
 *
 * In addition to the manifest check, never instead of it: this says who
 * witnessed the bytes, that one says whether they can run here.
 */

#include "Signature.h"
#include "Provenance.h"
#include "Keystore.h"
#include "InterfaceWire.h"
#include "MirrorBuffer.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#if defined(_WIN32)
#  include <windows.h>
#  include <bcrypt.h>
#else
#  include <fcntl.h>
#  include <sys/stat.h>
#  include <unistd.h>
#  if defined(__linux__) && !defined(__EMSCRIPTEN__)
#    include <sys/mman.h>
#    include <sys/random.h>
#  endif
#endif

#ifndef ETCS_MONOCYPHER_IMPLEMENTATION
#define ETCS_MONOCYPHER_IMPLEMENTATION      // one TU per loader binary
#endif
#include "../libs/monocypher.h"

namespace ETCS
{
namespace trust
{
namespace mc = ::etcs_monocypher;

inline bool zero(const sig::Key& k)
{
    for (uint8_t c : k) if (c) return false;
    return true;
}

inline std::string hex(const uint8_t* p, size_t n)
{
    static const char* d = "0123456789abcdef";
    std::string s(n * 2, '0');
    for (size_t i = 0; i < n; ++i) { s[2 * i] = d[p[i] >> 4]; s[2 * i + 1] = d[p[i] & 15]; }
    return s;
}

inline bool unhex(const std::string& s, uint8_t* out, size_t n)
{
    if (s.size() != n * 2) return false;
    auto v = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (size_t i = 0; i < n; ++i)
    {
        const int hi = v(s[2 * i]), lo = v(s[2 * i + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = uint8_t(hi << 4 | lo);
    }
    return true;
}

#define ETCS_TRUST_STR2(x) #x
#define ETCS_TRUST_STR(x)  ETCS_TRUST_STR2(x)

/*
 * THE IDENTITY: a checksum folded along a chain from its root down, one step
 * per key --
 *
 *   h = BLAKE2b("ETCSIDN1");  h = BLAKE2b(h | role u8 | key)  for each key, root first
 *
 * -- over keys and their roles, not certificates, so reissuing a certificate
 * moves nothing and only a change of keys does. A builder's identity is the
 * fold along its own registration down to its own key: root -> active ->
 * marketplace, for the minting server; an unregistered builder's is its key
 * alone, as a loader. It is what the builder is authorized to build with, and
 * what every loader it builds requires at the head of what it takes globally.
 */
inline sig::Key fold_start()
{
    sig::Key h{};
    mc::crypto_blake2b(h.data(), 32, reinterpret_cast<const uint8_t*>("ETCSIDN1"), 8);
    return h;
}

inline sig::Key fold(const sig::Key& h, sig::Role r, const sig::Key& k)
{
    uint8_t m[65];
    std::memcpy(m, h.data(), 32);
    m[32] = uint8_t(r);
    std::memcpy(m + 33, k.data(), 32);
    sig::Key out{};
    mc::crypto_blake2b(out.data(), 32, m, sizeof m);
    return out;
}

// One key of a chain's path, its role there, and the certificate it signed
// (an index into the chain, leaf first).
struct Step { sig::Role role; sig::Key key; size_t signs; };

// The role a key without a certificate of its own must hold to have signed one
// of role `r` (core/Signature.h's table, read upward).
inline sig::Role above(sig::Role r)
{
    switch (r)
    {
        case sig::Role::Root: case sig::Role::Intermediate:                          return sig::Role::Root;
        case sig::Role::Marketplace: case sig::Role::Release: case sig::Role::Registrar: return sig::Role::Intermediate;
        case sig::Role::Loader: case sig::Role::Identity:                           return sig::Role::Registrar;
        case sig::Role::Module:                                                     return sig::Role::Loader;
    }
    return sig::Role::Loader;
}

// A chain's path, its root (or its topmost key) first, the key that signed the
// leaf last. A root's own certificate adds no step: its key is the one above.
inline std::vector<Step> path_of(const std::vector<sig::Certificate>& chain)
{
    std::vector<Step> p;
    const size_t n = chain.size();
    if (!n) return p;
    size_t top = n - 1;
    if (n > 1 && chain[top].role == sig::Role::Root && chain[top].issuer == chain[top].subject) --top;
    for (size_t i = top + 1; i-- > 0; )
        p.push_back({ i + 1 < n ? chain[i + 1].role : above(chain[i].role), chain[i].issuer, i });
    return p;
}

// A registration's path: the chain a leaf this key signed would carry.
inline std::vector<Step> path_of(const std::vector<sig::Certificate>& reg, const sig::Key& key)
{
    sig::Certificate leaf;
    leaf.role = sig::Role::Module; leaf.issuer = key;
    std::vector<sig::Certificate> chain{ leaf };
    chain.insert(chain.end(), reg.begin(), reg.end());
    return path_of(chain);
}

inline sig::Key identity_of(const std::vector<Step>& p)
{
    sig::Key h = fold_start();
    for (const Step& s : p) h = fold(h, s.role, s.key);
    return h;
}

// The identity one step short of the path's own key: what certified it. Zero
// for a key with nothing above it.
inline sig::Key parent_of(const std::vector<Step>& p)
{
    if (p.size() < 2) return sig::Key{};
    return identity_of(std::vector<Step>(p.begin(), p.end() - 1));
}

// The step at which the fold from one of the path's roots -- or its top --
// comes to `head`; -1 if it never does. A root attesting another above it
// starts a fold of its own, so it moves no one's head.
inline long head_meets(const std::vector<Step>& p, const sig::Key& head)
{
    if (zero(head)) return -1;
    for (size_t s = 0; s < p.size(); ++s)
    {
        if (s && p[s].role != sig::Role::Root) continue;
        sig::Key h = fold_start();
        for (size_t j = s; j < p.size(); ++j)
            if ((h = fold(h, p[j].role, p[j].key)) == head) return long(j);
    }
    return -1;
}

/*
 * The identity built in, passed by the build (ace: -DETCS_IDENTITY, from the
 * building store's `etcs --identity`). No header carries it, so no epoch
 * depends on it: every builder's loaders share one. Zero is none: nothing is
 * global, and nothing is held local either.
 */
/*
 * AN UNTRUSTED BUILD (-DETCS_UNTRUSTED; ace untrusted on): for rapid
 * recompiles, a loader that checks nothing. Every module loads with no
 * signature check, so nothing has to be signed between a build and a run, and
 * no keystore is asked. It has no identity, attests nothing, and signs,
 * countersigns, issues, witnesses and publishes nothing -- so nothing it built
 * reaches anyone else, and a trusted loader refuses what it left unsigned. It
 * is no header, so switching costs no epoch: the modules are the same ones.
 * And it says so all the time it runs: at start, on every load, and at every
 * prompt (trust_warn, CommandExecutor.h; the prompt's prefix there).
 */
#if defined(ETCS_UNTRUSTED)
inline constexpr bool untrusted_build = true;
#else
inline constexpr bool untrusted_build = false;
#endif

// A warning on the channel this session has: the terminal, in colour
// (defined in core/CommandExecutor.h, where the console is).
inline void trust_warn(const std::string& text);

/*
 * AN UNTRUSTED BUILD IS NEVER A SIGNED LOADER. It carries a marker, and no
 * loader signs, passes off, mints or takes as a loader binary bytes that hold
 * it -- else a minting server building with `ace untrusted on` would mint a
 * loader that checks nothing. The needle is assembled at run time, so a
 * trusted binary never holds the marker whole.
 */
#if defined(ETCS_UNTRUSTED)
[[gnu::used]] inline const char untrusted_marker_text[] = "ETCS::UNTRUSTED::BUILD::v1";
#endif

inline bool carries_untrusted_marker(const uint8_t* b, size_t n)
{
    static const unsigned char enc[] = { 0x1f, 0x0e, 0x19, 0x09, 0x60, 0x60, 0x0f, 0x14, 0x0e, 0x08, 0x0f, 0x09, 0x0e,
                                         0x1f, 0x1e, 0x60, 0x60, 0x18, 0x0f, 0x13, 0x16, 0x1e, 0x60, 0x60, 0x2c, 0x6b };
    std::string needle(sizeof enc, '\0');
    for (size_t i = 0; i < sizeof enc; ++i) needle[i] = char(enc[i] ^ 0x5a);
    return std::search(b, b + n, needle.begin(), needle.end()) != b + n;
}

inline void untrusted_banner()
{
    if (!untrusted_build) return;
#if defined(ETCS_UNTRUSTED)
    (void)*static_cast<const volatile char*>(untrusted_marker_text);
#endif
    trust_warn("UNTRUSTED BUILD (ETCS_UNTRUSTED): every module loads with NO signature check, this loader has no "
               "identity, and it signs nothing. For rapid development only -- never run what you did not build. "
               "`ace untrusted off` and rebuild for a trusted loader.");
}

inline const sig::Key& ace_identity()
{
    static const sig::Key k = [] {
        sig::Key r{};
        if (untrusted_build) return r;
#if defined(ETCS_IDENTITY)
        if (!unhex(ETCS_TRUST_STR(ETCS_IDENTITY), r.data(), r.size())) r = sig::Key{};
#endif
        return r;
    }();
    return k;
}

/*
 * THE IDENTITY'S PARENT, built in beside it (ace: -DETCS_IDENTITY_PARENT):
 * the fold one step short of the builder's key -- root -> active, for the
 * minting server. A checksum is followed only DOWN a chain: from known keys
 * to a head, never from a head back up to the keys that made it. So a loader
 * that is to recognise a sibling of its head -- another marketplace the same
 * active key certified -- is told the parent; it cannot find it.
 */
inline const sig::Key& ace_identity_parent()
{
    static const sig::Key k = [] {
        sig::Key r{};
        if (untrusted_build) return r;
#if defined(ETCS_IDENTITY_PARENT)
        if (!unhex(ETCS_TRUST_STR(ETCS_IDENTITY_PARENT), r.data(), r.size())) r = sig::Key{};
#endif
        return r;
    }();
    return k;
}

// Whether `head` is `key`'s own: the identity of an unregistered builder.
inline bool own_head(const sig::Key& head, const sig::Key& key)
{
    return !zero(key) && head == fold(fold_start(), sig::Role::Loader, key);
}

inline bool read_file(const std::string& path, std::vector<uint8_t>& out)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return !f.bad();
}

/*
 * The digest a Module certificate names: BLAKE2b-256 over
 *   "ETCSDIG1" | format u8 | name length u8 | name | the signed bytes
 * so a binary renamed is a binary unsigned.
 */
inline bool digest(const std::string& name, const uint8_t* b, size_t n, const sig::Layout& L, sig::Key& out)
{
    if (name.empty() || name.size() > 255) return false;
    mc::crypto_blake2b_ctx ctx;
    mc::crypto_blake2b_init(&ctx, 32);
    const uint8_t head[10] = { 'E', 'T', 'C', 'S', 'D', 'I', 'G', '1',
                               uint8_t(L.format), uint8_t(name.size()) };
    mc::crypto_blake2b_update(&ctx, head, sizeof head);
    mc::crypto_blake2b_update(&ctx, reinterpret_cast<const uint8_t*>(name.data()), name.size());
    static const uint8_t zeros[16] = {};
    for (const sig::Span& s : L.covered)
    {
        size_t at = s.at;
        const size_t end = s.at + s.len;
        if (end > n) return false;
        while (at < end)
        {
            size_t next = end;
            const sig::Span* in = nullptr;
            for (const sig::Span& z : L.zeroed)
            {
                if (z.at <= at && at < z.at + z.len) { in = &z; break; }
                if (z.at > at && z.at < next) next = z.at;
            }
            if (in)
            {
                const size_t stop = std::min(end, in->at + in->len);
                for (size_t k = at; k < stop; k += sizeof zeros)
                    mc::crypto_blake2b_update(&ctx, zeros, std::min(sizeof zeros, stop - k));
                at = stop;
            }
            else
            {
                mc::crypto_blake2b_update(&ctx, b + at, next - at);
                at = next;
            }
        }
    }
    mc::crypto_blake2b_final(&ctx, out.data());
    return true;
}

/*
 * The epoch: BLAKE2b-256 over "ETCSEPO1" then every contract and lib key of a
 * manifest, sorted, as key NUL value NUL. Libs are in it and only [DIFF] in
 * the manifest check -- the two layers of hashing. ace computes the same from
 * the generated hash headers (ace_sign.py), so a stale signer still signs for
 * the tree being built.
 */
inline sig::Epoch epoch_of(const std::vector<std::pair<std::string, std::string>>& entries)
{
    std::vector<std::pair<std::string, std::string>> e;
    for (const auto& kv : entries)
        for (const char* p : { "CORE:", "ONTOLOGY:", "LIB:", "HEADER:" })
            if (kv.first.rfind(p, 0) == 0) { e.push_back(kv); break; }
    std::sort(e.begin(), e.end());
    mc::crypto_blake2b_ctx ctx;
    mc::crypto_blake2b_init(&ctx, 32);
    mc::crypto_blake2b_update(&ctx, reinterpret_cast<const uint8_t*>("ETCSEPO1"), 8);
    const uint8_t nul = 0;
    for (const auto& kv : e)
    {
        mc::crypto_blake2b_update(&ctx, reinterpret_cast<const uint8_t*>(kv.first.data()), kv.first.size());
        mc::crypto_blake2b_update(&ctx, &nul, 1);
        mc::crypto_blake2b_update(&ctx, reinterpret_cast<const uint8_t*>(kv.second.data()), kv.second.size());
        mc::crypto_blake2b_update(&ctx, &nul, 1);
    }
    sig::Epoch out{};
    mc::crypto_blake2b_final(&ctx, out.data());
    return out;
}

// This loader's, taken once and before any module can add to the manifest.
inline const sig::Epoch& loader_epoch()
{
    static const sig::Epoch e = [] {
        std::vector<std::pair<std::string, std::string>> kv;
        for (auto const& [k, v] : Entity::getManifest())
            kv.emplace_back(std::string(k), std::string(v));
        return epoch_of(kv);
    }();
    return e;
}

inline bool check(const sig::Certificate& c)
{
    const std::string m = sig::signed_part(c);
    return mc::crypto_ed25519_check(c.signature, c.issuer.data(),
                                    reinterpret_cast<const uint8_t*>(m.data()), m.size()) == 0;
}

inline bool has_epoch(const sig::Certificate& c, const sig::Epoch& e)
{
    return std::find(c.epochs.begin(), c.epochs.end(), e) != c.epochs.end();
}

/*
 * What this loader is for a session. `head`: the identity built in, or zero;
 * `head_parent` the fold one step short of it, built in beside it. `builder`: the key of the loader that built this one, built in. `reg`: the
 * chain above `pub`, once registered; empty is the local half alone.
 * `proven`: the keystore's key is `pub`'s, checked when the session began
 * (establish).
 */
struct Identity
{
    sig::Key    head{};
    sig::Key    head_parent{};              // the fold one step short of head; zero: none
    sig::Key    builder{};
    sig::Key    pub{};
    bool        proven = false;
    std::string unproven;
    std::vector<sig::Certificate> reg;      // this key's cert <- ... <- a root; empty: unregistered
    sig::Role   role = sig::Role::Loader;   // what reg[0] makes this key; Loader while unregistered

    // This binary, by its own account (establish).
    std::string self_name;
    sig::Key    self{};
    sig::Key    parent{};                   // the binary that passed this one off; zero: none
    bool        attested = false;
    std::string unattested;
};

/*
 * The frame at the gate. A module is megabytes and a frame is not, so the
 * frame REFERS to the bytes (the 256-byte rule, one size up). Going in:
 * name, bytes, epoch, and whether the bytes are a loader's own (`kernel`).
 * The Signature stage adds who signed -- role 0 when the chain is the local
 * half alone -- whether its head is this loader's identity (`head`), it is a
 * loader binary that bridges to a sibling of it (`bridge`), or it met this
 * loader's own keys only (`local`), and how the bytes were passed off
 * (`passed`, a loader binary's); on the way out (signing) it fills `chain`, or
 * says the bytes already carry it, and `local` says the registration's half
 * was withheld.
 */
enum class Passed : uint8_t { None, ByAttested, BySelf, ByUnattested, OtherHead };

struct LoadFrame
{
    char           magic[8];
    char           name[256];
    const uint8_t* bytes;
    uint64_t       len;
    sig::Epoch     epoch;
    uint8_t        signer_role;
    sig::Key       signer;
    std::string*   chain;
    uint8_t        already;
    uint8_t        kernel;
    uint8_t        local;
    uint8_t        passed;
    uint8_t        head;
    uint8_t        bridge;
};

inline bool load_frame(const std::string& name, const uint8_t* b, size_t n, const sig::Epoch& epoch, LoadFrame& f,
                       bool kernel = false)
{
    if (name.empty() || name.size() > 255) return false;
    f = LoadFrame{};
    std::memcpy(f.magic, "ETCSLOAD", 8);
    std::memcpy(f.name, name.data(), name.size());
    f.bytes = b; f.len = n; f.epoch = epoch; f.kernel = kernel;
    return true;
}

/*
 * Where a module's chain may meet this loader's own keys, other than at its
 * head: its registration's, its key, its builder's. `role` is what that key is
 * there, so the certificate below it can be checked against it.
 */
inline bool meets(const Identity& id, const sig::Key& k, sig::Role& role)
{
    if (zero(k)) return false;
    for (size_t i = 0; i + 1 < id.reg.size(); ++i)
        if (id.reg[i].issuer == k) { role = id.reg[i + 1].role; return true; }
    if (!zero(id.pub) && id.proven && k == id.pub) { role = id.reg.empty() ? sig::Role::Loader : id.reg[0].role; return true; }
    if (!zero(id.builder) && k == id.builder) { role = sig::Role::Loader; return true; }
    return false;
}

struct Stage : IWireWrapper
{
    std::shared_ptr<const Identity> id;
    std::string why;                         // the last refusal, for whoever ran the chain

    WireScope Scope() const override { return WireScope::NetworkOnly; }
    void Wrap(MBuffer&, SignalContext) override {}

protected:
    static bool take(const MBuffer& io, LoadFrame& f)
    {
        if (io.written != sizeof f) return false;
        std::memcpy(&f, io.buf, sizeof f);
        return std::memcmp(f.magic, "ETCSLOAD", 8) == 0 && f.name[255] == '\0';
    }
    static void give(MBuffer& io, const LoadFrame& f)
    {
        std::memcpy(io.buf, &f, sizeof f);
        io.written = sizeof f;
    }
    void refuse(MBuffer& io, const std::string& reason) { why = reason; wire_refuse(io); }
};

struct SignatureStage : Stage
{
    // Present only where the key is (etcs --sign); a loader that loads has none.
    std::function<void(const uint8_t*, size_t, uint8_t*)> signer;

    void Unwrap(MBuffer& io, SignalContext) override
    {
        LoadFrame f;
        sig::Layout L;
        std::string w;
        if (!take(io, f) || !id) return refuse(io, "not a load this gate was set up for");
        if (untrusted_build)
        {
            trust_warn(std::string("UNTRUSTED: '") + f.name + "' loaded with no signature check");
            return give(io, f);
        }
        if (f.kernel && carries_untrusted_marker(f.bytes, size_t(f.len)))
            return refuse(io, "an untrusted build (ETCS_UNTRUSTED): no loader takes it as a loader binary");
        if (!sig::layout(f.bytes, f.len, L, w)) return refuse(io, w);
        if (!L.has_chain) return refuse(io, "unsigned");
        std::vector<sig::Certificate> chain;
        if (!sig::decode_chain(f.bytes + L.chain.at, L.chain.len, chain) || chain.empty())
            return refuse(io, "unreadable signature");

        const sig::Certificate& leaf = chain[0];
        sig::Key d{};
        if (leaf.role != sig::Role::Module) return refuse(io, "leaf is not a module certificate");
        if (!digest(f.name, f.bytes, f.len, L, d) || leaf.subject != d)
            return refuse(io, "signed for other bytes, or another name");
        if (!has_epoch(leaf, f.epoch)) return refuse(io, "not witnessed under this epoch");
        if (!check(leaf)) return refuse(io, "bad signature");
        f.signer = leaf.issuer;
        f.signer_role = 0;
        // Where the chain meets this loader's: the highest key along it that
        // is on this loader's own chain, at whatever tail length. Every link
        // is checked, above the meeting too.
        long meet = -1;
        sig::Role at{}, r{};
        if (meets(*id, leaf.issuer, r)) { meet = 0; at = r; }
        if (chain.size() > 1)
        {
            if (!sig::may_issue(chain[1].role, sig::Role::Module))
                return refuse(io, "a key certified as " + std::string(sig::role_name(chain[1].role)) + " signs no binary");
            for (size_t i = 1; i < chain.size(); ++i)
            {
                const sig::Certificate& c = chain[i];
                if (c.subject != chain[i - 1].issuer || !sig::may_issue(c.role, chain[i - 1].role)) return refuse(io, "chain is broken");
                if (!c.epochs.empty() && !has_epoch(c, f.epoch)) return refuse(io, "witness not valid in this epoch");
                if (!check(c)) return refuse(io, "bad signature in chain");
                if (c.role == sig::Role::Root && c.issuer == c.subject && i + 1 != chain.size())
                    return refuse(io, "a root's own certificate is not the last");
                if (meets(*id, c.issuer, r)) { meet = long(i); at = r; }
            }
            f.signer_role = uint8_t(chain[1].role);
        }
        // Its head first: the fold from one of its roots coming to this
        // loader's identity is the global surface; meeting only this loader's
        // own keys runs local -- unless no identity is built in, and then
        // nothing is held local.
        const std::vector<Step> path = path_of(chain);
        const long h = head_meets(path, id->head);
        if (h >= 0) { meet = long(path[size_t(h)].signs); at = path[size_t(h)].role; }
        // THE BRIDGE: a loader binary minted by another marketplace the same
        // parent certified -- the identity this one moves to when its own
        // marketplace key is retired -- and countersigned by a release key
        // under that same active key. Two keys, neither of them the retired one.
        if (h < 0 && f.kernel && chain.size() > 1 && !zero(id->head_parent)
            && path.back().role == sig::Role::Marketplace
            && head_meets(path, id->head_parent) == long(path.size()) - 2)
        {
            sig::Countersign cs;
            if (!L.has_count) return refuse(io, "a successor under another marketplace of this identity's parent "
                                                "needs a release key's countersignature");
            if (!sig::decode(f.bytes + L.count.at, L.count.len, cs) || cs.child != d || cs.to != f.epoch)
                return refuse(io, "countersigned as other bytes, or for another epoch");
            if (cs.cert.role != sig::Role::Release || cs.cert.issuer != chain[1].issuer || !check(cs.cert)
                || !check(cs))
                return refuse(io, "the countersignature is not a release key's under the same active key");
            meet = 0; at = sig::Role::Marketplace;
            f.bridge = 1;
        }
        if (meet < 0)
        {
            if (chain.size() > 1) return refuse(io, zero(id->head) ? "signed under a root, and no identity is built into this loader"
                                                                   : "does not come to this loader's identity");
            if (leaf.issuer == id->pub) return refuse(io, "signed by a loader, and this one has not proven a key: " + id->unproven);
            return refuse(io, "signed by another loader's key, and nothing above it");
        }
        if (!sig::may_issue(at, chain[size_t(meet)].role)) return refuse(io, "chain is broken");
        f.head  = h >= 0 ? 1 : 0;
        f.local = (h < 0 && !f.bridge && !zero(id->head)) ? 1 : 0;
        if (L.has_pass)
        {
            sig::PassOff p;
            if (!sig::decode(f.bytes + L.pass.at, L.pass.len, p) || p.child != d || p.key != leaf.issuer || p.to != f.epoch)
                return refuse(io, "passed off as other bytes, or by another key");
            if (!check(p)) return refuse(io, "bad signature on the pass-off");
            f.passed = uint8_t(p.head != id->head ? Passed::OtherHead : p.parent == p.child ? Passed::BySelf
                               : p.attested ? Passed::ByAttested : Passed::ByUnattested);
        }
        give(io, f);
    }

    static bool check(const sig::PassOff& p)
    {
        const std::string m = sig::signed_part(p);
        return mc::crypto_ed25519_check(p.signature, p.key.data(), reinterpret_cast<const uint8_t*>(m.data()), m.size()) == 0;
    }
    static bool check(const sig::Certificate& c) { return trust::check(c); }
    static bool check(const sig::Countersign& c)
    {
        const std::string m = sig::signed_part(c);
        return mc::crypto_ed25519_check(c.signature, c.cert.subject.data(),
                                        reinterpret_cast<const uint8_t*>(m.data()), m.size()) == 0;
    }

    // A lineage may begin by itself only where the identity is no one else's:
    // none, or this loader's own key or its builder's, unregistered.
    bool may_begin() const
    {
        return zero(id->head) || own_head(id->head, id->pub) || own_head(id->head, id->builder);
    }

    // What this loader would accept of a pass-off into a binary of its own.
    bool acceptable(const sig::PassOff& p) const
    {
        return p.head == id->head && (p.parent == p.child ? may_begin() : p.attested);
    }

    // The chain for these bytes: this loader's leaf, and its registration's
    // half above its key if it has one and attests -- or is the marketplace
    // minting a loader binary, where a lineage begins. A leaf by this key on these same bytes, under this same half,
    // carries its epochs forward. A loader binary gets a pass-off from this
    // loader, unless it carries one this loader would take and this loader's
    // own would not do better.
    void Wrap(MBuffer& io, SignalContext) override
    {
        LoadFrame f;
        sig::Layout L;
        std::string w;
        sig::Certificate c;
        c.role = sig::Role::Module;
        if (!take(io, f) || !id || !f.chain) return refuse(io, "not a load this gate was set up for");
        if (untrusted_build) return refuse(io, "an untrusted build (ETCS_UNTRUSTED) signs nothing");
        if (!sig::layout(f.bytes, f.len, L, w)) return refuse(io, w);
        if (!digest(f.name, f.bytes, f.len, L, c.subject)) return refuse(io, "bad module name");
        if (zero(id->pub)) return refuse(io, "this loader has no key");
        if (id->role == sig::Role::Registrar) return refuse(io, "a registrar certifies loaders; it signs no binary");
        if (id->role == sig::Role::Release && !f.kernel) return refuse(io, "a release key signs loaders, not modules");

        const bool global = zero(id->head) || id->attested || (id->role == sig::Role::Marketplace && f.kernel);
        static const std::vector<sig::Certificate> none;
        const std::vector<sig::Certificate>& half = global ? id->reg : none;
        std::vector<sig::Certificate> old;
        const bool readable = L.has_chain && sig::decode_chain(f.bytes + L.chain.at, L.chain.len, old) && !old.empty()
                              && old[0].role == sig::Role::Module && old[0].issuer == id->pub
                              && old[0].subject == c.subject && check(old[0]);
        auto under = [&](const std::vector<sig::Certificate>& h) {
            return readable && old.size() == h.size() + 1
                   && sig::encode_chain({ old.begin() + 1, old.end() }) == sig::encode_chain(h);
        };
        const bool carried = under(half);

        sig::PassOff pass, had;
        bool keep = false;
        if (f.kernel)
        {
            pass.parent = id->self; pass.child = c.subject; pass.from = loader_epoch(); pass.to = f.epoch;
            pass.head = id->head; pass.attested = id->attested; pass.key = id->pub;
            keep = L.has_pass && sig::decode(f.bytes + L.pass.at, L.pass.len, had) && had.child == c.subject
                   && had.key == id->pub && had.to == f.epoch && check(had) && (acceptable(had) || !acceptable(pass));
        }
        const bool full = under(id->reg);
        if ((carried || full) && has_epoch(old[0], f.epoch) && (!f.kernel || keep))
        {
            f.already = 1;
            f.local = full ? 0 : 1;                 // there, but without the half this key has
            return give(io, f);
        }
        if (!signer) return refuse(io, "the key is not here");
        f.local = global ? 0 : 1;
        if (carried)
        {
            c.epochs = old[0].epochs;
            if (c.epochs.size() >= sig::kMaxEpochs) c.epochs.erase(c.epochs.begin());
        }
        c.epochs.push_back(f.epoch);
        c.issuer = id->pub;
        const std::string m = sig::signed_part(c);
        signer(reinterpret_cast<const uint8_t*>(m.data()), m.size(), c.signature);
        std::vector<sig::Certificate> chain{ c };
        chain.insert(chain.end(), half.begin(), half.end());
        *f.chain = sig::encode_chain(chain);
        if (f.kernel)
        {
            if (keep) pass = had;
            else
            {
                const std::string pm = sig::signed_part(pass);
                signer(reinterpret_cast<const uint8_t*>(pm.data()), pm.size(), pass.signature);
            }
            *f.chain += sig::encode(pass);
        }
        give(io, f);
    }
};

struct OwnStage : Stage
{
    void Unwrap(MBuffer& io, SignalContext) override
    {
        LoadFrame f;
        if (!take(io, f) || !id) return refuse(io, "not a load this gate was set up for");
        if (untrusted_build) return give(io, f);
        const bool ours = (!zero(id->pub) && f.signer == id->pub) || (!zero(id->builder) && f.signer == id->builder);
        // A loader binary the identity's marketplace key signed is minted, and
        // taken as it is; any other is this loader's own, and passed off.
        const bool minted = f.head && f.signer_role == uint8_t(sig::Role::Marketplace);
        if (f.kernel && !minted && !f.bridge)
        {
            if (!ours) return refuse(io, "a loader binary is taken from this loader's identity, or from its own key");
            const bool may_begin = zero(id->head) || own_head(id->head, id->pub) || own_head(id->head, id->builder);
            switch (Passed(f.passed))
            {
                case Passed::None:         return refuse(io, "no signed build passed this loader off");
                case Passed::BySelf:       if (!may_begin)
                                               return refuse(io, "it passed itself off: under a minted identity a lineage begins at a mint");
                                           break;
                case Passed::ByUnattested: return refuse(io, "passed off by a loader that did not attest");
                case Passed::OtherHead:    return refuse(io, "passed off under another identity: a loader enters an identity from a mint");
                case Passed::ByAttested:   break;
            }
        }
        if (!f.kernel && f.signer_role == uint8_t(sig::Role::Release))
            return refuse(io, "a release key signs loaders, not modules");
        give(io, f);
    }
};

/*
 * The chain. Attach order is Own, Signature, then the gate entity's Wrapper
 * children; a load unwraps in reverse, as every chain does, so what was
 * wrapped last is opened first and Own sees only what Signature passed.
 */
struct Gate
{
    OwnStage       own;
    SignatureStage signature;
    std::atomic<Entity*> entity{ nullptr };

    void use(std::shared_ptr<const Identity> id) { own.id = id; signature.id = std::move(id); }

    bool unwrap(LoadFrame& f, std::string& why)
    {
        MBuffer io;
        SignalContext ctx{};
        std::memcpy(io.buf, &f, sizeof f);
        io.written = sizeof f;
        IWireWrapper* outer[MAX_WRAP_STAGES] = {};
        const size_t n = outer_stages(outer);
        for (size_t i = n; i-- > 0; )
        {
            outer[i]->Unwrap(io, ctx);
            if (wire_refused(io)) { why = "refused by a stage on this loader's gate"; return false; }
        }
        signature.Unwrap(io, ctx);
        if (wire_refused(io)) { why = signature.why; return false; }
        own.Unwrap(io, ctx);
        if (wire_refused(io)) { why = own.why; return false; }
        std::memcpy(&f, io.buf, sizeof f);
        return true;
    }

    bool wrap(LoadFrame& f, std::string& why)
    {
        MBuffer io;
        SignalContext ctx{};
        std::memcpy(io.buf, &f, sizeof f);
        io.written = sizeof f;
        own.Wrap(io, ctx);
        signature.Wrap(io, ctx);
        if (wire_refused(io)) { why = signature.why; return false; }
        IWireWrapper* outer[MAX_WRAP_STAGES] = {};
        const size_t n = outer_stages(outer);
        for (size_t i = 0; i < n; ++i)
        {
            outer[i]->Wrap(io, ctx);
            if (wire_refused(io)) { why = "refused by a stage on this loader's gate"; return false; }
        }
        std::memcpy(&f, io.buf, sizeof f);
        return true;
    }

private:
    size_t outer_stages(IWireWrapper** out)
    {
        Entity* e = entity.load();
        return e ? MirrorBuffer::chainOf(e, WireScope::Pipe, out, MAX_WRAP_STAGES) : 0;
    }
};

// The verdict on bytes as `name`, for an identity given rather than this
// session's. True, or false and why.
inline bool verify(const std::string& name, const uint8_t* b, size_t n, const sig::Epoch& epoch,
                   const Identity& id, std::string& why, bool kernel = false, bool* local = nullptr)
{
    Gate g;
    LoadFrame f;
    g.use(std::make_shared<const Identity>(id));
    if (!load_frame(name, b, n, epoch, f, kernel)) { why = "bad module name"; return false; }
    const bool ok = g.unwrap(f, why);
    if (ok && local) *local = f.local != 0;
    return ok;
}

/*
 * THIS LOADER'S IDENTITY, under etcs_store_dir:
 *
 *   the key           its Ed25519 seed, in the OS keystore (core/Keystore.h) as
 *                     "loader-key" for this store -- never in a file
 *   loader.sign.pub   the public half; what verification reads
 *   loader.chain      the half above it, once registered: this key's
 *                     certificate <- ... <- a root
 *
 * UNREGISTERED, a loader has its local half only: what it signs carries its
 * key and nothing above, and its identity -- what the loaders it builds
 * require -- is that key alone. Registering joins a half on, issued elsewhere
 * -- by a registrar's loader, or by the active key at a ceremony -- and
 * injected (install_registration); the identity is then the fold along it.
 */
inline bool random_bytes(uint8_t* out, size_t n)
{
#if defined(_WIN32)
    return BCryptGenRandom(nullptr, out, ULONG(n), BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
#elif defined(__linux__) && !defined(__EMSCRIPTEN__)
    size_t at = 0;
    while (at < n)
    {
        const ssize_t r = ::getrandom(out + at, n - at, 0);
        if (r < 0) { if (errno == EINTR) continue; return false; }
        at += size_t(r);
    }
    return true;
#else
    for (size_t at = 0; at < n; at += 256)
        if (::getentropy(out + at, std::min<size_t>(256, n - at)) != 0) return false;
    return true;
#endif
}

// `reg` is a half above `pub`: a certificate for it in a role a loader can
// hold, each link certified as the table says (core/Signature.h), ending at a
// root's certificate or a key a root may certify. With a `head`, the fold along
// it -- from its top or from a root on it -- must come to `head` at `pub`: a
// registration whose identity that is. A root attesting another above is
// checked as any link is.
inline bool registration_valid(const std::vector<sig::Certificate>& reg, const sig::Key& pub, const sig::Key& head = sig::Key{})
{
    if (reg.empty() || reg[0].subject != pub) return false;
    const sig::Role r = reg[0].role;
    if (r != sig::Role::Loader && r != sig::Role::Marketplace && r != sig::Role::Release && r != sig::Role::Registrar)
        return false;
    for (size_t i = 0; i < reg.size(); ++i)
    {
        const sig::Certificate& c = reg[i];
        if (i > 0 && (c.subject != reg[i - 1].issuer || !sig::may_issue(c.role, reg[i - 1].role))) return false;
        if (!check(c)) return false;
        if (c.role == sig::Role::Root && c.issuer == c.subject && i + 1 != reg.size()) return false;
    }
    if (reg.back().role != sig::Role::Root && !sig::may_issue(sig::Role::Root, reg.back().role)) return false;
    if (zero(head)) return true;
    const std::vector<Step> p = path_of(reg, pub);
    return head_meets(p, head) == long(p.size()) - 1;
}

// Whether a registration holds in `epoch`: every certificate bounded by epochs
// (an ACE identity's) names it. One that does not was this key's in another
// epoch, and is asked for again.
inline bool registration_current(const std::vector<sig::Certificate>& reg, const sig::Epoch& epoch)
{
    for (const sig::Certificate& c : reg)
        if (!c.epochs.empty() && !has_epoch(c, epoch)) return false;
    return true;
}

#if !defined(__EMSCRIPTEN__)
inline bool make_dirs(const std::string& dir)
{
    for (size_t i = 1; i <= dir.size(); ++i)
    {
        if (i != dir.size() && dir[i] != '/' && dir[i] != '\\') continue;
        const std::string p = dir.substr(0, i);
#if defined(_WIN32)
        if (!CreateDirectoryA(p.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) return false;
#else
        if (::mkdir(p.c_str(), 0700) != 0 && errno != EEXIST) return false;
#endif
    }
    return true;
}

// Public files only: written beside, then renamed over.
inline bool write_public(const std::string& path, const std::string& bytes)
{
    const std::string tmp = path + ".new";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f.write(bytes.data(), std::streamsize(bytes.size())) || !f.flush()) return false;
    }
#if defined(_WIN32)
    return MoveFileExA(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
#else
    return std::rename(tmp.c_str(), path.c_str()) == 0;
#endif
}

inline bool read_hex_file(const std::string& path, uint8_t* out, size_t n)
{
    std::vector<uint8_t> raw;
    if (!read_file(path, raw)) return false;
    std::string s(raw.begin(), raw.end());
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    return unhex(s, out, n);
}

inline bool load_public(const std::string& dir, sig::Key& pub)
{
    return read_hex_file(dir + "/loader.sign.pub", pub.data(), pub.size());
}

// The seed, from the keystore; made and kept there on first use.
inline bool loader_seed(const std::string& dir, uint8_t seed[32], std::string& why)
{
    std::string text;
    keystore::Status st = keystore::load(dir, "loader-key", text, why);
    if (st == keystore::Status::Absent)
    {
        if (!random_bytes(seed, 32)) { why = "no randomness"; return false; }
        text = hex(seed, 32);
        st = keystore::store_new(dir, "loader-key", text, why);
        mc::crypto_wipe(&text[0], text.size());
        if (st != keystore::Status::Ok) { mc::crypto_wipe(seed, 32); return false; }
        return true;
    }
    const bool ok = st == keystore::Status::Ok && unhex(text, seed, 32);
    if (!text.empty()) mc::crypto_wipe(&text[0], text.size());
    if (st == keystore::Status::Ok && !ok) why = "the keystore's loader-key is not a key";
    return ok;
}

/*
 * This loader's key, made on first use. `secret` is wiped by the caller. A pub
 * file that disagrees with the keystore's key is a refusal, not something to
 * overwrite. The key comes before any registration: it is what gets registered.
 */
inline bool loader_key(const std::string& dir, uint8_t secret[64], sig::Key& pub, std::string& why)
{
    if (!make_dirs(dir)) { why = "cannot create " + dir; return false; }
    uint8_t seed[32];
    if (!loader_seed(dir, seed, why)) return false;
    mc::crypto_ed25519_key_pair(secret, pub.data(), seed);                    // wipes seed
    sig::Key filed{};
    if (load_public(dir, filed))
    {
        if (filed != pub) { mc::crypto_wipe(secret, 64); why = dir + "/loader.sign.pub is not this key's"; return false; }
    }
    else if (!write_public(dir + "/loader.sign.pub", hex(pub.data(), 32) + "\n"))
    { mc::crypto_wipe(secret, 64); why = "cannot write " + dir + "/loader.sign.pub"; return false; }
    return true;
}

/*
 * THE LOADER BINARY PUT IN PLACE FOR A LATER SESSION (replace, "etcs"): its
 * epoch and the identity it is built for, so this loader can register its key
 * for that epoch before the swap (registration_request, `next`), and the
 * successor starts registered.
 */
inline void successor_placed(const std::string& store, const sig::Epoch& epoch, const sig::Key& head)
{
    if (store.empty()) return;
    std::ofstream f(store + "/successor", std::ios::trunc);
    f << "epoch=" << hex(epoch.data(), 32) << " head=" << hex(head.data(), 32) << "\n";
}

inline bool successor(const std::string& store, sig::Epoch& epoch, sig::Key& head)
{
    std::ifstream f(store + "/successor");
    std::string e, h;
    return !store.empty() && f >> e >> h && e.rfind("epoch=", 0) == 0 && h.rfind("head=", 0) == 0
        && unhex(e.substr(6), epoch.data(), 32) && unhex(h.substr(5), head.data(), 32);
}

/*
 * This key's registration for `epoch`: loader.chain, or the one installed
 * ahead of a swap (loader.chain.next) once this loader is the successor it was
 * for -- which then takes loader.chain's place. `stale` says one of another
 * epoch was found and dropped.
 */
inline bool current_registration(const std::string& dir, const sig::Key& pub, const sig::Epoch& epoch,
                                 std::vector<sig::Certificate>& reg, bool& stale)
{
    stale = false;
    for (const bool ahead : { false, true })
    {
        const std::string path = dir + (ahead ? "/loader.chain.next" : "/loader.chain");
        std::vector<uint8_t> raw;
        reg.clear();
        if (!read_file(path, raw) || !sig::decode_chain(raw.data(), raw.size(), reg) || !registration_valid(reg, pub)) continue;
        if (!registration_current(reg, epoch)) { stale = true; continue; }
        if (ahead) { std::error_code ec; std::filesystem::rename(path, dir + "/loader.chain", ec); }
        return true;
    }
    reg.clear();
    return false;
}

// The key, and the half above it when there is one: what a signer needs.
inline bool identity(const std::string& dir, uint8_t secret[64], sig::Key& pub,
                     std::vector<sig::Certificate>& reg, std::string& why)
{
    if (!loader_key(dir, secret, pub, why)) return false;
    bool stale = false;
    current_registration(dir, pub, loader_epoch(), reg, stale);
    return true;
}

// What a store's builder is authorized to build with: the fold along its
// registration to its key, or its key alone -- and, in `parent`, the fold one
// step short of its key (zero unregistered). False if the store has no key.
inline bool store_identity(const std::string& dir, sig::Key& head, std::string& why, sig::Key* parent = nullptr)
{
    uint8_t secret[64];
    sig::Key pub{};
    std::vector<sig::Certificate> reg;
    if (!identity(dir, secret, pub, reg, why)) return false;
    mc::crypto_wipe(secret, 64);
    const std::vector<Step> p = path_of(reg, pub);
    head = identity_of(p);
    if (parent) *parent = reg.empty() ? sig::Key{} : parent_of(p);
    return true;
}
/*
 * INJECTING A REGISTRATION: the half above this loader's key, issued
 * elsewhere -- by a registrar's loader (etcs --issue), or by the active key at
 * a ceremony. Leaf first, as certificates (binary or hex, as Custody keeps
 * them) or as one chain. It must be for this key and end at a root
 * certificate; the identity this builder then builds with is the fold along it.
 */
inline bool parse_certificates(const std::string& raw, std::vector<sig::Certificate>& out, bool text_too = true)
{
    std::vector<sig::Certificate> chain;
    sig::Certificate c;
    size_t used = 0;
    if (sig::decode_chain(raw.data(), raw.size(), chain)) { out.insert(out.end(), chain.begin(), chain.end()); return true; }
    if (sig::decode(raw.data(), raw.size(), c, &used) && used == raw.size()) { out.push_back(c); return true; }
    if (!text_too) return false;
    std::string text = raw;
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ')) text.pop_back();
    std::string bin(text.size() / 2, '\0');
    if (text.size() % 2 || bin.empty() || !unhex(text, reinterpret_cast<uint8_t*>(&bin[0]), bin.size())) return false;
    return parse_certificates(bin, out, false);
}

inline bool read_certificates(const std::string& path, std::vector<sig::Certificate>& out)
{
    std::vector<uint8_t> raw;
    return read_file(path, raw) && parse_certificates(std::string(raw.begin(), raw.end()), out);
}

inline bool install_registration(const std::string& dir, const std::vector<sig::Certificate>& reg, std::string& why)
{
    uint8_t secret[64];
    sig::Key pub{};
    if (!loader_key(dir, secret, pub, why)) return false;                 // the key must be this loader's to register
    mc::crypto_wipe(secret, 64);
    if (reg.empty() || reg.back().role != sig::Role::Root) { why = "the chain does not end at a root certificate"; return false; }
    if (!registration_valid(reg, pub)) { why = "not a registration of this loader's key"; return false; }
    sig::Epoch next{};
    sig::Key head{};
    const bool ahead = !registration_current(reg, loader_epoch());
    if (ahead && !(successor(dir, next, head) && registration_current(reg, next)))
    { why = "a registration for an epoch neither this loader nor its successor runs in"; return false; }
    const std::string path = dir + (ahead ? "/loader.chain.next" : "/loader.chain");
    if (!write_public(path, sig::encode_chain(reg))) { why = "cannot write " + path; return false; }
    return true;
}

/*
 * THE LINEAGE, as the signer keeps it: one line per loader binary it passed
 * off, appended to lineage.log in its store. The pass-off itself is in the
 * child (Signature.h); this is the trace, and nothing is valid or invalid by
 * it. An edge says which binary's key signed which, across which epochs, when
 * and where. The signer signs the line.
 *
 *   etcs-lineage 3 <name> head=<identity|none> parent=<digest|genesis> attested=<0|1>
 *       child=<digest> from=<epoch> to=<epoch> key=<signer> at=<unix seconds> host=<name> sig=<hex>
 *
 * A version 2 line says root=<key|none> where 3 says head=, and a version 1
 * line has neither; both are read as they were written.
 *
 * `parent=genesis` is a loader signing itself: where a lineage starts.
 */
struct LineageEdge
{
    int         version = 3;
    std::string name, host;
    bool        genesis = false, attested = false;
    sig::Key    head{}, parent{}, child{}, key{};      // head: a version 2 line's root
    sig::Epoch  from{}, to{};
    uint64_t    at = 0;
    uint8_t     signature[sig::kSigBytes] = {};
};

inline std::string lineage_text(const LineageEdge& e)
{
    return "etcs-lineage " + std::to_string(e.version) + " " + e.name
         + (e.version >= 2 ? std::string(e.version == 2 ? " root=" : " head=")
                             + (zero(e.head) ? std::string("none") : hex(e.head.data(), 32)) : std::string())
         + " parent=" + (e.genesis ? std::string("genesis") : hex(e.parent.data(), 32))
         + " attested=" + (e.attested ? "1" : "0")
         + " child=" + hex(e.child.data(), 32)
         + " from=" + hex(e.from.data(), 32) + " to=" + hex(e.to.data(), 32)
         + " key=" + hex(e.key.data(), 32)
         + " at=" + std::to_string(e.at) + " host=" + e.host;
}

inline bool lineage_parse(const std::string& line, LineageEdge& e)
{
    const size_t cut = line.rfind(" sig=");
    if (cut == std::string::npos || line.compare(0, 13, "etcs-lineage ") != 0
        || line.size() < 15 || line[13] < '1' || line[13] > '3' || line[14] != ' ') return false;
    e.version = line[13] - '0';
    auto field = [&](const char* key) -> std::string {
        const std::string k = std::string(" ") + key + "=";
        const size_t at = line.find(k);
        if (at == std::string::npos || at > cut) return {};
        const size_t from = at + k.size(), to = line.find(' ', from);
        return line.substr(from, (to == std::string::npos ? line.size() : to) - from);
    };
    const size_t name_end = line.find(' ', 15);
    if (name_end == std::string::npos) return false;
    e.name = line.substr(15, name_end - 15);
    const std::string head = field(e.version == 2 ? "root" : "head");
    if (e.version >= 2 && head != "none" && !unhex(head, e.head.data(), 32)) return false;
    const std::string parent = field("parent");
    e.genesis = parent == "genesis";
    e.attested = field("attested") == "1";
    e.host = field("host");
    char* end = nullptr;
    const std::string at = field("at");
    e.at = std::strtoull(at.c_str(), &end, 10);
    if (at.empty() || *end) return false;
    if ((!e.genesis && !unhex(parent, e.parent.data(), 32)) || !unhex(field("child"), e.child.data(), 32)
        || !unhex(field("from"), e.from.data(), 32) || !unhex(field("to"), e.to.data(), 32)
        || !unhex(field("key"), e.key.data(), 32) || !unhex(line.substr(cut + 5), e.signature, sig::kSigBytes))
        return false;
    const std::string text = line.substr(0, cut);
    return text == lineage_text(e)
        && mc::crypto_ed25519_check(e.signature, e.key.data(), reinterpret_cast<const uint8_t*>(text.data()), text.size()) == 0;
}

// Every edge in the store's log, each one's signature checked. False, and
// which line, when one does not parse or verify.
inline bool lineage_read(const std::string& dir, std::vector<LineageEdge>& out, std::string& why)
{
    std::ifstream f(dir + "/lineage.log");
    std::string line;
    for (size_t n = 1; std::getline(f, line); ++n)
    {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        LineageEdge e;
        if (!lineage_parse(line, e)) { why = "lineage.log line " + std::to_string(n) + " is not a signed edge"; return false; }
        out.push_back(e);
    }
    return true;
}

inline std::string host_name()
{
    char buf[256] = {};
#if defined(_WIN32)
    DWORD n = sizeof buf;
    if (!GetComputerNameA(buf, &n)) buf[0] = '\0';
#else
    if (::gethostname(buf, sizeof buf - 1) != 0) buf[0] = '\0';
#endif
    std::string h;
    for (const char* p = buf; *p; ++p)
        if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '.' || *p == '-' || *p == '_')
            h += *p;
    return h.empty() ? "unnamed" : h;
}

inline std::string own_path()
{
    char path[4096] = {};
#if defined(_WIN32)
    if (GetModuleFileNameA(nullptr, path, sizeof path - 1) == 0) return {};
#else
    if (::readlink("/proc/self/exe", path, sizeof path - 1) <= 0) return {};
#endif
    return path;
}

// This process's own binary: its path's last name, and its bytes.
inline bool own_binary(std::string& name, std::vector<uint8_t>& bytes)
{
    const std::string path = own_path();
    if (path.empty()) return false;
    name = path;
    const size_t slash = name.find_last_of("/\\");
    if (slash != std::string::npos) name.erase(0, slash + 1);
    if (name.size() > 4 && name.compare(name.size() - 4, 4, ".exe") == 0) name.erase(name.size() - 4);
    return read_file(path, bytes) && !bytes.empty();
}

#endif // !__EMSCRIPTEN__


/*
 * THE HANDSHAKE: what this loader is, for this session, from its store. The
 * public files say what it claims; the keystore says whether the claim is its
 * own, and only then is anything it signed taken. A browser holds no key and
 * reads no store: its claim is the build's (ace's defines), its identity too.
 * `prove` false reads the claim only -- enough to recognise a chain as
 * already there, which is all a build with nothing to sign needs.
 */
inline std::shared_ptr<const Identity> establish(const std::string& dir, bool prove = true,
                                                 const sig::Key& built_in = ace_identity())
{
    auto id = std::make_shared<Identity>();
    id->head = built_in;
    if (built_in == ace_identity()) id->head_parent = ace_identity_parent();
#if defined(ETCS_BUILDER_KEY)
    unhex(ETCS_TRUST_STR(ETCS_BUILDER_KEY), id->builder.data(), id->builder.size());
#endif
#if defined(__EMSCRIPTEN__)
    (void)dir; (void)prove;
    id->unproven = "a browser holds no key";
    id->unattested = "a browser loader is what its page serves; it does not attest itself";
#else
    if (!load_public(dir, id->pub)) id->unproven = "this store has no loader key yet";
    else
    {
        bool stale = false;
        if (!current_registration(dir, id->pub, loader_epoch(), id->reg, stale) && stale)
            trust_warn("this loader's registration is for another epoch: it runs unregistered until it registers again");
        if (!id->reg.empty()) id->role = id->reg[0].role;
    }
    if (!prove) return id;
    if (id->unproven.empty())
    {
        std::string text, why;
        uint8_t seed[32], secret[64];
        sig::Key held{};
        const keystore::Status st = keystore::load(dir, "loader-key", text, why);
        if (st == keystore::Status::Ok && unhex(text, seed, 32))
        {
            mc::crypto_ed25519_key_pair(secret, held.data(), seed);
            mc::crypto_wipe(secret, 64);
            id->proven = held == id->pub;
            if (!id->proven) id->unproven = "the keystore's key is not loader.sign.pub's";
        }
        else id->unproven = st == keystore::Status::Absent ? "the keystore holds no key for this store"
                          : st == keystore::Status::Ok     ? "the keystore's loader-key is not a key" : why;
        if (!text.empty()) mc::crypto_wipe(&text[0], text.size());
    }

    if (untrusted_build)
    {
        id->unattested = "an untrusted build (ETCS_UNTRUSTED): it checks nothing, so it attests nothing";
        std::vector<uint8_t> me;
        sig::Layout L;
        std::string w;
        if (own_binary(id->self_name, me) && sig::layout(me.data(), me.size(), L, w))
            digest(id->self_name, me.data(), me.size(), L, id->self);
        return id;
    }
    // The loader attests itself: its own bytes through its own two stages,
    // as the identity just established. Nothing else will check this binary.
    // Minted, it asks nothing of the store -- a loader just downloaded
    // attests by the identity it was built with.
    std::vector<uint8_t> me;
    std::string w;
    if (!own_binary(id->self_name, me)) id->unattested = "this loader cannot read its own binary";
    else
    {
        sig::Layout L;
        sig::PassOff p;
        if (sig::layout(me.data(), me.size(), L, w))
        {
            digest(id->self_name, me.data(), me.size(), L, id->self);
            if (L.has_pass && sig::decode(me.data() + L.pass.at, L.pass.len, p)) id->parent = p.parent;
        }
        id->attested = verify(id->self_name, me.data(), me.size(), loader_epoch(), *id, id->unattested, true);
    }
#endif
    return id;
}

// This session's gate. The handshake runs once, at the first load.
inline Gate& gate()
{
    static Gate g;
    static std::once_flag once;
    std::call_once(once, [] {
        std::shared_ptr<const Identity> id = establish(etcs_store_dir());
        untrusted_banner();
#if !defined(__EMSCRIPTEN__)
        if (!id->attested && !untrusted_build)
        {
            std::fprintf(stderr, "etcs: warning: %s is not a signed build -- %s\n", id->self_name.c_str(), id->unattested.c_str());
            if (!zero(id->head))
                std::fprintf(stderr, "etcs: what it signs carries its local half alone; a signed etcs is the site's "
                                     "(CryptProvider Upstream, Update)\n");
        }
#endif
        g.use(std::move(id));
    });
    return g;
}

// The entity whose Wrapper children stand on the gate, outside the loader's
// own stages; made the first time one is wanted (core/TrustGate.h).
Entity* gate_entity();

#if !defined(__EMSCRIPTEN__)
/*
 * Signs `file` as `name` under `epoch`: the gate, wrapped. 0 the chain is in
 * `out` (and `why` says so if the registration's half was withheld), 3 already signed so
 * (nothing written), 2 refused. For a module, what is already there is judged
 * from the public files alone, and the key is asked for only when there is
 * something to sign; a loader binary is judged by the whole identity.
 *
 * `edge`, when given, is filled for a signing that happened: who this loader
 * is to the bytes it signed (succeed).
 */
inline int sign_module(const std::string& dir, const std::string& name, const std::string& file,
                       const sig::Epoch& epoch, const std::string& out, std::string& why,
                       LineageEdge* edge = nullptr, const sig::Key& built_in = ace_identity(), bool kernel = false)
{
    std::vector<uint8_t> b;
    std::string chain;
    LoadFrame f;
    if (!read_file(file, b)) { why = "cannot read " + file; return 2; }
    if (!load_frame(name, b.data(), b.size(), epoch, f, kernel)) { why = "bad module name"; return 2; }
    if (kernel && carries_untrusted_marker(b.data(), b.size()))
    { why = "an untrusted build (ETCS_UNTRUSTED) is never signed: it would attest where it checks nothing"; return 2; }
    f.chain = &chain;
    if (!kernel)                                    // a loader binary's pass-off is judged by the signer's whole identity
    {
        Gate seen;
        LoadFrame probe = f;
        std::string ignored;
        seen.use(establish(dir, false, built_in));
        if (seen.wrap(probe, ignored) && probe.already && !probe.local) return 3;
    }
    uint8_t secret[64];
    sig::Key pub{};
    std::vector<sig::Certificate> reg;
    if (!identity(dir, secret, pub, reg, why)) return 2;      // makes the key on first use
    const std::shared_ptr<const Identity> id = establish(dir, true, built_in);
    if (kernel)
    {
        // A loader binary that already attests here -- a minted one, say -- is
        // left as it is: signing it again could only make it this loader's.
        std::string w;
        if (verify(name, b.data(), b.size(), epoch, *id, w, true)) { mc::crypto_wipe(secret, 64); return 3; }
        // A build of the very bytes this loader is carries what this loader carries.
        sig::Layout L, M;
        sig::Key d{};
        std::vector<uint8_t> me;
        std::string me_name;
        if (id->attested && sig::layout(b.data(), b.size(), L, w) && digest(name, b.data(), b.size(), L, d) && d == id->self
            && own_binary(me_name, me) && sig::layout(me.data(), me.size(), M, w) && M.has_chain)
        {
            mc::crypto_wipe(secret, 64);
            const size_t len = M.chain.len + (M.has_pass ? M.pass.len : 0) + (M.has_count ? M.count.len : 0);
            std::ofstream o(out, std::ios::binary | std::ios::trunc);
            if (!o.write(reinterpret_cast<const char*>(me.data() + M.chain.at), std::streamsize(len)) || !o.flush())
            { why = "cannot write " + out; return 2; }
            return 0;
        }
    }
    Gate g;
    g.use(id);
    g.signature.signer = [&secret](const uint8_t* m, size_t n, uint8_t* sig64) { mc::crypto_ed25519_sign(sig64, secret, m, n); };
    const bool ok = g.wrap(f, why);
    if (ok && !f.already && f.local) why = "its local half alone: this loader does not attest -- " + id->unattested;
    if (ok && !f.already && edge)
    {
        sig::Layout L;
        std::string w;
        edge->name = name; edge->host = host_name(); edge->key = id->pub; edge->head = id->head;
        edge->from = loader_epoch(); edge->to = epoch;
        edge->at = uint64_t(std::time(nullptr));
        if (sig::layout(b.data(), b.size(), L, w)) digest(name, b.data(), b.size(), L, edge->child);
        edge->genesis = zero(id->self) || edge->child == id->self;
        edge->parent = id->self;
        edge->attested = id->attested;
        const std::string text = lineage_text(*edge);
        mc::crypto_ed25519_sign(edge->signature, secret, reinterpret_cast<const uint8_t*>(text.data()), text.size());
    }
    mc::crypto_wipe(secret, 64);
    if (!ok) return 2;
    if (f.already) return 3;
    std::ofstream o(out, std::ios::binary | std::ios::trunc);
    if (!o.write(chain.data(), std::streamsize(chain.size())) || !o.flush()) { why = "cannot write " + out; return 2; }
    return 0;
}

/*
 * Signs a loader binary and passes it off: this loader -> that one. The same
 * signing as any module's, with the pass-off after the chain, and the edge
 * appended to lineage.log.
 * The same edge -- this parent, this child, this epoch -- is not written twice,
 * and a loader signing its own bytes is an edge only where there is none yet.
 */
inline int succeed(const std::string& dir, const std::string& name, const std::string& file,
                   const sig::Epoch& epoch, const std::string& out, std::string& why, std::string* line_out = nullptr,
                   const sig::Key& built_in = ace_identity())
{
    LineageEdge e;
    const int rc = sign_module(dir, name, file, epoch, out, why, &e, built_in, true);
    if (rc != 0 || e.name.empty()) return rc;          // nothing passed off: this loader's own signature, copied
    std::vector<LineageEdge> had;
    std::string w;
    if (!lineage_read(dir, had, w)) { why = w; return 2; }
    had.erase(std::remove_if(had.begin(), had.end(), [&](const LineageEdge& h) { return h.version < 3 || h.head != e.head; }), had.end());
    if (e.genesis && !had.empty()) return 0;         // a loader re-signing itself begins nothing
    for (const LineageEdge& h : had)
        if (h.child == e.child && h.to == e.to && h.name == e.name && h.genesis == e.genesis
            && (h.genesis || h.parent == e.parent))
            return 0;
    const std::string line = lineage_text(e) + " sig=" + hex(e.signature, sig::kSigBytes);
    std::ofstream log(dir + "/lineage.log", std::ios::app);
    if (!(log << line << "\n") || !log.flush()) { why = "cannot append to " + dir + "/lineage.log"; return 2; }
    if (line_out) *line_out = line;
    return 0;
}

/*
 * A RELEASE KEY'S COUNTERSIGNATURE (core/Signature.h) on a loader binary that
 * carries its chain and pass-off already -- what the minting server minted.
 * Written to `out` as the whole section: chain, pass-off, countersignature.
 * Only a store registered as Release, in a session that proved its key.
 */
inline int countersign(const std::string& dir, const std::string& name, const std::string& file,
                       const sig::Epoch& epoch, const std::string& out, std::string& why)
{
    std::vector<uint8_t> b;
    sig::Layout L;
    std::string w;
    if (!read_file(file, b)) { why = "cannot read " + file; return 2; }
    if (!sig::layout(b.data(), b.size(), L, w) || !L.has_chain || !L.has_pass)
    { why = "not a minted loader binary: it carries no chain and pass-off to countersign"; return 2; }
    uint8_t secret[64];
    sig::Key pub{};
    std::vector<sig::Certificate> reg;
    if (!identity(dir, secret, pub, reg, why)) return 2;
    const std::shared_ptr<const Identity> id = establish(dir, true);
    auto refused = [&](const std::string& r) { mc::crypto_wipe(secret, 64); why = r; return 2; };
    if (reg.empty() || reg[0].role != sig::Role::Release) return refused("only a release key countersigns, and this loader is not registered as one");
    if (!id->proven) return refused("this loader has not proven its key: " + id->unproven);
    sig::Countersign cs;
    cs.to = epoch; cs.cert = reg[0];
    if (!digest(name, b.data(), b.size(), L, cs.child)) return refused("bad name");
    const std::string m = sig::signed_part(cs);
    mc::crypto_ed25519_sign(cs.signature, secret, reinterpret_cast<const uint8_t*>(m.data()), m.size());
    mc::crypto_wipe(secret, 64);
    std::string section(reinterpret_cast<const char*>(b.data() + L.chain.at), L.chain.len);
    section.append(reinterpret_cast<const char*>(b.data() + L.pass.at), L.pass.len);
    section += sig::encode(cs);
    std::ofstream o(out, std::ios::binary | std::ios::trunc);
    if (!o.write(section.data(), std::streamsize(section.size())) || !o.flush()) { why = "cannot write " + out; return 2; }
    return 0;
}

/*
 * A REGISTRAR'S WORK: certify another loader's key as `role`, under this
 * loader's own place in its tree, and write the chain that results -- the
 * half the other loader installs with --registration. That the key is the
 * asker's is for the asker to prove to the registrar, not for this to know.
 */
inline int issue(const std::string& dir, sig::Role role, const sig::Key& subject, const std::string& out,
                 std::string& why, const sig::Key& built_in = ace_identity())
{
    uint8_t secret[64];
    sig::Key pub{};
    std::vector<sig::Certificate> reg;
    if (!identity(dir, secret, pub, reg, why)) return 2;
    const std::shared_ptr<const Identity> id = establish(dir, true, built_in);
    auto refused = [&](const std::string& w) { mc::crypto_wipe(secret, 64); why = w; return 2; };
    if (reg.empty()) return refused("this loader is not registered, so it certifies no one");
    if (role == sig::Role::Identity) return refused("an ACE identity is derived by a registrar's route, for an epoch; none is issued by hand");
    if (!sig::may_issue(reg[0].role, role))
        return refused(std::string("a ") + sig::role_name(reg[0].role) + " does not certify a " + sig::role_name(role));
    if (!id->proven) return refused("this loader has not proven its key: " + id->unproven);
    if (!id->attested) return refused("only a signed build certifies, and this one does not attest: " + id->unattested);
    sig::Certificate c;
    c.role = role; c.subject = subject; c.issuer = pub;
    const std::string m = sig::signed_part(c);
    mc::crypto_ed25519_sign(c.signature, secret, reinterpret_cast<const uint8_t*>(m.data()), m.size());
    mc::crypto_wipe(secret, 64);
    std::vector<sig::Certificate> chain{ c };
    chain.insert(chain.end(), reg.begin(), reg.end());
    const std::string bytes = sig::encode_chain(chain);
    std::ofstream o(out, std::ios::binary | std::ios::trunc);
    if (!o.write(bytes.data(), std::streamsize(bytes.size())) || !o.flush()) { why = "cannot write " + out; return 2; }
    return 0;
}

/*
 * THE SITE (the forms are core/Signature.h's). The build server publishes a
 * signed list of what it serves; a loader asks the site's registrar for its
 * half, reads the list, and fetches what the site has moved past. The network
 * is a module's (CryptProvider::Upstream); whatever touches this loader's key
 * or store is here, and a module reaches it only through TrustEvent (answer).
 */
struct DistEntry
{
    std::string             name, file;
    sig::Key                digest{};
    std::vector<sig::Epoch> epochs;
};
struct Dist
{
    sig::Epoch             epoch{};
    std::vector<uint32_t>  kernel;          // the ABI's version it was published under
    bool                   sibling = false; // a marketplace of this loader's parent, not its own
    sig::Key               head{};          // the identity whose marketplace signed it
    std::vector<DistEntry> entries;
};

/*
 * THE ORDER LISTS COME IN: the ABI's version, "ETCS_Kernel_v<n>[.<n>...]"
 * (BASE_SOURCE_STRING, core/ETCS_API.h), which moves on an ABI change -- and
 * so with every change in trust a release makes. A loader takes no list older
 * than itself, nor than the newest it has taken (kernel.seen in its store):
 * a list replayed from before a key was retired is refused. Lists of one
 * version are not ordered among themselves.
 */
inline bool kernel_version_of(const std::string& text, std::vector<uint32_t>& v)
{
    static const std::string prefix = "ETCS_Kernel_v";
    v.clear();
    if (text.compare(0, prefix.size(), prefix) != 0 || text.size() == prefix.size()) return false;
    for (size_t at = prefix.size(); at <= text.size(); )
    {
        size_t dot = text.find('.', at);
        if (dot == std::string::npos) dot = text.size();
        if (dot == at || dot - at > 9) return false;
        uint32_t n = 0;
        for (size_t i = at; i < dot; ++i)
        {
            if (text[i] < '0' || text[i] > '9') return false;
            n = n * 10 + uint32_t(text[i] - '0');
        }
        v.push_back(n);
        at = dot + 1;
    }
    return true;
}

inline std::string kernel_text(const std::vector<uint32_t>& v)
{
    std::string t = "ETCS_Kernel_v";
    for (size_t i = 0; i < v.size(); ++i) t += (i ? "." : "") + std::to_string(v[i]);
    return t;
}

inline const std::vector<uint32_t>& kernel_version()
{
    static const std::vector<uint32_t> v = [] {
        std::vector<uint32_t> r;
        kernel_version_of(BASE_SOURCE_STRING, r);
        return r;
    }();
    return v;
}

// The oldest list this loader takes: itself, or the newest its store has taken.
inline std::vector<uint32_t> kernel_floor(const std::string& store)
{
    std::vector<uint32_t> seen;
    std::ifstream f(store + "/kernel.seen");
    std::string line;
    if (!store.empty() && std::getline(f, line) && kernel_version_of(line, seen) && kernel_version() < seen) return seen;
    return kernel_version();
}

inline void kernel_taken(const std::string& store, const std::vector<uint32_t>& v)
{
    if (store.empty() || !(kernel_floor(store) < v)) return;
    std::ofstream f(store + "/kernel.seen", std::ios::trunc);
    f << kernel_text(v) << "\n";
}

inline bool plain_file(const std::string& f)
{
    if (f.empty() || f.size() > 128 || f[0] == '.') return false;
    for (char c : f)
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-'))
            return false;
    return true;
}

// A binary's name in the list: its file's, less what the platform adds.
inline std::string dist_name(const std::string& file)
{
    for (const char* ext : { ".so", ".dll", ".wasm", ".exe" })
    {
        const size_t n = std::strlen(ext);
        if (file.size() > n && file.compare(file.size() - n, n, ext) == 0) return file.substr(0, file.size() - n);
    }
    return file;
}

// The list, read, and only if the marketplace whose identity is `head` signed
// it, under no older kernel than `floor` -- or, given the identity's `parent`,
// a sibling marketplace that parent certified, at a kernel newer than this
// loader's: the identity it moves to (`d.sibling`).
inline bool dist_read(const std::string& text, Dist& d, std::string& why, const sig::Key& head,
                      const std::vector<uint32_t>& floor = kernel_version(), const sig::Key& parent = sig::Key{})
{
    d.sibling = false;
    d.head = head;
    if (zero(head)) { why = "no identity is built into this loader, so no marketplace to believe"; return false; }
    const std::string tag = sig::kDistSig;
    const size_t last = text.rfind("\n" + tag);
    if (last == std::string::npos) { why = "the list is not signed"; return false; }
    const std::string body = text.substr(0, last + 1);
    std::istringstream sl(text.substr(last + 1 + tag.size()));
    std::string chain_hex, sig_hex;
    sl >> chain_hex >> sig_hex;
    std::string raw(chain_hex.size() / 2, '\0');
    std::vector<sig::Certificate> reg;
    uint8_t s[sig::kSigBytes];
    if (chain_hex.size() % 2 || raw.empty() || !unhex(chain_hex, reinterpret_cast<uint8_t*>(&raw[0]), raw.size())
        || !sig::decode_chain(raw.data(), raw.size(), reg) || reg.empty() || !unhex(sig_hex, s, sizeof s))
    { why = "the list's signature is unreadable"; return false; }
    if (reg[0].role != sig::Role::Marketplace) { why = "the list is not signed by the marketplace of this loader's identity"; return false; }
    if (!registration_valid(reg, reg[0].subject, head))
    {
        const std::vector<Step> p = path_of(reg, reg[0].subject);
        if (zero(parent) || !registration_valid(reg, reg[0].subject) || head_meets(p, parent) != long(p.size()) - 2)
        { why = "the list is not signed by the marketplace of this loader's identity"; return false; }
        d.sibling = true;
        d.head = fold(parent, sig::Role::Marketplace, reg[0].subject);
    }
    const std::string m = "ETCSDST1" + body;
    if (mc::crypto_ed25519_check(s, reg[0].subject.data(), reinterpret_cast<const uint8_t*>(m.data()), m.size()) != 0)
    { why = "the list's signature does not verify"; return false; }

    std::istringstream in(body);
    std::string line;
    const std::string first = std::string(sig::kDistMagic) + "epoch=";
    if (!std::getline(in, line) || line.compare(0, first.size(), first) != 0
        || line.size() < first.size() + 64 || !unhex(line.substr(first.size(), 64), d.epoch.data(), d.epoch.size()))
    { why = "the list names no epoch"; return false; }
    const std::string kv = line.substr(first.size() + 64);
    if (kv.compare(0, 8, " kernel=") != 0 || !kernel_version_of(kv.substr(8), d.kernel))
    { why = "the list names no kernel version"; return false; }
    if (d.kernel < floor)
    { why = "the list is of " + kernel_text(d.kernel) + ", older than " + kernel_text(floor) + " here: a rollback"; return false; }
    if (d.sibling && !(kernel_version() < d.kernel))
    { why = "a list of another marketplace under this identity's parent is taken only at a kernel newer than this loader's"; return false; }
    d.entries.clear();
    while (std::getline(in, line))
    {
        if (line.empty()) continue;
        std::istringstream ls(line);
        DistEntry e;
        std::string dg, eps;
        bool ok = static_cast<bool>(ls >> e.name >> e.file >> dg >> eps) && plain_file(e.name) && plain_file(e.file)
                  && unhex(dg, e.digest.data(), e.digest.size());
        for (size_t at = 0; ok && at < eps.size(); )
        {
            size_t c = eps.find(',', at);
            if (c == std::string::npos) c = eps.size();
            sig::Epoch ep{};
            ok = unhex(eps.substr(at, c - at), ep.data(), ep.size());
            e.epochs.push_back(ep);
            at = c + 1;
        }
        if (!ok) { why = "the list has a line it cannot read: " + line; return false; }
        d.entries.push_back(e);
    }
    return true;
}

/*
 * PUBLISHING (etcs --manifest, on the build server): the list of every binary
 * in `dir` a download would take in `epoch` -- what the marketplace minted,
 * modules and loader binaries ("etcs") alike -- signed by this loader, which
 * must be the marketplace. What would not be taken is left off, and said.
 */
inline bool publish(const std::string& store, const std::string& dir, const sig::Epoch& epoch, std::string& out,
                    std::string& why, std::vector<std::string>* left = nullptr, const sig::Key& built_in = ace_identity())
{
    if (zero(built_in)) { why = "no identity is built into this loader, so it publishes for no one"; return false; }
    uint8_t secret[64];
    sig::Key pub{};
    std::vector<sig::Certificate> reg;
    if (!identity(store, secret, pub, reg, why)) return false;
    auto refused = [&](const std::string& w) { mc::crypto_wipe(secret, 64); why = w; return false; };
    if (reg.empty() || reg[0].role != sig::Role::Marketplace)
        return refused("only the marketplace publishes, and this loader is not registered as it");
    const std::shared_ptr<const Identity> me = establish(store, true, built_in);
    if (!me->proven) return refused("this loader has not proven its key: " + me->unproven);

    namespace fs = std::filesystem;
    std::error_code ec;
    std::vector<std::string> files;
    for (const auto& entry : fs::directory_iterator(dir, ec))
        if (entry.is_regular_file()) files.push_back(entry.path().filename().string());
    if (ec) return refused("cannot read " + dir + ": " + ec.message());
    std::sort(files.begin(), files.end());

    Identity download;                                   // what takes these: the identity, nothing else
    download.head = built_in;
    std::string body = std::string(sig::kDistMagic) + "epoch=" + hex(epoch.data(), 32)
                     + " kernel=" + kernel_text(kernel_version()) + "\n";
    for (const std::string& f : files)
    {
        if (!plain_file(f) || f == "manifest" || f == "release.chain") continue;   // the release certs ride beside
        const std::string name = dist_name(f);
        std::vector<uint8_t> b;
        sig::Layout L;
        std::vector<sig::Certificate> c;
        std::string w;
        if (!read_file(dir + "/" + f, b) || !sig::layout(b.data(), b.size(), L, w) || !L.has_chain
            || !sig::decode_chain(b.data() + L.chain.at, L.chain.len, c) || c.empty())
        { if (left) left->push_back(f + ": not a signed binary"); continue; }
        if (!verify(name, b.data(), b.size(), epoch, download, w, name == "etcs"))
        { if (left) left->push_back(f + ": " + w); continue; }
        std::string eps;
        for (const sig::Epoch& e : c[0].epochs) eps += (eps.empty() ? "" : ",") + hex(e.data(), 32);
        body += name + " " + f + " " + hex(c[0].subject.data(), 32) + " " + eps + "\n";
    }
    uint8_t s[sig::kSigBytes];
    const std::string m = "ETCSDST1" + body;
    mc::crypto_ed25519_sign(s, secret, reinterpret_cast<const uint8_t*>(m.data()), m.size());
    mc::crypto_wipe(secret, 64);
    const std::string chain = sig::encode_chain(reg);
    out = body + sig::kDistSig + hex(reinterpret_cast<const uint8_t*>(chain.data()), chain.size()) + " " + hex(s, sizeof s) + "\n";
    return true;
}

/*
 * RE-SYNC: the marketplace key retired. The site's list is then a sibling's
 * -- another marketplace the same active key certified, at a newer kernel --
 * and what it gives this loader is only its successor, bridged (Unwrap). Put
 * in place, this loader's identity is retired in its store (nothing its old
 * marketplace signs is taken again), and the session restarts on the
 * successor where it left off (request_resync; the scene replays as each
 * persistable defines). `resync` in the store says which binary re-synced,
 * from what file, to what:
 *
 *   from=<the running binary's digest> was=<its file's BLAKE2b> to=<kernel> head=<identity>
 *
 * so a successor that never took is told from one that is waiting for the
 * restart, and nothing loops.
 */
inline bool head_retired(const std::string& store, const sig::Key& head)
{
    std::ifstream f(store + "/retired");
    std::string line;
    const std::string h = hex(head.data(), 32);
    while (!store.empty() && std::getline(f, line)) if (line == h) return true;
    return false;
}

inline void retire_head(const std::string& store, const sig::Key& head)
{
    if (store.empty() || zero(head) || head_retired(store, head)) return;
    std::ofstream f(store + "/retired", std::ios::app);
    f << hex(head.data(), 32) << "\n";
}

inline sig::Key file_hash(const std::string& path)
{
    std::vector<uint8_t> b;
    sig::Key h{};
    if (read_file(path, b)) mc::crypto_blake2b(h.data(), 32, b.data(), b.size());
    return h;
}

enum class Resync { None, Placed, Stuck, Done };

inline Resync resync_state(const std::string& store, const Identity& id, const std::string& self_path)
{
    std::ifstream f(store + "/resync");
    std::string line, from, was;
    if (store.empty() || !std::getline(f, line)) return Resync::None;
    std::istringstream in(line);
    for (std::string kv; in >> kv; )
    {
        if (kv.rfind("from=", 0) == 0) from = kv.substr(5);
        if (kv.rfind("was=", 0) == 0)  was = kv.substr(4);
    }
    if (from != hex(id.self.data(), 32)) return Resync::Done;
    const sig::Key now = file_hash(self_path);
    return hex(now.data(), 32) == was ? Resync::Stuck : Resync::Placed;
}

/*
 * WHAT THE SITE HAS MOVED PAST, here: each entry against the binary this
 * loader would read under that name (its own, for "etcs"). Current is the same
 * bytes, signed for the site's epoch. A binary this loader's own key signed is
 * the developer's and is never fetched over. Anything else not current is
 * fetched -- all of it, when the site is in another epoch. A sibling's list
 * names one thing to do: "resync etcs <file>".
 */
inline bool survey(const std::string& text, std::string& report, std::string& why, const Identity& id,
                   const sig::Epoch& local, const std::string& bin_dir, const std::string& self_path,
                   const std::string& store = std::string())
{
    Dist d;
    if (!dist_read(text, d, why, id.head, kernel_floor(store), id.head_parent)) return false;
    const Resync rs = resync_state(store, id, self_path);
    if (rs == Resync::Done) std::remove((store + "/resync").c_str());
    if (d.sibling)
    {
        report = "identity moved: " + hex(id.head.data(), 32) + " -> " + hex(d.head.data(), 32)
               + " at " + kernel_text(d.kernel) + "\n";
        if (rs == Resync::Placed) { report += "re-sync placed: restart\n"; return true; }
        if (rs == Resync::Stuck)
        { report += "a re-sync was placed and this is still the loader before it: reinstall from the site\n"; return true; }
        const DistEntry* e = nullptr;
        for (const DistEntry& x : d.entries) if (x.name == "etcs") e = &x;
        report += e ? "resync etcs " + e->file + "\n" : "the list names no loader to re-sync to\n";
        return true;
    }
    if (head_retired(store, id.head))
    { why = "this loader's identity was retired by a re-sync: the loader it re-synced to is the one to run"; return false; }
    kernel_taken(store, d.kernel);
    const bool moved = d.epoch != local;
    report = "site epoch " + hex(d.epoch.data(), 32) + (moved ? ", and this loader is in " + hex(local.data(), 32)
                                                              : ", this loader's") + "\n";
    for (const DistEntry& e : d.entries)
    {
        std::vector<uint8_t> b;
        if (!read_file(e.name == "etcs" ? self_path : bin_dir + e.file, b)) { report += e.name + " not here\n"; continue; }
        sig::Layout L;
        sig::Key dg{};
        std::vector<sig::Certificate> c;
        std::string w;
        const bool sealed = sig::layout(b.data(), b.size(), L, w) && L.has_chain
                            && sig::decode_chain(b.data() + L.chain.at, L.chain.len, c) && !c.empty()
                            && digest(e.name, b.data(), b.size(), L, dg);
        if (sealed && dg == e.digest && has_epoch(c[0], d.epoch)) { report += e.name + " current\n"; continue; }
        if (sealed && !zero(id.pub) && c[0].issuer == id.pub) { report += e.name + " yours, not fetched\n"; continue; }
        report += e.name + (moved ? " behind\n" : " differs\n");
        report += "fetch " + e.name + " " + e.file + "\n";
    }
    return true;
}

/*
 * Takes `bytes` as the list's entry `name` only if they are that entry's and
 * pass this loader's own stages in the site's epoch, then puts them where this
 * loader reads them: beside, then over. The process keeps what it mapped; the
 * next load, or start, reads these. From a sibling's list only "etcs", bridged,
 * and placing it is the re-sync: this identity retired, `resync` written.
 */
inline bool replace(const std::string& text, const std::string& name, const std::string& bytes, std::string& out,
                    std::string& why, const Identity& id, const std::string& bin_dir, const std::string& self_path,
                    const std::string& store = std::string())
{
    Dist d;
    if (!dist_read(text, d, why, id.head, kernel_floor(store), id.head_parent)) return false;
    if (d.sibling && name != "etcs") { why = "from a list of the identity this loader moves to, it takes only its successor"; return false; }
    if (!d.sibling && head_retired(store, id.head))
    { why = "this loader's identity was retired by a re-sync: the loader it re-synced to is the one to run"; return false; }
    if (!d.sibling) kernel_taken(store, d.kernel);
    const DistEntry* e = nullptr;
    for (const DistEntry& x : d.entries) if (x.name == name) e = &x;
    if (!e) { why = "the list has no '" + name + "'"; return false; }
    const uint8_t* b = reinterpret_cast<const uint8_t*>(bytes.data());
    sig::Layout L;
    sig::Key dg{};
    std::string w;
    if (!sig::layout(b, bytes.size(), L, w) || !digest(name, b, bytes.size(), L, dg) || dg != e->digest)
    { why = "these are not the bytes the list names"; return false; }
    const bool kernel = name == "etcs";
    if (!verify(name, b, bytes.size(), d.epoch, id, w, kernel)) { why = w; return false; }

    namespace fs = std::filesystem;
    const std::string path = kernel ? self_path : bin_dir + e->file, beside = path + ".fetched";
    const sig::Key was = file_hash(path);
    {
        std::ofstream o(beside, std::ios::binary | std::ios::trunc);
        if (!o.write(bytes.data(), std::streamsize(bytes.size())) || !o.flush()) { why = "cannot write " + beside; return false; }
    }
    std::error_code ec;
    fs::permissions(beside, fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec
                            | fs::perms::others_read | fs::perms::others_exec, ec);
    fs::rename(beside, path, ec);
    if (ec) { fs::remove(beside, ec); why = "cannot put it at " + path; return false; }
    out = path;
    if (kernel) successor_placed(store, d.epoch, d.head);
    if (d.sibling && !store.empty())
    {
        std::ofstream r(store + "/resync", std::ios::trunc);
        r << "from=" << hex(id.self.data(), 32) << " was=" << hex(was.data(), 32) << " to=" << kernel_text(d.kernel)
          << " head=" << hex(d.head.data(), 32) << "\n";
        retire_head(store, id.head);
        kernel_taken(store, d.kernel);
    }
    return true;
}

// The kernel a placed re-sync goes to, from `resync`; empty when none.
inline std::string resync_target(const std::string& store)
{
    std::ifstream f(store + "/resync");
    std::string line;
    if (store.empty() || !std::getline(f, line)) return {};
    std::istringstream in(line);
    for (std::string kv; in >> kv; ) if (kv.rfind("to=", 0) == 0) return kv.substr(3);
    return {};
}

/*
 * The restart a re-sync ends in. First the notice, on whatever channel this
 * session has (resync_notice, core/CommandExecutor.h: the terminal, in colour);
 * then the session closes as it would on a terminate -- the last save of the
 * scene included (runtime_close) -- and the loader becomes its successor
 * (resync_exec, called from drive_main_loop_then_exit) started --resynced: an
 * interactive one asks whether to resume the session or go to the menu, and
 * the place stays in the store either way. A loader with no prompt starts as
 * it was launched, the place kept. A browser's re-sync is its page's reload.
 */
inline void resync_notice(const std::string& text);

inline std::atomic<bool>& resync_flag() { static std::atomic<bool> f{ false }; return f; }
inline std::vector<std::string>& launch_args() { static std::vector<std::string> a; return a; }
inline std::string& resync_kernel() { static std::string k; return k; }

inline bool resync_pending() { return resync_flag().load(); }

inline void request_resync(const std::string& to_kernel)
{
    resync_kernel() = to_kernel.empty() ? std::string("a newer kernel") : to_kernel;
    resync_notice("re-syncing to " + resync_kernel() + ": this loader's identity moved. Saving your place, then "
                  "restarting on its successor.");
    resync_flag().store(true);
    g_sig_term.store(1, std::memory_order_release);
}

inline void resync_exec()
{
#if !defined(_WIN32) && !defined(__EMSCRIPTEN__)
    if (!resync_pending()) return;
    const std::string self = own_path();
    std::vector<std::string> args{ self, "--resynced", resync_kernel() };
#if !defined(ETCS_REPL_SHELL)
    // No prompt and no menu: it runs again as it was launched.
    const std::vector<std::string>& was = launch_args();
    for (size_t i = 1; i < was.size(); ++i)
    {
        if (was[i] == "--resynced") { ++i; continue; }
        if (was[i] != "--resume" && was[i] != "--fresh") args.push_back(was[i]);
    }
#endif
    std::vector<char*> argv;
    for (std::string& a : args) argv.push_back(&a[0]);
    argv.push_back(nullptr);
    std::fflush(nullptr);
    ::execv(self.c_str(), argv.data());
    std::fprintf(stderr, "etcs: the successor did not start (%s); start it again to finish the re-sync\n", std::strerror(errno));
#endif
}

// What a loader signs to ask a registrar for the half above its key, as a
// loader of the identity built in, running in this epoch -- or, `next`, as
// the successor it put in place (sig::registration_statement):
// "<key> <head> <epoch> <signature>", hex.
inline bool registration_request(const std::string& dir, const sig::Key& registrar, std::string& out, std::string& why,
                                 bool next = false)
{
    sig::Key head = ace_identity();
    sig::Epoch epoch = loader_epoch();
    if (next && !successor(dir, epoch, head)) { why = "no successor is in place to register for"; return false; }
    uint8_t secret[64], s[sig::kSigBytes];
    sig::Key pub{};
    if (!loader_key(dir, secret, pub, why)) return false;
    const std::string m = sig::registration_statement(pub, registrar, head, epoch);
    mc::crypto_ed25519_sign(s, secret, reinterpret_cast<const uint8_t*>(m.data()), m.size());
    mc::crypto_wipe(secret, 64);
    out = hex(pub.data(), 32) + " " + hex(head.data(), 32) + " " + hex(epoch.data(), 32) + " " + hex(s, sizeof s);
    return true;
}

// A member's word about an account, or a key's agreeing to join one
// (sig::join_statement, admit_statement, retire_statement, portal_statement),
// signed by this loader's key: "<key> <signature>", hex. `kind` is 'j', 'a',
// 'r' or 'p'; `other` is the member joined, the key admitted, the key retired,
// or the code a browser shows.
inline bool account_word(const std::string& dir, char kind, const sig::Key& registrar, const sig::Key& other,
                         std::string& out, std::string& why)
{
    if (kind != 'j' && kind != 'a' && kind != 'r' && kind != 'p') { why = "no such word"; return false; }
    uint8_t secret[64], s[sig::kSigBytes];
    sig::Key pub{};
    if (!loader_key(dir, secret, pub, why)) return false;
    if ((kind == 'j' || kind == 'a') && other == pub) { mc::crypto_wipe(secret, 64); why = "a key does not join itself"; return false; }
    const std::string m = kind == 'j' ? sig::join_statement(pub, other, registrar, loader_epoch())
                        : kind == 'a' ? sig::admit_statement(pub, other, registrar, loader_epoch())
                        : kind == 'r' ? sig::retire_statement(pub, other, registrar)
                                      : sig::portal_statement(pub, registrar, other);
    mc::crypto_ed25519_sign(s, secret, reinterpret_cast<const uint8_t*>(m.data()), m.size());
    mc::crypto_wipe(secret, 64);
    out = hex(pub.data(), 32) + " " + hex(s, sizeof s);
    return true;
}
#endif // !__EMSCRIPTEN__

/*
 * A LINK BETWEEN TWO LOADERS, as each loader vouches for its own end of it.
 * The link's module (NetworkProvider) makes the fresh exchange keys and the
 * transcript; the loader signs only the transcript, and only in the one form
 * (sig::link_statement), so what it lends a module by this is its word on a
 * link and nothing else. The far side's word is checked here too: its
 * signature under its key, its chain to a root if it has one, and the fold
 * that is its identity -- by the meet rule's own measure (path_of,
 * identity_of), so a registered peer and a module of the same registration
 * are the same identity.
 *
 * A BROWSER HOLDS NO KEYSTORE, so a tab vouches with a key of its own made
 * for the session and forgotten with it: a local identity that lasts as long
 * as the page. Kept nowhere, it is nowhere to be stolen from; a tab that
 * reloads is somebody new. (Rejected: keeping it in the page's storage,
 * which the page -- not the loader -- would then own.) An untrusted build
 * does the same, for its own reason (link_secret).
 */
// A key made for this session and kept only in memory: a browser's, and an
// untrusted build's (below).
inline bool session_link_key(uint8_t secret[64], sig::Key& pub, std::string& why)
{
    static std::mutex m;
    static uint8_t held[64];
    static sig::Key held_pub{};
    static bool made = false;
    std::lock_guard<std::mutex> g(m);
    if (!made)
    {
        uint8_t seed[32];
        if (!random_bytes(seed, 32)) { why = "no randomness"; return false; }
        mc::crypto_ed25519_key_pair(held, held_pub.data(), seed);        // wipes seed
        made = true;
    }
    std::memcpy(secret, held, 64);
    pub = held_pub;
    return true;
}

/*
 * AN UNTRUSTED BUILD VOUCHES WITH A SESSION KEY TOO, never the store's: it
 * runs modules nothing checked, and its loader key is the developer's word
 * everywhere that key is known -- lending it to an unchecked runtime would be
 * signing for what nobody verified. A session key says only "this runtime,
 * while it runs", which is all it can honestly say; the far side sees a local
 * identity. And it says so, every link, as it says everything else.
 */
inline bool link_secret(uint8_t secret[64], sig::Key& pub, std::vector<sig::Certificate>& reg, std::string& why)
{
    reg.clear();
#if defined(__EMSCRIPTEN__)
    return session_link_key(secret, pub, why);
#else
    if (untrusted_build)
    {
        trust_warn("UNTRUSTED: this link is vouched for by a key made for this session, not the loader's own; "
                   "the far side sees a local identity");
        return session_link_key(secret, pub, why);
    }
    if (!identity(etcs_store_dir(), secret, pub, reg, why)) return false;
    const Identity& id = *gate().signature.id;
    if (!id.proven || id.pub != pub) { mc::crypto_wipe(secret, 64); why = "this session's key is not proven: " + id.unproven; return false; }
    return true;
#endif
}

// Vouch: "<key> <chain|-> <signature>" for this end of the link.
inline bool vouch(char side, const std::string& transcript_hex, std::string& out, std::string& why)
{
    uint8_t t[sig::kLinkTranscript], secret[64], s[sig::kSigBytes];
    if ((side != 'd' && side != 'a') || !unhex(transcript_hex, t, sizeof t)) { why = "not a link's transcript"; return false; }
    sig::Key pub{};
    std::vector<sig::Certificate> reg;
    if (!link_secret(secret, pub, reg, why)) return false;
    const std::string m = sig::link_statement(side, t);
    mc::crypto_ed25519_sign(s, secret, reinterpret_cast<const uint8_t*>(m.data()), m.size());
    mc::crypto_wipe(secret, 64);
    const std::string chain = reg.empty() ? std::string() : sig::encode_chain(reg);
    out = hex(pub.data(), 32) + " " + (chain.empty() ? "-" : hex(reinterpret_cast<const uint8_t*>(chain.data()), chain.size()))
        + " " + hex(s, sizeof s);
    return true;
}

// Recognize: the far side's word on the same transcript -> "<identity> <registered> <guild>".
// A registration for another epoch is none here.
inline bool recognize(char side, const std::string& word, const std::string& transcript_hex, std::string& out, std::string& why)
{
    uint8_t t[sig::kLinkTranscript], s[sig::kSigBytes];
    sig::Key pub{};
    std::istringstream in(word);
    std::string k, c, g;
    in >> k >> c >> g;
    if ((side != 'd' && side != 'a') || !unhex(transcript_hex, t, sizeof t) || !unhex(k, pub.data(), pub.size())
        || !unhex(g, s, sizeof s) || c.empty())
    { why = "not a link's word"; return false; }
    const std::string m = sig::link_statement(side, t);
    if (mc::crypto_ed25519_check(s, pub.data(), reinterpret_cast<const uint8_t*>(m.data()), m.size()) != 0)
    { why = "its signature on this link does not verify"; return false; }
    std::vector<sig::Certificate> reg;
    if (c != "-")
    {
        std::string raw(c.size() / 2, '\0');
        if (c.size() % 2 || raw.empty() || !unhex(c, reinterpret_cast<uint8_t*>(&raw[0]), raw.size())
            || !sig::decode_chain(raw.data(), raw.size(), reg) || !registration_valid(reg, pub))
        { why = "its chain is not a registration of its key"; return false; }
        if (!registration_current(reg, loader_epoch())) reg.clear();
    }
    const std::vector<Step> p = path_of(reg, pub);
    const sig::Key ident = identity_of(p);
    // In this loader's guild: under its identity's parent (the same active
    // key), or under the identity itself.
    const bool guild = !reg.empty() && (head_meets(p, ace_identity_parent()) >= 0 || head_meets(p, ace_identity()) >= 0);
    out = hex(ident.data(), 32) + " " + (reg.empty() ? "0" : "1") + " " + (guild ? "1" : "0");
    return true;
}

/*
 * WHAT A MODULE MAY ASK (TrustEvent, core/EventNode.h), on the loader's
 * ordering thread, of this loader as it runs: its store, its session's
 * identity, its own directory and binary, its epoch. `op` in TrustEvent::Op's
 * order. A registration installed now is the next session's -- for this
 * epoch, or (op 0 with b = "next") for the successor put in place; a re-sync
 * (op 4) ends this session and starts the successor in its place. A link's
 * word (ops 5 and 6), and what this loader is (op 10: "<head> <epoch>"), are
 * asked of a browser loader too. An account's words (ops 7 to 9, and 11 for
 * the portal) take the registrar's key in `a` and the other key, or the
 * portal's code, in `b`.
 */
inline bool answer(uint8_t op, const std::string& name, const std::string& a, const std::string& b, std::string& out)
{
    out.clear();
    if (op == 5 || op == 6)
    {
        std::string why;
        const char side = name.empty() ? '\0' : name[0];
        const bool ok = op == 5 ? vouch(side, a, out, why) : recognize(side, a, b, out, why);
        if (!ok) out = why;
        return ok;
    }
    if (op == 10)
    {
        out = hex(ace_identity().data(), 32) + " " + hex(loader_epoch().data(), 32);
        return true;
    }
#if defined(__EMSCRIPTEN__)
    (void)op; (void)name; (void)a; (void)b;
    out = "a browser loader keeps no store";
    return false;
#else
    std::string why;
    const std::string dir = etcs_store_dir(), self = own_path();
    const std::string bin = self.substr(0, self.find_last_of("/\\") + 1);
    const Identity& id = *gate().signature.id;
    bool ok = false;
    switch (op)
    {
        case 0:
        {
            sig::Key registrar{};
            if (!unhex(a, registrar.data(), registrar.size())) { why = "not a registrar's key"; break; }
            ok = registration_request(dir, registrar, out, why, b == "next");
            break;
        }
        case 7: case 8: case 9: case 11:
        {
            sig::Key registrar{}, other{};
            if (!unhex(a, registrar.data(), registrar.size())) { why = "not a registrar's key"; break; }
            if (!unhex(b, other.data(), other.size())) { why = op == 11 ? "not a portal's code" : "not a loader's key"; break; }
            ok = account_word(dir, op == 7 ? 'j' : op == 8 ? 'a' : op == 9 ? 'r' : 'p', registrar, other, out, why);
            break;
        }
        case 1:
        {
            std::vector<sig::Certificate> reg;
            if (!parse_certificates(a, reg)) { why = "not a chain of certificates"; break; }
            if ((ok = install_registration(dir, reg, why)))
                out = std::string("registered as ") + sig::role_name(reg[0].role)
                    + (registration_current(reg, loader_epoch()) ? "; the next session is it" : "; the successor in place is it");
            break;
        }
        case 2: ok = survey(a, out, why, id, loader_epoch(), bin, self, dir); break;
        case 3: ok = replace(a, name, b, out, why, id, bin, self, dir); break;
        case 4:
        {
            const Resync rs = resync_state(dir, id, self);
            if (rs == Resync::Stuck) { why = "a re-sync was placed and this is still the loader before it: reinstall from the site"; break; }
            if (rs != Resync::Placed) { why = "no successor is placed to re-sync to"; break; }
            ok = true;
            out = "this loader's identity moved; restarting on its successor";
            request_resync(resync_target(dir));
            break;
        }
        default: why = "no such question";
    }
    if (!ok) out = why;
    return ok;
#endif
}

/*
 * THE BYTES THAT ARE CHECKED ARE THE BYTES THAT ARE MAPPED.
 *
 *   Linux    copied into a memfd, sealed against write, grow and shrink, then
 *            checked and opened as /proc/self/fd/N -- the file can change on
 *            disk afterwards and nothing loaded will.
 *   Windows  held open without FILE_SHARE_WRITE from the read through
 *            LoadLibrary, so it cannot change in between.
 *   web      read out of MEMFS. The page owns that filesystem and this memory
 *            alike; there is no one to race.
 *
 * A LOADED memfd IS NEVER CLOSED (kept()). Its number is the name the dynamic
 * linker knows the module by, and glibc answers a dlopen by name before it
 * opens anything: a number closed and reused is answered with whichever module
 * last had it -- still resident if a thread_local destructor pins it. Found
 * that way: NetworkProvider's dlopen returned CryptProvider. The cost is one
 * descriptor and the module's bytes per load, for the life of the process.
 */
struct Opened
{
    std::string path;
#if defined(_WIN32)
    HANDLE hold = INVALID_HANDLE_VALUE;
#else
    int fd = -1;
#endif
    Opened() = default;
    Opened(const Opened&) = delete;
    Opened& operator=(const Opened&) = delete;
    // After a successful load: the descriptor outlives this.
    void kept()
    {
#if !defined(_WIN32)
        static std::mutex m;
        static std::vector<int> held;
        if (fd < 0) return;
        std::lock_guard<std::mutex> g(m);
        held.push_back(fd);
        fd = -1;
#endif
    }
    ~Opened()
    {
#if defined(_WIN32)
        if (hold != INVALID_HANDLE_VALUE) CloseHandle(hold);
#else
        if (fd >= 0) ::close(fd);
#endif
    }
};

inline bool open_verified(const std::string& name, const std::string& file, Opened& o, std::string& why,
                          Gate& g = gate(), const sig::Epoch& epoch = loader_epoch(), bool* local = nullptr)
{
    std::vector<uint8_t> b;
#if defined(_WIN32)
    o.hold = CreateFileA(file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                         FILE_ATTRIBUTE_NORMAL, nullptr);
    if (o.hold == INVALID_HANDLE_VALUE) { why = "cannot open " + file; return false; }
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(o.hold, &size) || size.QuadPart > (1LL << 31)) { why = "cannot size " + file; return false; }
    b.resize(size_t(size.QuadPart));
    DWORD got = 0;
    if (!b.empty() && (!ReadFile(o.hold, b.data(), DWORD(b.size()), &got, nullptr) || got != b.size()))
    { why = "cannot read " + file; return false; }
    o.path = file;
#elif defined(__linux__) && !defined(__EMSCRIPTEN__)
    if (!read_file(file, b)) { why = "cannot read " + file; return false; }
    unsigned flags = MFD_CLOEXEC | MFD_ALLOW_SEALING;
#  ifdef MFD_EXEC
    o.fd = ::memfd_create(("etcs:" + name).c_str(), flags | MFD_EXEC);
    if (o.fd < 0 && errno == EINVAL)
#  endif
        o.fd = ::memfd_create(("etcs:" + name).c_str(), flags);
    if (o.fd < 0) { why = "memfd_create failed"; return false; }
    for (size_t at = 0; at < b.size();)
    {
        const ssize_t w = ::write(o.fd, b.data() + at, b.size() - at);
        if (w < 0) { if (errno == EINTR) continue; why = "cannot copy into the memfd"; return false; }
        at += size_t(w);
    }
    if (::fcntl(o.fd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE | F_SEAL_SEAL) != 0)
    { why = "cannot seal the memfd"; return false; }
    o.path = "/proc/self/fd/" + std::to_string(o.fd);
#else
    if (!read_file(file, b)) { why = "cannot read " + file; return false; }
    o.path = file;
#endif
    LoadFrame f;
    if (!load_frame(name, b.data(), b.size(), epoch, f)) { why = "bad module name"; return false; }
    if (!g.unwrap(f, why)) return false;
    if (local) *local = f.local != 0;
    return true;
}

#if !defined(__EMSCRIPTEN__)
/*
 * WITNESSES: a build machine's word on what its build came to, and the
 * agreement of several. A binary is compared by what a loader checks of it
 * (digest), never as a file: each signer's section differs.
 *
 *   etcs-witness 1 epoch=<hex> arch=<arch> [<key>=<value> ...]
 *   <name> <file> <digest hex>
 *   ...
 *   etcs-witness-sig <key hex> <signature hex>
 *
 * The signature is by a key made for the statement and forgotten, over
 * "ETCSWIT1" and every byte before the last line: it ties the lines to one
 * witness, and tells two witnesses apart. The <key>=<value>s are the builder's
 * account of what it built from (ace witness: commit, commons, tools, path,
 * cc) -- compared, not believed. A witness is the build it speaks for: the
 * loader that writes the statement is one of the binaries in it.
 *
 * `path` is the ACE_ROOT the tree was built at, which is compiled into the
 * loader and some modules: witnesses at one root build one thing, and
 * witnesses at two roots are two groups, each agreed on by its own.
 */
constexpr char kWitnessMagic[] = "etcs-witness 1 ";
constexpr char kWitnessSig[]   = "etcs-witness-sig ";

inline const char* build_arch()
{
#  if defined(_WIN32)
#    define ETCS_ARCH_OS "windows"
#  elif defined(__APPLE__)
#    define ETCS_ARCH_OS "darwin"
#  else
#    define ETCS_ARCH_OS "linux"
#  endif
#  if defined(__x86_64__) || defined(_M_X64)
    return ETCS_ARCH_OS "-x86_64";
#  elif defined(__aarch64__) || defined(_M_ARM64)
    return ETCS_ARCH_OS "-aarch64";
#  elif defined(__arm__)
    return ETCS_ARCH_OS "-arm";
#  elif defined(__riscv) && __riscv_xlen == 64
    return ETCS_ARCH_OS "-riscv64";
#  else
    return ETCS_ARCH_OS "-unknown";
#  endif
#  undef ETCS_ARCH_OS
}

// The digest a loader checks of `file` as `name` (its file's name, less what
// the platform adds, unless given). False if it is no binary a loader loads.
inline bool file_digest(const std::string& file, std::string& name, sig::Key& d, std::string& why)
{
    std::vector<uint8_t> b;
    sig::Layout L;
    if (!read_file(file, b)) { why = "cannot read " + file; return false; }
    if (!sig::layout(b.data(), b.size(), L, why)) return false;
    if (name.empty())
    {
        name = file.substr(file.find_last_of("/\\") + 1);
        name = dist_name(name);
    }
    if (!digest(name, b.data(), b.size(), L, d)) { why = "bad name " + name; return false; }
    return true;
}

struct Witness
{
    sig::Epoch                                                  epoch{};
    std::string                                                 arch;
    std::map<std::string, std::string>                          about;
    std::map<std::string, std::pair<std::string, sig::Key>>     files;      // name -> file, digest
    sig::Key                                                    key{};
};

inline bool about_word(const std::string& s)
{
    if (s.empty() || s.size() > 256) return false;
    for (char c : s) if (c <= ' ' || c == 0x7f) return false;
    return true;
}

/*
 * Two builds of one tree, `a` and `b`, and the statement that they came to
 * the same thing -- written to `out` only if every binary in `a` is in `b`
 * with the same digest, and this loader is one of them. `report` says what
 * was compared, or what differed.
 */
inline bool witness_write(const std::string& a, const std::string& b, const std::map<std::string, std::string>& about,
                          const std::string& out, std::string& report, std::string& why)
{
    namespace fs = std::filesystem;
    std::error_code ec;
    std::vector<std::string> names;
    for (const auto& e : fs::directory_iterator(a, ec))
        if (e.is_regular_file()) names.push_back(e.path().filename().string());
    if (ec) { why = "cannot read " + a + ": " + ec.message(); return false; }
    std::sort(names.begin(), names.end());

    Witness w;
    int differ = 0;
    for (const std::string& f : names)
    {
        std::string name, nb, wa, wb;
        sig::Key da{}, db{};
        if (!plain_file(f) || !file_digest(a + "/" + f, name, da, wa)) continue;      // not a binary
        nb = name;
        if (!file_digest(b + "/" + f, nb, db, wb)) { report += f + " is not in the second build\n"; ++differ; continue; }
        if (da != db) { report += f + " differs: " + hex(da.data(), 32) + " / " + hex(db.data(), 32) + "\n"; ++differ; continue; }
        report += f + " " + hex(da.data(), 32) + "\n";
        w.files[name] = { f, da };
    }
    if (differ) { why = std::to_string(differ) + " binar" + (differ == 1 ? "y" : "ies") + " came out differently in two builds"; return false; }
    if (w.files.empty()) { why = "no binaries in " + a; return false; }

    std::vector<uint8_t> me;
    std::string me_name;
    sig::Layout L;
    sig::Key mine{};
    const auto it = w.files.find("etcs");
    if (!own_binary(me_name, me) || !sig::layout(me.data(), me.size(), L, why) || !digest("etcs", me.data(), me.size(), L, mine)
        || it == w.files.end() || it->second.second != mine)
    { why = "this loader is not the build it witnesses: run " + a + "/etcs"; return false; }

    std::string body = std::string(kWitnessMagic) + "epoch=" + hex(loader_epoch().data(), 32) + " arch=" + build_arch();
    for (const auto& [k, v] : about)
    {
        if (k == "epoch" || k == "arch" || !about_word(k) || k.find('=') != std::string::npos || !about_word(v))
        { why = "not a word: " + k + "=" + v; return false; }
        body += " " + k + "=" + v;
    }
    body += "\n";
    for (const auto& [name, fd] : w.files) body += name + " " + fd.first + " " + hex(fd.second.data(), 32) + "\n";

    uint8_t seed[32], secret[64], s[sig::kSigBytes];
    sig::Key pub{};
    if (!random_bytes(seed, 32)) { why = "no randomness for the witness's key"; return false; }
    mc::crypto_ed25519_key_pair(secret, pub.data(), seed);
    const std::string m = "ETCSWIT1" + body;
    mc::crypto_ed25519_sign(s, secret, reinterpret_cast<const uint8_t*>(m.data()), m.size());
    mc::crypto_wipe(secret, 64);
    const std::string text = body + kWitnessSig + hex(pub.data(), 32) + " " + hex(s, sizeof s) + "\n";
    std::ofstream o(out, std::ios::binary | std::ios::trunc);
    if (!o.write(text.data(), std::streamsize(text.size())) || !o.flush()) { why = "cannot write " + out; return false; }
    report += "witness " + hex(pub.data(), 32) + "\n";
    return true;
}

inline bool witness_read(const std::string& text, Witness& w, std::string& why)
{
    const std::string tag = kWitnessSig;
    const size_t last = text.rfind("\n" + tag);
    if (text.compare(0, sizeof kWitnessMagic - 1, kWitnessMagic) != 0 || last == std::string::npos)
    { why = "not a witness statement"; return false; }
    const std::string body = text.substr(0, last + 1);
    std::istringstream sl(text.substr(last + 1 + tag.size()));
    std::string key_hex, sig_hex;
    uint8_t s[sig::kSigBytes];
    sl >> key_hex >> sig_hex;
    if (!unhex(key_hex, w.key.data(), 32) || !unhex(sig_hex, s, sizeof s)) { why = "the statement's signature is unreadable"; return false; }
    const std::string m = "ETCSWIT1" + body;
    if (mc::crypto_ed25519_check(s, w.key.data(), reinterpret_cast<const uint8_t*>(m.data()), m.size()) != 0)
    { why = "the statement's signature does not verify"; return false; }

    std::istringstream in(body);
    std::string line;
    std::getline(in, line);
    std::istringstream hs(line.substr(sizeof kWitnessMagic - 1));
    for (std::string kv; hs >> kv; )
    {
        const size_t eq = kv.find('=');
        if (eq == std::string::npos) { why = "a header word is not key=value: " + kv; return false; }
        w.about[kv.substr(0, eq)] = kv.substr(eq + 1);
    }
    if (!unhex(w.about["epoch"], w.epoch.data(), 32) || w.about["arch"].empty()) { why = "the statement names no epoch or arch"; return false; }
    w.arch = w.about["arch"];
    w.about.erase("epoch"); w.about.erase("arch");
    while (std::getline(in, line))
    {
        if (line.empty()) continue;
        std::istringstream ls(line);
        std::string name, file, dg;
        sig::Key d{};
        if (!(ls >> name >> file >> dg) || !plain_file(name) || !plain_file(file) || !unhex(dg, d.data(), 32))
        { why = "the statement has a line it cannot read: " + line; return false; }
        w.files[name] = { file, d };
    }
    return !w.files.empty() || (why = "the statement names no binaries", false);
}

/*
 * THE CONSENSUS: every statement read and verified; one epoch, and one account
 * of the source (commit, commons, tools), across all of them; and per arch and
 * root (`path`), every witness the same binaries with the same digests, from
 * at least `quorum` distinct witnesses. Where two witnesses of one group
 * disagree, the accounts they gave that differ are named -- the likely reason.
 * `check`, when given, is a directory whose binaries must be the consensus
 * build of one of this arch's roots.
 */
inline bool consensus(const std::vector<std::string>& texts, const std::vector<std::string>& names, size_t quorum,
                      const std::string& check, std::string& report)
{
    std::vector<Witness> ws(texts.size());
    bool ok = true;
    for (size_t i = 0; i < texts.size(); ++i)
    {
        std::string why;
        if (!witness_read(texts[i], ws[i], why)) { report += names[i] + ": " + why + "\n"; ok = false; }
    }
    if (!ok) return false;
    for (size_t i = 0; i < ws.size(); ++i)
        for (size_t j = i + 1; j < ws.size(); ++j)
            if (ws[i].key == ws[j].key) { report += names[i] + " and " + names[j] + " are one witness\n"; ok = false; }
    for (size_t i = 1; i < ws.size(); ++i)
    {
        if (ws[i].epoch != ws[0].epoch)
        { report += names[i] + " is in epoch " + hex(ws[i].epoch.data(), 32) + ", " + names[0] + " in " + hex(ws[0].epoch.data(), 32) + "\n"; ok = false; }
        for (const char* k : { "commit", "commons", "tools" })
            if (ws[i].about[k] != ws[0].about[k])
            { report += names[i] + " built " + k + " " + ws[i].about[k] + ", " + names[0] + " " + ws[0].about[k] + "\n"; ok = false; }
    }
    if (!ok) return false;

    // A group: one arch at one root.
    std::map<std::pair<std::string, std::string>, std::vector<size_t>> groups;
    for (size_t i = 0; i < ws.size(); ++i) groups[{ ws[i].arch, ws[i].about["path"] }].push_back(i);
    report += "epoch " + hex(ws[0].epoch.data(), 32) + "\n";
    std::vector<const Witness*> agreed;
    for (const auto& [group, idx] : groups)
    {
        const std::string arch = group.first + (group.second.empty() ? "" : " at " + group.second);
        const Witness& r = ws[idx[0]];
        bool agree = true;
        for (size_t k = 1; k < idx.size(); ++k)
        {
            const Witness& o = ws[idx[k]];
            std::string diff;
            for (const auto& [n, fd] : r.files)
            {
                const auto it = o.files.find(n);
                if (it == o.files.end()) diff += "  " + n + " is not in " + names[idx[k]] + "\n";
                else if (it->second.second != fd.second) diff += "  " + n + " differs\n";
            }
            for (const auto& [n, fd] : o.files)
                if (!r.files.count(n)) diff += "  " + n + " is not in " + names[idx[0]] + "\n";
            if (diff.empty()) continue;
            agree = false;
            report += arch + ": " + names[idx[k]] + " disagrees with " + names[idx[0]] + "\n" + diff;
            for (const auto& [key, v] : o.about)
            {
                const auto rv = r.about.find(key);
                if (rv == r.about.end() || rv->second != v)
                    report += "  they differ in " + key + ": " + (rv == r.about.end() ? "-" : rv->second) + " / " + v + "\n";
            }
        }
        if (!agree) { ok = false; continue; }
        if (idx.size() < quorum)
        {
            report += arch + ": " + std::to_string(idx.size()) + " witness" + (idx.size() == 1 ? "" : "es")
                    + ", fewer than " + std::to_string(quorum) + "\n";
            ok = false;
            continue;
        }
        report += arch + ": " + (idx.size() == 1 ? std::string("1 witness") : std::to_string(idx.size()) + " witnesses agree") + "\n";
        for (const auto& [n, fd] : r.files) report += "  " + n + " " + hex(fd.second.data(), 32) + "\n";
        if (group.first == build_arch()) agreed.push_back(&r);
    }
    if (!ok || check.empty()) return ok;

    if (agreed.empty()) { report += "check: no witness of " + std::string(build_arch()) + "\n"; return false; }
    namespace fs = std::filesystem;
    std::error_code ec;
    struct Bin { std::string file, name; sig::Key d; };
    std::vector<Bin> here;
    for (const auto& e : fs::directory_iterator(check, ec))
    {
        const std::string f = e.path().filename().string();
        std::string name, why;
        sig::Key d{};
        if (!e.is_regular_file() || !plain_file(f) || !file_digest(check + "/" + f, name, d, why)) continue;
        here.push_back({ f, name, d });
    }
    if (ec || here.empty()) { report += "check: no binaries in " + check + "\n"; return false; }
    std::sort(here.begin(), here.end(), [](const Bin& x, const Bin& y) { return x.file < y.file; });
    // The root whose consensus this directory is; else, against the first, what is not.
    for (const Witness* r : agreed)
    {
        bool all = true;
        for (const Bin& h : here)
        {
            const auto fd = r->files.find(h.name);
            all = all && fd != r->files.end() && fd->second.second == h.d;
        }
        if (!all) continue;
        const auto at = r->about.find("path");
        for (const Bin& h : here)
            report += "check: " + h.file + " is the consensus build" + (at == r->about.end() ? "" : " at " + at->second) + "\n";
        return true;
    }
    const Witness& r = *agreed[0];
    for (const Bin& h : here)
    {
        const auto fd = r.files.find(h.name);
        report += "check: " + h.file + (fd == r.files.end() ? " was not witnessed"
                                       : fd->second.second != h.d ? " is not the consensus build"
                                       : " is the consensus build") + "\n";
    }
    if (agreed.size() > 1) report += "check: and it is no other root's consensus either\n";
    return false;
}
#endif

#if !defined(__EMSCRIPTEN__)
/*
 * What ace asks of the loader that holds the key:
 *
 *   etcs --sign <module> <file> <epoch hex> <out>   0 signed, 3 already, 2 refused
 *   etcs --succeed <name> <file> <epoch hex> <out>  the same, for a loader binary: passes it off, and logs the edge
 *   etcs --registration <cert>...                   install the half above this key, leaf first; prints the identity
 *   etcs --issue <role> <key hex> <out>             a registrar certifies another loader's key; writes its chain
 *   etcs --request <registrar key hex> [next]       "<key> <head> <epoch> <signature>": a registration's ask, for
 *                                                   this loader or the successor it put in place
 *   etcs --join <registrar> <member key>            "<key> <signature>": this key agrees to join member's account
 *   etcs --admit <registrar> <joining key>          "<key> <signature>": this member admits it
 *   etcs --retire <registrar> <key>                 "<key> <signature>": this member retires a key of its account
 *   etcs --portal <registrar> <code>                "<key> <signature>": this member lets the browser showing
 *                                                   <code> act for its account until it signs out or times out
 *   etcs --manifest <dir> <out>                     the marketplace's signed list of what <dir> serves
 *   etcs --survey <list>                            what that list has moved past, here
 *   etcs --attest                                   what this loader says of itself; 0 only if it attests
 *   etcs --loader-key                               public key, made on first use
 *   etcs --identity                                 what this store builds with: the fold to its key (-DETCS_IDENTITY)
 *   etcs --identity-parent                          the fold one step short of it (-DETCS_IDENTITY_PARENT); 2 if none
 *   etcs --countersign <name> <file> <epoch hex> <out>
 *                                                   a release key countersigns a minted loader binary
 *   etcs --epoch                                    this loader's epoch
 *   etcs --digest <file> [name]                     the digest a loader checks of it
 *   etcs --witness <a> <b> <out> [key=value]...     two builds came to the same; run as <a>/etcs
 *   etcs --consensus [quorum=N] [check=<dir>] <statement>...
 *                                                   what the witnesses agree on; 0 only if they do
 *
 * -1 when argv names none of these.
 */
inline int cli(int argc, char** argv)
{
    launch_args().assign(argv, argv + argc);        // what a re-sync starts the successor with
    if (argc < 2) return -1;
    const std::string verb = argv[1];
    if (untrusted_build)
        for (const char* v : { "--sign", "--succeed", "--issue", "--countersign", "--manifest", "--witness" })
            if (verb == v)
            {
                std::fprintf(stderr, "etcs: an untrusted build (ETCS_UNTRUSTED) signs, witnesses and publishes nothing\n");
                return 4;
            }
    const std::string dir = etcs_store_dir();
    std::string why;
    if (verb == "--epoch" && argc == 2)
    {
        std::printf("%s\n", hex(loader_epoch().data(), 32).c_str());
        return 0;
    }
    if (verb == "--digest" && (argc == 3 || argc == 4))
    {
        std::string name = argc == 4 ? argv[3] : "";
        sig::Key d{};
        if (!file_digest(argv[2], name, d, why)) { std::fprintf(stderr, "etcs: %s\n", why.c_str()); return 2; }
        std::printf("%s %s\n", hex(d.data(), 32).c_str(), name.c_str());
        return 0;
    }
    if (verb == "--witness" && argc >= 5)
    {
        std::map<std::string, std::string> about;
        for (int i = 5; i < argc; ++i)
        {
            const std::string kv = argv[i];
            const size_t eq = kv.find('=');
            if (eq == std::string::npos || eq == 0) { std::fprintf(stderr, "etcs: not key=value: %s\n", argv[i]); return 2; }
            about[kv.substr(0, eq)] = kv.substr(eq + 1);
        }
        std::string report;
        const bool ok = witness_write(argv[2], argv[3], about, argv[4], report, why);
        std::printf("%s", report.c_str());
        if (!ok) { std::fprintf(stderr, "etcs: %s\n", why.c_str()); return 1; }
        std::printf("witnessed %s\n", argv[4]);
        return 0;
    }
    if (verb == "--consensus" && argc >= 3)
    {
        size_t quorum = 2;
        std::string check;
        std::vector<std::string> texts, names;
        for (int i = 2; i < argc; ++i)
        {
            const std::string a = argv[i];
            if (a.rfind("quorum=", 0) == 0) { quorum = std::strtoul(a.c_str() + 7, nullptr, 10); continue; }
            if (a.rfind("check=", 0) == 0)  { check = a.substr(6); continue; }
            std::vector<uint8_t> raw;
            if (!read_file(a, raw)) { std::fprintf(stderr, "etcs: cannot read %s\n", a.c_str()); return 2; }
            texts.emplace_back(raw.begin(), raw.end());
            names.push_back(a);
        }
        if (texts.empty()) { std::fprintf(stderr, "usage: etcs --consensus [quorum=N] [check=<dir>] <statement>...\n"); return 2; }
        std::string report;
        const bool ok = consensus(texts, names, quorum, check, report);
        std::printf("%s%s\n", report.c_str(), ok ? "consensus" : "no consensus");
        return ok ? 0 : 1;
    }
    if (verb == "--identity-parent" && argc == 2)
    {
        sig::Key head{}, parent{};
        if (!store_identity(dir, head, why, &parent)) { std::fprintf(stderr, "etcs: %s\n", why.c_str()); return 2; }
        if (zero(parent)) { std::fprintf(stderr, "etcs: this store is not registered: its identity has no parent\n"); return 2; }
        std::printf("%s\n", hex(parent.data(), 32).c_str());
        return 0;
    }
    if (verb == "--countersign")
    {
        sig::Epoch e{};
        if (argc != 6 || !unhex(argv[4], e.data(), e.size()))
        { std::fprintf(stderr, "usage: etcs --countersign <name> <file> <epoch hex> <out>\n"); return 2; }
        if (countersign(dir, argv[2], argv[3], e, argv[5], why) != 0) { std::fprintf(stderr, "etcs: %s: %s\n", argv[3], why.c_str()); return 2; }
        std::printf("countersigned %s\n", argv[3]);
        return 0;
    }
    if (verb == "--identity" && argc == 2)
    {
        sig::Key head{};
        if (!store_identity(dir, head, why)) { std::fprintf(stderr, "etcs: %s\n", why.c_str()); return 2; }
        std::printf("%s\n", hex(head.data(), 32).c_str());
        return 0;
    }
    if (verb == "--loader-key" && argc == 2)
    {
        uint8_t secret[64];
        sig::Key pub{};
        if (!loader_key(dir, secret, pub, why)) { std::fprintf(stderr, "etcs: %s\n", why.c_str()); return 2; }
        mc::crypto_wipe(secret, 64);
        std::printf("%s\n", hex(pub.data(), 32).c_str());
        return 0;
    }
    if (verb == "--issue")
    {
        sig::Role r{};
        sig::Key subject{};
        if (argc != 5 || !sig::role_from_name(argv[2], r) || !unhex(argv[3], subject.data(), subject.size()))
        { std::fprintf(stderr, "usage: etcs --issue <role> <key hex> <out>\n"); return 2; }
        if (issue(dir, r, subject, argv[4], why) != 0) { std::fprintf(stderr, "etcs: %s\n", why.c_str()); return 2; }
        std::printf("issued %s %s\n", sig::role_name(r), argv[3]);
        return 0;
    }
    if (verb == "--sign" || verb == "--succeed")
    {
        sig::Epoch e{};
        std::string line;
        if (argc != 6 || !unhex(argv[4], e.data(), e.size()))
        { std::fprintf(stderr, "usage: etcs %s <name> <file> <epoch hex> <out>\n", verb.c_str()); return 2; }
        const int rc = verb == "--sign" ? sign_module(dir, argv[2], argv[3], e, argv[5], why)
                                        : succeed(dir, argv[2], argv[3], e, argv[5], why, &line);
        if (rc == 2) std::fprintf(stderr, "etcs: %s: %s\n", argv[3], why.c_str());
        if (rc == 0 && !why.empty()) std::fprintf(stderr, "etcs: warning: %s: %s\n", argv[3], why.c_str());
        if (!line.empty()) std::printf("%s\n", line.c_str());
        return rc;
    }
    if (verb == "--registration" && argc >= 3)
    {
        std::vector<sig::Certificate> reg;
        for (int i = 2; i < argc; ++i)
            if (!read_certificates(argv[i], reg))
            { std::fprintf(stderr, "etcs: %s is not a certificate or a chain\n", argv[i]); return 2; }
        if (!install_registration(dir, reg, why)) { std::fprintf(stderr, "etcs: %s\n", why.c_str()); return 2; }
        sig::Key head{};
        store_identity(dir, head, why);
        std::printf("registered as %s under root %s\nidentity %s\n", sig::role_name(reg[0].role),
                    hex(reg.back().subject.data(), 32).c_str(), hex(head.data(), 32).c_str());
        return 0;
    }
    if (verb == "--request" && (argc == 3 || (argc == 4 && std::string(argv[3]) == "next")))
    {
        sig::Key registrar{};
        std::string line;
        if (!unhex(argv[2], registrar.data(), registrar.size())) { std::fprintf(stderr, "etcs: not a registrar's key\n"); return 2; }
        if (!registration_request(dir, registrar, line, why, argc == 4)) { std::fprintf(stderr, "etcs: %s\n", why.c_str()); return 2; }
        std::printf("%s\n", line.c_str());
        return 0;
    }
    if ((verb == "--join" || verb == "--admit" || verb == "--retire" || verb == "--portal") && argc == 4)
    {
        sig::Key registrar{}, other{};
        std::string line;
        if (!unhex(argv[2], registrar.data(), registrar.size()) || !unhex(argv[3], other.data(), other.size()))
        { std::fprintf(stderr, "usage: etcs %s <registrar key hex> <%s hex>\n", verb.c_str(), verb == "--portal" ? "code" : "key"); return 2; }
        if (!account_word(dir, verb[2], registrar, other, line, why)) { std::fprintf(stderr, "etcs: %s\n", why.c_str()); return 2; }
        if (verb == "--portal")
            std::fprintf(stderr, "etcs: this lets the browser showing that code act for your account -- see who is in it, admit and "
                                 "retire keys -- until it signs out or times out. Paste it only into a page you opened yourself.\n");
        std::printf("%s\n", line.c_str());
        return 0;
    }
    if (verb == "--manifest" && argc == 4)
    {
        std::string text;
        std::vector<std::string> left;
        if (!publish(dir, argv[2], loader_epoch(), text, why, &left)) { std::fprintf(stderr, "etcs: %s\n", why.c_str()); return 2; }
        for (const std::string& l : left) std::fprintf(stderr, "etcs: left off %s\n", l.c_str());
        std::ofstream o(argv[3], std::ios::binary | std::ios::trunc);
        if (!o.write(text.data(), std::streamsize(text.size())) || !o.flush()) { std::fprintf(stderr, "etcs: cannot write %s\n", argv[3]); return 2; }
        std::printf("published %s\n", argv[3]);
        return left.empty() ? 0 : 1;
    }
    if (verb == "--survey" && argc == 3)
    {
        std::vector<uint8_t> raw;
        std::string report;
        const std::string self = own_path();
        if (!read_file(argv[2], raw)) { std::fprintf(stderr, "etcs: cannot read %s\n", argv[2]); return 2; }
        if (!survey(std::string(raw.begin(), raw.end()), report, why, *establish(dir), loader_epoch(),
                    self.substr(0, self.find_last_of("/\\") + 1), self, dir))
        { std::fprintf(stderr, "etcs: %s\n", why.c_str()); return 2; }
        std::printf("%s", report.c_str());
        return 0;
    }
    if (verb == "--attest" && argc == 2)
    {
        const std::shared_ptr<const Identity> id = establish(dir);
        std::printf("binary    %s %s\n", id->self_name.c_str(), hex(id->self.data(), 32).c_str());
        if (zero(id->pub)) std::printf("key       none in this store\n");
        else std::printf("key       %s (%s, %s)\n", hex(id->pub.data(), 32).c_str(),
                         id->reg.empty() ? "local half only" : sig::role_name(id->role),
                         id->proven ? "proven" : "not proven");
        if (untrusted_build) std::printf("UNTRUSTED build (ETCS_UNTRUSTED): modules load unchecked; it signs nothing\n");
        if (zero(id->head)) std::printf("identity  none built in: an experimental epoch\n");
        else std::printf("identity  %s (built in)%s\n", hex(id->head.data(), 32).c_str(),
                         own_head(id->head, id->builder) ? ": its builder's key alone" : "");
        if (!zero(id->head_parent)) std::printf("parent    %s (built in): a successor under a sibling marketplace bridges\n",
                                                hex(id->head_parent.data(), 32).c_str());
        if (!zero(id->builder)) std::printf("builder   %s (built in)\n", hex(id->builder.data(), 32).c_str());
        std::printf("passed    %s\n", zero(id->parent) ? "by no one" : id->parent == id->self ? "by itself"
                                       : ("by " + hex(id->parent.data(), 32)).c_str());
        std::printf("attested  %s%s%s\n", id->attested ? "yes" : "no", id->attested ? "" : ": ",
                    id->attested ? "" : id->unattested.c_str());
        if (!id->proven) std::printf("unproven  %s\n", id->unproven.c_str());
        return id->attested ? 0 : 1;
    }
    return -1;
}
#endif

} // namespace trust
} // namespace ETCS

#endif // TRUST_H__
