#ifndef SANDBOX_GUARD_H__
#define SANDBOX_GUARD_H__

#include <vector>
#include <string>
#include <mutex>
#include <iostream>

#ifdef _WIN32
    #include <windows.h>
    #include <winternl.h>
    #include <dbghelp.h>
    #pragma comment(lib, "dbghelp.lib")
    using NativeHandle = HANDLE;
    #define INVALID_NATIVE_HANDLE INVALID_HANDLE_VALUE
#else
    #include <dlfcn.h>
    #include <fcntl.h>
    #include <unistd.h>
    #include <sys/stat.h>
    #include <stdarg.h>
    using NativeHandle = int;
    #define INVALID_NATIVE_HANDLE (-1)
#endif

// SandboxRegistry - Thread-safe resource tracking
namespace ETCS {

struct SandboxRegistry 
{
    static SandboxRegistry& get() 
    {
        static SandboxRegistry instance; // Meyers Singleton: thread-safe initialization
        return instance;
    }

    // No copying the registry
    SandboxRegistry(const SandboxRegistry&) = delete;
    SandboxRegistry& operator=(const SandboxRegistry&) = delete;

    void add(NativeHandle h) 
    {
        if (h != INVALID_NATIVE_HANDLE) 
        {
            std::lock_guard<std::mutex> lock(mtx);
            handles.push_back(h);
        }
    }

    void remove(NativeHandle h) 
    {
        if (h != INVALID_NATIVE_HANDLE) 
        {
            std::lock_guard<std::mutex> lock(mtx); // Erase-Remove idiom
            handles.erase(std::remove(handles.begin(), handles.end(), h), handles.end());
        }
    }

    SandboxRegistry() = default;
    ~SandboxRegistry() {} // SandboxGuard should always drain before this fires

    std::mutex mtx;
    std::vector<NativeHandle> handles;
};

}

#ifdef _WIN32
// --- WINDOWS IAT HOOKING LOGIC ---

typedef HANDLE (WINAPI *CreateFileW_t)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
static CreateFileW_t OriginalCreateFileW = nullptr;

typedef HANDLE (WINAPI *CreateFileA_t)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
static CreateFileA_t OriginalCreateFileA = nullptr;

typedef BOOL (WINAPI *CloseHandle_t)(HANDLE);
static CloseHandle_t OriginalCloseHandle = nullptr;

BOOL WINAPI HookedCloseHandle(HANDLE h) {
    {
        std::lock_guard<std::mutex> lock(ETCS::SandboxRegistry::get().mtx);
        auto& v = ETCS::SandboxRegistry::get().handles;
        v.erase(std::remove(v.begin(), v.end(), h), v.end());
    }
    return OriginalCloseHandle(h);
}

HANDLE WINAPI HookedCreateFileW(LPCWSTR n, DWORD a, DWORD s, LPSECURITY_ATTRIBUTES sa, DWORD d, DWORD f, HANDLE t) {
    HANDLE h = OriginalCreateFileW(n, a, s, sa, d, f, t);
    ETCS::SandboxRegistry::get().add(h);
    return h;
}

HANDLE WINAPI HookedCreateFileA(LPCSTR n, DWORD a, DWORD s, LPSECURITY_ATTRIBUTES sa, DWORD d, DWORD f, HANDLE t) {
    HANDLE h = OriginalCreateFileA(n, a, s, sa, d, f, t);
    ETCS::SandboxRegistry::get().add(h);
    return h;
}

