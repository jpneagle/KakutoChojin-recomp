// xboxkrnl file I/O and object-namespace exports.
//
// Xbox object names are ANSI ("\??\D:\foo", "\Device\CdRom0\foo"). They are
// resolved through the title's symbolic links to a device and mapped onto a
// host directory: \Device\CdRom0 is the game folder, \Device\Harddisk0\PartitionN
// is hdd/PartitionN (raw volume access goes to hdd/PartitionN.bin). Files are
// host files behind kernel object handles (ob.h, hostfs.h); all transfers
// complete before the call returns.
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <map>
#include <mutex>
#include <vector>

#include "hostfs.h"
#include "kernel.h"
#include "ob.h"

namespace fs = std::filesystem;

namespace {

fs::path g_game_root, g_hdd_root;
std::mutex g_mu;
std::map<std::string, std::string> g_symlinks;  // lower-case link -> device path

struct FileObject : ob::Object {
    FileObject() : Object(ob::Type::File) {}
    std::unique_ptr<hostfs::File> file;
    int raw_partition = -1;  // raw volume handle: partition number
    bool dvd_device = false;  // \Device\CdRom0 itself (SCSI requests)
    std::string xbox_name;
};

struct LinkObject : ob::Object {
    LinkObject() : Object(ob::Type::SymbolicLink) {}
    std::string target;
};

// Retail disk layout (bytes). 0 is the whole disk.
constexpr LONGLONG kPartitionSize[8] = {0x1DD156000, 0x1312D6000, 0x1F400000, 0x2EE00000,
                                        0x2EE00000, 0x2EE00000, 0, 0};

const bool g_trace_io = getenv("KT_TRACE") != nullptr;

std::string Lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(tolower(c)); });
    return s;
}

std::string AnsiOf(const XANSI_STRING* s) { return s && s->Buffer ? std::string(s->Buffer.get(), s->Length) : ""; }
std::string AnsiOf(GPtr<XANSI_STRING> s) { return AnsiOf(s.get()); }

bool StartsWithI(const std::string& s, const std::string& prefix) {
    return s.size() >= prefix.size() && Lower(s.substr(0, prefix.size())) == Lower(prefix);
}

// Xbox relative path ("\dir\file", CP932/ASCII) -> host path below `root`.
fs::path Below(const fs::path& root, const std::string& rel) {
    fs::path p = root;
    size_t i = 0;
    while (i < rel.size()) {
        size_t j = rel.find('\\', i);
        if (j == std::string::npos) j = rel.size();
        if (j > i) {
            std::string part = rel.substr(i, j - i);
#if KT_WIN32_API
            std::wstring w(part.size(), L'\0');
            int n = MultiByteToWideChar(932, 0, part.data(), int(part.size()), w.data(), int(w.size()));
            w.resize(n > 0 ? n : 0);
            p /= w;
#else
            p /= part;
#endif
        }
        i = j + 1;
    }
    return p;
}

std::string HostName(const fs::path& p) {
#if KT_WIN32_API
    std::wstring w = p.filename().wstring();
    char buf[1024];
    int n = WideCharToMultiByte(932, 0, w.data(), int(w.size()), buf, sizeof buf, nullptr, nullptr);
    return std::string(buf, n > 0 ? n : 0);
#else
    return p.filename().string();
#endif
}

// Xbox device path -> host path (empty if the device is unknown).
fs::path DeviceToHost(const std::string& path, int* raw_partition) {
    static const std::string kCdRom = "\\Device\\CdRom0", kHdd = "\\Device\\Harddisk0\\Partition";
    *raw_partition = -1;
    if (StartsWithI(path, kCdRom)) return Below(g_game_root, path.substr(kCdRom.size()));
    if (StartsWithI(path, kHdd)) {
        std::string rest = path.substr(kHdd.size());
        if (rest.size() == 1 && rest[0] >= '0' && rest[0] <= '7') {
            *raw_partition = rest[0] - '0';
            return g_hdd_root / ("Partition" + rest + ".bin");
        }
        size_t sep = rest.find('\\');
        std::string number = rest.substr(0, sep);
        return Below(g_hdd_root / ("Partition" + number), sep == std::string::npos ? "" : rest.substr(sep));
    }
    return {};
}

