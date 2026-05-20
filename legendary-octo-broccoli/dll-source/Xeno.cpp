// =============================================================================
// Dexe.cpp — C++ DLL Source for Dexe.dll
//
// All dependencies are bundled in deps/ — no vcpkg or online installs needed.
//
// Bundled in deps/:
//   deps/resource.h            — resource IDs
//   deps/Luau/                 — Luau compiler headers
//   deps/LuauSrc/              — Luau compiler sources (add all .cpp to project)
//   deps/httplib.h             — cpp-httplib single-header (MIT)
//   deps/json.hpp              — nlohmann/json single-header (MIT)
//   deps/base64.hpp            — tobiaslocker/base64 single-header (MIT)
//   deps/zstd/zstd.h + zstd.c — zstd single-file (see deps/zstd/README.txt)
//
// xxhash is implemented inline below — no external file needed.
//
// Improvements over original:
//   - Compile-time string obfuscation (no plaintext process/API names in binary)
//   - Randomised HTTP port via named shared memory (replaces hardcoded 19283)
//   - DataModel address cached with 600ms TTL (avoids redundant memory reads)
//   - condition_variable replaces Sleep() polling in client watcher
//   - No AllocConsole / no stdout/stderr in the DLL
//   - All ntdll APIs resolved at runtime via GetProcAddress
//   - xxhash XXH32 implemented inline — zero external library deps
// =============================================================================

#include <Windows.h>
#include <TlHelp32.h>
#include <objbase.h>
#include <psapi.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <future>
#include <mutex>
#include <regex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>


// =============================================================================
// Compile-time string obfuscation
// Each char XOR'd with (KEY ^ (index * STEP)) at compile time.
// .c() decrypts into a thread-local buffer at call site.
// =============================================================================
template<size_t N, uint8_t KEY = 0xC3, uint8_t STEP = 7>
struct XStr {
    char buf[N]{};
    constexpr XStr(const char (&s)[N]) {
        for (size_t i = 0; i < N; i++)
            buf[i] = s[i] ^ static_cast<char>(KEY ^ (i * STEP));
    }
    const char* c() const {
        thread_local char out[N]{};
        for (size_t i = 0; i < N; i++)
            out[i] = buf[i] ^ static_cast<char>(KEY ^ (i * STEP));
        return out;
    }
};
template<size_t N, uint8_t KEY = 0xC3, uint8_t STEP = 7>
struct XWStr {
    wchar_t buf[N]{};
    constexpr XWStr(const wchar_t (&s)[N]) {
        for (size_t i = 0; i < N; i++)
            buf[i] = s[i] ^ static_cast<wchar_t>(KEY ^ (i * STEP));
    }
    const wchar_t* c() const {
        thread_local wchar_t out[N]{};
        for (size_t i = 0; i < N; i++)
            out[i] = buf[i] ^ static_cast<wchar_t>(KEY ^ (i * STEP));
        return out;
    }
};
#define OBF(s)  XStr <sizeof(s)>                (s).c()
#define WOBF(s) XWStr<sizeof(s)/sizeof(wchar_t)>(s).c()


// =============================================================================
// Inline XXH32 — replaces the xxhash vcpkg dependency entirely.
// Bit-exact with the reference xxhash implementation.
// =============================================================================
namespace xxh {
    static constexpr uint32_t P1 = 0x9E3779B1u;
    static constexpr uint32_t P2 = 0x85EBCA77u;
    static constexpr uint32_t P3 = 0xC2B2AE3Du;
    static constexpr uint32_t P4 = 0x27D4EB2Fu;
    static constexpr uint32_t P5 = 0x165667B1u;

    inline uint32_t rotl(uint32_t x, int r) noexcept { return (x << r) | (x >> (32 - r)); }
    inline uint32_t round(uint32_t acc, uint32_t in) noexcept { return rotl(acc + in * P2, 13) * P1; }

    inline uint32_t XXH32(const void* data, size_t len, uint32_t seed) noexcept {
        const uint8_t* p   = static_cast<const uint8_t*>(data);
        const uint8_t* end = p + len;
        uint32_t h;
        if (len >= 16) {
            uint32_t v1 = seed + P1 + P2, v2 = seed + P2, v3 = seed, v4 = seed - P1;
            do {
                uint32_t t;
                memcpy(&t, p, 4); v1 = round(v1, t); p += 4;
                memcpy(&t, p, 4); v2 = round(v2, t); p += 4;
                memcpy(&t, p, 4); v3 = round(v3, t); p += 4;
                memcpy(&t, p, 4); v4 = round(v4, t); p += 4;
            } while (p <= end - 16);
            h = rotl(v1, 1) + rotl(v2, 7) + rotl(v3, 12) + rotl(v4, 18);
        } else { h = seed + P5; }
        h += static_cast<uint32_t>(len);
        while (p + 4 <= end) {
            uint32_t t; memcpy(&t, p, 4);
            h = rotl(h + t * P3, 17) * P4; p += 4;
        }
        while (p < end) h = rotl(h + (*p++) * P5, 11) * P1;
        h ^= h >> 15; h *= P2; h ^= h >> 13; h *= P3; h ^= h >> 16;
        return h;
    }
}


// =============================================================================
// ntdll API — all resolved at runtime to avoid suspicious static imports
// =============================================================================
#define NTSTATUS DWORD
typedef NTSTATUS(NtReadVirtualMemory_t) (HANDLE, LPCVOID, PVOID,  ULONG, PSIZE_T);
typedef NTSTATUS(NtWriteVirtualMemory_t)(HANDLE, PVOID,   PVOID,  ULONG, PSIZE_T);
typedef NTSTATUS(NtUnlockVirtualMemory_t)(HANDLE, PVOID*, PSIZE_T, ULONG);

struct NtAPI {
    NtReadVirtualMemory_t*   Read   = nullptr;
    NtWriteVirtualMemory_t*  Write  = nullptr;
    NtUnlockVirtualMemory_t* Unlock = nullptr;
} static Nt;

static void LoadNt() {
    HMODULE h = LoadLibraryA(OBF("ntdll.dll"));
    if (!h) return;
    Nt.Read   = reinterpret_cast<NtReadVirtualMemory_t*>  (GetProcAddress(h, OBF("NtReadVirtualMemory")));
    Nt.Write  = reinterpret_cast<NtWriteVirtualMemory_t*> (GetProcAddress(h, OBF("NtWriteVirtualMemory")));
    Nt.Unlock = reinterpret_cast<NtUnlockVirtualMemory_t*>(GetProcAddress(h, OBF("NtUnlockVirtualMemory")));
}