void ApplyIATHook() {
    HMODULE hModule = NULL;
    GetModuleHandleEx(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | 
                      GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                      (LPCTSTR)ApplyIATHook, &hModule);
    
    if (!hModule) return;
    
    ULONG size;
    auto* importDesc = (PIMAGE_IMPORT_DESCRIPTOR)ImageDirectoryEntryToData(
        hModule, TRUE, IMAGE_DIRECTORY_ENTRY_IMPORT, &size);
    
    if (!importDesc) return;
    
    HMODULE hKernel32 = GetModuleHandleA("kernel32.dll");
    if (!hKernel32) return;
    
    PROC procCreateFileW = GetProcAddress(hKernel32, "CreateFileW");
    PROC procCreateFileA = GetProcAddress(hKernel32, "CreateFileA");
    PROC procCloseHandle = GetProcAddress(hKernel32, "CloseHandle");

    while (importDesc->Name) {
        const char* modName = (const char*)((BYTE*)hModule + importDesc->Name);
        if (_stricmp(modName, "kernel32.dll") == 0) {
            auto* thunk = (PIMAGE_THUNK_DATA)((BYTE*)hModule + importDesc->FirstThunk);
            while (thunk->u1.Function) {
                PROC* fnPtr = (PROC*)&thunk->u1.Function;
                
                // Hook CreateFileW
                if (*fnPtr == procCreateFileW && !OriginalCreateFileW) {
                    DWORD oldProtect;
                    VirtualProtect(fnPtr, sizeof(PROC), PAGE_EXECUTE_READWRITE, &oldProtect);
                    OriginalCreateFileW = (CreateFileW_t)*fnPtr;
                    *fnPtr = (PROC)HookedCreateFileW;
                    VirtualProtect(fnPtr, sizeof(PROC), oldProtect, &oldProtect);
                }
                
                // Hook CreateFileA
                if (*fnPtr == procCreateFileA && !OriginalCreateFileA) {
                    DWORD oldProtect;
                    VirtualProtect(fnPtr, sizeof(PROC), PAGE_EXECUTE_READWRITE, &oldProtect);
                    OriginalCreateFileA = (CreateFileA_t)*fnPtr;
                    *fnPtr = (PROC)HookedCreateFileA;
                    VirtualProtect(fnPtr, sizeof(PROC), oldProtect, &oldProtect);
                }
                
                // Hook CloseHandle
                if (*fnPtr == procCloseHandle && !OriginalCloseHandle) {
                    DWORD oldProtect;
                    VirtualProtect(fnPtr, sizeof(PROC), PAGE_EXECUTE_READWRITE, &oldProtect);
                    OriginalCloseHandle = (CloseHandle_t)*fnPtr;
                    *fnPtr = (PROC)HookedCloseHandle;
                    VirtualProtect(fnPtr, sizeof(PROC), oldProtect, &oldProtect);
                }
                
                thunk++;
            }
        }
        importDesc++;
    }
}
#endif

// --- THE GUARD STRUCT ---
//
// CHANGED: was a plain static global (`static SandboxGuard sandbox;` at the
// bottom of this file), constructed at whatever point static init order
// happened to reach it — and, since its own constructor calls
// SandboxRegistry::get() (a SEPARATE, lazily-constructed function-local
// static), the two ended up with an implicit, easy-to-get-wrong relative
// construction/destruction order between them. Now a Meyers singleton like
// every other core subsystem (EventNode, ThreadPool, MemoryArena) — a
// module explicitly calls SandboxGuard::getInstance() once, early, in the
// SAME explicit init chain as those others (see RegisterEventNode,
// ETCS_API.h, and _core_init, DynamicLoader.h), giving predictable
// construction order instead of leaving it to whichever code path happens
// to touch it first.
//
// cleanup() is a NEW, separate, IDEMPOTENT method — the actual teardown
// logic, guarded by cleaned_ so calling it twice (once explicitly from
// Name##_Cleanup(), once more from whatever LATER, ordinary static
// destruction eventually also reaches this object) is always safe, the
// same pattern MemoryArena's isTeardown_ and ThreadPool's is_drained_
// already use. ~SandboxGuard() just calls cleanup() too, so a path that
// never explicitly cleans up (the loader's own instance, relying on
// ordinary process-exit static destruction) still works correctly.
struct SandboxGuard 
{
    static SandboxGuard& getInstance()
    {
        static SandboxGuard instance;
        return instance;
    }

    SandboxGuard(const SandboxGuard&)            = delete;
    SandboxGuard& operator=(const SandboxGuard&) = delete;

    void cleanup()
    {
        bool expected = false;
        if (!cleaned_.compare_exchange_strong(expected, true))
        {
            ETCS_LOG("SandboxGuard", "cleanup() already ran, skipping.");
            return;
        }

        std::lock_guard<std::mutex> lock(ETCS::SandboxRegistry::get().mtx);
        ETCS_LOG("SandboxGuard", "Exiting sandbox guard...");
        for (auto h : ETCS::SandboxRegistry::get().handles) {
#ifdef _WIN32
            // CloseHandle is safe to call on already-closed or invalid handles
            if (h != INVALID_HANDLE_VALUE) {
                CloseHandle(h);
            }
#else
            using close_t = int (*)(int);
            static close_t real_close = (close_t)dlsym(RTLD_NEXT, "close");

            // close() returns -1 for invalid fd, which is harmless
            if (h >= 0)
                if (real_close(h) != -1)
                    ETCS_LOG("SandboxGuard:Alert", "Closed hanging FD: " << h);
#endif
        }
        ETCS::SandboxRegistry::get().handles.clear();
    }

    ~SandboxGuard()
    {
        cleanup();
#ifdef _WIN32
        (void)0;
#endif
    }

private:
    SandboxGuard() 
    {
        ETCS::SandboxRegistry::get();
        ETCS_LOG("SandboxGuard", "Initialized sandbox registry...");
#ifdef _WIN32
        ApplyIATHook();
#endif
    }

    std::atomic<bool> cleaned_{false};
};

// --- LINUX PROXIES ---
#ifndef _WIN32
#include <sys/epoll.h>
#include <sys/socket.h>
#include <stdarg.h>

