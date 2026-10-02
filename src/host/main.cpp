#include <cstdint>
#if UINTPTR_MAX == 0xFFFFFFFFu  // 32-bit x86 only
// kt_host entry: maps the XBE over the loader image at 0x10000, applies the
// manifest patches and jumps to the title's entry point.
#include <cstddef>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <vector>

#include "host.h"
#include "manifest.h"

#include <shellapi.h>

namespace {

constexpr uint32_t kXbeBase = 0x10000;
constexpr uint32_t kEntryKeyRetail = 0xA8FC57AB;
constexpr uint32_t kEntryKeyDebug = 0x94859D4B;
constexpr uint32_t kThunkKeyRetail = 0x5B6D40B6;
constexpr uint32_t kThunkKeyDebug = 0xEFB1F152;

#pragma pack(push, 1)
struct XbeHeader {
    uint32_t magic;
    uint8_t signature[256];
    uint32_t base, headers_size, image_size, image_header_size, timestamp, cert_addr;
    uint32_t num_sections, section_headers_addr, init_flags, entry, tls_addr;
    uint32_t stack_size, heap_reserve, heap_commit, pe_base, pe_size, pe_checksum, pe_timestamp;
    uint32_t debug_path_addr, debug_file_addr, debug_ufile_addr, kernel_thunk;
};
struct XbeSection {
    uint32_t flags, va, vsize, raw, rsize, name_addr, ref_count;
    uint32_t head_ref_addr, tail_ref_addr;
    uint8_t digest[20];
};
#pragma pack(pop)

std::vector<uint8_t> ReadFileBytes(const std::wstring& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) Fatal("cannot open %ls", path.c_str());
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(f), {});
}

// Minimal PE header kept alive inside the XBE header. It lives in the XBE's
// 256-byte RSA signature (0x004-0x103), which only the boot ROM/kernel checks;
// the only visible change to the title is 'XB' -> 'MZ' in the magic.
constexpr uint32_t kPeStubOffset = 0x40;
constexpr uint32_t kPeStubSize = 4 + sizeof(IMAGE_FILE_HEADER) + offsetof(IMAGE_OPTIONAL_HEADER32, DataDirectory);
static_assert(kPeStubOffset + kPeStubSize <= 0x104, "PE stub must fit inside the XBE signature");

struct PeStub {
    uint8_t bytes[kPeStubSize];
};

PeStub SavePeStub(const uint8_t* image) {
    auto* nth = reinterpret_cast<const IMAGE_NT_HEADERS32*>(image + reinterpret_cast<const IMAGE_DOS_HEADER*>(image)->e_lfanew);
    PeStub s;
    memcpy(s.bytes, nth, kPeStubSize);
    auto* n = reinterpret_cast<IMAGE_NT_HEADERS32*>(s.bytes);
    n->FileHeader.NumberOfSections = 0;
    n->FileHeader.SizeOfOptionalHeader = offsetof(IMAGE_OPTIONAL_HEADER32, DataDirectory);
    n->OptionalHeader.NumberOfRvaAndSizes = 0;
    return s;
}

void GraftPeStub(uint8_t* image, const PeStub& s) {
    image[0] = 'M';
    image[1] = 'Z';
    uint32_t lfanew = kPeStubOffset;
    memcpy(image + offsetof(IMAGE_DOS_HEADER, e_lfanew), &lfanew, 4);
    memcpy(image + kPeStubOffset, s.bytes, kPeStubSize);
}

// The loader exe's image spans [0x10000, 0x10000 + reserve), so the range is
// ours; make it writable and replace its contents with the XBE.
void MapXbe(const std::vector<uint8_t>& xbe, uint32_t* entry, uint32_t* thunk) {
    auto* h = reinterpret_cast<const XbeHeader*>(xbe.data());
    if (h->magic != 'HEBX') Fatal("not an XBE (bad magic)");
    if (h->base != kXbeBase) Fatal("unsupported XBE base 0x%08x", h->base);

    MEMORY_BASIC_INFORMATION mbi;
    VirtualQuery(reinterpret_cast<void*>(kXbeBase), &mbi, sizeof mbi);
    auto* alloc_base = static_cast<uint8_t*>(mbi.AllocationBase);
    size_t span = 0;
    for (uint8_t* p = alloc_base; VirtualQuery(p, &mbi, sizeof mbi) && mbi.AllocationBase == alloc_base;
         p += mbi.RegionSize)
        span += mbi.RegionSize;
    if (alloc_base != reinterpret_cast<uint8_t*>(kXbeBase) || span < h->image_size)
        Fatal("address range 0x%08x+0x%x is not reserved by the loader (base %p, span 0x%zx)", kXbeBase,
              h->image_size, alloc_base, span);

    // Windows keeps reading the loader's PE header after we overwrite the
    // image (the kernel takes default thread stack sizes from it), so save
    // the essentials and graft a minimal PE header into the XBE header below.
    PeStub stub = SavePeStub(alloc_base);

    DWORD old;
    if (!VirtualProtect(alloc_base, span, PAGE_EXECUTE_READWRITE, &old))
        Fatal("VirtualProtect on XBE range failed: %lu", GetLastError());
    memset(alloc_base, 0, span);
    memcpy(alloc_base, xbe.data(), h->headers_size);
    GraftPeStub(alloc_base, stub);

    auto* secs = reinterpret_cast<const XbeSection*>(xbe.data() + h->section_headers_addr - kXbeBase);
    for (uint32_t i = 0; i < h->num_sections; i++) {
        const XbeSection& s = secs[i];
        const char* name = reinterpret_cast<const char*>(xbe.data() + s.name_addr - kXbeBase);
        if (s.raw + s.rsize > xbe.size()) Fatal("section %s exceeds file", name);
        memcpy(reinterpret_cast<void*>(s.va), xbe.data() + s.raw, s.rsize);
        Log("  section %-10s va=%08x vsize=%08x raw=%08x", name, s.va, s.vsize, s.rsize);
    }

    // Retail and debug XBEs scramble these with different keys; pick the one
    // that lands inside the image.
    auto in_image = [&](uint32_t va) { return va >= kXbeBase && va < kXbeBase + h->image_size; };
    *entry = h->entry ^ kEntryKeyRetail;
    *thunk = h->kernel_thunk ^ kThunkKeyRetail;
    if (!in_image(*entry)) {
        *entry = h->entry ^ kEntryKeyDebug;
        *thunk = h->kernel_thunk ^ kThunkKeyDebug;
    }
    if (!in_image(*entry) || !in_image(*thunk)) Fatal("cannot decode entry point / kernel thunk");
}

