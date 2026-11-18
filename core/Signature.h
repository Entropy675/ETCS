#ifndef SIGNATURE_H__
#define SIGNATURE_H__

#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

/*
 * THE CERTIFICATE, THE CHAIN, AND WHERE A BINARY CARRIES THEM. Layout only:
 * the loader parses these, CryptProvider and `etcs --sign` write them, and
 * neither owns the format alone. No cryptography here (core/Trust.h).
 *
 *   cert   "ETCSCRT1" | role u8 | subject[32] | issuer[32] | n u16le | epoch[32] x n | signature[64]
 *   chain  "ETCSCHN1" | n u8 | cert x n, leaf first
 *
 * The signature is Ed25519 (RFC 8032) by the issuer over everything before it,
 * magic included -- the magic is the domain separation. A root is its own
 * issuer. No epochs means not bounded by epoch: an authority or a loader's
 * registration, never a module. A Module certificate's subject is the digest
 * of the bytes it vouches for, not a key.
 */
namespace ETCS
{
namespace sig
{
using Key   = std::array<uint8_t, 32>;
using Epoch = std::array<uint8_t, 32>;

enum class Role : uint8_t { Root = 1, Intermediate = 2, Marketplace = 3, Loader = 4, Module = 5 };

inline bool role_valid(uint8_t r) { return r >= 1 && r <= 5; }
inline const char* role_name(Role r)
{
    switch (r)
    {
        case Role::Root:         return "root";
        case Role::Intermediate: return "intermediate";
        case Role::Marketplace:  return "marketplace";
        case Role::Loader:       return "loader";
        case Role::Module:       return "module";
    }
    return "?";
}
inline bool role_from_name(const std::string& s, Role& out)
{
    for (uint8_t r = 1; r <= 5; ++r)
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
 * Exactly one chain; a second is malformed, not ignored.
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
} // namespace detail

// False: not a format a loader loads, or malformed; `why` says which.
inline bool layout(const uint8_t* b, size_t n, Layout& L, std::string& why)
{
    L = Layout{};
    if (!b || n < 4) { why = "empty"; return false; }
    if (std::memcmp(b, "\x7f" "ELF", 4) == 0) return detail::elf(b, n, L, why);
    if (std::memcmp(b, "\0asm", 4) == 0)     return detail::wasm(b, n, L, why);
    if (b[0] == 'M' && b[1] == 'Z')          return detail::pe(b, n, L, why);
    why = "not ELF, wasm or PE";
    return false;
}

} // namespace sig
} // namespace ETCS

#endif // SIGNATURE_H__
