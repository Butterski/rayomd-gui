// SPDX-License-Identifier: Apache-2.0
#ifdef _WIN32
#define _CRT_RAND_S  // rand_s: RtlGenRandom through the C runtime, no extra import
#endif

#include "app/safefile.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>

#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace rgui {
namespace {

namespace fs = std::filesystem;

// The temporary file's name keeps at most this many bytes of the destination's name, so
// `.<name>.<16 hex>.tmp` stays within NAME_MAX (255).
constexpr size_t kMaxNameInTemp = 200;
// Attempts at a temporary name; a collision of 64 random bits means someone plants names.
constexpr int kTempAttempts = 16;

void SetError(std::string* error, std::string text) {
    if (error) *error = std::move(text);
}

bool RandomHex(char (&hex)[17]) {
    uint8_t bytes[8];
#ifdef _WIN32
    for (size_t i = 0; i < sizeof bytes; i += 4) {
        unsigned int value = 0;
        if (rand_s(&value) != 0) return false;
        std::memcpy(bytes + i, &value, 4);
    }
#else
    size_t got = 0;
    while (got < sizeof bytes) {
        const ssize_t n = getrandom(bytes + got, sizeof bytes - got, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        got += static_cast<size_t>(n);
    }
#endif
    static const char kDigits[] = "0123456789abcdef";
    for (size_t i = 0; i < sizeof bytes; ++i) {
        hex[2 * i] = kDigits[bytes[i] >> 4];
        hex[2 * i + 1] = kDigits[bytes[i] & 15];
    }
    hex[16] = '\0';
    return true;
}

#ifdef _WIN32

std::string Describe(const char* what) {
    char buffer[64];
    std::snprintf(buffer, sizeof buffer, "%s (error %lu)", what, static_cast<unsigned long>(GetLastError()));
    return buffer;
}

// `path` as the Win32 file functions take it past MAX_PATH: absolute, normalized and `\\?\`
// prefixed (`\\?\UNC\server\share\...` for a share), so a temporary name longer than its
// destination's never fails where the destination would not, whether or not the system enables
// long paths. Already prefixed and device paths stay as they are.
std::wstring LongPath(const fs::path& path) {
    const std::wstring& raw = path.native();
    if (raw.rfind(L"\\\\?\\", 0) == 0 || raw.rfind(L"\\\\.\\", 0) == 0) return raw;
    DWORD length = GetFullPathNameW(raw.c_str(), 0, nullptr, nullptr);
    if (length == 0) return raw;
    std::wstring full(length, L'\0');
    length = GetFullPathNameW(raw.c_str(), length, full.data(), nullptr);
    if (length == 0 || length >= full.size()) return raw;
    full.resize(length);
    if (full.rfind(L"\\\\", 0) == 0) return L"\\\\?\\UNC\\" + full.substr(2);
    if (full.size() >= 3 && full[1] == L':' && full[2] == L'\\') return L"\\\\?\\" + full;
    return raw;
}

// The file a symlink (or other name surrogate) at `path` finally names.
bool ResolveLink(const fs::path& path, fs::path& out) {
    const HANDLE handle = CreateFileW(LongPath(path).c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                      nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return false;
    std::wstring buffer(512, L'\0');
    DWORD length = GetFinalPathNameByHandleW(handle, buffer.data(), static_cast<DWORD>(buffer.size()),
                                             FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (length >= buffer.size()) {
        buffer.assign(length + 1, L'\0');
        length = GetFinalPathNameByHandleW(handle, buffer.data(), static_cast<DWORD>(buffer.size()),
                                           FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    }
    CloseHandle(handle);
    if (length == 0 || length >= buffer.size()) return false;
    buffer.resize(length);
    out = fs::path(buffer);
    return true;
}

#else

std::string Describe(const char* what) {
    const int code = errno;
    char buffer[128];
    // GNU strerror_r returns the text, the XSI one fills the buffer; take whichever applies.
    struct Pick {
        static const char* Text(const char* result, const char*) { return result; }
        static const char* Text(int result, const char* filled) { return result == 0 ? filled : "unknown error"; }
    };
    buffer[0] = '\0';
    return std::string(what) + ": " + Pick::Text(strerror_r(code, buffer, sizeof buffer), buffer);
}

bool WriteFully(int fd, std::string_view bytes) {
    while (!bytes.empty()) {
        const size_t chunk = bytes.size() < (size_t(1) << 30) ? bytes.size() : (size_t(1) << 30);
        const ssize_t n = write(fd, bytes.data(), chunk);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (n == 0) {
            errno = EIO;
            return false;
        }
        bytes.remove_prefix(static_cast<size_t>(n));
    }
    return true;
}

#endif

} // namespace

#ifdef _WIN32

bool WriteFileAtomically(const fs::path& path, std::string_view bytes, WriteMode mode, std::string* error) {
    fs::path target = path;
    DWORD attributes = GetFileAttributesW(LongPath(target).c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT) &&
        mode == WriteMode::Document) {
        // Write through the link: the file it names is replaced, the link stays.
        if (!ResolveLink(path, target)) {
            SetError(error, Describe("cannot resolve the link"));
            return false;
        }
        attributes = GetFileAttributesW(LongPath(target).c_str());
    }
    if (attributes != INVALID_FILE_ATTRIBUTES && mode == WriteMode::NewFile) {
        SetError(error, kFileExistsError);
        return false;
    }
    if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY)) {
        SetError(error, "not a regular file");
        return false;
    }
    const std::wstring name = target.filename().wstring();
    if (name.empty() || name == L"." || name == L"..") {
        SetError(error, "not a file name");
        return false;
    }
    const fs::path directory = target.parent_path();

    fs::path temp;
    HANDLE file = INVALID_HANDLE_VALUE;
    for (int attempt = 0; attempt < kTempAttempts && file == INVALID_HANDLE_VALUE; ++attempt) {
        char hex[17];
        if (!RandomHex(hex)) {
            SetError(error, "no random temporary name");
            return false;
        }
        std::wstring tempName = L".";
        tempName += name.size() > kMaxNameInTemp ? name.substr(0, kMaxNameInTemp) : name;
        tempName += L'.';
        for (const char* c = hex; *c; ++c) tempName += static_cast<wchar_t>(*c);
        tempName += L".tmp";
        temp = directory / tempName;
        // CREATE_NEW fails on anything already at the name, a planted link included; with
        // FILE_FLAG_OPEN_REPARSE_POINT no reparse point is ever followed.
        file = CreateFileW(LongPath(temp).c_str(), GENERIC_WRITE | DELETE, 0, nullptr, CREATE_NEW,
                           FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (file == INVALID_HANDLE_VALUE) {
            const DWORD code = GetLastError();
            if (code != ERROR_FILE_EXISTS && code != ERROR_ALREADY_EXISTS) {
                SetError(error, Describe("cannot create the temporary file"));
                return false;
            }
        }
    }
    if (file == INVALID_HANDLE_VALUE) {
        SetError(error, "no free temporary name");
        return false;
    }

    bool ok = true;
    std::string_view rest = bytes;
    while (ok && !rest.empty()) {
        const DWORD chunk = rest.size() < (size_t(1) << 30) ? static_cast<DWORD>(rest.size()) : (DWORD(1) << 30);
        DWORD written = 0;
        ok = WriteFile(file, rest.data(), chunk, &written, nullptr) && written != 0;
        if (ok) rest.remove_prefix(written);
    }
    if (!ok) SetError(error, Describe("cannot write"));
    if (ok && mode == WriteMode::Document && !FlushFileBuffers(file)) {
        SetError(error, Describe("cannot flush"));
        ok = false;
    }
    CloseHandle(file);
    if (ok) {
        if (mode == WriteMode::NewFile) {
            ok = MoveFileExW(LongPath(temp).c_str(), LongPath(target).c_str(), 0) != 0;
            if (!ok) {
                const DWORD code = GetLastError();
                if (code == ERROR_ALREADY_EXISTS || code == ERROR_FILE_EXISTS) SetError(error, kFileExistsError);
                else SetError(error, Describe("cannot create the file"));
            }
        } else {
            const DWORD flags = MOVEFILE_REPLACE_EXISTING | (mode == WriteMode::Document ? MOVEFILE_WRITE_THROUGH : 0);
            ok = MoveFileExW(LongPath(temp).c_str(), LongPath(target).c_str(), flags) != 0;
            if (!ok) SetError(error, Describe("cannot replace the file"));
        }
    }
    if (!ok) DeleteFileW(LongPath(temp).c_str());
    return ok;
}

bool ReadRegularFile(const fs::path& path, size_t limit, std::string& out, std::string* error) {
    const HANDLE file = CreateFileW(LongPath(path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        SetError(error, Describe("cannot open"));
        return false;
    }
    bool ok = true;
    LARGE_INTEGER size{};
    if (GetFileType(file) != FILE_TYPE_DISK) {
        SetError(error, "not a regular file");
        ok = false;
    } else if (!GetFileSizeEx(file, &size)) {
        SetError(error, Describe("cannot read the size"));
        ok = false;
    } else if (size.QuadPart < 0 || static_cast<unsigned long long>(size.QuadPart) > limit) {
        SetError(error, "too large");
        ok = false;
    }
    std::string data;
    if (ok) {
        // One byte past the size notices a file that grew past `limit` meanwhile.
        size_t capacity = static_cast<size_t>(size.QuadPart) + 1;
        data.resize(capacity);
        size_t filled = 0;
        for (;;) {
            if (filled == capacity) {
                if (capacity > limit) {
                    SetError(error, "too large");
                    ok = false;
                    break;
                }
                capacity = capacity > limit / 2 ? limit + 1 : capacity * 2;
                data.resize(capacity);
            }
            const size_t want = capacity - filled;
            DWORD got = 0;
            if (!ReadFile(file, &data[filled], want < (size_t(1) << 30) ? static_cast<DWORD>(want) : (DWORD(1) << 30),
                          &got, nullptr)) {
                SetError(error, Describe("cannot read"));
                ok = false;
                break;
            }
            if (got == 0) break;
            filled += got;
        }
        data.resize(filled);
    }
    CloseHandle(file);
    if (ok) out.swap(data);
    return ok;
}

#else

bool WriteFileAtomically(const fs::path& path, std::string_view bytes, WriteMode mode, std::string* error) {
    fs::path target = path;
    if (mode == WriteMode::Document) {
        struct stat link {};
        if (lstat(path.c_str(), &link) == 0 && S_ISLNK(link.st_mode)) {
            // Write through the link: the file it names is replaced, the link stays. A dangling
            // link is refused rather than guessed at.
            char* const real = realpath(path.c_str(), nullptr);
            if (!real) {
                SetError(error, Describe("cannot resolve the symlink"));
                return false;
            }
            target = real;
            std::free(real);
        }
    }
    const std::string name = target.filename().native();
    if (name.empty() || name == "." || name == "..") {
        SetError(error, "not a file name");
        return false;
    }
    const fs::path parent = target.parent_path();
    const int directory = open(parent.empty() ? "." : parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (directory < 0) {
        SetError(error, Describe("cannot open the folder"));
        return false;
    }

    // The destination as it is now, never through a link: a regular file keeps its permission
    // bits; a link (Output) is replaced; anything else is refused, so the rename can never put
    // a file where a FIFO, socket, device or directory was.
    bool keepMode = false;
    mode_t keptMode = 0;
    struct stat existing {};
    if (fstatat(directory, name.c_str(), &existing, AT_SYMLINK_NOFOLLOW) == 0) {
        if (mode == WriteMode::NewFile) {
            SetError(error, kFileExistsError);
            close(directory);
            return false;
        }
        if (S_ISREG(existing.st_mode)) {
            keepMode = true;
            keptMode = existing.st_mode & 0777;
        } else if (!S_ISLNK(existing.st_mode)) {
            SetError(error, "not a regular file");
            close(directory);
            return false;
        }
    } else if (errno != ENOENT) {
        SetError(error, Describe("cannot inspect the file"));
        close(directory);
        return false;
    }

    // O_CREAT | O_EXCL | O_NOFOLLOW: the name must be new, so a planted file, FIFO or symlink is
    // never opened. A file kept private while written gets its final bits by fchmod; a new
    // file is created 0666 for the kernel to apply the umask (or the folder's default ACL),
    // which umask() cannot tell another thread without changing it.
    std::string tempName;
    int fd = -1;
    for (int attempt = 0; attempt < kTempAttempts && fd < 0; ++attempt) {
        char hex[17];
        if (!RandomHex(hex)) {
            SetError(error, Describe("no random temporary name"));
            close(directory);
            return false;
        }
        tempName = "." + name.substr(0, kMaxNameInTemp) + "." + hex + ".tmp";
        fd = openat(directory, tempName.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC,
                    keepMode ? 0600 : 0666);
        if (fd < 0 && errno != EEXIST) {
            SetError(error, Describe("cannot create the temporary file"));
            close(directory);
            return false;
        }
    }
    if (fd < 0) {
        SetError(error, "no free temporary name");
        close(directory);
        return false;
    }

    bool ok = WriteFully(fd, bytes);
    if (!ok) SetError(error, Describe("cannot write"));
    if (ok && keepMode && fchmod(fd, keptMode) != 0) {
        SetError(error, Describe("cannot set the permissions"));
        ok = false;
    }
    if (ok && mode == WriteMode::Document && fsync(fd) != 0) {
        SetError(error, Describe("cannot flush"));
        ok = false;
    }
    if (close(fd) != 0 && ok) {
        SetError(error, Describe("cannot write"));
        ok = false;
    }
    if (ok && mode == WriteMode::NewFile) {
        // Nothing may be replaced. A file system without RENAME_NOREPLACE takes a hard link,
        // which fails on an existing name too; the temporary name then goes.
        if (renameat2(directory, tempName.c_str(), directory, name.c_str(), RENAME_NOREPLACE) != 0) {
            const int code = errno;
            if (code == EEXIST) {
                SetError(error, kFileExistsError);
                ok = false;
            } else if (code != EINVAL && code != ENOSYS) {
                SetError(error, Describe("cannot create the file"));
                ok = false;
            } else if (linkat(directory, tempName.c_str(), directory, name.c_str(), 0) != 0) {
                SetError(error, errno == EEXIST ? std::string(kFileExistsError) : Describe("cannot create the file"));
                ok = false;
            } else {
                unlinkat(directory, tempName.c_str(), 0);
            }
        }
    } else if (ok && renameat(directory, tempName.c_str(), directory, name.c_str()) != 0) {
        SetError(error, Describe("cannot replace the file"));
        ok = false;
    }
    if (!ok) {
        unlinkat(directory, tempName.c_str(), 0);
    } else if (mode == WriteMode::Document) {
        // The rename itself reaches the disk. Some file systems cannot fsync a folder; the
        // file is in place either way.
        fsync(directory);
    }
    close(directory);
    return ok;
}

bool ReadRegularFile(const fs::path& path, size_t limit, std::string& out, std::string* error) {
    // O_NONBLOCK: opening a FIFO without a writer, or a device, returns at once.
    const int fd = open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOCTTY);
    if (fd < 0) {
        SetError(error, Describe("cannot open"));
        return false;
    }
    struct stat info {};
    bool ok = true;
    if (fstat(fd, &info) != 0) {
        SetError(error, Describe("cannot inspect"));
        ok = false;
    } else if (!S_ISREG(info.st_mode)) {
        SetError(error, "not a regular file");
        ok = false;
    } else if (info.st_size < 0 || static_cast<unsigned long long>(info.st_size) > limit) {
        SetError(error, "too large");
        ok = false;
    } else {
        const int flags = fcntl(fd, F_GETFL);
        if (flags < 0 || fcntl(fd, F_SETFL, flags & ~O_NONBLOCK) != 0) {
            SetError(error, Describe("cannot read"));
            ok = false;
        }
    }
    std::string data;
    if (ok) {
        // One byte past the size notices a file that grew past `limit` meanwhile.
        size_t capacity = static_cast<size_t>(info.st_size) + 1;
        data.resize(capacity);
        size_t filled = 0;
        for (;;) {
            if (filled == capacity) {
                if (capacity > limit) {
                    SetError(error, "too large");
                    ok = false;
                    break;
                }
                capacity = capacity > limit / 2 ? limit + 1 : capacity * 2;
                data.resize(capacity);
            }
            const ssize_t n = read(fd, &data[filled], capacity - filled);
            if (n < 0) {
                if (errno == EINTR) continue;
                SetError(error, Describe("cannot read"));
                ok = false;
                break;
            }
            if (n == 0) break;
            filled += static_cast<size_t>(n);
        }
        data.resize(filled);
    }
    close(fd);
    if (ok) out.swap(data);
    return ok;
}

#endif

} // namespace rgui