// Resolve "\??\X:\rest" through the symlink table (links may chain).
std::string ResolveLinks(std::string path) {
    std::lock_guard<std::mutex> lk(g_mu);
    for (int depth = 0; depth < 8 && StartsWithI(path, "\\??\\"); depth++) {
        size_t end = path.find('\\', 4);
        auto it = g_symlinks.find(Lower(path.substr(0, end)));
        if (it == g_symlinks.end()) break;
        path = it->second + (end == std::string::npos ? "" : path.substr(end));
    }
    return path;
}

// OBJECT_ATTRIBUTES -> host path.
NTSTATUS HostPath(const XOBJECT_ATTRIBUTES* x, fs::path* out, int* raw_partition, std::string* xbox_name,
                  std::string* device = nullptr) {
    *xbox_name = AnsiOf(x->ObjectName);
    *raw_partition = -1;
    std::string name = *xbox_name;
    GHandle root = x->RootDirectory;
    // XAPI passes DOS paths ("D:\foo") relative to ObDosDevicesDirectory().
    if (root == ob::kDosDevices) root = 0, name = "\\??\\" + name;
    if (root) {
        auto dir = ob::GetAs<FileObject>(root, ob::Type::File);
        if (!dir || !dir->file) return STATUS_INVALID_HANDLE_X;
        *out = Below(dir->file->path(), name);
        return STATUS_SUCCESS;
    }
    // The Xbox file system tolerates doubled separators ("dir\\file").
    std::string clean;
    for (char c : name)
        if (!(c == '\\' && clean.size() > 1 && clean.back() == '\\')) clean += c;
    std::string dev = ResolveLinks(clean);
    if (device) *device = dev;
    *out = DeviceToHost(dev, raw_partition);
    if (out->empty()) {
        Log("io: cannot map Xbox path '%s' (resolved '%s')", xbox_name->c_str(), dev.c_str());
        return STATUS_OBJECT_NAME_NOT_FOUND_X;
    }
    return STATUS_SUCCESS;
}

std::shared_ptr<FileObject> FileOf(GHandle h) {
    auto f = ob::GetAs<FileObject>(h, ob::Type::File);
    return f && f->file ? f : nullptr;
}

void SetIosb(XIO_STATUS_BLOCK* iosb, NTSTATUS st, ULONG info) {
    if (iosb) iosb->Status = st, iosb->Information = info;
}

// ---- Symbolic links --------------------------------------------------------------

NTSTATUS NTAPI x_IoCreateSymbolicLink(const XANSI_STRING* link, const XANSI_STRING* device) {
    std::lock_guard<std::mutex> lk(g_mu);
    g_symlinks[Lower(AnsiOf(link))] = AnsiOf(device);
    Log("IoCreateSymbolicLink: %s -> %s", AnsiOf(link).c_str(), AnsiOf(device).c_str());
    return STATUS_SUCCESS;
}

NTSTATUS NTAPI x_NtOpenSymbolicLinkObject(GHandle* h, const XOBJECT_ATTRIBUTES* oa) {
    std::lock_guard<std::mutex> lk(g_mu);
    auto it = g_symlinks.find(Lower(AnsiOf(oa->ObjectName)));
    if (it == g_symlinks.end()) return STATUS_OBJECT_NAME_NOT_FOUND_X;
    auto link = std::make_shared<LinkObject>();
    link->target = it->second;
    *h = ob::Insert(link);
    return STATUS_SUCCESS;
}

