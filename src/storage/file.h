#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

namespace atlas {

// Minimal random-access file with explicit durability (sync) support.
class File {
public:
    File() = default;
    ~File();
    File(const File&) = delete;
    File& operator=(const File&) = delete;
    File(File&& o) noexcept;
    File& operator=(File&& o) noexcept;

    // Opens (creating if needed) a file for reading and writing.
    static File open(const std::filesystem::path& path);

    // Reads up to n bytes at offset; returns the number of bytes read (short at EOF).
    size_t read_at(uint64_t offset, void* buf, size_t n);
    void write_at(uint64_t offset, const void* buf, size_t n);
    void sync();
    uint64_t size();
    void truncate(uint64_t size);
    void close();
    bool is_open() const { return fd_ >= 0; }
    const std::filesystem::path& path() const { return path_; }

private:
    int fd_ = -1;
    std::filesystem::path path_;
};

// Holds an exclusive OS-level lock on a file for its lifetime, so that two
// processes never open the same database directory concurrently.
class FileLock {
public:
    FileLock() = default;
    ~FileLock();
    FileLock(const FileLock&) = delete;
    FileLock& operator=(const FileLock&) = delete;
    FileLock(FileLock&& o) noexcept : fd_(o.fd_) { o.fd_ = -1; }

    static FileLock acquire(const std::filesystem::path& path);
    void release();

private:
    int fd_ = -1;
};

// Writes `data` to `path` atomically (temp file + fsync + rename).
void write_file_atomic(const std::filesystem::path& path, const std::string& data);

}  // namespace atlas