std::vector<std::wstring> Args() {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<std::wstring> out(argv, argv + argc);
    LocalFree(argv);
    return out;
}

}  // namespace

uint8_t* ExecAlloc(size_t n) {
    static uint8_t* pool = nullptr;
    static size_t used = 0, cap = 0;
    n = (n + 15) & ~size_t(15);
    if (used + n > cap) {
        cap = 1 << 20;
        pool = static_cast<uint8_t*>(VirtualAlloc(nullptr, cap, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
        used = 0;
        if (!pool) Fatal("ExecAlloc failed");
    }
    uint8_t* p = pool + used;
    used += n;
    return p;
}

// usage: kt_loader.exe <game dir | xbe file> <manifest.txt> [hdd dir]
// With a directory the title is its default.xbe; with an .xbe file, the
// file's directory is the game (D:) root.
extern "C" __declspec(dllexport) void __stdcall KtMain() {
    auto args = Args();
    if (args.size() < 3) {
        fprintf(stderr, "usage: kt_loader <game dir containing default.xbe | xbe file> <manifest.txt> [hdd dir]\n");
        ExitProcess(2);
    }
    std::wstring game = args[1], manifest_path = args[2], xbe_path = game + L"\\default.xbe";
    DWORD attr = GetFileAttributesW(game.c_str());
    if (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
        xbe_path = game;
        game = game.substr(0, game.find_last_of(L"\\/"));
    }
    std::wstring hdd = args.size() > 3 ? args[3] : (HostDir() / "hdd").wstring();
    LogInit(HostDir() / "kt_host.log");
    Log("kt_host: game=%ls manifest=%ls hdd=%ls", game.c_str(), manifest_path.c_str(), hdd.c_str());

    Manifest manifest = LoadManifest(manifest_path);
    auto xbe = ReadFileBytes(xbe_path);
    uint32_t entry, thunk;
    MapXbe(xbe, &entry, &thunk);
    Log("entry=%08x kernel_thunk=%08x", entry, thunk);

    IoSetGameRoot(game);
    IoSetHddRoot(hdd);
    CrashHandlerInstall();
    KpcrInit();
    ApplyFsPatches(manifest);
    KernelInstallThunks(thunk);
    HleInstall(manifest);
    LiftInstall(manifest);
    KernelStartTimers();
    FlushInstructionCache(GetCurrentProcess(), nullptr, 0);

    // The Xbox kernel runs the entry point on a system thread with TLS. Size
    // it the way XAPI does: aligned template + zero fill, plus the array slot.
    auto* xh = reinterpret_cast<const XbeHeader*>(xbe.data());
    uint32_t tls_size = 4, tls_raw = 0, tls_start = 0;
    if (xh->tls_addr) {
        auto* tls_dir = reinterpret_cast<const uint32_t*>(uintptr_t(xh->tls_addr));
        tls_start = tls_dir[0];
        tls_raw = tls_dir[1] - tls_dir[0];
        tls_size = ((tls_raw + tls_dir[4] + 15) & ~15u) + 4;
    }
    XThreadSetTitleTlsSize(tls_size);
    XThread* t = XThreadCreate(tls_size);
    if (tls_raw) memcpy(G2H<uint8_t>(t->tls_data) + 4, reinterpret_cast<void*>(uintptr_t(tls_start)), tls_raw);
    XThreadBindCurrent(t);

    Log("jumping to entry point %08x", entry);
    reinterpret_cast<void(__cdecl*)()>(entry)();
    Log("entry point returned; waiting for title threads");
    for (;;) Sleep(INFINITE);
}

#endif  // 32-bit x86
