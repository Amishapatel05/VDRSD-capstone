#pragma once
// P3: file-backed nuRaft log store. Compile only with VDR_WITH_NURAFT.
// WAL: [u32be len][log_entry::serialize bytes]* ; rewrite on write_at/compact (rare).
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "libnuraft/nuraft.hxx"

class VdrLogStore : public nuraft::log_store {
 public:
  explicit VdrLogStore(const std::string& dir) : dir_(dir), start_idx_(1) { Load(); }
  ~VdrLogStore() override = default;

  nuraft::ulong next_slot() const override {
    std::lock_guard<std::mutex> l(mu_);
    return start_idx_ + logs_.size();
  }

  nuraft::ulong start_index() const override {
    std::lock_guard<std::mutex> l(mu_);
    return start_idx_;
  }

  nuraft::ptr<nuraft::log_entry> last_entry() const override {
    std::lock_guard<std::mutex> l(mu_);
    if (logs_.empty()) return nuraft::cs_new<nuraft::log_entry>(0, nuraft::buffer::alloc(1));
    return logs_.back();
  }

  nuraft::ulong append(nuraft::ptr<nuraft::log_entry>& entry) override {
    std::lock_guard<std::mutex> l(mu_);
    logs_.push_back(entry);
    PersistRecord(entry);  // fsync before return: RPO=0 needs durable log.
    return start_idx_ + logs_.size() - 1;
  }

  void write_at(nuraft::ulong index, nuraft::ptr<nuraft::log_entry>& entry) override {
    std::lock_guard<std::mutex> l(mu_);
    if (index < start_idx_) return;
    const size_t pos = static_cast<size_t>(index - start_idx_);
    if (pos < logs_.size()) logs_.erase(logs_.begin() + pos, logs_.end());
    logs_.push_back(entry);
    Rewrite();
  }

  nuraft::ptr<std::vector<nuraft::ptr<nuraft::log_entry>>> log_entries(nuraft::ulong start,
                                                                      nuraft::ulong end) override {
    std::lock_guard<std::mutex> l(mu_);
    if (start < start_idx_) return nullptr;
    if (end > start_idx_ + logs_.size()) end = start_idx_ + logs_.size();
    auto out = nuraft::cs_new<std::vector<nuraft::ptr<nuraft::log_entry>>>();
    for (nuraft::ulong i = start; i < end; ++i) out->push_back(logs_[i - start_idx_]);
    return out;
  }

  nuraft::ptr<nuraft::log_entry> entry_at(nuraft::ulong index) override {
    std::lock_guard<std::mutex> l(mu_);
    if (index < start_idx_ || index >= start_idx_ + logs_.size()) return nullptr;
    return logs_[index - start_idx_];
  }

  nuraft::ulong term_at(nuraft::ulong index) override {
    std::lock_guard<std::mutex> l(mu_);
    if (index < start_idx_) return 0;
    if (index >= start_idx_ + logs_.size()) return logs_.empty() ? 0 : logs_.back()->get_term();
    return logs_[index - start_idx_]->get_term();
  }

  nuraft::ptr<nuraft::buffer> pack(nuraft::ulong index, int32_t cnt) override {
    std::lock_guard<std::mutex> l(mu_);
    size_t total = 4;
    for (int32_t i = 0; i < cnt && index + i < start_idx_ + logs_.size(); ++i)
      total += 4 + logs_[index + i - start_idx_]->serialize()->size();
    auto buf = nuraft::buffer::alloc(total);
    nuraft::buffer_serializer bs(buf);
    bs.put_u32(static_cast<uint32_t>(cnt));
    for (int32_t i = 0; i < cnt && index + i < start_idx_ + logs_.size(); ++i) {
      auto s = logs_[index + i - start_idx_]->serialize();
      bs.put_u32(static_cast<uint32_t>(s->size()));
      nuraft::buffer_serializer rs(*s);
      rs.pos(0);
      bs.put_raw(rs.data(), s->size());
    }
    buf->pos(0);
    return buf;
  }

  void apply_pack(nuraft::ulong index, nuraft::buffer& pack) override {
    nuraft::buffer_serializer bs(pack);
    bs.pos(0);
    const uint32_t cnt = bs.get_u32();
    std::lock_guard<std::mutex> l(mu_);
    if (index < start_idx_) return;
    const size_t pos = static_cast<size_t>(index - start_idx_);
    if (pos < logs_.size()) logs_.erase(logs_.begin() + pos, logs_.end());
    for (uint32_t i = 0; i < cnt; ++i) {
      const uint32_t len = bs.get_u32();
      auto tmp = nuraft::buffer::alloc(len);
      nuraft::buffer_serializer ws(tmp);
      ws.put_raw(bs.get_raw(len), len);
      tmp->pos(0);
      logs_.push_back(nuraft::log_entry::deserialize(*tmp));
    }
    Rewrite();
  }

