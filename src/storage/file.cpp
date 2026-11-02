#include "storage/file.h"

#include <cerrno>
#include <cstring>
#include <string>

#include "common/error.h"

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <share.h>
#include <sys/stat.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace atlas {

namespace {

[[noreturn]] void io_fail(const std::string& what, const std::filesystem::path& p) {
    throw DbError(ErrorCode::IO, what + " '" + p.string() + "': " + std::strerror(errno));
}

int open_fd(const std::filesystem::path& path, bool deny_others) {
#ifdef _WIN32
    int fd = -1;
    int share = deny_others ? _SH_DENYRW : _SH_DENYNO;
    _wsopen_s(&fd, path.wstring().c_str(), _O_RDWR | _O_CREAT | _O_BINARY, share, _S_IREAD | _S_IWRITE);
    return fd;
#else
    (void)deny_others;
    return ::open(path.c_str(), O_RDWR | O_CREAT, 0644);
#endif
}

}  // namespace

File::~File() { close(); }

File::File(File&& o) noexcept : fd_(o.fd_), path_(std::move(o.path_)) { o.fd_ = -1; }

File& File::operator=(File&& o) noexcept {
    if (this != &o) {
        close();
        fd_ = o.fd_;
        path_ = std::move(o.path_);
        o.fd_ = -1;
    }
    return *this;
}

File File::open(const std::filesystem::path& path) {
    File f;
    f.fd_ = open_fd(path, false);
    f.path_ = path;
    if (f.fd_ < 0) io_fail("cannot open", path);
    return f;
}

size_t File::read_at(uint64_t offset, void* buf, size_t n) {
    size_t done = 0;
    auto* out = static_cast<char*>(buf);
#ifdef _WIN32
    if (_lseeki64(fd_, static_cast<__int64>(offset), SEEK_SET) < 0) io_fail("seek failed on", path_);
    while (done < n) {
        int r = _read(fd_, out + done, static_cast<unsigned>(n - done));
        if (r < 0) io_fail("read failed on", path_);
        if (r == 0) break;
        done += static_cast<size_t>(r);
    }
#else
    while (done < n) {
        ssize_t r = ::pread(fd_, out + done, n - done, static_cast<off_t>(offset + done));
        if (r < 0) {
            if (errno == EINTR) continue;
            io_fail("read failed on", path_);
        }
        if (r == 0) break;
        done += static_cast<size_t>(r);
    }
#endif
    return done;
}

void File::write_at(uint64_t offset, const void* buf, size_t n) {
    size_t done = 0;
    const auto* in = static_cast<const char*>(buf);
#ifdef _WIN32
    if (_lseeki64(fd_, static_cast<__int64>(offset), SEEK_SET) < 0) io_fail("seek failed on", path_);
    while (done < n) {
        int w = _write(fd_, in + done, static_cast<unsigned>(n - done));
        if (w <= 0) io_fail("write failed on", path_);
        done += static_cast<size_t>(w);
    }
#else
    while (done < n) {
        ssize_t w = ::pwrite(fd_, in + done, n - done, static_cast<off_t>(offset + done));
        if (w < 0) {
            if (errno == EINTR) continue;
            io_fail("write failed on", path_);
        }
        done += static_cast<size_t>(w);
    }
#endif
}

void File::sync() {
#ifdef _WIN32
    if (_commit(fd_) != 0) io_fail("fsync failed on", path_);
#else
    if (::fsync(fd_) != 0) io_fail("fsync failed on", path_);
#endif
}

uint64_t File::size() {
#ifdef _WIN32
    struct _stat64 st;
    if (_fstat64(fd_, &st) != 0) io_fail("stat failed on", path_);
#else
    struct stat st;
    if (::fstat(fd_, &st) != 0) io_fail("stat failed on", path_);
#endif
    return static_cast<uint64_t>(st.st_size);
}

void File::truncate(uint64_t size) {
#ifdef _WIN32
    if (_chsize_s(fd_, static_cast<__int64>(size)) != 0) io_fail("truncate failed on", path_);
#else
    if (::ftruncate(fd_, static_cast<off_t>(size)) != 0) io_fail("truncate failed on", path_);
#endif
}

void File::close() {
    if (fd_ >= 0) {
#ifdef _WIN32
        _close(fd_);
#else
        ::close(fd_);
#endif
        fd_ = -1;
    }
}

FileLock::~FileLock() { release(); }

FileLock FileLock::acquire(const std::filesystem::path& path) {
    FileLock l;
#ifdef _WIN32
    l.fd_ = open_fd(path, true);
    if (l.fd_ < 0)
        throw DbError(ErrorCode::Locked, "database directory is in use by another process (" + path.string() + ")");
#else
    l.fd_ = open_fd(path, false);
    if (l.fd_ < 0) io_fail("cannot open", path);
    if (::flock(l.fd_, LOCK_EX | LOCK_NB) != 0) {
        ::close(l.fd_);
        l.fd_ = -1;
        throw DbError(ErrorCode::Locked, "database directory is in use by another process (" + path.string() + ")");
    }
#endif
    return l;
}

void FileLock::release() {
    if (fd_ >= 0) {
#ifdef _WIN32
        _close(fd_);
#else
        ::flock(fd_, LOCK_UN);
        ::close(fd_);
#endif
        fd_ = -1;
    }
}

void write_file_atomic(const std::filesystem::path& path, const std::string& data) {
    auto tmp = path;
    tmp += ".tmp";
    {
        File f = File::open(tmp);
        f.truncate(0);
        f.write_at(0, data.data(), data.size());
        f.sync();
    }
    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);
    if (ec) throw DbError(ErrorCode::IO, "cannot replace '" + path.string() + "': " + ec.message());
}

}  // namespace atlas
