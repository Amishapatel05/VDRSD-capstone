#pragma once
// P3: nuRaft state machine applying block writes to BlockStore.
// Payload: [u64 blk][u32 fnv1a(data)][4096 data]. Snapshots off (chk false):
// logs grow instead — fine at test scale, revisit with real snapshots (P6+).
// ponytail: FNV-1a, not CRC32C; swap when corruption-rate math demands it.
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>

#include <fcntl.h>
#include <unistd.h>

#include "libnuraft/nuraft.hxx"

#include "block_store.h"

namespace vdr {

inline uint32_t Fnv1a(const char* p, size_t n) {
  uint32_t h = 2166136261u;
  for (size_t i = 0; i < n; ++i) {
    h ^= static_cast<uint8_t>(p[i]);
    h *= 16777619u;
  }
  return h;
}

inline nuraft::ptr<nuraft::buffer> StatusBuf(uint8_t code) {
  auto b = nuraft::buffer::alloc(1);
  nuraft::buffer_serializer bs(b);
  bs.put_u8(code);
  b->pos(0);
  return b;
}

class VdrStateMachine : public nuraft::state_machine {
 public:
  VdrStateMachine(BlockStore* store, const std::string& dir) : store_(store), dir_(dir) {
    const int fd = ::open(CommitPath().c_str(), O_RDONLY);
    if (fd >= 0) {
      uint64_t v = 0;
      if (::read(fd, &v, sizeof v) == static_cast<ssize_t>(sizeof v)) last_commit_ = v;
      ::close(fd);
    }
  }

  nuraft::ptr<nuraft::buffer> commit(const nuraft::ulong log_idx, nuraft::buffer& data) override {
    if (data.size() != 8 + 4 + BlockStore::kBlock) return StatusBuf(3);  // bad payload
    nuraft::buffer_serializer bs(data);
    bs.pos(0);
    const uint64_t blk = bs.get_u64();
    const uint32_t crc = bs.get_u32();
    const char* payload = static_cast<const char*>(bs.get_raw(BlockStore::kBlock));
    if (blk >= store_->num_blocks() || Fnv1a(payload, BlockStore::kBlock) != crc) return StatusBuf(1);
    char tmp[BlockStore::kBlock];
    std::memcpy(tmp, payload, sizeof tmp);  // buffer owned by caller; copy before use.
    std::string err;
    if (!store_->Write(blk, tmp, &err) || !store_->Fsync(&err)) return StatusBuf(2);
    last_commit_ = log_idx;
    PersistCommit();
    return StatusBuf(0);
  }

  void commit_config(const nuraft::ulong log_idx, nuraft::ptr<nuraft::cluster_config>&) override {
    last_commit_ = log_idx;
    PersistCommit();
  }

  bool apply_snapshot(nuraft::snapshot&) override { return false; }  // unreachable: chk false.

  nuraft::ptr<nuraft::snapshot> last_snapshot() override {
    std::lock_guard<std::mutex> l(mu_);
    return snapshot_;  // null until first snapshot (echo pattern); chk false so none.
  }

  nuraft::ulong last_commit_index() override { return last_commit_.load(); }

  void create_snapshot(nuraft::snapshot& s, nuraft::async_result<bool>::handler_type& when_done) override {
    {
      std::lock_guard<std::mutex> l(mu_);
      nuraft::ptr<nuraft::buffer> b = s.serialize();
      snapshot_ = nuraft::snapshot::deserialize(*b);
    }
    nuraft::ptr<std::exception> e(nullptr);
    bool ok = true;
    when_done(ok, e);  // handler takes (bool&, ...) — no rvalue literal.
  }

  bool chk_create_snapshot() override { return false; }

 private:
  std::string CommitPath() const { return dir_ + "/commit.idx"; }

  void PersistCommit() {  // replay is idempotent: durability of this file is best-effort.
    const int fd = ::open(CommitPath().c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return;
    const uint64_t v = last_commit_.load();
    size_t done = 0;
    while (done < sizeof v) {
      const ssize_t n = ::write(fd, reinterpret_cast<const char*>(&v) + done, sizeof v - done);
      if (n <= 0) break;
      done += static_cast<size_t>(n);
    }
    ::close(fd);
  }

  BlockStore* store_;
  std::string dir_;
  std::atomic<nuraft::ulong> last_commit_{0};
  nuraft::ptr<nuraft::snapshot> snapshot_;
  std::mutex mu_;
};

class VdrStateMgr : public nuraft::state_mgr {
 public:
  VdrStateMgr(int id, const std::vector<std::pair<int, std::string>>& peers,
              nuraft::ptr<nuraft::log_store> store, const std::string& dir)
      : id_(id), store_(store), dir_(dir) {
    config_ = nuraft::cs_new<nuraft::cluster_config>();
    for (const auto& p : peers)
      config_->get_servers().push_back(nuraft::cs_new<nuraft::srv_config>(p.first, p.second));
    if (!LoadFile(dir_ + "/cluster.conf", &raw_)) return;
    nuraft::ptr<nuraft::buffer> b = ToBuffer(raw_);
    if (b) config_ = nuraft::cluster_config::deserialize(*b);
  }

  nuraft::ptr<nuraft::cluster_config> load_config() override { return config_; }

  void save_config(const nuraft::cluster_config& config) override {
    nuraft::ptr<nuraft::buffer> b = config.serialize();
    config_ = nuraft::cluster_config::deserialize(*b);
    SaveFile(dir_ + "/cluster.conf", *b);
  }

  void save_state(const nuraft::srv_state& state) override {
    nuraft::ptr<nuraft::buffer> b = state.serialize();
    SaveFile(dir_ + "/srv_state.bin", *b);
  }

  nuraft::ptr<nuraft::srv_state> read_state() override {
    if (!LoadFile(dir_ + "/srv_state.bin", &raw_)) return nullptr;
    nuraft::ptr<nuraft::buffer> b = ToBuffer(raw_);
    if (!b) return nullptr;
    return nuraft::srv_state::deserialize(*b);
  }

  nuraft::ptr<nuraft::log_store> load_log_store() override { return store_; }
  int32_t server_id() override { return id_; }
  void system_exit(const int exit_code) override { ::_exit(exit_code); }

 private:
  static nuraft::ptr<nuraft::buffer> ToBuffer(const std::string& raw) {
    auto b = nuraft::buffer::alloc(raw.size());
    if (raw.empty()) return b;
    nuraft::buffer_serializer ws(b);
    ws.put_raw(raw.data(), raw.size());
    b->pos(0);
    return b;
  }

  static bool LoadFile(const std::string& path, std::string* out) {
    const int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) return false;
    char tmp[4096];
    out->clear();
    for (;;) {
      const ssize_t n = ::read(fd, tmp, sizeof tmp);
      if (n <= 0) break;
      out->append(tmp, static_cast<size_t>(n));
    }
    ::close(fd);
    return true;
  }

  static void SaveFile(const std::string& path, nuraft::buffer& b) {
    nuraft::buffer_serializer rs(b);
    rs.pos(0);
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return;
    size_t done = 0;
    while (done < b.size()) {
      const ssize_t n =
          ::write(fd, static_cast<const char*>(rs.data()) + done, b.size() - done);
      if (n <= 0) break;
      done += static_cast<size_t>(n);
    }
    ::fsync(fd);
    ::close(fd);
  }

  int id_;
  nuraft::ptr<nuraft::log_store> store_;
  std::string dir_;
  nuraft::ptr<nuraft::cluster_config> config_;
  std::string raw_;
};

}  // namespace vdr
