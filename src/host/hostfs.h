// Host file system access with NT semantics (status codes, create
// dispositions, file information), used by the Xbox file I/O exports.
#pragma once

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace hostfs {

using NTSTATUS = int32_t;
constexpr NTSTATUS kSuccess = 0;
constexpr NTSTATUS kNoMoreFiles = int32_t(0x80000006);
constexpr NTSTATUS kNoSuchFile = int32_t(0xC000000F);
constexpr NTSTATUS kEndOfFile = int32_t(0xC0000011);
constexpr NTSTATUS kAccessDenied = int32_t(0xC0000022);
constexpr NTSTATUS kNameNotFound = int32_t(0xC0000034);
constexpr NTSTATUS kNameCollision = int32_t(0xC0000035);
constexpr NTSTATUS kPathNotFound = int32_t(0xC000003A);
constexpr NTSTATUS kFileIsDirectory = int32_t(0xC00000BA);
constexpr NTSTATUS kNotADirectory = int32_t(0xC0000103);
constexpr NTSTATUS kDirectoryNotEmpty = int32_t(0xC0000101);
constexpr NTSTATUS kInvalidParameter = int32_t(0xC000000D);
constexpr NTSTATUS kIoError = int32_t(0xC0000185);

// NtCreateFile dispositions and the IO_STATUS_BLOCK.Information results.
enum Disposition : uint32_t { kSupersede, kOpen, kCreate, kOpenIf, kOverwrite, kOverwriteIf };
enum Result : uint32_t { kSuperseded, kOpened, kCreated, kOverwritten };

constexpr uint32_t kAttrReadOnly = 0x01, kAttrDirectory = 0x10, kAttrNormal = 0x80;

struct Info {
    bool directory = false;
    uint64_t size = 0, allocation = 0;
    uint64_t creation = 0, access = 0, write = 0, change = 0;  // 100 ns since 1601
    uint32_t attributes = 0;
};

struct DirEntry {
    std::string name;  // host file name (UTF-8)
    Info info;
};

// Finds `p` ignoring case (on case-sensitive hosts); returns `p` unchanged
// when nothing matches.
std::filesystem::path Resolve(const std::filesystem::path& p);
NTSTATUS Stat(const std::filesystem::path& p, Info* out);

class File {
  public:
    // options: NtCreateFile CreateOptions (FILE_DIRECTORY_FILE, ...).
    static std::unique_ptr<File> Open(const std::filesystem::path& p, bool write, uint32_t disposition,
                                      uint32_t options, NTSTATUS* status, uint32_t* result);
    ~File();

    NTSTATUS Read(void* buf, uint32_t len, const uint64_t* offset, uint32_t* done);
    NTSTATUS Write(const void* buf, uint32_t len, const uint64_t* offset, uint32_t* done);
    NTSTATUS Query(Info* out);
    NTSTATUS SetSize(uint64_t size);
    NTSTATUS Flush();
    NTSTATUS Rename(const std::filesystem::path& to, bool replace);
    // Returns the next directory entry matching `mask` (NT wildcards * and ?).
    NTSTATUS NextEntry(const std::string& mask, bool restart, DirEntry* out);

    bool directory() const { return directory_; }
    const std::filesystem::path& path() const { return path_; }
    uint64_t position = 0;
    bool delete_on_close = false;

  private:
    std::filesystem::path path_;
    bool directory_ = false;
    FILE* f_ = nullptr;
    std::mutex mu_;
    std::vector<DirEntry> listing_;
    size_t next_ = 0;
    bool listed_ = false;
    std::string mask_;
};

// Free and total bytes of the volume holding `p`.
bool Space(const std::filesystem::path& p, uint64_t* total, uint64_t* available);

}  // namespace hostfs
