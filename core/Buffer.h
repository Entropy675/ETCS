#ifndef TBUFFER_H__
#define TBUFFER_H__

#include <iostream>
#include <csignal>
#include <charconv>
#include <string_view>
#include <functional>
#include <cstring>
#include <fstream>
#include <sys/stat.h>
#include <sstream>
#include <iomanip>
#include <cassert> 

#include "ETCS_API.h"
#include "Log.h"


#ifdef _WIN32
    #include <windows.h>
    // GetModuleHandleEx is the Windows equivalent of dladdr
#else
    #include <dlfcn.h>
#endif


inline std::string getCurrentModulePath() {
#ifdef _WIN32
    char path[MAX_PATH];
    HMODULE hm = NULL;
    // Get the handle of the module containing THIS function
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | 
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)&getCurrentModulePath, &hm)) {
        GetModuleFileNameA(hm, path, sizeof(path));
        return std::string(path);
    }
#else
    Dl_info info;
    if (dladdr((void*)getCurrentModulePath, &info) && info.dli_fname) {
        return std::string(info.dli_fname);
    }
#endif
    return "unknown_current_module_path";
}

inline std::ofstream& getModuleLog()
{
    static thread_local std::ofstream s_log;
    if (!s_log.is_open())
    {
        std::string fullPath = getCurrentModulePath();
        size_t slash = fullPath.find_last_of("/\\");
        std::string name = (slash != std::string::npos) ? fullPath.substr(slash + 1) : fullPath;
        size_t dot = name.rfind('.');
        if (dot != std::string::npos) name = name.substr(0, dot);

#ifdef _WIN32
        CreateDirectoryA("logs", nullptr); // no-op if exists
#else
        mkdir("logs", 0755); // no-op if exists
        // needs #include <sys/stat.h>
#endif

        std::string logPath = "logs/" + name + ".log";
        s_log.open(logPath, std::ios::app);
        
        // verify it actually opened
        if (!s_log.is_open())
            std::cerr << "ETCS_LOG: failed to open log file: " << logPath << "\n";
    }
    return s_log;
}

namespace ETCS
{ 

class Entity; // forward declared, include entity or context header wherever you define your dictionary
class TagManager;
struct Module;
struct WorkBundle;
struct ModuleBundle;



// this is a primitive version, eventually overflow to a shared page managed by the tag manager will be implemented
template <size_t N>
struct alignas(8) TBuffer  // C-style TBuffer struct, cross DLL info transfer
{
    char buf[N];
    static constexpr size_t bufsize = N;
    size_t written;
    size_t read_offset;
    
    TBuffer() : written(0), read_offset(0)
    {
        std::memset(buf, 0, bufsize); 
    }
    
    // Constructor for std::string
    TBuffer(const std::string& input) : written(0), read_offset(0) {
        std::memset(buf, 0, bufsize);
        this->writeString(input.c_str());
    }

    // ctor for c string
    TBuffer(const char* input) : written(0), read_offset(0) {
        std::memset(buf, 0, bufsize);
        if (input) this->writeString(input);
    }
    
    TBuffer(const TBuffer&) = default; // Explicitly allow copying (since its just a bytes wrapper)
    TBuffer(TBuffer&&) noexcept = default; // same for move
    TBuffer& operator=(const TBuffer& other) = default;
    TBuffer& operator=(TBuffer&&) noexcept = default;
    
    operator const char*() const {
        return buf;
    }
    
    operator bool() const {
        return buf[0] != '\0';
    }
    
    void reset() 
    {
        written = 0;
        read_offset = 0;
        if (bufsize > 0) buf[0] = '\0';
    }
    
    void clear() 
    {
        written = 0;
        read_offset = 0;
        std::memset(buf, 0, bufsize); 
    }   // clear completely, config data leakage is bad when explicitly disallowed
    
    std::string toString() const 
    {   // Convert TBuffer content to std::string
        if (written == 0) return "";
        return std::string(buf, written);
    }
    
    std::string restAsString() const 
    {   // Convert TBuffer content to std::string
        if (written == 0) return "";
        return std::string(buf + read_offset, written - read_offset);
    }
    
    const char* c_str() const { return buf; }

    bool operator<(const TBuffer& other) const 
    {   // Lexicographical comparison for map ordering
        return std::strcmp(this->buf, other.buf) < 0;
    }

    bool operator==(const TBuffer& other) const 
    {
        return std::strcmp(this->buf, other.buf) == 0;
    }
    
    bool operator!=(const TBuffer& other) const
    {
        return !(*this == other);
    }
    
    bool writeRaw(const void* data, size_t len)
    {
        if (written + len > bufsize)
        {
            ETCS_LOG("TBuffer", "writeRaw truncated: attempted " << len
                     << " more bytes onto " << written << " already written, capacity "
                     << bufsize << " (TBuffer<" << N << ">)");
            return false;
        }
        std::memcpy(buf + written, data, len);
        written += len;
        return true;
    }

    bool readRaw(void* data, size_t len)
    {
        if (read_offset + len > written) return false;
        std::memcpy(data, buf + read_offset, len);
        read_offset += len;
        return true;
    }