NTSTATUS NTAPI x_NtQuerySymbolicLinkObject(GHandle h, XANSI_STRING* target, PULONG returned) {
    auto link = ob::GetAs<LinkObject>(h, ob::Type::SymbolicLink);
    if (!link) return STATUS_INVALID_HANDLE_X;
    const std::string& s = link->target;
    if (returned) *returned = ULONG(s.size());
    if (target->MaximumLength < s.size()) return STATUS_BUFFER_TOO_SMALL_X;
    memcpy(target->Buffer.get(), s.data(), s.size());
    target->Length = USHORT(s.size());
    return STATUS_SUCCESS;
}

NTSTATUS NTAPI x_NtClose(GHandle h) { return ob::Close(h) ? STATUS_SUCCESS : STATUS_INVALID_HANDLE_X; }

// ---- Files ---------------------------------------------------------------------------

// XAPI formats a cache partition by writing a FATX superblock to its raw
// volume; mirror that by emptying the directory backing the partition.
void OnRawWrite(int partition, const void* buf, ULONG len, const uint64_t* offset) {
    if (partition < 1 || !offset || *offset != 0 || len < 4 || memcmp(buf, "FATX", 4) != 0) return;
    fs::path dir = g_hdd_root / ("Partition" + std::to_string(partition));
    std::error_code ec;
    for (auto& e : fs::directory_iterator(dir, ec)) fs::remove_all(e.path(), ec);
    Log("io: Partition%d formatted (FATX superblock written); host directory cleared", partition);
}

constexpr ACCESS_MASK kWriteAccess = 0x2 /* FILE_WRITE_DATA */ | 0x4 /* FILE_APPEND_DATA */ | 0x40000000 /* GENERIC_WRITE */ |
                                     0x10000000 /* GENERIC_ALL */ | 0x00010000 /* DELETE */;

NTSTATUS OpenFile(GHandle* gh, ACCESS_MASK access, const XOBJECT_ATTRIBUTES* xoa, XIO_STATUS_BLOCK* iosb,
                  ULONG disposition, ULONG options, const char* what) {
    *gh = 0;
    fs::path path;
    auto obj = std::make_shared<FileObject>();
    std::string device;
    NTSTATUS st = HostPath(xoa, &path, &obj->raw_partition, &obj->xbox_name, &device);
    obj->dvd_device = Lower(device) == "\\device\\cdrom0";
    uint32_t result = 0;
    if (st == STATUS_SUCCESS) {
        hostfs::NTSTATUS fst = 0;
        obj->file = hostfs::File::Open(path, (access & kWriteAccess) != 0, disposition, options, &fst, &result);
        st = fst;
        if (obj->file) *gh = ob::Insert(obj);
    }
    SetIosb(iosb, st, result);
    Log("%s('%s', access=%08lx, disp=%lu, opt=%08lx) -> %08lx", what, obj->xbox_name.c_str(), access, disposition,
        options, st);
    return st;
}

NTSTATUS NTAPI x_NtCreateFile(GHandle* gh, ACCESS_MASK access, const XOBJECT_ATTRIBUTES* xoa,
                              XIO_STATUS_BLOCK* iosb, PLARGE_INTEGER, ULONG, ULONG, ULONG disposition, ULONG options) {
    return OpenFile(gh, access, xoa, iosb, disposition, options, "NtCreateFile");
}

NTSTATUS NTAPI x_NtOpenFile(GHandle* gh, ACCESS_MASK access, const XOBJECT_ATTRIBUTES* xoa, XIO_STATUS_BLOCK* iosb,
                            ULONG, ULONG options) {
    return OpenFile(gh, access, xoa, iosb, hostfs::kOpen, options, "NtOpenFile");
}

// The title's event is set and its APC (XAPI's ReadFileEx / WriteFileEx
// completion) runs right away, as if the transfer had finished at once.
void CompleteIo(GHandle ev, uint32_t apc, uint32_t apc_ctx, XIO_STATUS_BLOCK* iosb) {
    if (ev) ob::SetEventHandle(ev);
    if (apc) CallGuest(apc, {apc_ctx, H2G(iosb), 0});
}