// =============================================================================
// Memory helpers
// =============================================================================
template<typename T>
static T read_mem(std::uintptr_t addr, HANDLE h) {
    T val{};
    if (Nt.Read) Nt.Read(h, reinterpret_cast<LPCVOID>(addr), &val, sizeof(T), nullptr);
    return val;
}

template<typename T>
static bool write_mem(std::uintptr_t addr, const T& val, HANDLE h) {
    DWORD old, d; SIZE_T bw;
    if (!VirtualProtectEx(h, reinterpret_cast<LPVOID>(addr), sizeof(T), PAGE_READWRITE, &old))
        return false;
    bool ok = Nt.Write
        && (Nt.Write(h, reinterpret_cast<PVOID>(addr), const_cast<T*>(&val), sizeof(T), &bw) == 0)
        && bw == sizeof(T);
    VirtualProtectEx(h, reinterpret_cast<LPVOID>(addr), sizeof(T), old, &d);
    return ok;
}

static std::string read_rbx_str(std::uintptr_t addr, HANDLE h) {
    uint64_t len = read_mem<uint64_t>(addr + 0x10, h);
    if (!len || len > 15000) return {};
    std::uintptr_t ptr = (len > 15) ? read_mem<std::uintptr_t>(addr, h) : addr;
    std::string buf(len, '\0');
    if (Nt.Read) Nt.Read(h, reinterpret_cast<LPCVOID>(ptr), buf.data(), static_cast<ULONG>(len), nullptr);
    return buf;
}


// =============================================================================
// Roblox memory offsets
// =============================================================================
namespace off {
    constexpr uint64_t Self        = 0x08;
    constexpr uint64_t Name        = 0x50;
    constexpr uint64_t Children    = 0x58;
    constexpr uint64_t ClassDesc   = 0x18;
    constexpr uint64_t ClassName   = 0x08;
    constexpr uint64_t ModuleEmbed = 0x160;
    constexpr uint64_t CoreScript  = 0x1A8;
    constexpr uint64_t ModuleFlags = CoreScript - 0x4;
    constexpr uint64_t LocalEmbed  = 0x1C0;
    constexpr uint64_t Bytecode    = 0x10;
    constexpr uint64_t BytecodeSize= 0x20;
    constexpr uint64_t LocalPlayer = 0x110;
    constexpr uint64_t ObjValue    = 0xC0;
}

constexpr std::string_view VERSION = "1.0.8";


// =============================================================================
// Luau + zstd includes (all from deps/)
// =============================================================================
#include "deps/Luau/Compiler.h"
#include "deps/Luau/BytecodeBuilder.h"
#include "deps/Luau/BytecodeUtils.h"

// zstd single-file — place deps/zstd/zstd.h + zstd.c per deps/zstd/README.txt
#include "deps/zstd/zstd.h"

class BytecodeEncoder : public Luau::BytecodeEncoder {
    void encode(uint32_t* data, size_t count) override {
        for (size_t i = 0; i < count;) {
            auto& op = *reinterpret_cast<uint8_t*>(data + i);
            i += Luau::getOpLength(LuauOpcode(op));
            op *= 227;
        }
    }
};

// RSB1 compress: ZSTD-compress bytecode then XOR-scramble with xxhash key
static std::string CompressRSB1(const std::string_view bc) {
    const size_t src = bc.size(), bound = ZSTD_compressBound(src);
    std::vector<char> buf(bound + 8);
    buf[0]='R'; buf[1]='S'; buf[2]='B'; buf[3]='1';
    memcpy(&buf[4], &src, 4);
    size_t csz = ZSTD_compress(&buf[8], bound, bc.data(), src, ZSTD_maxCLevel());
    if (ZSTD_isError(csz)) return {};
    size_t total = csz + 8;
    uint32_t key  = xxh::XXH32(buf.data(), total, 42u);
    const uint8_t* kb = reinterpret_cast<const uint8_t*>(&key);
    for (size_t i = 0; i < total; i++) buf[i] ^= kb[i % 4] + static_cast<uint8_t>(i * 41u);
    return std::string(buf.data(), total);
}

// RSB1 decompress: reverse the XOR scramble then ZSTD-decompress
static std::string DecompressRSB1(const std::string_view in) {
    if (in.size() < 8) return {};
    const uint8_t sig[4]={'R','S','B','1'};
    std::vector<uint8_t> d(in.begin(), in.end());
    uint8_t hdr[4];
    for (int i = 0; i < 4; i++) {
        hdr[i] = d[i] ^ sig[i];
        hdr[i] = static_cast<uint8_t>((hdr[i] - i * 41) % 256);
    }
    for (size_t i = 0; i < d.size(); i++)
        d[i] ^= (hdr[i % 4] + static_cast<uint8_t>(i * 41)) % 256;
    uint32_t expected = 0;
    for (int i = 0; i < 4; i++) expected |= hdr[i] << (i * 8);
    if (xxh::XXH32(d.data(), d.size(), 42u) != expected) return {};
    uint32_t dsz = 0;
    for (int i = 4; i < 8; i++) dsz |= d[i] << ((i - 4) * 8);
    std::vector<uint8_t> out(dsz);
    size_t actual = ZSTD_decompress(out.data(), dsz, d.data() + 8, d.size() - 8);
    if (ZSTD_isError(actual)) return {};
    return std::string(reinterpret_cast<char*>(out.data()), actual);
}

static std::string Compile(const std::string& src) {
    static BytecodeEncoder enc;
    return CompressRSB1(Luau::compile(src, {}, {}, &enc));
}

// Returns "success" or the compiler error message
static std::string CheckCompile(const std::string& src, bool returnBytecode = false) {
    static BytecodeEncoder enc;
    std::string bc = Luau::compile(src, {}, {}, &enc);
    if (bc.empty()) return "success";
    if (bc[0] == '\0') {
        bc.erase(std::remove(bc.begin(), bc.end(), '\0'), bc.end());
        return bc;
    }
    return returnBytecode ? bc : "success";
}


// =============================================================================
// Instance — thin wrapper over a Roblox object in game memory
// =============================================================================
class Instance {
    std::uintptr_t _self;
    HANDLE _h;

