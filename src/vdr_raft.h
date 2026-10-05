#pragma once
// P3: nuRaft node wiring. Static full-membership config (all peers in load_config),
// so 3 nodes elect with no manual add_srv step.
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "libnuraft/nuraft.hxx"

#include "block_store.h"
#include "vdr_logstore.h"
#include "vdr_raft_sm.h"

namespace vdr {

// nuRaft diagnostics go to stderr (we used to pass nullptr: blind). Level via
// VDR_LOG_LEVEL (6 trace … 1 fatal), default 4 (info).
class StderrLogger : public nuraft::logger {
 public:
  StderrLogger() {
    if (const char* e = std::getenv("VDR_LOG_LEVEL")) level_ = std::atoi(e);
  }
  void debug(const std::string& s) override { Out(5, "DBUG", s); }
  void info(const std::string& s) override { Out(4, "INFO", s); }
  void warn(const std::string& s) override { Out(3, "WARN", s); }
  void err(const std::string& s) override { Out(2, "ERR", s); }
  void set_level(int l) override { level_ = l; }
  int get_level() override { return level_; }
  void put_details(int level, const char* file, const char* func, size_t line,
                   const std::string& s) override {
    if (level > level_) return;
    std::fprintf(stderr, "[raft %d %s:%s:%zu] %s\n", level, file, func, line, s.c_str());
  }

 private:
  void Out(int level, const char* tag, const std::string& s) {
    if (level > level_) return;
    std::fprintf(stderr, "[raft %s] %s\n", tag, s.c_str());
  }
  int level_ = 4;
};

class VdrRaft {
 public:
  VdrRaft(int id, const std::vector<std::pair<int, std::string>>& peers, const std::string& dir)
      : id_(id), peers_(peers), dir_(dir) {}

  bool Start(BlockStore* store, std::string* err) {
    sm_ = nuraft::cs_new<VdrStateMachine>(store, dir_);
    ls_ = nuraft::cs_new<VdrLogStore>(dir_);
    mgr_ = nuraft::cs_new<VdrStateMgr>(id_, peers_, ls_, dir_);
    nuraft::asio_service::options asio_opt;
    nuraft::raft_params params;
    params.election_timeout_lower_bound_ = 150;
    params.election_timeout_upper_bound_ = 300;
    params.heart_beat_interval_ = 50;
    params.snapshot_distance_ = 100000;  // snapshots off (chk false); logs grow instead.
    server_ = launcher_.init(sm_, mgr_, logger_, Port(), asio_opt, params);
    if (!server_) {
      if (err) *err = "raft init returned null";
      return false;
    }
    // 30s window: compose/K3s start members with stagger, and init needs
    // peers reachable. Fast path still exits in ms via the poll below.
    // ponytail: no retry-in-process; a persistent miss exits(1) and the
    // container runtime restarts us (compose `restart:`, K8s Always).
    for (int i = 0; i < 600 && !server_->is_initialized(); ++i)
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    if (!server_->is_initialized()) {
      if (err) *err = "raft init timeout";
      return false;
    }
    return true;
  }

  bool IsLeader() const { return server_ && server_->is_leader(); }
  int Leader() const { return server_ ? server_->get_leader() : -1; }
  int Id() const { return id_; }

  nuraft::ulong CommitIndex() const { return sm_ ? sm_->last_commit_index() : 0; }

  std::string LeaderEndpoint() const {
    const int l = Leader();
    if (l < 0) return "none";
    for (const auto& p : peers_)
      if (p.first == l) return p.second;
    return "none";
  }

  // Blocking quorum append. False when not leader / not committed.
  bool Append(uint64_t blk, const char data[BlockStore::kBlock], std::string* err) {
    auto buf = nuraft::buffer::alloc(8 + 4 + BlockStore::kBlock);
    nuraft::buffer_serializer bs(buf);
    bs.put_u64(blk);
    bs.put_u32(Fnv1a(data, BlockStore::kBlock));
    bs.put_raw(data, BlockStore::kBlock);
    buf->pos(0);
    nuraft::ptr<nuraft::buffer> ret;
    try {
      ret = server_->append_entries({buf})->get();
    } catch (...) {
      if (err) *err = "append_entries threw (not leader?)";
      return false;
    }
    if (!ret || ret->size() < 1) {
      if (err) *err = "not committed (not leader?)";
      return false;
    }
    nuraft::buffer_serializer rs(*ret);
    rs.pos(0);
    const uint8_t code = rs.get_u8();
    if (code != 0) {
      if (err) *err = "commit rejected, code=" + std::to_string(code);
      return false;
    }
    return true;
  }

 private:
  int Port() const {
    for (const auto& p : peers_)
      if (p.first == id_) return std::stoi(p.second.substr(p.second.rfind(':') + 1));
    return 50051;
  }

  int id_;
  std::vector<std::pair<int, std::string>> peers_;
  std::string dir_;
  nuraft::ptr<StderrLogger> logger_ = nuraft::cs_new<StderrLogger>();
  nuraft::ptr<VdrStateMachine> sm_;
  nuraft::ptr<VdrLogStore> ls_;
  nuraft::ptr<VdrStateMgr> mgr_;
  nuraft::ptr<nuraft::raft_server> server_;
  nuraft::raft_launcher launcher_;  // must outlive server_.
};

// Startup self-test: leader appends N patterned blocks, reads them back.
// Non-leaders skip (exit via return false with skipped=true). Serves after.
inline bool SelfTest(VdrRaft& raft, BlockStore* store, int n, bool* skipped) {
  *skipped = false;
  for (int i = 0; i < 600 && !raft.IsLeader(); ++i) {  // 60s: election follows slow init.
    if (raft.Leader() > 0) {  // someone else leads; my write path is P4's connector job.
      std::cout << "[selftest] skipped, leader=" << raft.Leader() << std::endl;
      *skipped = true;
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  if (!raft.IsLeader()) {
    std::cout << "[selftest] skipped, no leader yet" << std::endl;
    *skipped = true;
    return true;
  }
  char w[BlockStore::kBlock], r[BlockStore::kBlock];
  std::string err;
  for (int i = 0; i < n; ++i) {
    std::memset(w, static_cast<char>(i & 0xFF), sizeof w);
    if (!raft.Append(static_cast<uint64_t>(i), w, &err)) {
      std::cerr << "[selftest] append " << i << " failed: " << err << std::endl;
      return false;
    }
  }
  for (int i = 0; i < n; ++i) {  // local apply may trail commit; poll briefly.
    bool ok = false;
    for (int t = 0; t < 50; ++t) {
      if (store->Read(static_cast<uint64_t>(i), r, &err) && r[0] == static_cast<char>(i & 0xFF)) {
        ok = true;
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (!ok) {
      std::cerr << "[selftest] verify " << i << " failed: " << err << std::endl;
      return false;
    }
  }
  std::cout << "[selftest] SELFTEST OK (" << n << " blocks replicated)" << std::endl;
  return true;
}

}  // namespace vdr