// ByteOffset: null or FILE_USE_FILE_POINTER_POSITION (-2) means the current position.
const uint64_t* OffsetOf(PLARGE_INTEGER offset, uint64_t* storage) {
    if (!offset || offset->QuadPart == -2) return nullptr;
    *storage = uint64_t(offset->QuadPart);
    return storage;
}

NTSTATUS NTAPI x_NtReadFile(GHandle gh, GHandle ev, uint32_t apc, uint32_t apc_ctx, XIO_STATUS_BLOCK* iosb, void* buf,
                            ULONG len, PLARGE_INTEGER offset) {
    auto f = FileOf(gh);
    if (!f) return STATUS_INVALID_HANDLE_X;
    uint64_t at;
    uint32_t done = 0;
    NTSTATUS st = f->file->Read(buf, len, OffsetOf(offset, &at), &done);
    SetIosb(iosb, st, done);
    if (st >= 0) CompleteIo(ev, apc, apc_ctx, iosb);
    if (g_trace_io || (st < 0 && st != hostfs::kEndOfFile))
        Log("NtReadFile(%08x, len=%lx, off=%llx) -> %08lx", gh, len, offset ? offset->QuadPart : -1LL, st);
    return st;
}

NTSTATUS NTAPI x_NtWriteFile(GHandle gh, GHandle ev, uint32_t apc, uint32_t apc_ctx, XIO_STATUS_BLOCK* iosb, void* buf,
                             ULONG len, PLARGE_INTEGER offset) {
    auto f = FileOf(gh);
    if (!f) return STATUS_INVALID_HANDLE_X;
    uint64_t at;
    const uint64_t* off = OffsetOf(offset, &at);
    if (f->raw_partition >= 0) OnRawWrite(f->raw_partition, buf, len, off);
    uint32_t done = 0;
    NTSTATUS st = f->file->Write(buf, len, off, &done);
    SetIosb(iosb, st, done);
    if (st >= 0) CompleteIo(ev, apc, apc_ctx, iosb);
    if (g_trace_io || st < 0)
        Log("NtWriteFile(%08x, len=%lx, off=%llx) -> %08lx", gh, len, offset ? offset->QuadPart : -1LL, st);
    return st;
}

// Little-endian field writer for information buffers.
struct Out {
    uint8_t* p;
    ULONG len;
    template <typename T>
    void Put(ULONG off, T v) {
        if (off + sizeof v <= len) memcpy(p + off, &v, sizeof v);
    }
};

void PutTimes(Out& o, ULONG off, const hostfs::Info& i) {
    o.Put<uint64_t>(off, i.creation), o.Put<uint64_t>(off + 8, i.access);
    o.Put<uint64_t>(off + 16, i.write), o.Put<uint64_t>(off + 24, i.change);
}

// FILE_NETWORK_OPEN_INFORMATION
void PutNetworkOpen(Out& o, const hostfs::Info& i) {
    PutTimes(o, 0, i);
    o.Put<uint64_t>(32, i.allocation), o.Put<uint64_t>(40, i.size), o.Put<uint32_t>(48, i.attributes);
}

NTSTATUS NTAPI x_NtQueryInformationFile(GHandle h, XIO_STATUS_BLOCK* iosb, void* info, ULONG len, ULONG cls) {
    auto f = FileOf(h);
    if (!f) return STATUS_INVALID_HANDLE_X;
    hostfs::Info i;
    NTSTATUS st = f->file->Query(&i);
    if (st < 0) return SetIosb(iosb, st, 0), st;
    memset(info, 0, len);
    Out o{static_cast<uint8_t*>(info), len};
    ULONG size;
    switch (cls) {
        case 4:  // FileBasicInformation
            PutTimes(o, 0, i), o.Put<uint32_t>(32, i.attributes), size = 40;
            break;
        case 5:  // FileStandardInformation
            o.Put<uint64_t>(0, i.allocation), o.Put<uint64_t>(8, i.size), o.Put<uint32_t>(16, 1);
            o.Put<uint8_t>(20, f->file->delete_on_close), o.Put<uint8_t>(21, i.directory), size = 24;
            break;
        case 6:  // FileInternalInformation
            o.Put<uint64_t>(0, std::hash<std::string>()(f->file->path().string())), size = 8;
            break;
        case 7: case 8: case 16: case 17:  // EA size, access, mode, alignment: 0
            size = 4;
            break;
        case 14:  // FilePositionInformation
            o.Put<uint64_t>(0, f->file->position), size = 8;
            break;
        case 34:  // FileNetworkOpenInformation
            PutNetworkOpen(o, i), size = 56;
            break;
        default:
            Log("NtQueryInformationFile(%08x, class %lu): not supported", h, cls);
            return STATUS_INVALID_PARAMETER_X;
    }
    if (len < size) return STATUS_BUFFER_TOO_SMALL_X;
    SetIosb(iosb, STATUS_SUCCESS, size);
    return STATUS_SUCCESS;
}