    // strips comma seperation
    bool writeStringSanitized(const char* input) 
    {
        char temp[N];
        size_t i = 0;
        for (; input[i] != '\0' && i < N - 1; ++i) 
        {
            // Replace commas with spaces, skip extra whitespace
            if (input[i] == ',') temp[i] = ' ';
            else temp[i] = input[i];
        }
        // If the loop stopped because it hit the N-1 bound rather than
        // input's own null terminator, input was longer than this buffer
        // can hold — temp[] is already silently clipped at this point
        // (before writeString() below is even called), so writeString's
        // own truncation check would never fire for this specific case.
        // Flag it here instead, where the actual clip happened.
        if (input[i] != '\0')
        {
            ETCS_LOG("TBuffer", "writeStringSanitized truncated: input exceeds capacity "
                     << bufsize << " (TBuffer<" << N << ">)");
        }
        temp[i] = '\0';
        return this->writeString(temp);
    }
    
    bool writeString(const char* str)
    {
        reset();
        size_t str_len = std::strlen(str);
        if (str_len + 1 > bufsize)
        {
            ETCS_LOG("TBuffer", "writeString truncated: string length " << str_len
                     << " exceeds capacity " << bufsize << " (TBuffer<" << N << ">)");
            return false;
        }
        
        std::memcpy(buf, str, str_len);
        written = str_len;
        
        buf[written] = '\0'; 
        return true;
    }

    bool writeString(const char* str, size_t len)
    {
        if (written + len + 1 > bufsize)
        {
            ETCS_LOG("TBuffer", "writeString truncated: attempted " << len
                     << " more bytes onto " << written << " already written, capacity "
                     << bufsize << " (TBuffer<" << N << ">)");
            return false;
        }
        std::memcpy(buf + written, str, len);
        written += len;
        buf[written] = '\0';
        return true;
    }