    std::vector<std::uintptr_t> _children() const {
        std::vector<std::uintptr_t> out;
        auto ptr   = read_mem<std::uintptr_t>(_self + off::Children, _h); if (!ptr) return out;
        auto start = read_mem<std::uintptr_t>(ptr,        _h);
        auto last  = read_mem<std::uintptr_t>(ptr + 0x8,  _h) + 1;
        for (auto a = start; a < last; a += 0x10) {
            auto ch = read_mem<std::uintptr_t>(a, _h);
            if (ch) out.push_back(ch);
        }
        return out;
    }

public:
    Instance(std::uintptr_t addr, HANDLE h) : _self(addr), _h(h) {}

    std::uintptr_t ptr()       const { return _self; }
    std::string    Name()      const { return read_rbx_str(read_mem<std::uintptr_t>(_self + off::Name, _h), _h); }
    std::string    ClassName() const {
        return read_rbx_str(
            read_mem<std::uintptr_t>(
                read_mem<std::uintptr_t>(_self + off::ClassDesc, _h) + off::ClassName, _h), _h);
    }

    std::uintptr_t FindAddr(const std::string_view name) const {
        for (auto a : _children())
            if (read_rbx_str(read_mem<std::uintptr_t>(a + off::Name, _h), _h) == name) return a;
        return 0;
    }
    std::unique_ptr<Instance> Find(const std::string_view name) const {
        auto a = FindAddr(name);
        return a ? std::make_unique<Instance>(a, _h) : nullptr;
    }
    std::uintptr_t FindClassAddr(const std::string_view cls) const {
        for (auto a : _children())
            if (read_rbx_str(
                    read_mem<std::uintptr_t>(
                        read_mem<std::uintptr_t>(a + off::ClassDesc, _h) + off::ClassName, _h), _h) == cls)
                return a;
        return 0;
    }
    std::unique_ptr<Instance> FindClass(const std::string_view cls) const {
        auto a = FindClassAddr(cls);
        return a ? std::make_unique<Instance>(a, _h) : nullptr;
    }
    // Polls until a named child appears or timeout elapses
    std::uintptr_t WaitAddr(const std::string_view name, int timeoutSec = 300) const {
        auto dl = std::chrono::steady_clock::now() + std::chrono::seconds(timeoutSec);
        while (std::chrono::steady_clock::now() < dl) {
            auto a = FindAddr(name); if (a) return a;
            Sleep(80);
        }
        return 0;
    }

    std::string GetBytecode() const {
        auto cls = ClassName();
        if (cls != "LocalScript" && cls != "ModuleScript") return {};
        auto emb  = read_mem<std::uintptr_t>(_self + (cls == "LocalScript" ? off::LocalEmbed : off::ModuleEmbed), _h);
        auto ptr  = read_mem<std::uintptr_t>(emb + off::Bytecode,     _h);
        auto sz   = read_mem<uint64_t>       (emb + off::BytecodeSize, _h);
        std::string buf(sz, '\0');
        MEMORY_BASIC_INFORMATION bi{}; VirtualQueryEx(_h, reinterpret_cast<LPCVOID>(ptr), &bi, sizeof(bi));
        if (Nt.Read) Nt.Read(_h, reinterpret_cast<LPCVOID>(ptr), buf.data(), static_cast<ULONG>(sz), nullptr);
        if (Nt.Unlock) { PVOID ba=bi.AllocationBase; SIZE_T bsz=bi.RegionSize; Nt.Unlock(_h, &ba, &bsz, 1); }
        return DecompressRSB1(buf);
    }

    bool SetBytecode(const std::string& compressed, bool revert = false) const {
        auto cls = ClassName();
        if (cls != "LocalScript" && cls != "ModuleScript") return false;
        auto emb  = read_mem<std::uintptr_t>(_self + (cls == "LocalScript" ? off::LocalEmbed : off::ModuleEmbed), _h);
        auto origPtr = read_mem<std::uintptr_t>(emb + off::Bytecode,     _h);
        auto origSz  = read_mem<uint64_t>       (emb + off::BytecodeSize, _h);
        if (revert) {
            auto e=emb, op=origPtr, os=origSz, hh=_h;
            std::thread([e,op,os,hh]{ Sleep(850);
                write_mem<std::uintptr_t>(e+off::Bytecode,     op, hh);
                write_mem<uint64_t>      (e+off::BytecodeSize, os, hh); }).detach();
        }
        LPVOID alloc = VirtualAllocEx(_h, nullptr, compressed.size(), MEM_COMMIT|MEM_RESERVE, PAGE_READWRITE);
        if (!alloc) return false;
        DWORD old, d;
        VirtualProtectEx(_h, alloc, compressed.size(), PAGE_READWRITE, &old);
        if (Nt.Write) Nt.Write(_h, alloc, const_cast<char*>(compressed.c_str()), static_cast<ULONG>(compressed.size()), nullptr);
        VirtualProtectEx(_h, alloc, compressed.size(), old, &d);
        return write_mem<std::uintptr_t>(emb+off::Bytecode,     reinterpret_cast<std::uintptr_t>(alloc), _h)
            && write_mem<uint64_t>       (emb+off::BytecodeSize, compressed.size(), _h);
    }

    void UnlockModule() const {
        if (ClassName() == "ModuleScript") {
            write_mem<std::uintptr_t>(_self + off::ModuleFlags, 0x100000000, _h);
            write_mem<std::uintptr_t>(_self + off::CoreScript,  0x1,         _h);
        }
    }

    std::vector<std::string> GetProperties() const {
        std::vector<std::string> out;
        auto desc  = read_mem<std::uintptr_t>(_self + off::ClassDesc, _h);
        auto start = read_mem<std::uintptr_t>(desc + 0x28, _h);
        auto end   = read_mem<std::uintptr_t>(desc + 0x30, _h);
        for (auto a = start; a < end; a += 0x8) {
            auto p = read_mem<std::uintptr_t>(a, _h);
            if (p) out.push_back(read_rbx_str(read_mem<std::uintptr_t>(p + 0x8, _h), _h));
        }
        return out;
    }
};


// =============================================================================
// RBXClient — represents one attached Roblox process
// =============================================================================
class RBXClient {
    HANDLE _h = nullptr;
    std::uintptr_t _rv = 0;

    // DataModel pointer cached for 600 ms to avoid redundant memory reads
    mutable std::uintptr_t _dmCache = 0;
    mutable std::chrono::steady_clock::time_point _dmTime{};
    static constexpr auto DM_TTL = std::chrono::milliseconds(600);

public:
    std::string Username = "N/A";
    std::string Version;
    std::string GUID;
    DWORD PID = 0;
    std::string TeleportQueue;
    std::filesystem::path ClientDir;

