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
 *              and climbs through strictly higher roles to the root. Passing,
 *              it says who signed.
 *   Own        a Loader-signed module is taken only by the loader whose key
 *              signed it, and only when that loader has proven the key is its
 *              own this session. Under a temporary root that is the rule for
 *              the marketplace's role too: such a root vouches for no one else.
 *
 * THE LOADER IS THE ONE BINARY NOTHING ELSE CHECKS, so it attests itself: at
 * the handshake it puts its own bytes through its own two stages. What that
 * buys is the LINEAGE -- once the root is ACE's, only a loader that attests
 * signs, so the next etcs is signed by a valid etcs, and each such signing is
 * an edge appended to lineage.log beside the epochs it joins.
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
#include "AceRoot.h"
#include "Provenance.h"
#include "Keystore.h"
#include "InterfaceWire.h"
#include "MirrorBuffer.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <atomic>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
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

/*
 * THE ROOT, built in: the super-root's public key, pinned in ACE-Build-Tools
 * and generated into core/AceRoot.h -- a core header, so the root is in the
 * epoch. It is what a downloaded loader holds before it holds anything else:
 * every chain it takes ends here, its own binary's first.
 *
 * Zero is no root, and a loader then works under its store's (configuring).
 * What follows takes the root as `built_in`, this by default, so either state
 * can be held to account from a loader built in the other.
 */
inline const sig::Key& ace_root()
{
    static const sig::Key k = [] {
        sig::Key r{};
        if (!unhex(ETCS_ACE_ROOT_KEY, r.data(), r.size())) r = sig::Key{};
        return r;
    }();
    return k;
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
 * What this loader is for a session. `temporary`: the root is not ACE's -- it
 * is whatever this store was configured with, and vouches for this loader
 * only. `proven`: the keystore's key is `pub`'s, checked when the session
 * began (establish).
 */
struct Identity
{
    sig::Key    root{};
    bool        temporary = true;
    sig::Key    pub{};
    bool        proven = false;
    std::string unproven;
    std::vector<sig::Certificate> reg;      // this key's cert <- ... <- root
    sig::Role   role = sig::Role::Loader;   // what this key is registered as: Loader, or Marketplace

    // This binary, by its own account (attest_self).
    std::string self_name;
    sig::Key    self{};
    bool        attested = false;
    std::string unattested;
    bool        genesis = false;            // the store has no lineage yet: this loader may begin one
};

/*
 * The frame at the gate. A module is megabytes and a frame is not, so the
 * frame REFERS to the bytes (the 256-byte rule, one size up). Going in:
 * name, bytes, epoch. The Signature stage adds who signed; on the way out
 * (signing) it fills `chain`, or says the bytes already carry it.
 */
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
};