NTSTATUS NTAPI x_NtSetInformationFile(GHandle h, XIO_STATUS_BLOCK* iosb, void* info, ULONG len, ULONG cls) {
    auto f = FileOf(h);
    if (!f) return STATUS_INVALID_HANDLE_X;
    auto* p = static_cast<const uint8_t*>(info);
    auto u64 = [&](ULONG off) {
        uint64_t v = 0;
        if (off + 8 <= len) memcpy(&v, p + off, 8);
        return v;
    };
    NTSTATUS st = STATUS_SUCCESS;
    switch (cls) {
        case 4:   // FileBasicInformation: times and attributes are not kept
        case 19:  // FileAllocationInformation
            break;
        case 10: {  // FileRenameInformation: ReplaceIfExists, RootDirectory, ANSI FileName
            XOBJECT_ATTRIBUTES oa{};
            memcpy(&oa.RootDirectory, p + 4, 4);
            oa.ObjectName = GPtr<XANSI_STRING>{H2G(p + 8)};
            fs::path to;
            int raw;
            std::string name;
            st = HostPath(&oa, &to, &raw, &name);
            if (st == STATUS_SUCCESS) st = f->file->Rename(to, p[0] != 0);
            Log("NtSetInformationFile: rename '%s' -> '%s' -> %08lx", f->xbox_name.c_str(), name.c_str(), st);
            break;
        }
        case 13:  // FileDispositionInformation
            f->file->delete_on_close = p[0] != 0;
            break;
        case 14:  // FilePositionInformation
            f->file->position = u64(0);
            break;
        case 20:  // FileEndOfFileInformation
            st = f->file->SetSize(u64(0));
            break;
        default:
            Log("NtSetInformationFile(%08x, class %lu): not supported", h, cls);
            return STATUS_INVALID_PARAMETER_X;
    }
    SetIosb(iosb, st, 0);
    return st;
}

// XAPI validates that partitions have FATX geometry (16 KB clusters of 512-byte
// sectors), so host sizes are expressed in those units.
constexpr ULONG kFatxBytesPerSector = 512, kFatxSectorsPerCluster = 32;
constexpr uint64_t kFatxCluster = kFatxBytesPerSector * kFatxSectorsPerCluster;