    explicit RBXClient(DWORD pid);
    ~RBXClient() { if (_h && _h != INVALID_HANDLE_VALUE) CloseHandle(_h); }

    bool alive() const {
        DWORD code = 0;
        return GetExitCodeProcess(_h, &code) && code == STILL_ACTIVE;
    }

    std::uintptr_t DataModel() const {
        auto now = std::chrono::steady_clock::now();
        if (_dmCache && now - _dmTime < DM_TTL) return _dmCache;
        auto fdm = read_mem<std::uintptr_t>(_rv + 0x118, _h);
        if (!fdm) return 0;
        _dmCache = fdm + 0x190;
        _dmTime  = now;
        return _dmCache;
    }

    void execute(const std::string& src) const;
    bool loadstring(const std::string& src, const std::string& sname, const std::string& cname) const;
    std::string GetBytecode(const std::string_view name) const;
    void UnlockModule(const std::string_view name) const;
    void SpoofInstance(const std::string_view name, std::uintptr_t addr) const;
    std::vector<std::string> GetProperties(const std::string_view name) const;
    std::uintptr_t ObjValPtr(std::string_view name) const;
};

struct ClientInfo { const char* Version; const char* Username; int PID; };

std::mutex                                     g_clientsMtx;
std::vector<std::shared_ptr<RBXClient>>        g_clients;
std::unordered_map<std::string, nlohmann::json> g_globals;


// =============================================================================
// Process / window helpers
// =============================================================================
static std::vector<DWORD> PidsByName(const wchar_t* name) {
    std::vector<DWORD> out;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return out;
    PROCESSENTRY32W e{sizeof(e)};
    if (Process32FirstW(snap, &e)) do {
        if (_wcsicmp(name, e.szExeFile) == 0) out.push_back(e.th32ProcessID);
    } while (Process32NextW(snap, &e));
    CloseHandle(snap);
    return out;
}

static HWND HwndFromPid(DWORD pid) {
    HWND found = nullptr;
    struct P { DWORD pid; HWND* out; };
    P p{pid, &found};
    EnumWindows([](HWND hw, LPARAM lp) -> BOOL {
        DWORD wp; GetWindowThreadProcessId(hw, &wp);
        auto* pp = reinterpret_cast<P*>(lp);
        if (wp == pp->pid) { *pp->out = hw; return FALSE; }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&p));
    return found;
}

static std::string GenerateGUID() {
    GUID g; CoCreateGuid(&g);
    char buf[40];
    snprintf(buf, sizeof(buf), "%08lX-%04X-%04X-%04X-%012llX",
        g.Data1, g.Data2, g.Data3,
        (g.Data4[0]<<8)|g.Data4[1],
        (static_cast<uint64_t>(g.Data4[2])<<40)|(static_cast<uint64_t>(g.Data4[3])<<32)|
        (static_cast<uint64_t>(g.Data4[4])<<24)|(static_cast<uint64_t>(g.Data4[5])<<16)|
        (static_cast<uint64_t>(g.Data4[6])<<8 )| static_cast<uint64_t>(g.Data4[7]));
    return buf;
}

static std::uintptr_t GetRenderView(HANDLE h) {
    wchar_t tmp[MAX_PATH]; GetTempPathW(MAX_PATH, tmp);
    auto logs = std::filesystem::path(tmp).parent_path().parent_path()
                / OBF("Roblox") / OBF("logs");
    if (!std::filesystem::is_directory(logs)) return 0;

    // Pattern that Roblox logs when it initialises its render surface
    const std::regex rgx(R"(\bSurfaceController\[_:1\]::initialize view\((.*?)\))");

    for (int tries = 5; tries > 0; tries--) {
        std::vector<std::filesystem::path> locked;
        for (auto& e : std::filesystem::directory_iterator(logs)) {
            if (!e.is_regular_file() || e.path().extension() != OBF(".log")) continue;
            try { std::filesystem::remove(e.path()); }
            catch (...) { locked.push_back(e.path()); }
        }
        for (auto& lp : locked) {
            std::ifstream f(lp); if (!f.is_open()) continue;
            std::string txt((std::istreambuf_iterator<char>(f)), {});
            std::smatch m;
            if (std::regex_search(txt, m, rgx) && m.size() > 1) {
                auto addr = std::strtoull(m[1].str().c_str(), nullptr, 16);
                if (read_mem<std::uintptr_t>(addr + 0x118, h)) return addr;
            }
        }
        Sleep(1000);
    }
    return 0;
}