inline bool load_frame(const std::string& name, const uint8_t* b, size_t n, const sig::Epoch& epoch, LoadFrame& f)
{
    if (name.empty() || name.size() > 255) return false;
    f = LoadFrame{};
    std::memcpy(f.magic, "ETCSLOAD", 8);
    std::memcpy(f.name, name.data(), name.size());
    f.bytes = b; f.len = n; f.epoch = epoch;
    return true;
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
        if (chain.size() < 2) return refuse(io, "the signer has no certificate");
        if (chain[1].role != sig::Role::Loader && chain[1].role != sig::Role::Marketplace)
            return refuse(io, "modules are signed by the marketplace or by a loader");

        sig::Key key = leaf.issuer;
        sig::Role below = sig::Role::Module;
        for (size_t i = 1; i < chain.size(); ++i)
        {
            const sig::Certificate& c = chain[i];
            if (c.subject != key || uint8_t(c.role) >= uint8_t(below)) return refuse(io, "chain is broken");
            if (!c.epochs.empty() && !has_epoch(c, f.epoch)) return refuse(io, "witness not valid in this epoch");
            if (!check(c)) return refuse(io, "bad signature in chain");
            if (c.role == sig::Role::Root && (c.issuer != c.subject || i + 1 != chain.size()))
                return refuse(io, "root is not last or not self-issued");
            key = c.issuer;
            below = c.role;
        }
        if (zero(id->root) || key != id->root) return refuse(io, "does not chain to this loader's root");
        f.signer_role = uint8_t(chain[1].role);
        f.signer = leaf.issuer;
        give(io, f);
    }

    // The chain for these bytes: this loader's leaf and its registration. A
    // leaf by this key on these same bytes, under this same registration,
    // carries its epochs forward.
    void Wrap(MBuffer& io, SignalContext) override
    {
        LoadFrame f;
        sig::Layout L;
        std::string w;
        sig::Certificate c;
        c.role = sig::Role::Module;
        if (!take(io, f) || !id || !f.chain) return refuse(io, "not a load this gate was set up for");
        if (!sig::layout(f.bytes, f.len, L, w)) return refuse(io, w);
        if (!digest(f.name, f.bytes, f.len, L, c.subject)) return refuse(io, "bad module name");
        if (id->reg.empty() || zero(id->pub)) return refuse(io, "this loader has no registration");

        std::vector<sig::Certificate> old;
        const bool carried =
            L.has_chain && sig::decode_chain(f.bytes + L.chain.at, L.chain.len, old)
            && old.size() == id->reg.size() + 1
            && sig::encode_chain({ old.begin() + 1, old.end() }) == sig::encode_chain(id->reg)
            && old[0].role == sig::Role::Module && old[0].issuer == id->pub && old[0].subject == c.subject && check(old[0]);
        if (carried && has_epoch(old[0], f.epoch)) { f.already = 1; return give(io, f); }
        if (!signer) return refuse(io, "the key is not here");
        if (!id->temporary && !id->attested && !id->genesis)
            return refuse(io, "only a loader in the lineage signs, and this one does not attest: " + id->unattested);
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
        chain.insert(chain.end(), id->reg.begin(), id->reg.end());
        *f.chain = sig::encode_chain(chain);
        give(io, f);
    }
};

