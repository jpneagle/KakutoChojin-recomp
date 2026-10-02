// hostfs.h on std::filesystem and stdio.
#include "hostfs.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <system_error>

#include "os.h"

#ifdef _WIN32
#define KT_FSEEK _fseeki64
#else
#include <unistd.h>
#define KT_FSEEK fseeko
#endif

namespace fs = std::filesystem;

namespace hostfs {

namespace {

bool EqualsI(const std::string& a, const std::string& b) {
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) { return tolower((unsigned char)x) == tolower((unsigned char)y); });
}

// NT wildcard match (* and ?), case-insensitive. "*.*" matches names without dots too.
bool Match(const char* m, const char* s) {
    if (!*m) return !*s;
    if (*m == '*') {
        if (!strcmp(m, "*.*") || !strcmp(m, "*")) return true;
        for (const char* p = s;; p++) {
            if (Match(m + 1, p)) return true;
            if (!*p) return false;
        }
    }
    if (!*s) return false;
    if (*m == '?' || tolower((unsigned char)*m) == tolower((unsigned char)*s)) return Match(m + 1, s + 1);
    return false;
}

uint64_t FileTime(fs::file_time_type t) {
    // file_time_type's epoch is unspecified before C++20: convert through "now".
    auto sys = std::chrono::system_clock::now() + std::chrono::duration_cast<std::chrono::system_clock::duration>(
                                                      t - fs::file_time_type::clock::now());
    constexpr uint64_t kEpochDelta = 116444736000000000ull;
    return kEpochDelta + uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(sys.time_since_epoch()).count() / 100);
}

NTSTATUS FromError(const std::error_code& ec, bool parent_missing) {
    if (ec == std::errc::no_such_file_or_directory) return parent_missing ? kPathNotFound : kNameNotFound;
    if (ec == std::errc::permission_denied || ec == std::errc::read_only_file_system) return kAccessDenied;
    if (ec == std::errc::file_exists) return kNameCollision;
    if (ec == std::errc::directory_not_empty) return kDirectoryNotEmpty;
    if (ec == std::errc::not_a_directory) return kPathNotFound;
    return kIoError;
}

}  // namespace

fs::path Resolve(const fs::path& p) {
#ifdef _WIN32
    return p;  // case-insensitive already
#else
    std::error_code ec;
    if (fs::exists(p, ec)) return p;
    fs::path out = p.root_path();
    for (const fs::path& part : p.relative_path()) {
        fs::path next = out / part;
        if (!fs::exists(next, ec)) {
            for (const auto& e : fs::directory_iterator(out, ec))
                if (EqualsI(e.path().filename().string(), part.string())) {
                    next = e.path();
                    break;
                }
        }
        out = next;
    }
    return out;
#endif
}

NTSTATUS Stat(const fs::path& p, Info* out) {
    std::error_code ec;
    fs::file_status st = fs::status(p, ec);
    if (ec || !fs::exists(st)) return fs::exists(p.parent_path(), ec) ? kNameNotFound : kPathNotFound;
    *out = {};
    out->directory = fs::is_directory(st);
    if (!out->directory) out->size = fs::file_size(p, ec);
    out->allocation = (out->size + 4095) & ~uint64_t(4095);
    uint64_t t = FileTime(fs::last_write_time(p, ec));
    out->creation = out->access = out->write = out->change = t;
    bool readonly = (st.permissions() & fs::perms::owner_write) == fs::perms::none;
    out->attributes = out->directory ? kAttrDirectory : kAttrNormal;
    if (readonly && !out->directory) out->attributes = kAttrReadOnly;
    return kSuccess;
}

bool Space(const fs::path& p, uint64_t* total, uint64_t* available) {
    std::error_code ec;
    fs::space_info s = fs::space(p, ec);
    if (ec) return false;
    *total = s.capacity, *available = s.available;
    return true;
}