NTSTATUS NTAPI x_NtQueryVolumeInformationFile(GHandle h, XIO_STATUS_BLOCK* iosb, void* info, ULONG len, ULONG cls) {
    auto f = FileOf(h);
    if (!f) return STATUS_INVALID_HANDLE_X;
    memset(info, 0, len);
    Out o{static_cast<uint8_t*>(info), len};
    ULONG size;
    uint64_t total = 0, avail = 0;
    hostfs::Space(f->file->directory() ? f->file->path() : f->file->path().parent_path(), &total, &avail);
    switch (cls) {
        case 1:  // FileFsVolumeInformation: creation time, serial, label length, SupportsObjects, ANSI label
            o.Put<uint32_t>(8, 0x4B540001), size = 18;
            break;
        case 3:  // FileFsSizeInformation
            o.Put<uint64_t>(0, total / kFatxCluster), o.Put<uint64_t>(8, avail / kFatxCluster);
            o.Put<uint32_t>(16, kFatxSectorsPerCluster), o.Put<uint32_t>(20, kFatxBytesPerSector), size = 24;
            break;
        case 4:  // FileFsDeviceInformation: FILE_DEVICE_DISK
            o.Put<uint32_t>(0, 7), size = 8;
            break;
        case 5:  // FileFsAttributeInformation: attributes, max name length, ANSI "FATX"
            o.Put<uint32_t>(0, 0x2), o.Put<uint32_t>(4, 42), o.Put<uint32_t>(8, 4);
            if (len >= 16) memcpy(o.p + 12, "FATX", 4);
            size = 16;
            break;
        case 7:  // FileFsFullSizeInformation
            o.Put<uint64_t>(0, total / kFatxCluster), o.Put<uint64_t>(8, avail / kFatxCluster);
            o.Put<uint64_t>(16, avail / kFatxCluster);
            o.Put<uint32_t>(24, kFatxSectorsPerCluster), o.Put<uint32_t>(28, kFatxBytesPerSector), size = 32;
            break;
        default:
            Log("NtQueryVolumeInformationFile(%08x, class %lu): not supported", h, cls);
            return STATUS_INVALID_PARAMETER_X;
    }
    if (len < size) return STATUS_BUFFER_TOO_SMALL_X;
    SetIosb(iosb, STATUS_SUCCESS, size);
    return STATUS_SUCCESS;
}

NTSTATUS NTAPI x_NtFlushBuffersFile(GHandle h, XIO_STATUS_BLOCK* iosb) {
    auto f = FileOf(h);
    if (!f) return STATUS_INVALID_HANDLE_X;
    NTSTATUS st = f->file->Flush();
    SetIosb(iosb, st, 0);
    return st;
}

NTSTATUS NTAPI x_NtQueryFullAttributesFile(const XOBJECT_ATTRIBUTES* xoa, void* info) {
    fs::path path;
    int raw;
    std::string name;
    NTSTATUS st = HostPath(xoa, &path, &raw, &name);
    hostfs::Info i;
    if (st == STATUS_SUCCESS) st = hostfs::Stat(hostfs::Resolve(path), &i);
    if (st == STATUS_SUCCESS) {
        Out o{static_cast<uint8_t*>(info), 56};
        memset(info, 0, 56);
        PutNetworkOpen(o, i);
    }
    Log("NtQueryFullAttributesFile('%s') -> %08lx", name.c_str(), st);
    return st;
}

// FILE_DIRECTORY_INFORMATION (class 1) is the only class XAPI's
// FindFirstFile/FindNextFile use; the Xbox variant stores an ANSI name.
NTSTATUS NTAPI x_NtQueryDirectoryFile(GHandle h, GHandle, uint32_t, uint32_t, XIO_STATUS_BLOCK* iosb, void* info,
                                      ULONG len, ULONG cls, const XANSI_STRING* mask, BOOLEAN restart) {
    if (cls != 1) {
        Log("NtQueryDirectoryFile: class %lu not supported", cls);
        return STATUS_INVALID_PARAMETER_X;
    }
    auto f = FileOf(h);
    if (!f) return STATUS_INVALID_HANDLE_X;
    hostfs::DirEntry e;
    NTSTATUS st = f->file->NextEntry(AnsiOf(mask), restart != 0, &e);
    if (st != STATUS_SUCCESS) return SetIosb(iosb, st, 0), st;
    std::string name = HostName(fs::path(e.name));
    constexpr ULONG kNameOffset = 0x40;
    if (len < kNameOffset + name.size()) return STATUS_BUFFER_OVERFLOW_X;
    memset(info, 0, kNameOffset);
    Out o{static_cast<uint8_t*>(info), len};
    PutTimes(o, 8, e.info);
    o.Put<uint64_t>(40, e.info.size), o.Put<uint64_t>(48, e.info.allocation);
    o.Put<uint32_t>(56, e.info.attributes), o.Put<uint32_t>(60, uint32_t(name.size()));
    memcpy(o.p + kNameOffset, name.data(), name.size());
    SetIosb(iosb, STATUS_SUCCESS, ULONG(kNameOffset + name.size()));
    return STATUS_SUCCESS;
}