// =============================================================================
// RBXClient constructor — opens process and injects the client script
// =============================================================================
RBXClient::RBXClient(DWORD pid) : PID(pid) {
    _h = OpenProcess(PROCESS_ALL_ACCESS, TRUE, pid);
    if (!_h) return;

    // Resolve client path from image name
    {
        char img[MAX_PATH]; DWORD sz = MAX_PATH;
        QueryFullProcessImageNameA(_h, 0, img, &sz);
        ClientDir = std::filesystem::path(img).parent_path();
        Version   = ClientDir.filename().string();
    }
    GUID = GenerateGUID();

    // Wait until Roblox has loaded enough memory (~150 MB working set)
    {
        PROCESS_MEMORY_COUNTERS mc;
        while (K32GetProcessMemoryInfo(_h, &mc, sizeof(mc)) && mc.WorkingSetSize < 150'000'000)
            Sleep(100);
    }

    // Wait for main window
    HWND hwnd = nullptr;
    while (!(hwnd = HwndFromPid(pid))) Sleep(25);
    Sleep(1000);

    _rv = GetRenderView(_h);
    if (!_rv) return;

    std::uintptr_t dm = DataModel();
    if (!dm) return;
    Instance DM(dm, _h);

    // Wait for LocalPlayer to be assigned
    auto playersAddr = DM.FindClassAddr("Players");
    if (!playersAddr) return;
    std::uintptr_t lpAddr = 0;
    while (!(lpAddr = read_mem<std::uintptr_t>(playersAddr + off::LocalPlayer, _h))) Sleep(25);

    Instance LP(lpAddr, _h);
    while ((Username = LP.Name()) == "Player") Sleep(25);
    if (Username.empty()) Username = "Unknown";

    // Locate injection target: CoreGui > RobloxGui > Modules > Common > Url
    auto coreGui   = DM.Find("CoreGui");        if (!coreGui)   return;
    auto robloxGui = coreGui->Find("RobloxGui"); if (!robloxGui) return;
    auto modules   = robloxGui->Find("Modules"); if (!modules)   return;
    std::unique_ptr<Instance> patchScript;
    { auto common = modules->Find("Common"); if (!common) return; patchScript = common->Find("Url"); }
    if (!patchScript) return;

    // Load embedded client.lua from DLL resource (ID 254, type 255)
    std::string clientScript;
    {
        HMODULE self = nullptr;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                           reinterpret_cast<LPCSTR>(&GenerateGUID), &self);
        HRSRC   res  = FindResourceA(self, MAKEINTRESOURCEA(254), MAKEINTRESOURCEA(255));
        if (!res) return;
        HGLOBAL hg   = LoadResource(self, res);
        DWORD   size = SizeofResource(self, res);
        clientScript.assign(static_cast<char*>(LockResource(hg)), size);
    }

    auto sub = [](std::string& s, const std::string_view from, const std::string_view to) {
        auto p = s.find(from); if (p != std::string::npos) s.replace(p, from.size(), to);
    };
    sub(clientScript, "%XENO_UNIQUE_ID%", GUID);
    sub(clientScript, "%XENO_VERSION%",   std::string(VERSION));

    // Minimal URL-spoof patch compiled alongside the client
    const std::string patchSrc =
        "--!native\n--!optimize 1\n--!nonstrict\n"
        "local a={}local b=game:GetService(\"ContentProvider\")"
        "local function c(d)local e,f=d:find(\"%.\")local g=d:sub(f+1)"
        "if g:sub(-1)~=\"/\"then g=g..\"/\"end;return g end;"
        "local d=b.BaseUrl;local g=c(d)"
        "local h=string.format(\"https://games.%s\",g)"
        "local i=string.format(\"https://apis.rcs.%s\",g)"
        "local j=string.format(\"https://apis.%s\",g)"
        "local k=string.format(\"https://accountsettings.%s\",g)"
        "local l=string.format(\"https://gameinternationalization.%s\",g)"
        "local m=string.format(\"https://locale.%s\",g)"
        "local n=string.format(\"https://users.%s\",g)"
        "local o={GAME_URL=h,RCS_URL=i,APIS_URL=j,ACCOUNT_SETTINGS_URL=k,"
        "GAME_INTERNATIONALIZATION_URL=l,LOCALE_URL=m,ROLES_URL=n}"
        "setmetatable(a,{__newindex=function(p,q,r)end,__index=function(p,r)return o[r]end})return a";

    auto wrap = [](const std::string& s) {
        return "coroutine.wrap(function(...)" + s + "\nend)();";
    };

    // Handle lobby / teleport datamodel states
    if (DM.Name() == "LuaApp" || DM.Name() == "App") {
        patchScript->SetBytecode(Compile(wrap(clientScript) + patchSrc));
        return;
    }

    // Already injected check
    auto rrs = DM.FindClass("RobloxReplicatedStorage");
    if (rrs && rrs->Find("Xeno")) {
        patchScript->SetBytecode(Compile(wrap(clientScript) + patchSrc));
        return;
    }

    std::lock_guard<std::mutex> lk(g_clientsMtx);

    // Locate PlayerListManager (injection pivot)
    std::unique_ptr<Instance> plm;
    { auto pl = modules->Find("PlayerList"); if (!pl) return; plm = pl->Find("PlayerListManager"); }
    if (!plm) return;

    // Wait for the player to fully join a game
    if (!robloxGui->FindAddr("DropDownFullscreenFrame")) {
        robloxGui->WaitAddr("DropDownFullscreenFrame");
        if (DM.Name() == "App") {
            patchScript->SetBytecode(Compile(wrap(clientScript) + patchSrc));
            return;
        }
        Sleep(2500);
    }

    // Navigate to VRNavigation — the hollow module used as injection vehicle
    std::unique_ptr<Instance> vrNav;
    {
        auto sp  = DM.FindClass("StarterPlayer");             if (!sp)  return;
        auto sps = sp->Find("StarterPlayerScripts");          if (!sps) return;
        auto pm  = sps->Find("PlayerModule");                 if (!pm)  return;
        auto cm  = pm->Find("ControlModule");                 if (!cm)  return;
        vrNav = cm->Find("VRNavigation");
    }
    if (!vrNav) return;

    vrNav->UnlockModule();
    write_mem<std::uintptr_t>(plm->ptr() + off::Self, vrNav->ptr(), _h);
    vrNav->SetBytecode(Compile("script.Parent=nil;" + wrap(clientScript) + "while wait(9e9)do wait(9e9);end"), true);
    patchScript->SetBytecode(Compile(wrap(clientScript) + patchSrc));

    // Briefly foreground Roblox to send an Escape keypress that clears the UI lock
    HWND prev = GetForegroundWindow();
    for (int _fg = 0; GetForegroundWindow() != hwnd && _fg < 200; _fg++)
        { SetForegroundWindow(hwnd); Sleep(5); }
    keybd_event(VK_ESCAPE, MapVirtualKey(VK_ESCAPE, 0), KEYEVENTF_SCANCODE, 0);
    keybd_event(VK_ESCAPE, MapVirtualKey(VK_ESCAPE, 0), KEYEVENTF_SCANCODE|KEYEVENTF_KEYUP, 0);
    Sleep(100);
    if (prev) SetForegroundWindow(prev);
    Sleep(800);
    write_mem<std::uintptr_t>(plm->ptr() + off::Self, plm->ptr(), _h);

    if (Username == "Unknown") {
        auto la = read_mem<std::uintptr_t>(DM.FindClassAddr("Players") + off::LocalPlayer, _h);
        Username = Instance(la, _h).Name();
        if (Username.empty()) Username = "Unknown";
    }
}

// Helpers that route through ObjValPtr
std::uintptr_t RBXClient::ObjValPtr(std::string_view name) const {
    auto dm = DataModel(); if (!dm) return 0;
    Instance DM(dm, _h);
    auto rrs = DM.FindClass("RobloxReplicatedStorage"); if (!rrs) return 0;
    auto xf  = rrs->Find("Xeno");                       if (!xf)  return 0;
    auto ip  = xf->Find("Instance Pointers");            if (!ip)  return 0;
    auto ova = ip->FindAddr(name);                       if (!ova) return 0;
    return read_mem<std::uintptr_t>(ova + off::ObjValue, _h);
}

