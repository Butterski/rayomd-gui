// SPDX-License-Identifier: Apache-2.0
// A whole file mapped read-only into memory, for font files.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace rgui::text {

class MappedFile {
public:
    // Files larger than this are refused.
    static constexpr uint64_t kMaxBytes = 64ull << 20;

    MappedFile() = default;
    ~MappedFile();
    MappedFile(MappedFile&& other) noexcept;
    MappedFile& operator=(MappedFile&& other) noexcept;
    MappedFile(const MappedFile&) = delete;
    MappedFile& operator=(const MappedFile&) = delete;

    // Maps the file at `utf8Path` (Linux: open, fstat, mmap PROT_READ MAP_PRIVATE; Windows:
    // CreateFileW, CreateFileMappingW, MapViewOfFile). False, with the file closed, when it
    // cannot be opened, is empty, is not a regular file or is over kMaxBytes.
    bool Open(const std::string& utf8Path);
    void Close();

    bool IsOpen() const { return data_ != nullptr; }
    const uint8_t* Data() const { return data_; }
    size_t Size() const { return size_; }

private:
    const uint8_t* data_ = nullptr;
    size_t size_ = 0;
};

} // namespace rgui::text
