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
 *   Own        what a signature may stand for: a loader binary is a release
 *              key's, or this loader's own or its builder's; a release key
 *              signs no module.
 *
 * THE LOADER IS THE ONE BINARY NOTHING ELSE CHECKS, so it attests itself: at
 * the handshake it puts its own bytes through its own two stages. What that
 * buys is the LINEAGE -- once the root is ACE's, only a loader that attests
 * signs, so the next etcs is signed by a valid etcs, and each such signing is
 * an edge appended to lineage.log beside the epochs it joins.
 *
 * A CHAIN IS TWO HALVES (core/Signature.h). The local half -- this loader's key
 * and what it signs -- is taken by nothing but that loader. ACE's half is
 * joined on by a registration, and is what lets anyone else take what the key
 * signs.
 *
 * A MODULE'S CHAIN MUST MEET THIS LOADER'S, at any length: it ends at a key on
 * this loader's own chain -- its key, proven at the handshake; the key it was
 * built by (ETCS_BUILDER_KEY); a certificate of its registration; the root it
 * was built with. A module signed by another loader's key with nothing above
 * it meets nothing, and is the case refused by default. What meets only below
 * the root -- this loader's own, its builder's -- runs LOCAL: no transport
 * bridges it (MirrorBuffer::localFrame). What reaches the root is the global
 * surface. With ACE's root built in, a loader is in the GUILD; with none, it is
 * EXPERIMENTAL, everything it runs is local, and nothing places its epoch among
 * anyone else's.
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
#include <filesystem>
#include <fstream>
#include <functional>
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

/*
 * THE ROOT, built in: the super-root's public key, pinned in ACE-Build-Tools
 * and generated into core/AceRoot.h -- a core header, so the root is in the
 * epoch. It is what a downloaded loader holds before it holds anything else:
 * every chain it takes ends here, its own binary's first.
 *
 * Zero is no root: the loader then has its local half and nothing else.
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
 * What this loader is for a session. `root`: ACE's, built in, or zero -- the
 * guild, or an experimental epoch. `builder`: the key of the loader that built
 * this one, built in. `reg`: ACE's half above `pub`, once registered; empty is
 * the local half alone. `proven`: the keystore's key is `pub`'s, checked when
 * the session began (establish).
 */
struct Identity
{
    sig::Key    root{};
    sig::Key    builder{};
    sig::Key    pub{};
    bool        proven = false;
    std::string unproven;
    std::vector<sig::Certificate> reg;      // this key's cert <- ... <- root; empty: unregistered
    sig::Role   role = sig::Role::Loader;   // what reg[0] makes this key; Loader while unregistered

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
 * name, bytes, epoch, and whether the bytes are a loader's own (`kernel`).
 * The Signature stage adds who signed -- role 0 when the chain is the local
 * half alone -- and whether the chain met this loader's below the root
 * (`local`); on the way out (signing) it fills `chain`, or says the bytes
 * already carry it.
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
    uint8_t        kernel;
    uint8_t        local;
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
 * Where a module's chain may end: on this loader's own chain. `role` is what
 * that key is there, so the certificate below it can be checked against it.
 */
inline bool meets(const Identity& id, const sig::Key& k, sig::Role& role)
{
    if (zero(k)) return false;
    if (!zero(id.root) && k == id.root) { role = sig::Role::Root; return true; }
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
        sig::Key  top = leaf.issuer;
        sig::Role top_role = sig::Role::Module;
        if (chain.size() > 1)
        {
            if (!sig::may_issue(chain[1].role, sig::Role::Module))
                return refuse(io, "a key certified as " + std::string(sig::role_name(chain[1].role)) + " signs no binary");
            for (size_t i = 1; i < chain.size(); ++i)
            {
                const sig::Certificate& c = chain[i];
                if (c.subject != top || !sig::may_issue(c.role, chain[i - 1].role)) return refuse(io, "chain is broken");
                if (!c.epochs.empty() && !has_epoch(c, f.epoch)) return refuse(io, "witness not valid in this epoch");
                if (!check(c)) return refuse(io, "bad signature in chain");
                if (c.role == sig::Role::Root && (c.issuer != c.subject || i + 1 != chain.size()))
                    return refuse(io, "root is not last or not self-issued");
                top = c.issuer;
            }
            top_role = chain.back().role;
            f.signer_role = uint8_t(chain[1].role);
        }
        sig::Role at{};
        if (!meets(*id, top, at))
        {
            if (chain.size() > 1) return refuse(io, zero(id->root) ? "signed under a root, and none is built into this loader"
                                                                   : "does not chain to this loader's root");
            if (top == id->pub) return refuse(io, "signed by a loader, and this one has not proven a key: " + id->unproven);
            return refuse(io, "signed by another loader's key, and nothing above it");
        }
        if (top_role != sig::Role::Root && !sig::may_issue(at, top_role)) return refuse(io, "chain is broken");
        f.local = (zero(id->root) || top != id->root) ? 1 : 0;
        give(io, f);
    }