void RBXClient::execute(const std::string& src) const {
    auto dm = DataModel(); if (!dm) return;
    Instance DM(dm, _h);
    auto rrs = DM.FindClass("RobloxReplicatedStorage"); if (!rrs) return;
    auto xf  = rrs->Find("Xeno");                       if (!xf)  return;
    auto xs  = xf->Find("Scripts");                      if (!xs)  return;
    auto xm  = xs->FindClass("ModuleScript");            if (!xm)  return;
    xm->SetBytecode(Compile(
        "return{['x e n o']=function(...)do local function s(i,v)"
        "getfenv(debug.info(0,'f'))[i]=v;getfenv(debug.info(1,'f'))[i]=v;end;"
        "for i,v in pairs(getfenv(debug.info(1,'f')))do s(i,v)end;"
        "setmetatable(getgenv(),{__newindex=function(t,i,v)rawset(t,i,v)s(i,v)end})end;"
        + src + "\nend}"), true);
    xm->UnlockModule();
}

bool RBXClient::loadstring(const std::string& src, const std::string& sname, const std::string& cname) const {
    auto dm = DataModel(); if (!dm) return false;
    Instance DM(dm, _h);
    auto rrs = DM.FindClass("RobloxReplicatedStorage"); if (!rrs) return false;
    auto xf  = rrs->Find("Xeno");                       if (!xf)  return false;
    auto cm  = xf->Find(sname);                          if (!cm)  return false;
    cm->SetBytecode(Compile(
        "return{[[[" + cname + "]]]=function(...)do local function s(i,v)"
        "getfenv(debug.info(0,'f'))[i]=v;getfenv(debug.info(1,'f'))[i]=v;end;"
        "for i,v in pairs(getfenv(debug.info(1,'f')))do s(i,v)end;"
        "setmetatable(getgenv and getgenv()or{},{__newindex=function(t,i,v)rawset(t,i,v)s(i,v)end})end;"
        + src + "\nend}"), true);
    cm->UnlockModule();
    return true;
}

std::string RBXClient::GetBytecode(const std::string_view n) const {
    auto p = ObjValPtr(n); return p ? Instance(p,_h).GetBytecode() : "";
}
void RBXClient::UnlockModule(const std::string_view n) const {
    auto p = ObjValPtr(n); if (p) Instance(p,_h).UnlockModule();
}
void RBXClient::SpoofInstance(const std::string_view n, std::uintptr_t addr) const {
    auto p = ObjValPtr(n); if (p) write_mem<std::uintptr_t>(p + off::Self, addr, _h);
}
std::vector<std::string> RBXClient::GetProperties(const std::string_view n) const {
    auto p = ObjValPtr(n); return p ? Instance(p,_h).GetProperties() : std::vector<std::string>{};
}


// =============================================================================
// HTTP server — listens on a randomised port; port published via shared memory
// =============================================================================
#define CPPHTTPLIB_OPENSSL_SUPPORT
#include "deps/httplib.h"
#include "deps/json.hpp"
#include "deps/base64.hpp"

using json = nlohmann::json;
using namespace httplib;

// Innocuous shared-memory name — no product name in the binary
static constexpr char SHM_NAME[] = "Local\\__xp";
static int g_port = 0;

static bool InDir(const std::filesystem::path& base, const std::filesystem::path& rel) {
    auto canon_base = std::filesystem::weakly_canonical(base);
    auto canon_full = std::filesystem::weakly_canonical(canon_base / rel);
    auto [mb, mf]   = std::mismatch(canon_base.begin(), canon_base.end(), canon_full.begin(), canon_full.end());
    return mb == canon_base.end();
}