// XAPI's ReadFileEx/WriteFileEx pass this as the APC routine, with the
// title's completion routine as context; the IO_STATUS_BLOCK is the start of
// the OVERLAPPED.
VOID NTAPI x_NtUserIoApcDispatcher(uint32_t routine, XIO_STATUS_BLOCK* iosb, ULONG) {
    DWORD error = iosb->Status >= 0 ? 0 : StatusToDosError(iosb->Status);
    CallGuest(routine, {error, iosb->Information, H2G(iosb)});
}

NTSTATUS NTAPI x_NtFsControlFile(GHandle gh, GHandle, uint32_t, uint32_t, XIO_STATUS_BLOCK* iosb, ULONG code, void*,
                                 ULONG, void*, ULONG) {
    constexpr ULONG kLockVolume = 0x90018, kUnlockVolume = 0x9001C, kDismountVolume = 0x90020;
    auto f = FileOf(gh);
    if (f && f->raw_partition >= 0 && (code == kLockVolume || code == kUnlockVolume || code == kDismountVolume)) {
        SetIosb(iosb, STATUS_SUCCESS, 0);
        return STATUS_SUCCESS;
    }
    Log("NtFsControlFile(%08x, code=%08lx): not supported", gh, code);
    return STATUS_INVALID_DEVICE_REQUEST_X;
}

// SCSI requests to the DVD drive. XAPI of later XDKs refuses to start a title
// that may only boot from DVD unless the drive reports the disc as an
// authenticated Xbox DVD (MODE SENSE page 0x3E, the drive's security page).
// The files come from the user's own disc, so the answer is yes.
NTSTATUS DvdScsi(XIO_STATUS_BLOCK* iosb, const uint8_t* spt, ULONG len) {
    // SCSI_PASS_THROUGH_DIRECT (x86): Length, ScsiStatus, PathId, TargetId, Lun,
    // CdbLength, SenseInfoLength, DataIn, DataTransferLength @0x0C, TimeOutValue,
    // DataBuffer @0x14, SenseInfoOffset, Cdb[16] @0x1C.
    if (len < 0x2C) return STATUS_INVALID_PARAMETER_X;
    uint32_t transfer, buffer;
    memcpy(&transfer, spt + 0x0C, 4);
    memcpy(&buffer, spt + 0x14, 4);
    const uint8_t* cdb = spt + 0x1C;
    constexpr uint8_t kModeSense10 = 0x5A, kSecurityPage = 0x3E;
    if (cdb[0] == kModeSense10 && (cdb[2] & 0x3F) == kSecurityPage && buffer && transfer >= 13) {
        auto* d = G2H<uint8_t>(buffer);
        memset(d, 0, transfer);
        uint16_t data_len = uint16_t(transfer - 2);
        d[0] = uint8_t(data_len >> 8), d[1] = uint8_t(data_len);  // mode parameter header
        d[8] = kSecurityPage;
        d[9] = uint8_t(transfer - 10);  // page length
        d[10] = 1;                      // PartitionArea: the game partition is active
        d[11] = 1;                      // CDFValid
        d[12] = 1;                      // Authentication: passed
        SetIosb(iosb, STATUS_SUCCESS, transfer);
        return STATUS_SUCCESS;
    }
    Log("DVD SCSI command %02x %02x %02x: not supported", cdb[0], cdb[1], cdb[2]);
    return STATUS_INVALID_DEVICE_REQUEST_X;
}

