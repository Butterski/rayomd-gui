// SPDX-License-Identifier: Apache-2.0
#include "text/mapped_file.h"

#include <utility>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace rgui::text {

MappedFile::~MappedFile() {
    Close();
}

MappedFile::MappedFile(MappedFile&& other) noexcept
    : data_(std::exchange(other.data_, nullptr)), size_(std::exchange(other.size_, 0)) {}

MappedFile& MappedFile::operator=(MappedFile&& other) noexcept {
    if (this != &other) {
        Close();
        data_ = std::exchange(other.data_, nullptr);
        size_ = std::exchange(other.size_, 0);
    }
    return *this;
}

#ifdef _WIN32

bool MappedFile::Open(const std::string& utf8Path) {
    Close();
    if (utf8Path.empty() || utf8Path.size() > 32767) return false;
    const int wideLength = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8Path.data(),
        static_cast<int>(utf8Path.size()), nullptr, 0);
    if (wideLength <= 0) return false;
    std::wstring wide(static_cast<size_t>(wideLength), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8Path.data(), static_cast<int>(utf8Path.size()),
        wide.data(), wideLength);
    const HANDLE file = CreateFileW(wide.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    bool ok = GetFileSizeEx(file, &size) && GetFileType(file) == FILE_TYPE_DISK && size.QuadPart > 0 &&
        static_cast<uint64_t>(size.QuadPart) <= kMaxBytes;
    if (ok) {
        const HANDLE mapping = CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
        if (mapping != nullptr) {
            const void* view = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
            CloseHandle(mapping);   // the view keeps the mapping alive
            if (view != nullptr) {
                data_ = static_cast<const uint8_t*>(view);
                size_ = static_cast<size_t>(size.QuadPart);
            }
        }
    }
    CloseHandle(file);
    return data_ != nullptr;
}

void MappedFile::Close() {
    if (data_ != nullptr) UnmapViewOfFile(data_);
    data_ = nullptr;
    size_ = 0;
}

#else

bool MappedFile::Open(const std::string& utf8Path) {
    Close();
    const int fd = open(utf8Path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    struct stat info {};
    if (fstat(fd, &info) == 0 && S_ISREG(info.st_mode) && info.st_size > 0 &&
        static_cast<uint64_t>(info.st_size) <= kMaxBytes) {
        void* const view = mmap(nullptr, static_cast<size_t>(info.st_size), PROT_READ, MAP_PRIVATE, fd, 0);
        if (view != MAP_FAILED) {
            data_ = static_cast<const uint8_t*>(view);
            size_ = static_cast<size_t>(info.st_size);
        }
    }
    close(fd);
    return data_ != nullptr;
}

void MappedFile::Close() {
    if (data_ != nullptr) munmap(const_cast<uint8_t*>(data_), size_);
    data_ = nullptr;
    size_ = 0;
}

#endif

} // namespace rgui::text
