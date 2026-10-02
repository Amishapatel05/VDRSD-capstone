#pragma once
// P1: fixed 4K-block sparse file store. Header-only: fewest files, no new deps.
// ponytail: global shared_mutex; per-block locks only if measured contention.
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unistd.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>

class BlockStore {
 public:
  static constexpr std::size_t kBlock = 4096;

  BlockStore() = default;
  ~BlockStore() { Close(); }
  BlockStore(const BlockStore&) = delete;
  BlockStore& operator=(const BlockStore&) = delete;

  bool Open(const std::string& path, uint64_t num_blocks, std::string* err) {
    int fd = ::open(path.c_str(), O_RDWR | O_CREAT, 0644);
    if (fd < 0) return Fail("open", err);
    struct stat st {};
    if (::fstat(fd, &st) != 0) {
      ::close(fd);
      return Fail("fstat", err);
    }
    const auto want = static_cast<off_t>(num_blocks * kBlock);
    if (st.st_size != want && ::ftruncate(fd, want) != 0) {
      ::close(fd);
      return Fail("ftruncate", err);
    }
    fd_ = fd;
    path_ = path;
    num_blocks_ = num_blocks;
    return true;
  }

  bool Read(uint64_t blk, char out[kBlock], std::string* err) const {
    if (blk >= num_blocks_) {
      if (err) *err = "read: block out of range";
      return false;
    }
    std::shared_lock lock(mu_);
    if (FullIO(::pread, blk, out, false, err)) return true;
    return false;
  }

  bool Write(uint64_t blk, const char data[kBlock], std::string* err) {
    if (blk >= num_blocks_) {
      if (err) *err = "write: block out of range";
      return false;
    }
    std::unique_lock lock(mu_);
    return FullIO(::pwrite, blk, const_cast<char*>(data), true, err);
  }

  bool Fsync(std::string* err) {
    std::shared_lock lock(mu_);
    if (::fsync(fd_) != 0) return Fail("fsync", err);
    return true;
  }

  void Close() {
    if (fd_ >= 0) {
      ::close(fd_);
      fd_ = -1;
    }
  }

  uint64_t num_blocks() const { return num_blocks_; }

 private:
  template <typename F>
  bool FullIO(F io, uint64_t blk, char* buf, bool is_write, std::string* err) const {
    const off_t off = static_cast<off_t>(blk * kBlock);
    std::size_t done = 0;
    while (done < kBlock) {
      ssize_t n = io(fd_, buf + done, kBlock - done, off + done);
      if (n <= 0) return Fail(is_write ? "pwrite" : "pread", err);
      done += static_cast<std::size_t>(n);
    }
    return true;
  }

  static bool Fail(const std::string& what, std::string* err) {
    if (err) *err = what + ": " + std::string(::strerror(errno));
    return false;
  }

  int fd_ = -1;
  std::string path_;
  uint64_t num_blocks_ = 0;
  mutable std::shared_mutex mu_;
};