std::unique_ptr<File> File::Open(const fs::path& raw, bool write, uint32_t disposition, uint32_t options,
                                 NTSTATUS* status, uint32_t* result) {
    constexpr uint32_t kDirectoryFile = 0x1, kNonDirectoryFile = 0x40, kDeleteOnClose = 0x1000;
    fs::path p = Resolve(raw);
    std::error_code ec;
    fs::file_status st = fs::status(p, ec);
    bool exists = fs::exists(st), is_dir = fs::is_directory(st);
    bool parent = fs::is_directory(p.parent_path(), ec);
    *result = kOpened;
    auto fail = [&](NTSTATUS s) {
        *status = s;
        return nullptr;
    };
    if (exists && is_dir && (options & kNonDirectoryFile)) return fail(kFileIsDirectory);
    if (exists && !is_dir && (options & kDirectoryFile)) return fail(kNotADirectory);
    bool create = false, truncate = false;
    switch (disposition) {
        case kOpen:
            if (!exists) return fail(parent ? kNameNotFound : kPathNotFound);
            break;
        case kCreate:
            if (exists) return fail(kNameCollision);
            create = true;
            break;
        case kOpenIf:
            create = !exists;
            break;
        case kOverwrite:
            if (!exists) return fail(parent ? kNameNotFound : kPathNotFound);
            truncate = true;
            break;
        case kOverwriteIf:
        case kSupersede:
            create = !exists, truncate = exists;
            break;
        default:
            return fail(kInvalidParameter);
    }
    if (create && !parent) return fail(kPathNotFound);

    auto f = std::unique_ptr<File>(new File);
    f->path_ = p;
    f->delete_on_close = (options & kDeleteOnClose) != 0;
    if (is_dir || (create && (options & kDirectoryFile))) {
        if (create && !fs::create_directory(p, ec) && ec) return fail(FromError(ec, false));
        f->directory_ = true;
        *result = create ? kCreated : kOpened;
        *status = kSuccess;
        return f;
    }
    const char* mode = create || truncate ? "w+b" : write ? "r+b" : "rb";
    f->f_ = os::OpenFile(p, mode);
    if (!f->f_ && write && !create && !truncate) {
        // Read-only host files (e.g. copied from a disc) still open for reading.
        f->f_ = os::OpenFile(p, "rb");
    }
    if (!f->f_) return fail(kAccessDenied);
    *result = create ? kCreated : truncate ? (disposition == kSupersede ? kSuperseded : kOverwritten) : kOpened;
    *status = kSuccess;
    return f;
}

File::~File() {
    if (f_) fclose(f_);
    if (delete_on_close) {
        std::error_code ec;
        fs::remove(path_, ec);
    }
}

NTSTATUS File::Read(void* buf, uint32_t len, const uint64_t* offset, uint32_t* done) {
    *done = 0;
    if (directory_ || !f_) return kInvalidParameter;
    std::lock_guard<std::mutex> lk(mu_);
    uint64_t at = offset ? *offset : position;
    if (KT_FSEEK(f_, int64_t(at), SEEK_SET) != 0) return kIoError;
    size_t n = fread(buf, 1, len, f_);
    position = at + n;
    *done = uint32_t(n);
    if (n == 0 && len > 0) return feof(f_) ? kEndOfFile : kIoError;
    return kSuccess;
}

NTSTATUS File::Write(const void* buf, uint32_t len, const uint64_t* offset, uint32_t* done) {
    *done = 0;
    if (directory_ || !f_) return kInvalidParameter;
    std::lock_guard<std::mutex> lk(mu_);
    uint64_t at = offset ? *offset : position;
    if (KT_FSEEK(f_, int64_t(at), SEEK_SET) != 0) return kIoError;
    size_t n = fwrite(buf, 1, len, f_);
    position = at + n;
    *done = uint32_t(n);
    return n == len ? kSuccess : kAccessDenied;
}

NTSTATUS File::Query(Info* out) {
    if (f_) {
        std::lock_guard<std::mutex> lk(mu_);
        fflush(f_);
    }
    return Stat(path_, out);
}

NTSTATUS File::SetSize(uint64_t size) {
    if (directory_ || !f_) return kInvalidParameter;
    std::lock_guard<std::mutex> lk(mu_);
    fflush(f_);
    std::error_code ec;
    fs::resize_file(path_, size, ec);
    return ec ? FromError(ec, false) : kSuccess;
}

NTSTATUS File::Flush() {
    if (f_) {
        std::lock_guard<std::mutex> lk(mu_);
        fflush(f_);
    }
    return kSuccess;
}

NTSTATUS File::Rename(const fs::path& to, bool replace) {
    std::lock_guard<std::mutex> lk(mu_);
    std::error_code ec;
    fs::path target = Resolve(to);
    if (fs::exists(target, ec) && !EqualsI(target.string(), path_.string())) {
        if (!replace) return kNameCollision;
        fs::remove(target, ec);
    }
    if (f_) fflush(f_);
    fs::rename(path_, target, ec);
    if (ec) return FromError(ec, !fs::exists(target.parent_path()));
    path_ = target;
    return kSuccess;
}

NTSTATUS File::NextEntry(const std::string& mask, bool restart, DirEntry* out) {
    if (!directory_) return kInvalidParameter;
    std::lock_guard<std::mutex> lk(mu_);
    bool first = !listed_ || restart;
    if (first) {
        // The mask is fixed by the first query (or a restart).
        mask_ = mask.empty() ? "*" : mask;
        listing_.clear();
        std::error_code ec;
        for (const auto& e : fs::directory_iterator(path_, ec)) {
            std::string name = e.path().filename().string();
            if (!Match(mask_.c_str(), name.c_str())) continue;
            DirEntry d;
            d.name = name;
            Stat(e.path(), &d.info);
            listing_.push_back(std::move(d));
        }
        std::sort(listing_.begin(), listing_.end(), [](const DirEntry& a, const DirEntry& b) { return a.name < b.name; });
        listed_ = true;
        next_ = 0;
    }
    if (next_ >= listing_.size()) return first ? kNoSuchFile : kNoMoreFiles;
    *out = listing_[next_++];
    return kSuccess;
}

}  // namespace hostfs