    bool write(const char* str)
    {
        size_t str_len = std::strlen(str);
        if (written + str_len + 1 > bufsize)
        {
            ETCS_LOG("TBuffer", "write truncated: attempted \"" << str
                     << "\" (" << str_len << " bytes) onto " << written
                     << " already written, capacity " << bufsize << " (TBuffer<" << N << ">)");
            return false;
        }

        std::memcpy(buf + written, str, str_len);
        written += str_len;
        
        buf[written] = '\0'; 
        return true;
    }
};

using Buffer  = TBuffer<MAX_TAG_BUFFER_SIZE>;
using LBuffer = TBuffer<MAX_LMAX_BUFFER_SIZE>;

// BBuffer — dedicated instance for cross-ABI-boundary manifest transport
// (TagName_List / module tag discovery, see discoverActions/discoverTags
// in DynamicLoader.h). Same reasoning as LBuffer's dedicated sizing for
// LMAX input queues: this traffic has a different shape than ordinary
// Buffer usage (it scales with action/tag COUNT, not with any single
// item's size), so it gets its own instance rather than stretching the
// general-purpose Buffer to cover a case it wasn't sized for. See
// MAX_BOUNDARY_BUFFER_SIZE in ETCS_API.h for the sizing rationale.
using BBuffer = TBuffer<MAX_BOUNDARY_BUFFER_SIZE>;

// MBuffer — MirrorBuffer's own stream transport (packConsumer/
// packProducer/unpack). Never seen by end-user work-function bodies --
// those still receive a plain Buffer, extracted via
// MirrorBuffer::getConfig() inside the macro-generated trampoline before
// the developer's own code ever runs.
using MBuffer = TBuffer<MAX_MIRROR_TRANSPORT_SIZE>;

// for network usages, 8kb stack buffer.
using NBuffer = TBuffer<ETCS_NETWORK_MAX_HEADER_SIZE>;

template<size_t N, typename T>
inline TBuffer<N>& operator>>(TBuffer<N>& b, T& val) 
{
    if (b.read_offset >= b.written) return b;

    const char* start = b.buf + b.read_offset;
    const char* end = b.buf + b.written;

    // Direct byte-to-primitive conversion
    auto [ptr, ec] = std::from_chars(start, end, val);

    if (ec == std::errc())
        b.read_offset = static_cast<size_t>(ptr - b.buf);
    else 
        while (b.read_offset < b.written && b.buf[b.read_offset] != ' ' && b.buf[b.read_offset] != ',')
            b.read_offset++;

    // Always skip trailing delimiters to prime the NEXT extraction
    while (b.read_offset < b.written && (b.buf[b.read_offset] == ' ' || b.buf[b.read_offset] == ','))
        b.read_offset++;

    return b;
}

template<size_t N, size_t M>
inline TBuffer<N>& operator>>(TBuffer<N>& src, TBuffer<M>& dest)
{   // Logic to move/copy all remaining data from src to dest
    dest.write(src.buf);
    return src;
}

template<size_t N>
inline TBuffer<N>& operator>>(TBuffer<N>& b, std::string& val) 
{
    if (b.read_offset >= b.written) 
    {
        val.clear();
        return b;
    }

    const char* start = b.buf + b.read_offset;
    const char* end = b.buf + b.written;
    
    char quote = *start;
    bool is_quoted = (quote == '"' || quote == '\'');

    if (is_quoted) 
    {
        val.clear();
        const char* current = start + 1; // Skip opening quote

        while (current < end) 
        {
            if (*current == '\\') 
            {
                // Escape logic: if there's a character after the backslash, take it literally
                if (++current < end) 
                {
                    val.push_back(*current);
                    current++;
                }
            } 
            else if (*current == quote) 
            {
                // Found matching closing quote
                current++; // Move past it
                break;
            } 
            else 
            {
                val.push_back(*current);
                current++;
            }
        }
        b.read_offset = static_cast<size_t>(current - b.buf);
    }
    else 
    {
        // Standard delimiter-based extraction (no escaping in unquoted tokens)
        const char* current = start;
        while (current < end && *current != ' ' && *current != ',') current++;
        val.assign(start, static_cast<size_t>(current - start));
        b.read_offset = static_cast<size_t>(current - b.buf);
    }

    // Skip trailing delimiters so the NEXT extraction is primed
    while (b.read_offset < b.written && (b.buf[b.read_offset] == ' ' || b.buf[b.read_offset] == ',')) b.read_offset++;
    return b;
}

template<size_t N>
inline TBuffer<N>& operator>>(TBuffer<N>& buf, bool& val) 
{
    int temp = 0;
    buf >> temp;
    val = (temp != 0);
    return buf;
}

template<size_t N>
inline std::istream& operator>>(std::istream& is, ETCS::TBuffer<N>& b)
{
    std::string temp;
    if (is >> temp) {
        b.writeString(temp.c_str());
    }
    return is;
}

template<size_t N, typename T>
inline TBuffer<N>& operator<<(TBuffer<N>& b, const T& val) 
{
    if (b.written >= N - 1) return b;

    char* start = b.buf + b.written;
    char* end = b.buf + N - 1;

    auto [ptr, ec] = std::to_chars(start, end, val);

    if (ec == std::errc()) 
    {
        b.written = static_cast<size_t>(ptr - b.buf);
        if (b.written < N - 1) b.buf[b.written++] = ' ';
        b.buf[b.written] = '\0'; // Explicit, safe termination
    }
    return b;
}

template<size_t N>
inline TBuffer<N>& operator<<(TBuffer<N>& b, const std::string& val) 
{
    // Need room for quotes, potential escapes, space, and null terminator
    if (b.written >= N - 4) return b;

    b.buf[b.written++] = '"'; // Open the string token

    for (char c : val) 
    {
        // Safety check to ensure we don't overflow during escaping
        if (b.written >= N - 3) break; 
        
        if (c == '"' || c == '\\') 
        {
            b.buf[b.written++] = '\\'; // Escape internal quotes and backslashes
        }
        b.buf[b.written++] = c;
    }

    b.buf[b.written++] = '"'; // Close the string token
    
    // Inject the trailing space delimiter
    if (b.written < N - 1) 
    {
        b.buf[b.written++] = ' '; 
    }
    
    b.buf[b.written] = '\0';
    
    return b;
}

template<size_t N>
inline TBuffer<N>& operator<<(TBuffer<N>& buf, const bool& val) {
    // Manually write '1' or '0' (or "true"/"false") to your TBuffer
    return buf << (val ? 1 : 0); 
}

// Overload for piping one TBuffer into another (Append operation)
template<size_t N, size_t M>
inline TBuffer<N>& operator<<(TBuffer<N>& b, const TBuffer<M>& val) 
{
    // Simply use the existing write method to append the raw contents
    b.write(val.c_str());
    
    // Maintain your delimiter consistency
    if (b.written < N - 1) {
        b.buf[b.written++] = ' ';
        b.buf[b.written] = '\0';
    }
    return b;
}

template<size_t N>
inline std::ostream& operator<<(std::ostream& os, const TBuffer<N>& TBuffer) 
{
    os << TBuffer.buf; 
    return os;
}

// Compare TBuffer to string_view (covers string literals AND std::string seamlessly)
template<size_t N>
inline bool operator==(const ETCS::TBuffer<N>& lhs, std::string_view rhs) { 
    return rhs == std::string_view(lhs.buf, lhs.written); 
}
template<size_t N>
inline bool operator!=(const ETCS::TBuffer<N>& lhs, std::string_view rhs) { return !(lhs == rhs); }

// Reverse
template<size_t N>
inline bool operator==(std::string_view lhs, const ETCS::TBuffer<N>& rhs) { return rhs == lhs; }
template<size_t N>
inline bool operator!=(std::string_view lhs, const ETCS::TBuffer<N>& rhs) { return !(lhs == rhs); }

} // namespace ETCS

// for unordered map compatibility
namespace std 
{
    template <size_t N>
    struct hash<ETCS::TBuffer<N>> 
    {
        // Use string_view to hash the actual content safely
        std::size_t operator()(const ETCS::TBuffer<N>& b) const {
            return std::hash<std::string_view>{}(std::string_view(b.buf, b.written));
        }
    };
}

#endif