struct OwnStage : Stage
{
    void Unwrap(MBuffer& io, SignalContext) override
    {
        LoadFrame f;
        if (!take(io, f) || !id) return refuse(io, "not a load this gate was set up for");
        const bool loader = f.signer_role == uint8_t(sig::Role::Loader);
        if (!loader && f.signer_role != uint8_t(sig::Role::Marketplace))
            return refuse(io, "nothing has said who signed this");
        if (loader || id->temporary)
        {
            if (!id->proven) return refuse(io, "signed by a loader, and this one has not proven a key: " + id->unproven);
            if (f.signer != id->pub)
                return refuse(io, loader ? "signed by another loader"
                                         : "a temporary root has no marketplace but this loader's own");
        }
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
                   const Identity& id, std::string& why)
{
    Gate g;
    LoadFrame f;
    g.use(std::make_shared<const Identity>(id));
    if (!load_frame(name, b, n, epoch, f)) { why = "bad module name"; return false; }
    return g.unwrap(f, why);
}

/*
 * THIS LOADER'S IDENTITY, under etcs_store_dir:
 *
 *   the key           its Ed25519 seed, in the OS keystore (core/Keystore.h) as
 *                     "loader-key" for this store -- never in a file
 *   loader.sign.pub   the public half; what verification reads
 *   loader.chain      its registration: Loader cert <- intermediate <- root
 *   root.pub          the root registration ends at, while ace_root() is zero
 *
 * WHILE THERE IS NO ACE ROOT the loader registers itself under a TEMPORARY
 * root, made here, so the whole path runs before ACE registers anyone: root
 * and intermediate are generated, certify, and are wiped; only their
 * certificates stay. Once ace_root() is set, a chain not ending there is no
 * registration: one is injected (install_registration), and a loader with
 * none signs nothing but still takes what the marketplace signed.
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

inline bool load_root(const std::string& dir, sig::Key& root, const sig::Key& built_in = ace_root())
{
    if (!zero(built_in)) { root = built_in; return true; }
    return read_hex_file(dir + "/root.pub", root.data(), root.size());
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

// `reg` links up to `root` and starts at a certificate for `pub` as a signer of
// modules: Loader, or Marketplace (a build server's loader).
inline bool registration_valid(const std::vector<sig::Certificate>& reg, const sig::Key& pub, const sig::Key& root)
{
    if (reg.size() < 2 || reg[0].subject != pub || zero(root)) return false;
    if (reg[0].role != sig::Role::Loader && reg[0].role != sig::Role::Marketplace) return false;
    sig::Key key = pub;
    sig::Role below = sig::Role::Module;
    for (size_t i = 0; i < reg.size(); ++i)
    {
        const sig::Certificate& c = reg[i];
        if (c.subject != key || uint8_t(c.role) >= uint8_t(below) || !check(c)) return false;
        if (c.role == sig::Role::Root && (c.issuer != c.subject || i + 1 != reg.size())) return false;
        key = c.issuer;
        below = c.role;
    }
    return key == root;
}

inline bool temporary_registration(const std::string& dir, const sig::Key& pub,
                                   std::vector<sig::Certificate>& reg, std::string& why)
{
    uint8_t seed[32], root_sk[64], inter_sk[64];
    sig::Key root{}, inter{};
    if (!random_bytes(seed, 32)) { why = "no randomness"; return false; }
    mc::crypto_ed25519_key_pair(root_sk, root.data(), seed);
    if (!random_bytes(seed, 32)) { mc::crypto_wipe(root_sk, 64); why = "no randomness"; return false; }
    mc::crypto_ed25519_key_pair(inter_sk, inter.data(), seed);
    auto issue = [](const uint8_t sk[64], const sig::Key& issuer, sig::Role role, const sig::Key& subject) {
        sig::Certificate c;
        c.role = role; c.subject = subject; c.issuer = issuer;
        const std::string m = sig::signed_part(c);
        mc::crypto_ed25519_sign(c.signature, sk, reinterpret_cast<const uint8_t*>(m.data()), m.size());
        return c;
    };
    reg = { issue(inter_sk, inter, sig::Role::Loader, pub),
            issue(root_sk, root, sig::Role::Intermediate, inter),
            issue(root_sk, root, sig::Role::Root, root) };
    mc::crypto_wipe(root_sk, 64);
    mc::crypto_wipe(inter_sk, 64);
    if (!write_public(dir + "/root.pub", hex(root.data(), 32) + "\n")
        || !write_public(dir + "/loader.chain", sig::encode_chain(reg)))
    { why = "cannot write the registration into " + dir; return false; }
    return true;
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

// The key and its registration: what a signer needs. Only a temporary root
// registers on first use; under ACE's the registration is injected.
inline bool identity(const std::string& dir, uint8_t secret[64], sig::Key& pub,
                     std::vector<sig::Certificate>& reg, std::string& why, const sig::Key& built_in = ace_root())
{
    if (!loader_key(dir, secret, pub, why)) return false;
    std::vector<uint8_t> raw;
    sig::Key root{};
    if (read_file(dir + "/loader.chain", raw) && sig::decode_chain(raw.data(), raw.size(), reg)
        && load_root(dir, root, built_in) && registration_valid(reg, pub, root))
        return true;
    reg.clear();
    if (!zero(built_in))
    {
        mc::crypto_wipe(secret, 64);
        why = "this loader's key is not registered under ACE's root (etcs --loader-key, then etcs --registration)";
        return false;
    }
    if (!temporary_registration(dir, pub, reg, why)) { mc::crypto_wipe(secret, 64); return false; }
    return true;
}
/*
 * INJECTING A REGISTRATION issued somewhere else -- by the active key, at a
 * ceremony -- for this loader's key: the place the higher-order certificates
 * enter. Leaf first, as certificates (binary or hex, as Custody keeps them) or
 * one chain. It must be for this loader's key, link to a self-issued root, and
 * that root must be ACE's once ace_root() is set; while it is not, the root
 * the chain brings becomes this store's (any key, while configuring).
 */
inline bool read_certificates(const std::string& path, std::vector<sig::Certificate>& out)
{
    std::vector<uint8_t> raw;
    if (!read_file(path, raw)) return false;
    std::vector<sig::Certificate> chain;
    sig::Certificate c;
    size_t used = 0;
    if (sig::decode_chain(raw.data(), raw.size(), chain)) { out.insert(out.end(), chain.begin(), chain.end()); return true; }
    if (sig::decode(raw.data(), raw.size(), c, &used) && used == raw.size()) { out.push_back(c); return true; }
    std::string text(raw.begin(), raw.end());
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ')) text.pop_back();
    std::vector<uint8_t> bin(text.size() / 2);
    if (text.size() % 2 || bin.empty() || !unhex(text, bin.data(), bin.size())) return false;
    if (!sig::decode(bin.data(), bin.size(), c, &used) || used != bin.size()) return false;
    out.push_back(c);
    return true;
}

inline bool install_registration(const std::string& dir, const std::vector<sig::Certificate>& reg, std::string& why,
                                 const sig::Key& built_in = ace_root())
{
    uint8_t secret[64];
    sig::Key pub{};
    if (!loader_key(dir, secret, pub, why)) return false;                 // the key must be this loader's to register
    mc::crypto_wipe(secret, 64);
    if (reg.empty() || reg.back().role != sig::Role::Root) { why = "the chain does not end at a root certificate"; return false; }
    const sig::Key root = reg.back().subject;
    if (!registration_valid(reg, pub, root)) { why = "not a registration of this loader's key"; return false; }
    if (!zero(built_in) && root != built_in) { why = "the chain's root is not ACE's"; return false; }
    if (zero(built_in) && !write_public(dir + "/root.pub", hex(root.data(), 32) + "\n"))
    { why = "cannot write " + dir + "/root.pub"; return false; }
    if (!write_public(dir + "/loader.chain", sig::encode_chain(reg))) { why = "cannot write " + dir + "/loader.chain"; return false; }
    return true;
}

/*
 * THE LINEAGE: one line per loader binary signed, appended to lineage.log in
 * the store. An edge says which binary's key signed which, across which
 * epochs, when and where -- the last two are the signer's word, for the
 * trace; nothing is valid or invalid by them. The signer signs the line.
 *
 *   etcs-lineage 1 <name> parent=<digest|genesis> attested=<0|1> child=<digest>
 *       from=<epoch> to=<epoch> key=<signer> at=<unix seconds> host=<name> sig=<hex>
 *
 * `parent=genesis` is a loader signing itself: where a lineage starts.
 */
struct LineageEdge
{
    std::string name, host;
    bool        genesis = false, attested = false;
    sig::Key    parent{}, child{}, key{};
    sig::Epoch  from{}, to{};
    uint64_t    at = 0;
    uint8_t     signature[sig::kSigBytes] = {};
};

inline std::string lineage_text(const LineageEdge& e)
{
    return "etcs-lineage 1 " + e.name
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
    if (cut == std::string::npos || line.compare(0, 15, "etcs-lineage 1 ") != 0) return false;
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

// This process's own binary: its path's last name, and its bytes.
inline bool own_binary(std::string& name, std::vector<uint8_t>& bytes)
{
    char path[4096] = {};
#if defined(_WIN32)
    if (GetModuleFileNameA(nullptr, path, sizeof path - 1) == 0) return false;
#else
    if (::readlink("/proc/self/exe", path, sizeof path - 1) <= 0) return false;
#endif
    name = path;
    const size_t slash = name.find_last_of("/\\");
    if (slash != std::string::npos) name.erase(0, slash + 1);
    if (name.size() > 4 && name.compare(name.size() - 4, 4, ".exe") == 0) name.erase(name.size() - 4);
    return read_file(path, bytes) && !bytes.empty();
}

#endif // !__EMSCRIPTEN__

#define ETCS_TRUST_STR2(x) #x
#define ETCS_TRUST_STR(x)  ETCS_TRUST_STR2(x)

/*
 * THE HANDSHAKE: what this loader is, for this session, from its store. The
 * public files say what it claims; the keystore says whether the claim is its
 * own, and only then is a Loader-signed module taken. A browser holds no key
 * and reads no store: its claim and its root are the build's (ace's defines).
 * `prove` false reads the claim only -- enough to recognise a chain as
 * already there, which is all a build with nothing to sign needs.
 */
inline std::shared_ptr<const Identity> establish(const std::string& dir, bool prove = true,
                                                 const sig::Key& built_in = ace_root())
{
    auto id = std::make_shared<Identity>();
    id->root = built_in;
    id->temporary = zero(id->root);
#if defined(ETCS_TEMP_ROOT_KEY)
    if (id->temporary) unhex(ETCS_TRUST_STR(ETCS_TEMP_ROOT_KEY), id->root.data(), id->root.size());
#endif
#if defined(ETCS_BUILDER_KEY)
    if (unhex(ETCS_TRUST_STR(ETCS_BUILDER_KEY), id->pub.data(), id->pub.size())) id->proven = true;
#endif
#if defined(__EMSCRIPTEN__)
    (void)dir; (void)prove;
    if (!id->proven) id->unproven = "this page was built without a loader key";
    id->unattested = "a browser loader is what its page serves; it does not attest itself";
#else
    if (id->temporary) load_root(dir, id->root, built_in);
    if (!load_public(dir, id->pub)) id->unproven = "this store has no loader key yet";
    else
    {
        std::vector<uint8_t> raw;
        if (!read_file(dir + "/loader.chain", raw) || !sig::decode_chain(raw.data(), raw.size(), id->reg)
            || !registration_valid(id->reg, id->pub, id->root))
            id->reg.clear();
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

    // The loader attests itself: its own bytes through its own two stages,
    // as the identity just established. Nothing else will check this binary.
    // Under ACE's root this asks nothing of the store -- a loader just
    // downloaded attests by the root it was built with.
    std::vector<LineageEdge> edges;
    std::vector<uint8_t> me;
    std::string w;
    id->genesis = lineage_read(dir, edges, w) && edges.empty();
    if (!own_binary(id->self_name, me)) id->unattested = "this loader cannot read its own binary";
    else
    {
        sig::Layout L;
        if (sig::layout(me.data(), me.size(), L, w)) digest(id->self_name, me.data(), me.size(), L, id->self);
        id->attested = verify(id->self_name, me.data(), me.size(), loader_epoch(), *id, id->unattested);
    }
#endif
    return id;
}

// This session's gate. The handshake runs once, at the first load.
inline Gate& gate()
{
    static Gate g;
    static std::once_flag once;
    std::call_once(once, [] { g.use(establish(etcs_store_dir())); });
    return g;
}

// The entity whose Wrapper children stand on the gate, outside the loader's
// own stages; made the first time one is wanted (core/TrustGate.h).
Entity* gate_entity();

#if !defined(__EMSCRIPTEN__)
/*
 * Signs `file` as `name` under `epoch`: the gate, wrapped. 0 the chain is in
 * `out`, 3 already signed so (nothing written), 2 refused. What is already
 * there is judged from the public files alone; the key is asked for only when
 * there is something to sign.
 *
 * `edge`, when given, is filled for a signing that happened: who this loader
 * is to the bytes it signed (succeed).
 */
inline int sign_module(const std::string& dir, const std::string& name, const std::string& file,
                       const sig::Epoch& epoch, const std::string& out, std::string& why,
                       LineageEdge* edge = nullptr, const sig::Key& built_in = ace_root())
{
    std::vector<uint8_t> b;
    std::string chain;
    LoadFrame f;
    if (!read_file(file, b)) { why = "cannot read " + file; return 2; }
    if (!load_frame(name, b.data(), b.size(), epoch, f)) { why = "bad module name"; return 2; }
    f.chain = &chain;
    {
        Gate seen;
        LoadFrame probe = f;
        std::string ignored;
        seen.use(establish(dir, false, built_in));
        if (seen.wrap(probe, ignored) && probe.already) return 3;
    }
    uint8_t secret[64];
    sig::Key pub{};
    std::vector<sig::Certificate> reg;
    if (!identity(dir, secret, pub, reg, why, built_in)) return 2;      // makes the key on first use
    const std::shared_ptr<const Identity> id = establish(dir, true, built_in);
    Gate g;
    g.use(id);
    g.signature.signer = [&secret](const uint8_t* m, size_t n, uint8_t* sig64) { mc::crypto_ed25519_sign(sig64, secret, m, n); };
    const bool ok = g.wrap(f, why);
    if (ok && !f.already && edge)
    {
        sig::Layout L;
        std::string w;
        edge->name = name; edge->host = host_name(); edge->key = id->pub;
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
 * Signs the next loader binary and records the edge: this loader -> that one.
 * The same signing as any module's; what it adds is the line in lineage.log.
 * The same edge -- this parent, this child, this epoch -- is not written twice,
 * and a loader signing its own bytes is an edge only where there is none yet.
 */
inline int succeed(const std::string& dir, const std::string& name, const std::string& file,
                   const sig::Epoch& epoch, const std::string& out, std::string& why, std::string* line_out = nullptr,
                   const sig::Key& built_in = ace_root())
{
    LineageEdge e;
    const int rc = sign_module(dir, name, file, epoch, out, why, &e, built_in);
    if (rc != 0) return rc;
    std::vector<LineageEdge> had;
    std::string w;
    if (!lineage_read(dir, had, w)) { why = w; return 2; }
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
#endif // !__EMSCRIPTEN__

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
                          Gate& g = gate(), const sig::Epoch& epoch = loader_epoch())
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
    return g.unwrap(f, why);
}

#if !defined(__EMSCRIPTEN__)
/*
 * What ace asks of the loader that holds the key:
 *
 *   etcs --sign <module> <file> <epoch hex> <out>   0 signed, 3 already, 2 refused
 *   etcs --succeed <name> <file> <epoch hex> <out>  the same, for the next loader binary: adds the lineage edge
 *   etcs --registration <cert>...                   install a registration issued elsewhere, leaf first
 *   etcs --attest                                   what this loader says of itself; 0 only if it attests
 *   etcs --loader-key                               public key, made on first use (and registered, under a temporary root)
 *   etcs --root                                     the root: ACE's if built in, else this store's
 *   etcs --epoch                                    this loader's epoch
 *
 * -1 when argv names none of these.
 */
inline int cli(int argc, char** argv)
{
    if (argc < 2) return -1;
    const std::string verb = argv[1];
    const std::string dir = etcs_store_dir();
    std::string why;
    if (verb == "--epoch" && argc == 2)
    {
        std::printf("%s\n", hex(loader_epoch().data(), 32).c_str());
        return 0;
    }
    if ((verb == "--loader-key" || verb == "--root") && argc == 2)
    {
        uint8_t secret[64];
        sig::Key pub{}, root = ace_root();
        std::vector<sig::Certificate> reg;
        // Under ACE's root neither answer waits on a registration: the root is
        // built in, and the key is asked for in order to be registered.
        const bool built_in = !zero(root);
        if (!(built_in && verb == "--root"))
        {
            if (!(built_in ? loader_key(dir, secret, pub, why) : identity(dir, secret, pub, reg, why)))
            { std::fprintf(stderr, "etcs: %s\n", why.c_str()); return 2; }
            mc::crypto_wipe(secret, 64);
        }
        if (verb == "--root" && !load_root(dir, root)) { std::fprintf(stderr, "etcs: no root\n"); return 2; }
        const sig::Key& k = verb == "--root" ? root : pub;
        std::printf("%s\n", hex(k.data(), 32).c_str());
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
        std::printf("registered as %s under root %s\n", sig::role_name(reg[0].role), hex(reg.back().subject.data(), 32).c_str());
        return 0;
    }
    if (verb == "--attest" && argc == 2)
    {
        const std::shared_ptr<const Identity> id = establish(dir);
        std::printf("binary    %s %s\n", id->self_name.c_str(), hex(id->self.data(), 32).c_str());
        if (zero(id->pub)) std::printf("key       none in this store\n");
        else std::printf("key       %s (%s%s)\n", hex(id->pub.data(), 32).c_str(), sig::role_name(id->role),
                         id->reg.empty() ? ", not registered" : id->proven ? ", proven" : ", not proven");
        std::printf("root      %s (%s)\n", hex(id->root.data(), 32).c_str(),
                    id->temporary ? "this store's, configuring" : "ACE, built in");
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