NTSTATUS NTAPI x_NtDeviceIoControlFile(GHandle gh, GHandle, uint32_t, uint32_t, XIO_STATUS_BLOCK* iosb, ULONG code,
                                       void* in, ULONG in_len, void* out, ULONG out_len) {
    constexpr ULONG kGetDriveGeometry = 0x70000, kGetPartitionInfo = 0x74004, kScsiPassThroughDirect = 0x4D014;
    auto f = FileOf(gh);
    if (f && f->dvd_device && code == kScsiPassThroughDirect && in)
        return DvdScsi(iosb, static_cast<const uint8_t*>(in), in_len);
    int part = f ? f->raw_partition : -1;
    if (part >= 0 && code == kGetDriveGeometry && out_len >= 24) {
        // DISK_GEOMETRY: Cylinders, MediaType, TracksPerCylinder, SectorsPerTrack, BytesPerSector
        auto* g = static_cast<uint8_t*>(out);
        LONGLONG cylinders = kPartitionSize[0] / 512;
        ULONG fields[4] = {12 /* FixedMedia */, 1, 1, 512};
        memcpy(g, &cylinders, 8);
        memcpy(g + 8, fields, 16);
        SetIosb(iosb, STATUS_SUCCESS, 24);
        return STATUS_SUCCESS;
    }
    if (part >= 0 && code == kGetPartitionInfo && out_len >= 32) {
        // PARTITION_INFORMATION: StartingOffset, PartitionLength, HiddenSectors, PartitionNumber, type, flags
        auto* p = static_cast<uint8_t*>(out);
        memset(p, 0, 32);
        LONGLONG start = 0x80000 + LONGLONG(part) * 0x10000000;  // only needs to be plausible
        memcpy(p, &start, 8);
        memcpy(p + 8, &kPartitionSize[part], 8);
        ULONG number = part;
        memcpy(p + 20, &number, 4);
        p[24] = 0x0E;  // PartitionType
        p[26] = 1;     // RecognizedPartition
        SetIosb(iosb, STATUS_SUCCESS, 32);
        return STATUS_SUCCESS;
    }
    Log("NtDeviceIoControlFile(%08x, code=%08lx): not supported", gh, code);
    return STATUS_INVALID_DEVICE_REQUEST_X;
}

}  // namespace

void IoSetGameRoot(const std::filesystem::path& root) {
    g_game_root = fs::absolute(root);
    // The kernel maps D: to the directory the XBE was launched from.
    g_symlinks["\\??\\d:"] = "\\Device\\CdRom0";
}

void IoSetHddRoot(const std::filesystem::path& root) {
    g_hdd_root = fs::absolute(root);
    std::error_code ec;
    fs::create_directories(g_hdd_root, ec);
    for (int i = 1; i <= 7; i++) fs::create_directories(g_hdd_root / ("Partition" + std::to_string(i)), ec);
    fs::create_directories(g_hdd_root / "Partition1" / "TDATA", ec);
    // Backing files for raw volume access. Partition0 holds the config area
    // (refurb info, cache database) that precedes the first partition.
    for (int i = 0; i <= 7; i++) {
        fs::path bin = g_hdd_root / ("Partition" + std::to_string(i) + ".bin");
        if (fs::exists(bin, ec)) continue;
        if (FILE* f = fopen(bin.string().c_str(), "wb")) fclose(f);
        if (i == 0) fs::resize_file(bin, 0x80000, ec);
    }
}

std::vector<KExport> KernelIoExports() {
    return {
        KX(IoCreateSymbolicLink), KX(NtOpenSymbolicLinkObject), KX(NtQuerySymbolicLinkObject), KX(NtClose),
        KX(NtCreateFile), KX(NtOpenFile), KX(NtReadFile), KX(NtWriteFile), KX(NtQueryInformationFile),
        KX(NtSetInformationFile), KX(NtQueryVolumeInformationFile), KX(NtFlushBuffersFile),
        KX(NtQueryFullAttributesFile), KX(NtQueryDirectoryFile), KX(NtFsControlFile), KX(NtDeviceIoControlFile),
        KX(NtUserIoApcDispatcher),
    };
}