  bool compact(nuraft::ulong last_log_index) override {
    std::lock_guard<std::mutex> l(mu_);
    if (last_log_index < start_idx_) return true;
    const size_t drop = static_cast<size_t>(std::min<nuraft::ulong>(last_log_index - start_idx_ + 1, logs_.size()));
    logs_.erase(logs_.begin(), logs_.begin() + drop);
    start_idx_ = last_log_index + 1;
    Rewrite();
    return true;
  }

  bool flush() override {
    std::lock_guard<std::mutex> l(mu_);
    return Fsync();
  }

 private:
  std::string WalPath() const { return dir_ + "/raft.wal"; }

  static void PutU32Be(std::string& out, uint32_t v) {
    out.push_back(static_cast<char>(v >> 24));
    out.push_back(static_cast<char>(v >> 16));
    out.push_back(static_cast<char>(v >> 8));
    out.push_back(static_cast<char>(v));
  }

  // mu_ held.
  bool Fsync() {
    const int fd = ::open(WalPath().c_str(), O_RDWR | O_CREAT, 0644);
    if (fd < 0) return false;
    const bool ok = ::fsync(fd) == 0;
    ::close(fd);
    return ok;
  }

  // mu_ held. O(1) append + fsync.
  void PersistRecord(nuraft::ptr<nuraft::log_entry>& entry) {
    auto s = entry->serialize();
    std::string rec;
    PutU32Be(rec, static_cast<uint32_t>(s->size()));
    nuraft::buffer_serializer rs(*s);
    rs.pos(0);
    rec.append(static_cast<const char*>(rs.data()), s->size());
    const int fd = ::open(WalPath().c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0) return;
    size_t done = 0;
    while (done < rec.size()) {
      const ssize_t n = ::write(fd, rec.data() + done, rec.size() - done);
      if (n <= 0) break;
      done += static_cast<size_t>(n);
    }
    ::fsync(fd);
    ::close(fd);
  }

  // mu_ held. Full rewrite (write_at/compact/load only — never on hot append).
  void Rewrite() {
    const std::string tmp = WalPath() + ".tmp";
    const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return;
    for (auto& e : logs_) {
      auto s = e->serialize();
      std::string rec;
      PutU32Be(rec, static_cast<uint32_t>(s->size()));
      nuraft::buffer_serializer rs(*s);
      rs.pos(0);
      rec.append(static_cast<const char*>(rs.data()), s->size());
      size_t done = 0;
      while (done < rec.size()) {
        const ssize_t n = ::write(fd, rec.data() + done, rec.size() - done);
        if (n <= 0) break;
        done += static_cast<size_t>(n);
      }
    }
    ::fsync(fd);
    ::close(fd);
    ::rename(tmp.c_str(), WalPath().c_str());
  }

  void Load() {
    const int fd = ::open(WalPath().c_str(), O_RDONLY);
    if (fd < 0) return;
    struct stat st {};
    if (::fstat(fd, &st) != 0) {
      ::close(fd);
      return;
    }
    std::string data(static_cast<size_t>(st.st_size), '\0');
    size_t done = 0;
    while (done < data.size()) {
      const ssize_t n = ::read(fd, &data[done], data.size() - done);
      if (n <= 0) break;
      done += static_cast<size_t>(n);
    }
    ::close(fd);
    size_t pos = 0;  // corrupt tail (crash mid-append) is dropped, not fatal.
    while (pos + 4 <= data.size()) {
      const uint32_t len = (static_cast<uint8_t>(data[pos]) << 24) |
                           (static_cast<uint8_t>(data[pos + 1]) << 16) |
                           (static_cast<uint8_t>(data[pos + 2]) << 8) | static_cast<uint8_t>(data[pos + 3]);
      if (len == 0 || len > (1 << 24) || pos + 4 + len > data.size()) break;
      auto tmp = nuraft::buffer::alloc(len);
      nuraft::buffer_serializer ws(tmp);
      ws.put_raw(data.data() + pos + 4, len);
      tmp->pos(0);
      logs_.push_back(nuraft::log_entry::deserialize(*tmp));
      pos += 4 + len;
    }
  }

  std::string dir_;
  nuraft::ulong start_idx_;
  std::vector<nuraft::ptr<nuraft::log_entry>> logs_;
  mutable std::mutex mu_;
};
