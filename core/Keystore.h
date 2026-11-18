#ifndef KEYSTORE_H__
#define KEYSTORE_H__

/*
 * THE OS KEYSTORE: where a Private secret is kept between runs. Named secrets
 * held by the OS for this OS user, scoped to one installation (its store dir),
 * so two installations under one user are two loaders. The equivalent of a
 * browser's session-token store, one level down.
 *
 *   Linux    the Secret Service over D-Bus, through libsecret loaded at
 *            runtime -- no build dependency, and a machine without one says
 *            so instead of failing to link.
 *   Windows  the Credential Manager, a generic credential per secret.
 *   web      none: a browser holds no key (the token contract instead).
 *
 * Core, not a module: the loader signs with what is kept here, and a module
 * would have to be signed before the loader could reach its own key.
 * DatabaseProvider's Keystore leaf is the same backend, for scripts.
 *
 * A secret is never replaced (store_new refuses one that is held); erase is
 * the only way one goes.
 */

#include <cstdint>
#include <cstring>
#include <string>

#if defined(_WIN32)
#  include <windows.h>
#  include <wincred.h>
#elif defined(__linux__) && !defined(__EMSCRIPTEN__)
#  include <dlfcn.h>
#  include <mutex>
#endif

namespace ETCS
{
namespace keystore
{
enum class Status { Ok, Absent, Unavailable, Refused };

#if defined(__linux__) && !defined(__EMSCRIPTEN__)
namespace detail
{
// libsecret's ABI, as much of it as is used: SecretSchema, GError, and the
// three *_sync password calls (attributes as NULL-terminated name/value pairs).
struct SchemaAttribute { const char* name; int type; };
struct Schema
{
    const char*     name;
    int             flags;
    SchemaAttribute attributes[32];
    int             reserved;
    void*           reserved1; void* reserved2; void* reserved3; void* reserved4;
    void*           reserved5; void* reserved6; void* reserved7;
};
struct GErrorT { uint32_t domain; int code; char* message; };

using StoreFn  = int   (*)(const Schema*, const char*, const char*, const char*, void*, GErrorT**, ...);
using LookupFn = char* (*)(const Schema*, void*, GErrorT**, ...);
using ClearFn  = int   (*)(const Schema*, void*, GErrorT**, ...);
using FreeFn   = void  (*)(char*);
using ErrFreeFn= void  (*)(GErrorT*);

struct Lib
{
    StoreFn store = nullptr; LookupFn lookup = nullptr; ClearFn clear = nullptr;
    FreeFn  free_pw = nullptr; ErrFreeFn err_free = nullptr;
    bool ok = false;
};

inline const Lib& lib()
{
    static Lib l;
    static std::once_flag once;
    std::call_once(once, [] {
        void* h = ::dlopen("libsecret-1.so.0", RTLD_NOW | RTLD_LOCAL);
        if (!h) return;
        l.store    = reinterpret_cast<StoreFn>(::dlsym(h, "secret_password_store_sync"));
        l.lookup   = reinterpret_cast<LookupFn>(::dlsym(h, "secret_password_lookup_sync"));
        l.clear    = reinterpret_cast<ClearFn>(::dlsym(h, "secret_password_clear_sync"));
        l.free_pw  = reinterpret_cast<FreeFn>(::dlsym(h, "secret_password_free"));
        l.err_free = reinterpret_cast<ErrFreeFn>(::dlsym(h, "g_error_free"));
        l.ok = l.store && l.lookup && l.clear && l.free_pw && l.err_free;
    });
    return l;
}

inline const Schema* schema()
{
    static const Schema s = { "org.anticurrententropy.etcs.Secret", 0,
                              { { "store", 0 }, { "item", 0 }, { nullptr, 0 } }, 0,
                              nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr };
    return &s;
}

constexpr const char* kHelp =
    " -- the Secret Service keeps this loader's key. Install libsecret and a keyring "
    "(gnome-keyring), or on a headless machine run inside one: "
    "dbus-run-session -- sh -c 'echo \"$PASS\" | gnome-keyring-daemon --unlock --components=secrets; <command>'";

inline Status failed(GErrorT* e, std::string& why)
{
    why = std::string("Secret Service: ") + (e && e->message ? e->message : "unknown error") + kHelp;
    if (e) lib().err_free(e);
    return Status::Unavailable;
}
} // namespace detail

inline Status load(const std::string& store, const std::string& item, std::string& out, std::string& why)
{
    const detail::Lib& l = detail::lib();
    if (!l.ok) { why = std::string("no libsecret-1.so.0") + detail::kHelp; return Status::Unavailable; }
    detail::GErrorT* e = nullptr;
    char* pw = l.lookup(detail::schema(), nullptr, &e, "store", store.c_str(), "item", item.c_str(), nullptr);
    if (e) return detail::failed(e, why);
    if (!pw) return Status::Absent;
    out.assign(pw);
    l.free_pw(pw);                                   // secret_password_free wipes
    return Status::Ok;
}

inline Status store_new(const std::string& store, const std::string& item, const std::string& secret, std::string& why)
{
    std::string held;
    const Status s = load(store, item, held, why);
    if (!held.empty()) std::memset(&held[0], 0, held.size());
    if (s == Status::Ok) { why = "'" + item + "' is already held; a secret is never replaced"; return Status::Refused; }
    if (s != Status::Absent) return s;
    const detail::Lib& l = detail::lib();
    detail::GErrorT* e = nullptr;
    const std::string label = "ETCS " + item + " (" + store + ")";
    if (!l.store(detail::schema(), nullptr, label.c_str(), secret.c_str(), nullptr, &e,
                 "store", store.c_str(), "item", item.c_str(), nullptr) || e)
        return detail::failed(e, why);
    return Status::Ok;
}

inline Status erase(const std::string& store, const std::string& item, std::string& why)
{
    const detail::Lib& l = detail::lib();
    if (!l.ok) { why = std::string("no libsecret-1.so.0") + detail::kHelp; return Status::Unavailable; }
    detail::GErrorT* e = nullptr;
    const int gone = l.clear(detail::schema(), nullptr, &e, "store", store.c_str(), "item", item.c_str(), nullptr);
    if (e) return detail::failed(e, why);
    return gone ? Status::Ok : Status::Absent;
}

#elif defined(_WIN32)

namespace detail
{
inline std::string target(const std::string& store, const std::string& item)
{
    return "etcs:" + store + ":" + item;
}
inline Status failed(const char* what, std::string& why)
{
    why = std::string("Credential Manager: ") + what + " failed (error " + std::to_string(GetLastError()) + ")";
    return Status::Unavailable;
}
} // namespace detail

inline Status load(const std::string& store, const std::string& item, std::string& out, std::string& why)
{
    PCREDENTIALA c = nullptr;
    if (!CredReadA(detail::target(store, item).c_str(), CRED_TYPE_GENERIC, 0, &c))
        return GetLastError() == ERROR_NOT_FOUND ? Status::Absent : detail::failed("CredRead", why);
    out.assign(reinterpret_cast<const char*>(c->CredentialBlob), c->CredentialBlobSize);
    SecureZeroMemory(c->CredentialBlob, c->CredentialBlobSize);
    CredFree(c);
    return Status::Ok;
}

inline Status store_new(const std::string& store, const std::string& item, const std::string& secret, std::string& why)
{
    std::string held;
    const Status s = load(store, item, held, why);
    if (!held.empty()) SecureZeroMemory(&held[0], held.size());
    if (s == Status::Ok) { why = "'" + item + "' is already held; a secret is never replaced"; return Status::Refused; }
    if (s != Status::Absent) return s;
    if (secret.size() > CRED_MAX_CREDENTIAL_BLOB_SIZE) { why = "secret too large for a credential"; return Status::Refused; }
    std::string t = detail::target(store, item), user = "etcs";
    CREDENTIALA c{};
    c.Type = CRED_TYPE_GENERIC;
    c.TargetName = &t[0];
    c.UserName = &user[0];
    c.CredentialBlobSize = DWORD(secret.size());
    c.CredentialBlob = reinterpret_cast<LPBYTE>(const_cast<char*>(secret.data()));
    c.Persist = CRED_PERSIST_LOCAL_MACHINE;
    return CredWriteA(&c, 0) ? Status::Ok : detail::failed("CredWrite", why);
}

inline Status erase(const std::string& store, const std::string& item, std::string& why)
{
    if (CredDeleteA(detail::target(store, item).c_str(), CRED_TYPE_GENERIC, 0)) return Status::Ok;
    return GetLastError() == ERROR_NOT_FOUND ? Status::Absent : detail::failed("CredDelete", why);
}

#else

inline Status load(const std::string&, const std::string&, std::string&, std::string& why)
{ why = "no OS keystore here"; return Status::Unavailable; }
inline Status store_new(const std::string&, const std::string&, const std::string&, std::string& why)
{ why = "no OS keystore here"; return Status::Unavailable; }
inline Status erase(const std::string&, const std::string&, std::string& why)
{ why = "no OS keystore here"; return Status::Unavailable; }

#endif

} // namespace keystore
} // namespace ETCS

#endif // KEYSTORE_H__