static void Serve(Response& res, const json& body) {
    const std::string c = body.value("c", "");

    if (c=="rf") {
        std::string p=body["p"];
        if (!InDir(std::filesystem::current_path(),p)||!std::filesystem::is_regular_file(p)){res.status=400;return;}
        std::ifstream f(p); std::stringstream ss; ss<<f.rdbuf();
        res.status=200; res.set_content(ss.str(),"text/plain"); return;
    }
    if (c=="lf") {
        std::string p=body["p"];
        if (!InDir(std::filesystem::current_path(),p)||!std::filesystem::is_directory(p)){res.status=400;return;}
        json r; for(auto& e:std::filesystem::directory_iterator(p)) r.push_back(e.path().string());
        res.status=200; res.set_content(r.dump(),"application/json"); return;
    }
    if (c=="if") {
        std::string p=body["p"];
        if (!InDir(std::filesystem::current_path(),p)){res.status=400;return;}
        res.status=200;
        if (std::filesystem::is_directory(p))    {res.set_content("dir","text/plain");return;}
        if (std::filesystem::is_regular_file(p)) {res.set_content("file","text/plain");return;}
        return;
    }
    if (c=="mf") {
        std::string p=body["p"];
        if (!InDir(std::filesystem::current_path(),p)){res.status=400;return;}
        try{res.status=std::filesystem::create_directory(p)?201:500;}catch(...){res.status=500;} return;
    }
    if (c=="dfl") {
        std::string p=body["p"];
        if (!InDir(std::filesystem::current_path(),p)||!std::filesystem::is_directory(p)){res.status=400;return;}
        try{std::filesystem::remove_all(p);res.status=200;}catch(...){res.status=500;} return;
    }
    if (c=="df") {
        std::string p=body["p"];
        if (!InDir(std::filesystem::current_path(),p)||!std::filesystem::is_regular_file(p)){res.status=400;return;}
        try{std::filesystem::remove(p);res.status=200;}catch(...){res.status=500;} return;
    }
    if (c=="cas") {
        std::string path=body["p"],pid=body["pid"];
        if (!InDir(std::filesystem::current_path(),path)){res.status=400;return;}
        auto src=std::filesystem::current_path()/path;
        if (!std::filesystem::is_regular_file(src)){res.status=400;return;}
        std::lock_guard<std::mutex> lk(g_clientsMtx);
        for (auto& cl:g_clients) if (std::to_string(cl->PID)==pid) {
            auto dst=cl->ClientDir/OBF("content")/src.filename();
            std::filesystem::copy_file(src,dst,std::filesystem::copy_options::overwrite_existing);
            res.status=200; res.set_content(std::string(OBF("rbxasset://"))+dst.filename().string(),"text/plain"); return;
        }
        res.status=400; return;
    }
    if (c=="rq") {
        const std::string url=body["l"],method=body["m"],rb=base64::from_base64(body["b"].get<std::string>());
        const json hj=body["h"];
        const std::regex urlR(R"(^(http[s]?://)?([^/]+)(/.*)?$)");
        std::smatch um; std::string host,path;
        if (std::regex_match(url,um,urlR)){host=um[2];path=um[3].str();}else{res.status=400;return;}
        Client cl(host); cl.set_follow_location(true);
        Headers hdrs; for(auto it=hj.begin();it!=hj.end();++it) hdrs.insert({it.key(),it.value()});
        Result pr;
        if      (method=="GET")    pr=cl.Get(path,hdrs);
        else if (method=="POST")   pr=cl.Post(path,hdrs,rb,"application/json");
        else if (method=="PUT")    pr=cl.Put(path,hdrs,rb,"application/json");
        else if (method=="DELETE") pr=cl.Delete(path,hdrs,rb,"application/json");
        else if (method=="PATCH")  pr=cl.Patch(path,hdrs,rb,"application/json");
        else{res.status=400;return;}
        if (pr){
            json rj; rj["c"]=pr->status;rj["r"]=pr->reason;rj["v"]=pr->version;
            json rh; for(auto& kv:pr->headers) rh[kv.first]=kv.second; rj["h"]=rh;
            auto ct=pr->get_header_value("Content-Type");
            if (ct.find("application/json")==std::string::npos&&ct.find("text/")==std::string::npos)
                {rj["b"]=base64::to_base64(pr->body);rj["b64"]=true;}
            else rj["b"]=pr->body;
            res.status=200;res.set_content(rj.dump(),"application/json");
        }else{res.status=200;res.set_content("x","text/plain");}
        return;
    }
    if (c=="qtp"){
        std::string pid=body["pid"],type=body["t"];
        std::lock_guard<std::mutex> lk(g_clientsMtx);
        for (auto& cl:g_clients) if (std::to_string(cl->PID)==pid){
            if (type=="g"){res.status=200;res.set_content(cl->TeleportQueue,"text/plain");return;}
            if (type=="s"&&body.contains("ct")){cl->TeleportQueue=body["ct"].get<std::string>();res.status=200;return;}
        }
        res.status=400;return;
    }
    if (c=="btc"){
        std::string pid=body["pid"],cn=body["cn"];
        std::lock_guard<std::mutex> lk(g_clientsMtx);
        for (auto& cl:g_clients) if(std::to_string(cl->PID)==pid){res.status=200;res.set_content(cl->GetBytecode(cn),"text/plain");return;}
        res.status=400;return;
    }
    if (c=="rc"){res.status=200;return;} // rconsole handled by UI
    if (c=="ax"){
        std::string content;
        auto ax=std::filesystem::current_path().parent_path()/OBF("autoexec");
        if(!std::filesystem::exists(ax))std::filesystem::create_directory(ax);
        for(auto& e:std::filesystem::directory_iterator(ax)){
            if(!e.is_regular_file())continue;
            std::ifstream f(e.path()); std::string s((std::istreambuf_iterator<char>(f)),{});
            if(CheckCompile(s)=="success") content+="task.spawn(function(...)"+s+"\nend)\n";
        }
        res.status=200;res.set_content(content,"text/plain");return;
    }
    if (c=="hw"){
        HW_PROFILE_INFO hw; std::string hwid;
        if(GetCurrentHwProfile(&hw)){std::wstring ws=hw.szHwProfileGuid;hwid.assign(ws.begin(),ws.end());}
        res.status=200;res.set_content(hwid,"text/plain");return;
    }
    if (c=="adr"){
        std::string pid=body["pid"],cn=body["cn"];
        std::lock_guard<std::mutex> lk(g_clientsMtx);
        for(auto& cl:g_clients) if(std::to_string(cl->PID)==pid){res.status=200;res.set_content(std::to_string(cl->ObjValPtr(cn)),"text/plain");return;}
        res.status=400;return;
    }
    if (c=="spf"){
        std::string pid=body["pid"],cn=body["cn"];
        auto addr=static_cast<std::uintptr_t>(std::stoull(body["adr"].get<std::string>()));
        std::lock_guard<std::mutex> lk(g_clientsMtx);
        for(auto& cl:g_clients) if(std::to_string(cl->PID)==pid){cl->SpoofInstance(cn,addr);res.status=200;return;}
        res.status=400;return;
    }
    if (c=="clt"){
        std::string guid=body["gd"],name=body.value("n","");
        std::lock_guard<std::mutex> lk(g_clientsMtx);
        for(auto& cl:g_clients) if(cl->GUID==guid||cl->Username==name){res.status=200;res.set_content(std::to_string(cl->PID),"text/plain");return;}
        res.status=400;return;
    }
    if (c=="gb"){
        std::string type=body["t"],name=body["n"];
        if(type=="s"){
            g_globals[name]={{"d",body["v"].get<std::string>()},{"t",body["vt"].get<std::string>()}};
            res.status=200;return;
        }
        if(type=="g"){
            auto it=g_globals.find(name);
            res.status=200;res.set_content(it!=g_globals.end()?it->second.dump():R"({"d":null,"t":null})","application/json");return;
        }
        res.status=400;return;
    }
    if (c=="um"){
        std::string pid=body["pid"],cn=body["cn"];
        std::lock_guard<std::mutex> lk(g_clientsMtx);
        for(auto& cl:g_clients) if(std::to_string(cl->PID)==pid){cl->UnlockModule(cn);res.status=200;return;}
        res.status=400;return;
    }
    if (c=="prp"){
        std::string pid=body["pid"],cn=body["cn"];
        std::lock_guard<std::mutex> lk(g_clientsMtx);
        for(auto& cl:g_clients) if(std::to_string(cl->PID)==pid){res.status=200;res.set_content(json(cl->GetProperties(cn)).dump(),"application/json");return;}
        res.status=400;return;
    }
    res.status=400;
}