int open(const char* pathname, int flags, ...) noexcept {
    using open_t = int (*)(const char*, int, mode_t);
    static open_t real_open = (open_t)dlsym(RTLD_NEXT, "open");
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list args; va_start(args, flags);
        mode = va_arg(args, mode_t);
        va_end(args);
    }
    int fd = real_open(pathname, flags, mode);
    if (fd != -1) ETCS::SandboxRegistry::get().add(fd);
    else return -1;
    ETCS_LOG("SandboxGuard", "open() FD: " << fd << " -> " << pathname);
    return fd;
}

extern "C" int open64(const char* pathname, int flags, ...) {
    using open64_t = int (*)(const char*, int, mode_t);
    static open64_t real_open64 = (open64_t)dlsym(RTLD_NEXT, "open64");
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list args; va_start(args, flags);
        mode = va_arg(args, mode_t);
        va_end(args);
    }
    int fd = real_open64(pathname, flags, mode);
    if (fd != -1) ETCS::SandboxRegistry::get().add(fd);
    else return -1;
    ETCS_LOG("SandboxGuard", "open64() FD: " << fd << " -> " << pathname);
    return fd;
}

int openat(int dirfd, const char* pathname, int flags, ...) noexcept {
    using openat_t = int (*)(int, const char*, int, mode_t);
    static openat_t real_openat = (openat_t)dlsym(RTLD_NEXT, "openat");
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list args; va_start(args, flags);
        mode = va_arg(args, mode_t);
        va_end(args);
    }
    int fd = real_openat(dirfd, pathname, flags, mode);
    if (fd != -1) ETCS::SandboxRegistry::get().add(fd);
    else return -1;
    ETCS_LOG("SandboxGuard", "openat() FD: " << fd << " -> " << pathname);
    return fd;
}

int socket(int domain, int type, int protocol) noexcept {
    using real_socket_t = int (*)(int, int, int);
    static real_socket_t real_socket = (real_socket_t)dlsym(RTLD_NEXT, "socket");
    int fd = real_socket(domain, type, protocol);
    if (fd != -1) ETCS::SandboxRegistry::get().add(fd);
    else return -1;
    ETCS_LOG("SandboxGuard", "socket() FD: " << fd);
    return fd;
}

int accept(int sockfd, struct sockaddr *addr, socklen_t *addrlen) noexcept {
    using real_accept_t = int (*)(int, struct sockaddr*, socklen_t*);
    static real_accept_t real_accept = (real_accept_t)dlsym(RTLD_NEXT, "accept");
    int fd = real_accept(sockfd, addr, addrlen);
    if (fd != -1) ETCS::SandboxRegistry::get().add(fd);
    else return -1;
    ETCS_LOG("SandboxGuard", "accept() FD: " << fd << " (from listener: " << sockfd << ")");
    return fd;
}

int accept4(int sockfd, struct sockaddr *addr, socklen_t *addrlen, int flags) noexcept {
    using real_accept4_t = int (*)(int, struct sockaddr*, socklen_t*, int);
    static real_accept4_t real_accept4 = (real_accept4_t)dlsym(RTLD_NEXT, "accept4");
    int fd = real_accept4(sockfd, addr, addrlen, flags);
    if (fd != -1) ETCS::SandboxRegistry::get().add(fd);
    else return -1;
    ETCS_LOG("SandboxGuard", "accept4() FD: " << fd << " (from listener: " << sockfd << ")");
    return fd;
}

int epoll_create(int size) noexcept {
    using real_epoll_t = int (*)(int);
    static real_epoll_t real_epoll = (real_epoll_t)dlsym(RTLD_NEXT, "epoll_create");
    int fd = real_epoll(size);
    if (fd != -1) ETCS::SandboxRegistry::get().add(fd);
    else return -1;
    ETCS_LOG("SandboxGuard", "epoll_create() FD: " << fd);
    return fd;
}

int epoll_create1(int flags) noexcept {
    using real_epoll1_t = int (*)(int);
    static real_epoll1_t real_epoll1 = (real_epoll1_t)dlsym(RTLD_NEXT, "epoll_create1");
    int fd = real_epoll1(flags);
    if (fd != -1) ETCS::SandboxRegistry::get().add(fd);
    ETCS_LOG("SandboxGuard", "epoll_create1() FD: " << fd);
    return fd;
}

int close(int fd) noexcept {
    using close_t = int (*)(int);
    static close_t real_close = (close_t)dlsym(RTLD_NEXT, "close");
    ETCS::SandboxRegistry::get().remove(fd);
    ETCS_LOG("SandboxGuard", "close() FD: " << fd);
    return real_close(fd);
}

#endif

// The old bottom-of-file `static SandboxGuard sandbox;` eager instance is
// gone — construction now happens explicitly via SandboxGuard::getInstance()
// in the same init chain as EventNode/ThreadPool/MemoryArena (see
// RegisterEventNode() in ETCS_API.h for modules, _core_init in
// DynamicLoader.h for the loader), rather than implicitly whenever
// something first happens to touch it.

#endif // SANDBOX_GUARD_H__
