#ifndef SIGNATURE_H__
#define SIGNATURE_H__

#include <array>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <string>
#include <vector>

/*
 * THE CERTIFICATE, THE CHAIN, AND WHERE A BINARY CARRIES THEM. Layout only:
 * the loader parses these, CryptProvider and `etcs --sign` write them, and
 * neither owns the format alone. No cryptography here (core/Trust.h).
 *
 *   cert   "ETCSCRT1" | role u8 | subject[32] | issuer[32] | n u16le | epoch[32] x n | signature[64]
 *   chain  "ETCSCHN1" | n u8 | cert x n, leaf first
 *   pass   "ETCSGEN1" | parent[32] | child[32] | from[32] | to[32] | head[32] | attested u8 | key[32] | signature[64]
 *   count  "ETCSCSG1" | child[32] | to[32] | cert | signature[64]
 *
 * The signature is Ed25519 (RFC 8032) by the issuer over everything before it,
 * magic included -- the magic is the domain separation. A root's own
 * certificate is self-issued, and last; a root certificate issued by another
 * root is that root attesting it. No epochs means not bounded by epoch: an authority or a loader's
 * registration, never a module. A Module certificate's subject is the digest
 * of the bytes it vouches for, not a key.
 */
namespace ETCS
{
namespace sig
{
using Key   = std::array<uint8_t, 32>;
using Epoch = std::array<uint8_t, 32>;

/*
 * A CHAIN IS TWO HALVES joined at a loader's key. ACE's half is a tree: the
 * root certifies the active key, and the active key the keys that share it as
 * parent -- one per loader that acts for ACE (the minting server's, the site's
 * registrar). The local half is a loader's own key and what it signs. Release
 * stays in the table and is held by no one: the minting server's marketplace
 * key signs loader binaries as well as modules.
 *
 * THE JOIN IS ONE TO ONE. A loader's ACE IDENTITY is ACE's key for that one
 * loader key: derived by the registrar from the loader's public key under a
 * secret only ACE holds, so ACE re-derives it whenever it is asked and keeps
 * none. The registrar certifies the ACE identity, for the epoch it registered
 * in; the ACE identity certifies the loader's key. An ACCOUNT is ACE's set of
 * ACE identities, and is no key: who admits a key to it, or retires one, is a
 * member's word to the registrar (the statements at the end of this file). A
 * registrar may still certify a key as Loader directly -- a ceremony's
 * authority, not an account.
 *
 * Who may certify whom is the table below, and nothing else: a key holds one
 * role, and two roles are two keys under one parent.
 *
 * ROOTS ATTEST EACH OTHER THE SAME WAY: a root certifying another root's key
 * continues the chain past it, as a registration continues a loader's key into
 * ACE's half. What carries the continuation is taken by a loader built with
 * an identity folded from either root along it (core/Trust.h).
 */
enum class Role : uint8_t
{
    Root = 1, Intermediate = 2, Marketplace = 3, Loader = 4, Module = 5, Release = 6, Registrar = 7, Identity = 8
};

inline bool role_valid(uint8_t r) { return r >= 1 && r <= 8; }

inline bool may_issue(Role issuer, Role subject)
{
    switch (issuer)
    {
        case Role::Root:         return subject == Role::Root || subject == Role::Intermediate;
        case Role::Intermediate: return subject == Role::Marketplace || subject == Role::Release
                                     || subject == Role::Registrar;
        case Role::Registrar:    return subject == Role::Loader || subject == Role::Identity;
        case Role::Identity:     return subject == Role::Loader;
        case Role::Marketplace:
        case Role::Release:
        case Role::Loader:       return subject == Role::Module;
        case Role::Module:       return false;
    }
    return false;
}

inline const char* role_name(Role r)
{
    switch (r)
    {
        case Role::Root:         return "root";
        case Role::Intermediate: return "intermediate";
        case Role::Marketplace:  return "marketplace";
        case Role::Loader:       return "loader";
        case Role::Module:       return "module";
        case Role::Release:      return "release";
        case Role::Registrar:    return "registrar";
        case Role::Identity:     return "identity";
    }
    return "?";
}
inline bool role_from_name(const std::string& s, Role& out)
{
    for (uint8_t r = 1; r <= 8; ++r)
        if (s == role_name(static_cast<Role>(r))) { out = static_cast<Role>(r); return true; }
    return false;
}

constexpr char        kCertMagic[8]  = { 'E', 'T', 'C', 'S', 'C', 'R', 'T', '1' };
constexpr char        kChainMagic[8] = { 'E', 'T', 'C', 'S', 'C', 'H', 'N', '1' };
constexpr size_t      kSigBytes      = 64;
constexpr uint16_t    kMaxEpochs     = 1024;

struct Certificate
{
    Role               role = Role::Loader;
    Key                subject{};
    Key                issuer{};
    std::vector<Epoch> epochs;
    uint8_t            signature[kSigBytes] = {};
};

// What the issuer signs.
inline std::string signed_part(const Certificate& c)
{
    std::string out(kCertMagic, sizeof kCertMagic);
    out += static_cast<char>(c.role);
    out.append(reinterpret_cast<const char*>(c.subject.data()), c.subject.size());
    out.append(reinterpret_cast<const char*>(c.issuer.data()), c.issuer.size());
    const uint16_t n = static_cast<uint16_t>(c.epochs.size());
    out += static_cast<char>(n & 0xff);
    out += static_cast<char>(n >> 8);
    for (const Epoch& e : c.epochs) out.append(reinterpret_cast<const char*>(e.data()), e.size());
    return out;
}

inline std::string encode(const Certificate& c)
{
    std::string out = signed_part(c);
    out.append(reinterpret_cast<const char*>(c.signature), kSigBytes);
    return out;
}

// One certificate from the front of p; `used` is its length. False on any
// short, unknown or oversized field.
inline bool decode(const void* p, size_t len, Certificate& c, size_t* used = nullptr)
{
    const uint8_t* b = static_cast<const uint8_t*>(p);
    const size_t head = sizeof kCertMagic + 1 + 32 + 32 + 2;
    if (!b || len < head || std::memcmp(b, kCertMagic, sizeof kCertMagic) != 0) return false;
    size_t at = sizeof kCertMagic;
    if (!role_valid(b[at])) return false;
    c.role = static_cast<Role>(b[at++]);
    std::memcpy(c.subject.data(), b + at, 32); at += 32;
    std::memcpy(c.issuer.data(), b + at, 32);  at += 32;
    const uint16_t n = static_cast<uint16_t>(b[at] | (b[at + 1] << 8)); at += 2;
    if (n > kMaxEpochs || len < at + size_t(n) * 32 + kSigBytes) return false;
    c.epochs.assign(n, Epoch{});
    for (uint16_t i = 0; i < n; ++i) { std::memcpy(c.epochs[i].data(), b + at, 32); at += 32; }
    std::memcpy(c.signature, b + at, kSigBytes); at += kSigBytes;
    if (used) *used = at;
    return true;
}

// Empty when the chain is longer than its count byte can say.
inline std::string encode_chain(const std::vector<Certificate>& chain)
{
    if (chain.size() > 255) return {};
    std::string out(kChainMagic, sizeof kChainMagic);
    out += static_cast<char>(chain.size() & 0xff);
    for (const Certificate& c : chain) out += encode(c);
    return out;
}

// Exactly the chain: trailing bytes are a refusal, not slack.
inline bool decode_chain(const void* p, size_t len, std::vector<Certificate>& chain)
{
    const uint8_t* b = static_cast<const uint8_t*>(p);
    chain.clear();
    if (!b || len < sizeof kChainMagic + 1 || std::memcmp(b, kChainMagic, sizeof kChainMagic) != 0) return false;
    const size_t n = b[sizeof kChainMagic];
    size_t at = sizeof kChainMagic + 1;
    for (size_t i = 0; i < n; ++i)
    {
        Certificate c;
        size_t used = 0;
        if (!decode(b + at, len - at, c, &used)) { chain.clear(); return false; }
        chain.push_back(c);
        at += used;
    }
    if (at != len) { chain.clear(); return false; }
    return true;
}

/*
 * A GENERATION PASSED OFF: the loader binary `parent`, running in `from` under
 * the identity `head` (core/Trust.h), hands its place to the binary `child`, signed for `to`, with `key` --
 * the key that signs the child's leaf. `attested` is whether the parent
 * attested itself when it did. It rides in the child's signature section after
 * the chain, so the child carries the one fragment its parent could sign for;
 * the lineage before it is the marketplace's to attest. A parent that is its
 * own child is a lineage beginning.
 */
constexpr char   kPassMagic[8] = { 'E', 'T', 'C', 'S', 'G', 'E', 'N', '1' };
constexpr size_t kPassBytes    = 8 + 32 * 5 + 1 + 32 + kSigBytes;

struct PassOff
{
    Key     parent{}, child{};
    Epoch   from{}, to{};
    Key     head{};
    bool    attested = false;
    Key     key{};
    uint8_t signature[kSigBytes] = {};
};

inline std::string signed_part(const PassOff& p)
{
    std::string out(kPassMagic, sizeof kPassMagic);
    for (const auto* k : { &p.parent, &p.child, &p.from, &p.to, &p.head })
        out.append(reinterpret_cast<const char*>(k->data()), k->size());
    out += static_cast<char>(p.attested ? 1 : 0);
    out.append(reinterpret_cast<const char*>(p.key.data()), p.key.size());
    return out;
}

inline std::string encode(const PassOff& p)
{
    std::string out = signed_part(p);
    out.append(reinterpret_cast<const char*>(p.signature), kSigBytes);
    return out;
}

inline bool decode(const void* q, size_t len, PassOff& p)
{
    const uint8_t* b = static_cast<const uint8_t*>(q);
    if (!b || len != kPassBytes || std::memcmp(b, kPassMagic, sizeof kPassMagic) != 0 || b[8 + 32 * 5] > 1) return false;
    size_t at = sizeof kPassMagic;
    for (auto* k : { &p.parent, &p.child, &p.from, &p.to, &p.head }) { std::memcpy(k->data(), b + at, 32); at += 32; }
    p.attested = b[at++] == 1;
    std::memcpy(p.key.data(), b + at, 32); at += 32;
    std::memcpy(p.signature, b + at, kSigBytes);
    return true;
}

/*
 * A RELEASE KEY'S COUNTERSIGNATURE on a loader binary: the key `cert`
 * certifies as Release signs that `child` is a release for `to`. It rides
 * after the pass-off, and carries its certificate, so a loader that holds
 * only an identity can see which active key the release key is under. It is
 * the second key a loader needs to take a successor minted by another
 * marketplace of its identity's parent (core/Trust.h, the bridge).
 */
constexpr char kCountMagic[8] = { 'E', 'T', 'C', 'S', 'C', 'S', 'G', '1' };

struct Countersign
{
    Key         child{};
    Epoch       to{};
    Certificate cert;
    uint8_t     signature[kSigBytes] = {};
};

inline std::string signed_part(const Countersign& c)
{
    std::string out(kCountMagic, sizeof kCountMagic);
    out.append(reinterpret_cast<const char*>(c.child.data()), 32);
    out.append(reinterpret_cast<const char*>(c.to.data()), 32);
    out += encode(c.cert);
    return out;
}

inline std::string encode(const Countersign& c)
{
    std::string out = signed_part(c);
    out.append(reinterpret_cast<const char*>(c.signature), kSigBytes);
    return out;
}

// Exactly one countersignature: trailing bytes are a refusal.
inline bool decode(const void* q, size_t len, Countersign& c)
{
    const uint8_t* b = static_cast<const uint8_t*>(q);
    if (!b || len < sizeof kCountMagic + 64 || std::memcmp(b, kCountMagic, sizeof kCountMagic) != 0) return false;
    size_t at = sizeof kCountMagic, used = 0;
    std::memcpy(c.child.data(), b + at, 32); at += 32;
    std::memcpy(c.to.data(), b + at, 32);    at += 32;
    if (!decode(b + at, len - at, c.cert, &used)) return false;
    at += used;
    if (len - at != kSigBytes) return false;
    std::memcpy(c.signature, b + at, kSigBytes);
    return true;
}

/*
 * A LINK'S WORD: what a loader signs to say that one link is its own --
 * "ETCSLNK1" | 'd' (it dialled) or 'a' (it accepted) | the transcript, a hash
 * of both sides' names and fresh exchange keys. Nothing else is ever signed
 * after this prefix, so a module asking the loader for it can be given no
 * module's, list's or registration's signature by asking.
 */
constexpr size_t kLinkTranscript = 32;
inline std::string link_statement(char side, const uint8_t transcript[kLinkTranscript])
{
    std::string m("ETCSLNK1");
    m.push_back(side);
    m.append(reinterpret_cast<const char*>(transcript), kLinkTranscript);
    return m;
}

/*
 * WHAT A LOADER SIGNS TO A REGISTRAR, each a fixed form, so a loader's key
 * signs nothing else this way and a word to one registrar is none to another:
 *
 *   "ETCSREG2" | loader | registrar | head | epoch     register: ACE's half for this key, as a
 *                                                      loader built for `head`, running in `epoch`
 *   "ETCSJON2" | joining | member | registrar | epoch  a key agrees to join member's account
 *   "ETCSADM2" | member | joining | registrar | epoch  a member admits it
 *   "ETCSRET1" | member | target | registrar           a member retires a key of its account
 *   "ETCSPRT1" | member | registrar | code             a member lets the browser showing `code` act
 *                                                      for its account, for a while (the portal)
 *
 * A key is admitted only with both words, its own and a member's: a member
 * alone could otherwise claim any key it has seen, and every key is public.
 * Both name the epoch they are said in, so neither is good in a later one.
 * A retirement is for good, and names none.
 */
inline std::string statement(const char magic[9], std::initializer_list<const std::array<uint8_t, 32>*> parts)
{
    std::string out(magic, 8);
    for (const auto* k : parts) out.append(reinterpret_cast<const char*>(k->data()), k->size());
    return out;
}
inline std::string registration_statement(const Key& loader, const Key& registrar, const Key& head, const Epoch& epoch)
{
    return statement("ETCSREG2", { &loader, &registrar, &head, &epoch });
}
inline std::string join_statement(const Key& joining, const Key& member, const Key& registrar, const Epoch& epoch)
{
    return statement("ETCSJON2", { &joining, &member, &registrar, &epoch });
}
inline std::string admit_statement(const Key& member, const Key& joining, const Key& registrar, const Epoch& epoch)
{
    return statement("ETCSADM2", { &member, &joining, &registrar, &epoch });
}
inline std::string retire_statement(const Key& member, const Key& target, const Key& registrar)
{
    return statement("ETCSRET1", { &member, &target, &registrar });
}
inline std::string portal_statement(const Key& member, const Key& registrar, const Key& code)
{
    return statement("ETCSPRT1", { &member, &registrar, &code });
}

/*
 * THE SITE'S LIST OF WHAT IT SERVES (core/Trust.h publish, survey):
 *
 *   etcs-dist 3 epoch=<hex> seq=<n> kernel=<ETCS_Kernel_v...>
 *   <name> <file> <digest hex> <epoch hex>[,<epoch hex>...]
 *   ...
 *   etcs-dist-sig <signer's chain, hex> <signature hex>
 *
 * The signature is the marketplace's, over "ETCSDST1" and every byte before
 * the last line. The order lists come in is (kernel, seq): `kernel` is the
 * ABI's version the list was published under (BASE_SOURCE_STRING,
 * core/ETCS_API.h), `seq` the marketplace's count of the epochs it has
 * published under it. A loader takes no list older than the newest it has
 * taken, or than itself.
 */
constexpr char kDistMagic[] = "etcs-dist 3 ";
constexpr char kDistSig[]   = "etcs-dist-sig ";

/*
 * WHERE A BINARY CARRIES ITS CHAIN, AND WHICH OF ITS BYTES ARE SIGNED.
 *
 * The signed bytes are what the platform's loader maps and nothing it is free
 * to rewrite, so the chain can be replaced -- a new epoch, a new witness --
 * without moving the digest:
 *
 *   ELF64 LE   section ".etcs.sig". Signed: the ELF header with e_shoff,
 *              e_shnum and e_shstrndx read as zero (adding a section moves
 *              them), the program headers, then every PT_LOAD's file bytes in
 *              program-header order. What ld.so never maps is not signed.
 *   wasm       custom section "etcs.sig". Signed: the preamble and every other
 *              section, whole, in order.
 *   PE         trailer: chain | len u32le | "ETCSSIG1", after everything else.
 *              Signed: every byte before it. (objcopy re-lays a PE's section
 *              table when it adds one, with a section below the image base,
 *              so a section is not something the build can add safely.)
 *
 * Exactly one chain; a second is malformed, not ignored. A loader binary's
 * section may hold a pass-off after it (`pass`); the chain's span is then the
 * chain alone.
 */
enum class Format : uint8_t { Elf = 1, Wasm = 2, Pe = 3 };

constexpr char kElfSection[]  = ".etcs.sig";
constexpr char kWasmSection[] = "etcs.sig";
constexpr char kPeTrailer[8]  = { 'E', 'T', 'C', 'S', 'S', 'I', 'G', '1' };

struct Span { size_t at = 0, len = 0; };

struct Layout
{
    Format            format = Format::Elf;
    std::vector<Span> covered;          // digest order
    std::vector<Span> zeroed;           // read as zero inside any covered span
    bool              has_chain = false;
    Span              chain;
    bool              has_pass = false;
    Span              pass;
    bool              has_count = false;
    Span              count;
};

namespace detail
{
inline uint16_t u16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }
inline uint32_t u32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
inline uint64_t u64(const uint8_t* p) { return uint64_t(u32(p)) | uint64_t(u32(p + 4)) << 32; }
inline bool fits(size_t n, uint64_t at, uint64_t len) { return at <= n && len <= n - at; }

inline bool uleb(const uint8_t* b, size_t n, size_t& at, uint32_t& v)
{
    v = 0;
    for (int shift = 0; shift < 35; shift += 7)
    {
        if (at >= n) return false;
        const uint8_t c = b[at++];
        v |= uint32_t(c & 0x7f) << shift;
        if (!(c & 0x80)) return shift < 28 || (c >> 4) == 0;
    }
    return false;
}

inline bool elf(const uint8_t* b, size_t n, Layout& L, std::string& why)
{
    if (n < 64 || b[4] != 2 || b[5] != 1) { why = "not a 64-bit little-endian ELF"; return false; }
    const uint64_t phoff = u64(b + 0x20), shoff = u64(b + 0x28);
    const uint16_t ehsize = u16(b + 0x34), phentsize = u16(b + 0x36), phnum = u16(b + 0x38);
    const uint16_t shentsize = u16(b + 0x3A), shnum = u16(b + 0x3C), shstrndx = u16(b + 0x3E);
    if (ehsize != 64 || phentsize < 56 || !fits(n, phoff, uint64_t(phentsize) * phnum))
    { why = "malformed ELF header"; return false; }
    L.format = Format::Elf;
    L.covered = { { 0, 64 }, { size_t(phoff), size_t(phentsize) * phnum } };
    L.zeroed  = { { 0x28, 8 }, { 0x3C, 4 } };
    for (uint16_t i = 0; i < phnum; ++i)
    {
        const uint8_t* ph = b + phoff + size_t(i) * phentsize;
        if (u32(ph) != 1) continue;                                   // PT_LOAD
        const uint64_t off = u64(ph + 8), filesz = u64(ph + 32);
        if (!fits(n, off, filesz)) { why = "PT_LOAD outside the file"; return false; }
        L.covered.push_back({ size_t(off), size_t(filesz) });
    }
    if (shoff == 0 || shnum == 0) return true;
    if (shentsize < 64 || shstrndx >= shnum || !fits(n, shoff, uint64_t(shentsize) * shnum))
    { why = "malformed section headers"; return false; }
    const uint8_t* strh = b + shoff + size_t(shstrndx) * shentsize;
    const uint64_t stroff = u64(strh + 24), strsz = u64(strh + 32);
    if (!fits(n, stroff, strsz)) { why = "section names outside the file"; return false; }
    const size_t want = sizeof kElfSection;                           // with its NUL
    for (uint16_t i = 0; i < shnum; ++i)
    {
        const uint8_t* sh = b + shoff + size_t(i) * shentsize;
        const uint32_t name = u32(sh);
        if (name >= strsz || strsz - name < want) continue;
        if (std::memcmp(b + stroff + name, kElfSection, want) != 0) continue;
        if (L.has_chain) { why = "two signature sections"; return false; }
        const uint64_t off = u64(sh + 24), size = u64(sh + 32);
        if (u32(sh + 4) == 8 || !fits(n, off, size)) { why = "signature section outside the file"; return false; }
        L.has_chain = true;
        L.chain = { size_t(off), size_t(size) };
    }
    return true;
}

inline bool wasm(const uint8_t* b, size_t n, Layout& L, std::string& why)
{
    if (n < 8 || u32(b + 4) != 1) { why = "not a version-1 wasm module"; return false; }
    L.format = Format::Wasm;
    L.covered = { { 0, 8 } };
    size_t at = 8;
    while (at < n)
    {
        const size_t start = at;
        const uint8_t id = b[at++];
        uint32_t size = 0;
        if (!uleb(b, n, at, size) || !fits(n, at, size)) { why = "wasm section outside the file"; return false; }
        const size_t end = at + size;
        bool ours = false;
        if (id == 0)
        {
            size_t p = at;
            uint32_t nl = 0;
            if (!uleb(b, end, p, nl) || !fits(end, p, nl)) { why = "malformed custom section"; return false; }
            ours = nl == sizeof kWasmSection - 1 && std::memcmp(b + p, kWasmSection, nl) == 0;
            if (ours)
            {
                if (L.has_chain) { why = "two signature sections"; return false; }
                L.has_chain = true;
                L.chain = { p + nl, end - (p + nl) };
            }
        }
        if (!ours) L.covered.push_back({ start, end - start });
        at = end;
    }
    return true;
}

inline bool pe(const uint8_t* b, size_t n, Layout& L, std::string& why)
{
    if (n < 0x40) { why = "short PE"; return false; }
    size_t body = n;
    L.format = Format::Pe;
    if (n >= 12 && std::memcmp(b + n - 8, kPeTrailer, 8) == 0)
    {
        const uint32_t len = u32(b + n - 12);
        if (len > n - 12) { why = "signature trailer longer than the file"; return false; }
        body = n - 12 - len;
        L.has_chain = true;
        L.chain = { body, len };
    }
    const uint32_t lfanew = u32(b + 0x3C);
    if (!fits(body, lfanew, 4) || std::memcmp(b + lfanew, "PE\0\0", 4) != 0) { why = "not a PE image"; return false; }
    L.covered = { { 0, body } };
    return true;
}
// The chain's own length at the front of a section: its count, then each
// certificate's epochs. False if the section is shorter than that says.
inline bool chain_length(const uint8_t* b, size_t len, size_t& out)
{
    if (len < sizeof kChainMagic + 1 || std::memcmp(b, kChainMagic, sizeof kChainMagic) != 0) return false;
    const size_t n = b[sizeof kChainMagic], head = sizeof kCertMagic + 1 + 32 + 32 + 2;
    size_t at = sizeof kChainMagic + 1;
    for (size_t i = 0; i < n; ++i)
    {
        if (len - at < head) return false;
        const size_t body = head + size_t(u16(b + at + head - 2)) * 32 + kSigBytes;
        if (len - at < body) return false;
        at += body;
    }
    out = at;
    return true;
}

// A section that is a chain, one pass-off, and at most one countersignature
// is split so; anything else is left whole, for the chain's own decoding to
// refuse.
inline void split_pass(const uint8_t* b, Layout& L)
{
    size_t c = 0;
    if (!L.has_chain || !chain_length(b + L.chain.at, L.chain.len, c) || L.chain.len - c < kPassBytes
        || std::memcmp(b + L.chain.at + c, kPassMagic, sizeof kPassMagic) != 0) return;
    const size_t rest = L.chain.len - c - kPassBytes, at = L.chain.at + c + kPassBytes;
    Countersign cs;
    if (rest && !decode(b + at, rest, cs)) return;
    L.has_pass  = true;
    L.pass      = { L.chain.at + c, kPassBytes };
    L.has_count = rest != 0;
    L.count     = { at, rest };
    L.chain     = { L.chain.at, c };
}
} // namespace detail

// False: not a format a loader loads, or malformed; `why` says which.
inline bool layout(const uint8_t* b, size_t n, Layout& L, std::string& why)
{
    L = Layout{};
    if (!b || n < 4) { why = "empty"; return false; }
    bool ok = false;
    if      (std::memcmp(b, "\x7f" "ELF", 4) == 0) ok = detail::elf(b, n, L, why);
    else if (std::memcmp(b, "\0asm", 4) == 0)     ok = detail::wasm(b, n, L, why);
    else if (b[0] == 'M' && b[1] == 'Z')          ok = detail::pe(b, n, L, why);
    else { why = "not ELF, wasm or PE"; return false; }
    if (ok) detail::split_pass(b, L);
    return ok;
}

} // namespace sig
} // namespace ETCS

#endif // SIGNATURE_H__