static void RunServer() {
    Server svr;
    char exe[MAX_PATH]; GetModuleFileNameA(nullptr,exe,MAX_PATH);
    auto ws=std::filesystem::path(exe).parent_path()/OBF("workspace");
    if(!std::filesystem::exists(ws)) std::filesystem::create_directory(ws);
    else if(!std::filesystem::is_directory(ws)){std::filesystem::remove(ws);std::filesystem::create_directory(ws);}
    std::filesystem::current_path(ws);

    svr.Post("/send",[](const Request& req,Response& res){
        json body; try{body=json::parse(req.body);}catch(...){res.status=400;return;}
        if(!body.contains("c")){res.status=400;return;}
        Serve(res,body);
    });
    svr.Post("/writefile",[](const Request& req,Response& res){
        if(!req.has_param("p")||req.body.empty()){res.status=400;return;}
        std::string p=req.get_param_value("p");
        if(!InDir(std::filesystem::current_path(),p)||std::filesystem::is_directory(p)){res.status=400;return;}
        std::ofstream f(p,std::ios::trunc|std::ios::binary);
        if(!f){res.status=500;return;}
        f.write(req.body.c_str(),req.body.size());res.status=200;
    });
    svr.Post("/compilable",[](const Request& req,Response& res){
        if(req.body.empty()){res.status=400;return;}
        res.status=200;res.set_content(CheckCompile(req.body,req.has_param("btc")),"text/plain");
    });
    svr.Post("/loadstring",[](const Request& req,Response& res){
        if(!req.has_param("n")||!req.has_param("pid")||!req.has_param("cn")||req.body.empty()){res.status=400;return;}
        std::string pid=req.get_param_value("pid"),sn=req.get_param_value("n"),cn=req.get_param_value("cn");
        std::lock_guard<std::mutex> lk(g_clientsMtx);
        for(auto& cl:g_clients) if(std::to_string(cl->PID)==pid){cl->loadstring(req.body,sn,cn);res.status=200;return;}
        res.status=400;
    });
    svr.Post("/setclipboard",[](const Request& req,Response& res){
        if(req.body.empty()){res.status=400;return;}
        if(!OpenClipboard(nullptr)){res.status=500;return;}
        EmptyClipboard();
        HGLOBAL hm=GlobalAlloc(GMEM_MOVEABLE,req.body.size()+1);
        auto* pm=static_cast<char*>(GlobalLock(hm));
        if(pm){std::copy(req.body.begin(),req.body.end(),pm);pm[req.body.size()]='\0';GlobalUnlock(hm);}
        SetClipboardData(CF_TEXT,hm);CloseClipboard();res.status=200;
    });
    svr.set_exception_handler([](const Request&,Response& res,std::exception_ptr ep){
        try{std::rethrow_exception(ep);}catch(std::exception& e){res.set_content(std::string(R"({"error":")")+e.what()+"\"}");}
        catch(...){res.set_content(R"({"error":"unknown"})");}
        res.status=500;
    });
    svr.listen(OBF("127.0.0.1"),g_port);
}


// =============================================================================
// Client watcher — condition_variable instead of Sleep() polling
// =============================================================================
static std::unordered_set<DWORD> g_closed, g_initing;
static std::condition_variable   g_cv;
static std::mutex                g_cvMtx;

static void AttachClient(DWORD pid) {
    {
        std::lock_guard<std::mutex> lk(g_clientsMtx);
        if (g_closed.count(pid)) return;
        g_initing.insert(pid);
    }
    auto cl = std::make_shared<RBXClient>(pid);
    {
        std::lock_guard<std::mutex> lk(g_clientsMtx);
        g_clients.push_back(std::move(cl));
        g_initing.erase(pid);
    }
}

static void WatchClients() {
    while (true) {
        auto p1 = PidsByName(WOBF(L"RobloxPlayerBeta.exe"));
        auto p2 = PidsByName(WOBF(L"eurotrucks2.exe"));
        p1.insert(p1.end(), p2.begin(), p2.end());
        std::unordered_set<DWORD> live(p1.begin(), p1.end());
        {
            std::lock_guard<std::mutex> lk(g_clientsMtx);
            g_clients.erase(std::remove_if(g_clients.begin(), g_clients.end(),
                [&](const std::shared_ptr<RBXClient>& cl){
                    if (!live.count(cl->PID)||!cl->alive()){g_closed.insert(cl->PID);return true;}
                    return false;
                }), g_clients.end());
            for (DWORD pid : live) {
                bool known = std::any_of(g_clients.begin(),g_clients.end(),
                    [pid](const std::shared_ptr<RBXClient>& c){return c->PID==pid;});
                if (!known && !g_initing.count(pid))
                    std::thread(AttachClient,pid).detach();
            }
        }
        std::unique_lock<std::mutex> lk(g_cvMtx);
        g_cv.wait_for(lk, std::chrono::milliseconds(250));
    }
}


// =============================================================================
// DLL exports
// =============================================================================
extern "C" {

__declspec(dllexport) void Initialize() {
    LoadNt();

    // Kill duplicate host processes
    DWORD self=GetCurrentProcessId();
    wchar_t img[MAX_PATH]; GetModuleFileNameW(nullptr,img,MAX_PATH);
    const wchar_t* exe=wcsrchr(img,L'\\')+1;
    for (DWORD pid:PidsByName(exe)) {
        if (pid==self) continue;
        HANDLE h=OpenProcess(PROCESS_TERMINATE,FALSE,pid);
        if(h){TerminateProcess(h,0);CloseHandle(h);}
    }

    // Pick a random unprivileged port and advertise it via shared memory
    unsigned int rval = 0;
    rand_s(&rval);
    g_port = 49152 + static_cast<int>(rval % 16383);
    HANDLE hMap=CreateFileMappingA(INVALID_HANDLE_VALUE,nullptr,PAGE_READWRITE,0,4,SHM_NAME);
    if (hMap) {
        auto* v=static_cast<int*>(MapViewOfFile(hMap,FILE_MAP_ALL_ACCESS,0,0,4));
        if(v){*v=g_port;UnmapViewOfFile(v);}
        // hMap kept open for lifetime of the DLL so the mapping stays alive
    }

    std::thread(WatchClients).detach();
    std::thread(RunServer).detach();
}

__declspec(dllexport) ClientInfo* GetClients() {
    static std::vector<ClientInfo> snap;
    std::lock_guard<std::mutex> lk(g_clientsMtx);
    snap.clear();
    for (auto& cl:g_clients)
        snap.push_back({cl->Version.c_str(),cl->Username.c_str(),static_cast<int>(cl->PID)});
    snap.push_back({nullptr,nullptr,0});
    return snap.data();
}

__declspec(dllexport) void Execute(const char* src, const char** users, int n) {
    std::string s(src);
    std::lock_guard<std::mutex> lk(g_clientsMtx);
    for (int i=0;i<n;i++)
        for (auto& cl:g_clients)
            if(cl->Username==users[i]){cl->execute(s);break;}
}

__declspec(dllexport) const char* Compilable(const char* src) {
    std::string r=CheckCompile(src);
    // Allocated with CoTaskMemAlloc so the C# caller can free with Marshal.FreeCoTaskMem
    char* out=static_cast<char*>(CoTaskMemAlloc(r.size()+1));
    if(out) strcpy_s(out,r.size()+1,r.c_str());
    return out;
}

} // extern "C"