    // The chain for these bytes: this loader's leaf, and ACE's half above its
    // key if it has one. A leaf by this key on these same bytes, under this
    // same half, carries its epochs forward.
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
        if (zero(id->pub)) return refuse(io, "this loader has no key");
        if (id->role == sig::Role::Registrar) return refuse(io, "a registrar certifies loaders; it signs no binary");
        if (id->role == sig::Role::Release && !f.kernel) return refuse(io, "a release key signs loaders, not modules");

        std::vector<sig::Certificate> old;
        const bool carried =
            L.has_chain && sig::decode_chain(f.bytes + L.chain.at, L.chain.len, old)
            && old.size() == id->reg.size() + 1
            && sig::encode_chain({ old.begin() + 1, old.end() }) == sig::encode_chain(id->reg)
            && old[0].role == sig::Role::Module && old[0].issuer == id->pub && old[0].subject == c.subject && check(old[0]);
        if (carried && has_epoch(old[0], f.epoch)) { f.already = 1; return give(io, f); }
        if (!signer) return refuse(io, "the key is not here");
        if (!zero(id->root) && !id->attested && !id->genesis)
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
        const bool ours = (!zero(id->pub) && f.signer == id->pub) || (!zero(id->builder) && f.signer == id->builder);
        if (f.kernel && f.signer_role != uint8_t(sig::Role::Release) && !ours)
            return refuse(io, "a loader binary is taken from a release key, or from this loader's own");
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
 *   loader.chain      ACE's half above it, once registered: this key's
 *                     certificate <- ... <- the built-in root
 *
 * UNREGISTERED, a loader has its local half only: what it signs carries its
 * key and nothing above, and runs on it alone. Registering joins ACE's half
 * on. That needs a root built in, and its certificates are issued elsewhere --
 * by a registrar's loader, or by the active key at a ceremony -- and injected
 * (install_registration).
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

// `reg` is ACE's half above `pub`: a certificate for it in a role a loader can
// hold, each link certified as the table says (core/Signature.h), up to `root`.
inline bool registration_valid(const std::vector<sig::Certificate>& reg, const sig::Key& pub, const sig::Key& root)
{
    if (reg.empty() || reg[0].subject != pub || zero(root)) return false;
    const sig::Role r = reg[0].role;
    if (r != sig::Role::Loader && r != sig::Role::Marketplace && r != sig::Role::Release && r != sig::Role::Registrar)
        return false;
    for (size_t i = 0; i < reg.size(); ++i)
    {
        const sig::Certificate& c = reg[i];
        if (i > 0 && (c.subject != reg[i - 1].issuer || !sig::may_issue(c.role, reg[i - 1].role))) return false;
        if (!check(c)) return false;
        if (c.role == sig::Role::Root && (c.issuer != c.subject || i + 1 != reg.size())) return false;
    }
    if (reg.back().role != sig::Role::Root && !sig::may_issue(sig::Role::Root, reg.back().role)) return false;
    return reg.back().issuer == root;
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

// The key, and ACE's half above it when there is one: what a signer needs.
inline bool identity(const std::string& dir, uint8_t secret[64], sig::Key& pub,
                     std::vector<sig::Certificate>& reg, std::string& why, const sig::Key& built_in = ace_root())
{
    if (!loader_key(dir, secret, pub, why)) return false;
    std::vector<uint8_t> raw;
    if (!read_file(dir + "/loader.chain", raw) || !sig::decode_chain(raw.data(), raw.size(), reg)
        || !registration_valid(reg, pub, built_in))
        reg.clear();
    return true;
}
/*
 * INJECTING A REGISTRATION: ACE's half, issued elsewhere -- by a registrar's
 * loader (etcs --issue), or by the active key at a ceremony -- for this
 * loader's key. Leaf first, as certificates (binary or hex, as Custody keeps
 * them) or as one chain. It must be for this key and end at the root built
 * into this loader; with none built in there is nothing to join.
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

inline bool install_registration(const std::string& dir, const std::vector<sig::Certificate>& reg, std::string& why,
                                 const sig::Key& built_in = ace_root())
{
    if (zero(built_in)) { why = "no root is built into this loader (ACE_ROOT_KEY): there is no half to join"; return false; }
    uint8_t secret[64];
    sig::Key pub{};
    if (!loader_key(dir, secret, pub, why)) return false;                 // the key must be this loader's to register
    mc::crypto_wipe(secret, 64);
    if (reg.empty() || reg.back().role != sig::Role::Root) { why = "the chain does not end at a root certificate"; return false; }
    if (reg.back().subject != built_in) { why = "the chain's root is not ACE's"; return false; }
    if (!registration_valid(reg, pub, built_in)) { why = "not a registration of this loader's key"; return false; }
    if (!write_public(dir + "/loader.chain", sig::encode_chain(reg))) { why = "cannot write " + dir + "/loader.chain"; return false; }
    return true;
}

/*
 * THE LINEAGE: one line per loader binary signed, appended to lineage.log in
 * the store, under the root the signer was built with -- a guild's lineage
 * begins once, whatever experimental history the store has. An edge says which binary's key signed which, across which
 * epochs, when and where -- the last two are the signer's word, for the
 * trace; nothing is valid or invalid by them. The signer signs the line.
 *
 *   etcs-lineage 2 <name> root=<key|none> parent=<digest|genesis> attested=<0|1>
 *       child=<digest> from=<epoch> to=<epoch> key=<signer> at=<unix seconds> host=<name> sig=<hex>
 *
 * A version 1 line has no root= and is an experimental epoch's.
 *
 * `parent=genesis` is a loader signing itself: where a lineage starts.
 */
struct LineageEdge
{
    int         version = 2;
    std::string name, host;
    bool        genesis = false, attested = false;
    sig::Key    root{}, parent{}, child{}, key{};
    sig::Epoch  from{}, to{};
    uint64_t    at = 0;
    uint8_t     signature[sig::kSigBytes] = {};
};

inline std::string lineage_text(const LineageEdge& e)
{
    return "etcs-lineage " + std::to_string(e.version) + " " + e.name
         + (e.version >= 2 ? " root=" + (zero(e.root) ? std::string("none") : hex(e.root.data(), 32)) : std::string())
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
        || (line.compare(13, 2, "1 ") != 0 && line.compare(13, 2, "2 ") != 0)) return false;
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
    const std::string root = field("root");
    if (e.version >= 2 && root != "none" && !unhex(root, e.root.data(), 32)) return false;
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

#define ETCS_TRUST_STR2(x) #x
#define ETCS_TRUST_STR(x)  ETCS_TRUST_STR2(x)

/*
 * THE HANDSHAKE: what this loader is, for this session, from its store. The
 * public files say what it claims; the keystore says whether the claim is its
 * own, and only then is anything it signed taken. A browser holds no key and
 * reads no store: its claim is the build's (ace's define), its root the pin.
 * `prove` false reads the claim only -- enough to recognise a chain as
 * already there, which is all a build with nothing to sign needs.
 */
inline std::shared_ptr<const Identity> establish(const std::string& dir, bool prove = true,
                                                 const sig::Key& built_in = ace_root())
{
    auto id = std::make_shared<Identity>();
    id->root = built_in;
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
    // Signed by a release key it asks nothing of the store -- a loader just
    // downloaded attests by the root it was built with.
    std::vector<LineageEdge> edges;
    std::vector<uint8_t> me;
    std::string w;
    id->genesis = lineage_read(dir, edges, w)
                  && std::none_of(edges.begin(), edges.end(), [&](const LineageEdge& e) { return e.root == id->root; });
    if (!own_binary(id->self_name, me)) id->unattested = "this loader cannot read its own binary";
    else
    {
        sig::Layout L;
        if (sig::layout(me.data(), me.size(), L, w)) digest(id->self_name, me.data(), me.size(), L, id->self);
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
                       LineageEdge* edge = nullptr, const sig::Key& built_in = ace_root(), bool kernel = false)
{
    std::vector<uint8_t> b;
    std::string chain;
    LoadFrame f;
    if (!read_file(file, b)) { why = "cannot read " + file; return 2; }
    if (!load_frame(name, b.data(), b.size(), epoch, f, kernel)) { why = "bad module name"; return 2; }
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
        edge->name = name; edge->host = host_name(); edge->key = id->pub; edge->root = id->root;
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
    const int rc = sign_module(dir, name, file, epoch, out, why, &e, built_in, true);
    if (rc != 0) return rc;
    std::vector<LineageEdge> had;
    std::string w;
    if (!lineage_read(dir, had, w)) { why = w; return 2; }
    had.erase(std::remove_if(had.begin(), had.end(), [&](const LineageEdge& h) { return h.root != e.root; }), had.end());
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
 * A REGISTRAR'S WORK: certify another loader's key as `role`, under this
 * loader's own place in ACE's tree, and write the chain that results -- the
 * half the other loader installs with --registration. That the key is the
 * asker's is for the asker to prove to the registrar, not for this to know.
 */
inline int issue(const std::string& dir, sig::Role role, const sig::Key& subject, const std::string& out,
                 std::string& why, const sig::Key& built_in = ace_root())
{
    uint8_t secret[64];
    sig::Key pub{};
    std::vector<sig::Certificate> reg;
    if (!identity(dir, secret, pub, reg, why, built_in)) return 2;
    const std::shared_ptr<const Identity> id = establish(dir, true, built_in);
    auto refused = [&](const std::string& w) { mc::crypto_wipe(secret, 64); why = w; return 2; };
    if (reg.empty()) return refused("this loader is not registered, so it certifies no one");
    if (!sig::may_issue(reg[0].role, role))
        return refused(std::string("a ") + sig::role_name(reg[0].role) + " does not certify a " + sig::role_name(role));
    if (!id->proven) return refused("this loader has not proven its key: " + id->unproven);
    if (!id->attested && !id->genesis)
        return refused("only a loader in the lineage certifies, and this one does not attest: " + id->unattested);
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
 * signed list of what it serves; a loader asks the site's registrar for ACE's
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
    std::vector<DistEntry> entries;
};

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

// The list, read, and only if the marketplace under `root` signed it.
inline bool dist_read(const std::string& text, Dist& d, std::string& why, const sig::Key& root)
{
    if (zero(root)) { why = "no root is built into this loader, so no marketplace to believe"; return false; }
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
    if (reg[0].role != sig::Role::Marketplace || !registration_valid(reg, reg[0].subject, root))
    { why = "the list is not signed by a marketplace under this loader's root"; return false; }
    const std::string m = "ETCSDST1" + body;
    if (mc::crypto_ed25519_check(s, reg[0].subject.data(), reinterpret_cast<const uint8_t*>(m.data()), m.size()) != 0)
    { why = "the list's signature does not verify"; return false; }

    std::istringstream in(body);
    std::string line;
    const std::string head = std::string(sig::kDistMagic) + "epoch=";
    if (!std::getline(in, line) || line.compare(0, head.size(), head) != 0
        || !unhex(line.substr(head.size()), d.epoch.data(), d.epoch.size()))
    { why = "the list names no epoch"; return false; }
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
 * in `dir` a download would take in `epoch` -- a module the marketplace signed,
 * a loader binary ("etcs") a release key signed -- signed by this loader, which
 * must be the marketplace. What would not be taken is left off, and said.
 */
inline bool publish(const std::string& store, const std::string& dir, const sig::Epoch& epoch, std::string& out,
                    std::string& why, std::vector<std::string>* left = nullptr, const sig::Key& built_in = ace_root())
{
    if (zero(built_in)) { why = "no root is built into this loader, so it publishes for no one"; return false; }
    uint8_t secret[64];
    sig::Key pub{};
    std::vector<sig::Certificate> reg;
    if (!identity(store, secret, pub, reg, why, built_in)) return false;
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

    Identity download;                                   // what takes these: the root, nothing else
    download.root = built_in;
    std::string body = std::string(sig::kDistMagic) + "epoch=" + hex(epoch.data(), 32) + "\n";
    for (const std::string& f : files)
    {
        if (!plain_file(f) || f == "manifest") continue;
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
 * WHAT THE SITE HAS MOVED PAST, here: each entry against the binary this
 * loader would read under that name (its own, for "etcs"). Current is the same
 * bytes, signed for the site's epoch. A binary this loader's own key signed is
 * the developer's and is never fetched over. Anything else not current is
 * fetched -- all of it, when the site is in another epoch.
 */
inline bool survey(const std::string& text, std::string& report, std::string& why, const Identity& id,
                   const sig::Epoch& local, const std::string& bin_dir, const std::string& self_path)
{
    Dist d;
    if (!dist_read(text, d, why, id.root)) return false;
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
 * next load, or start, reads these.
 */
inline bool replace(const std::string& text, const std::string& name, const std::string& bytes, std::string& out,
                    std::string& why, const Identity& id, const std::string& bin_dir, const std::string& self_path)
{
    Dist d;
    if (!dist_read(text, d, why, id.root)) return false;
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
    return true;
}

// What a loader signs to ask a registrar for ACE's half (sig::registration_statement).
inline bool registration_request(const std::string& dir, const sig::Key& registrar, sig::Key& pub,
                                 uint8_t s[sig::kSigBytes], std::string& why)
{
    uint8_t secret[64];
    if (!loader_key(dir, secret, pub, why)) return false;
    const std::string m = sig::registration_statement(pub, registrar);
    mc::crypto_ed25519_sign(s, secret, reinterpret_cast<const uint8_t*>(m.data()), m.size());
    mc::crypto_wipe(secret, 64);
    return true;
}
#endif // !__EMSCRIPTEN__

/*
 * WHAT A MODULE MAY ASK (TrustEvent, core/EventNode.h), on the loader's
 * ordering thread, of this loader as it runs: its store, its session's
 * identity, its own directory and binary, its epoch. `op` in TrustEvent::Op's
 * order. A registration installed now is the next session's.
 */
inline bool answer(uint8_t op, const std::string& name, const std::string& a, const std::string& b, std::string& out)
{
    out.clear();
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
            sig::Key registrar{}, pub{};
            uint8_t s[sig::kSigBytes];
            if (!unhex(a, registrar.data(), registrar.size())) { why = "not a registrar's key"; break; }
            if ((ok = registration_request(dir, registrar, pub, s, why))) out = hex(pub.data(), 32) + " " + hex(s, sizeof s);
            break;
        }
        case 1:
        {
            std::vector<sig::Certificate> reg;
            if (!parse_certificates(a, reg)) { why = "not a chain of certificates"; break; }
            if ((ok = install_registration(dir, reg, why)))
                out = std::string("registered as ") + sig::role_name(reg[0].role) + "; the next session is it";
            break;
        }
        case 2: ok = survey(a, out, why, id, loader_epoch(), bin, self); break;
        case 3: ok = replace(a, name, b, out, why, id, bin, self); break;
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
 * What ace asks of the loader that holds the key:
 *
 *   etcs --sign <module> <file> <epoch hex> <out>   0 signed, 3 already, 2 refused
 *   etcs --succeed <name> <file> <epoch hex> <out>  the same, for the next loader binary: adds the lineage edge
 *   etcs --registration <cert>...                   install ACE's half for this key, leaf first
 *   etcs --issue <role> <key hex> <out>             a registrar certifies another loader's key; writes its chain
 *   etcs --request <registrar key hex>              "<this loader's key> <signature>": a registration's ask
 *   etcs --manifest <dir> <out>                     the marketplace's signed list of what <dir> serves
 *   etcs --survey <list>                            what that list has moved past, here
 *   etcs --attest                                   what this loader says of itself; 0 only if it attests
 *   etcs --loader-key                               public key, made on first use
 *   etcs --root                                     the root built in; 2 when there is none
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
    if (verb == "--root" && argc == 2)
    {
        if (zero(ace_root())) { std::fprintf(stderr, "etcs: no root is built into this loader\n"); return 2; }
        std::printf("%s\n", hex(ace_root().data(), 32).c_str());
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
    if (verb == "--request" && argc == 3)
    {
        sig::Key registrar{}, pub{};
        uint8_t s[sig::kSigBytes];
        if (!unhex(argv[2], registrar.data(), registrar.size())) { std::fprintf(stderr, "etcs: not a registrar's key\n"); return 2; }
        if (!registration_request(dir, registrar, pub, s, why)) { std::fprintf(stderr, "etcs: %s\n", why.c_str()); return 2; }
        std::printf("%s %s\n", hex(pub.data(), 32).c_str(), hex(s, sizeof s).c_str());
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
                    self.substr(0, self.find_last_of("/\\") + 1), self))
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
        if (zero(id->root)) std::printf("root      none built in: an experimental epoch\n");
        else std::printf("root      %s (ACE, built in): the guild\n", hex(id->root.data(), 32).c_str());
        if (!zero(id->builder)) std::printf("builder   %s (built in)\n", hex(id->builder.data(), 32).c_str());
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
