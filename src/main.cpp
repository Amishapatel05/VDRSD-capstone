// P2: --serve runs BlockStore + NbdServer single-node (no Raft until P3).
#include <cstdint>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

#include "block_store.h"
#include "nbd_server.h"
#ifdef VDR_WITH_NURAFT
#include <thread>
#include "mgmt_http.h"
#include "vdr_raft.h"
#endif

namespace {

struct Config {
  int id = 1;
  std::string peers = "1@127.0.0.1:50051";  // id@host:port,... (static membership, no add_srv)
  std::string data_dir = "/data";
  int nbd_port = 10809;
  int raft_port = 50051;
  int mgmt_port = 50052;
  uint64_t blocks = 1048576;  // 4 GiB sparse; costs nothing until written.
  int selftest = 0;           // N>0: leader appends N blocks at startup, keeps serving.
  bool serve = false;
};

bool ParsePeers(const std::string& s, std::vector<std::pair<int, std::string>>* out) {
  size_t pos = 0;
  while (pos < s.size()) {
    const size_t comma = s.find(',', pos);
    const std::string tok = s.substr(pos, comma == std::string::npos ? comma : comma - pos);
    const size_t at = tok.find('@');
    if (at == std::string::npos) return false;
    out->push_back({std::stoi(tok.substr(0, at)), tok.substr(at + 1)});
    if (comma == std::string::npos) break;
    pos = comma + 1;
  }
  return !out->empty();
}

void print_help(const char* prog) {
  std::cout << "Usage: " << prog << " [options]\n"
            << "\nVirtual Distributed Replicated Storage Device (vdrd) — P0 stub\n\n"
            << "Options:\n"
            << "  --id N          node id (default 1)\n"
            << "  --peers LIST    comma-separated raft peers host:port (default 127.0.0.1:50051)\n"
            << "  --data-dir DIR  data directory (default /data)\n"
            << "  --nbd-port P    NBD port (default 10809)\n"
            << "  --raft-port P   nuRaft transport port (default 50051, P3+)\n"
            << "  --mgmt-port P   mgmt HTTP port (default 50052, P4+)\n"
            << "  --blocks N      export size in 4K blocks (default 1048576 = 4GiB sparse)\n"
            << "  --selftest N    leader appends N test blocks at startup, keeps serving (needs raft)\n"
            << "  --serve         run NBD server (P2 single-node; quorum-gated from P4)\n"
            << "  -h, --help      show this help\n";
}

bool parse_args(int argc, char** argv, Config& cfg) {
  std::vector<std::string> args(argv + 1, argv + argc);
  for (size_t i = 0; i < args.size(); ++i) {
    const auto& a = args[i];
    auto need_val = [&](const char* flag, std::string& out) -> bool {
      if (a != flag) return false;
      if (i + 1 >= args.size()) {
        std::cerr << flag << " requires a value\n";
        return true;  // consumed, but error -> caller checks empty
      }
      out = args[++i];
      return true;
    };
    std::string v;
    if (a == "-h" || a == "--help") {
      print_help(argv[0]);
      return false;
    } else if (a == "--id" || a == "--nbd-port" || a == "--raft-port" || a == "--mgmt-port") {
      if (i + 1 >= args.size()) {
        std::cerr << a << " requires a value\n";
        return false;
      }
      int n = std::stoi(args[++i]);
      if (a == "--id") cfg.id = n;
      if (a == "--nbd-port") cfg.nbd_port = n;
      if (a == "--raft-port") cfg.raft_port = n;
      if (a == "--mgmt-port") cfg.mgmt_port = n;
    } else if (a == "--serve") {
      cfg.serve = true;
    } else if (a == "--selftest") {
      if (i + 1 >= args.size()) {
        std::cerr << a << " requires a value\n";
        return false;
      }
      cfg.selftest = std::stoi(args[++i]);
#ifndef VDR_WITH_NURAFT
      std::cerr << "--selftest needs -DVDR_WITH_NURAFT=ON build\n";
      return false;
#endif
    } else if (a == "--blocks") {
      if (i + 1 >= args.size()) {
        std::cerr << a << " requires a value\n";
        return false;
      }
      cfg.blocks = static_cast<uint64_t>(std::stoull(args[++i]));
    } else if (need_val("--peers", v)) {
      if (v.empty()) return false;
      cfg.peers = v;
    } else if (need_val("--data-dir", v)) {
      if (v.empty()) return false;
      cfg.data_dir = v;
    } else {
      std::cerr << "unknown arg: " << a << "\n";
      print_help(argv[0]);
      return false;
    }
  }
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  // ponytail: hand-rolled parse, no CLI11 until P4 needs subcommands.
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "-h" || a == "--help") {
      print_help(argv[0]);
      return 0;
    }
  }
  Config cfg;
  if (!parse_args(argc, argv, cfg)) return 1;

  if (!cfg.serve) {
    std::cout << "[vdrd] id=" << cfg.id << " peers=" << cfg.peers << " data-dir=" << cfg.data_dir
              << " nbd=" << cfg.nbd_port << " raft=" << cfg.raft_port << " mgmt=" << cfg.mgmt_port
              << "\n[vdrd] pass --serve to export NBD"
#ifndef VDR_WITH_NURAFT
              << " (stdlib-only build: no replication; rebuild with -DVDR_WITH_NURAFT=ON)"
#endif
              << ".\n";
    return 0;
  }
  BlockStore store;
  std::string err;
  if (!store.Open(cfg.data_dir + "/data.blk", cfg.blocks, &err)) {
    std::cerr << "open store: " << err << "\n";
    return 1;
  }
#ifdef VDR_WITH_NURAFT
  std::vector<std::pair<int, std::string>> peers;
  if (!ParsePeers(cfg.peers, &peers)) {
    std::cerr << "bad --peers (want id@host:port,...): " << cfg.peers << "\n";
    return 1;
  }
  vdr::VdrRaft raft(cfg.id, peers, cfg.data_dir);
  if (!raft.Start(&store, &err)) {
    std::cerr << "raft: " << err << "\n";
    return 1;
  }
  std::cout << "[vdrd] raft up id=" << cfg.id << " leader=" << raft.Leader() << "\n";
  if (cfg.selftest > 0) {
    bool skipped = false;
    if (!vdr::SelfTest(raft, &store, cfg.selftest, &skipped)) return 1;
  }
  NbdServer srv(&store, cfg.nbd_port);
  srv.SetQuorum([&] { return raft.IsLeader(); },
                [&](uint64_t b, const char* d) {
                  std::string e;
                  return raft.Append(b, d, &e);
                });
  MgmtServer mgmt(cfg.mgmt_port, [&] { return raft.LeaderEndpoint(); },
                  [&] { return "ok leader=" + std::to_string(raft.Leader()); },
                  [&] {
                    return "vdr_is_leader " + std::to_string(raft.IsLeader() ? 1 : 0) +
                           "\nvdr_leader_id " + std::to_string(raft.Leader()) + "\nvdr_commit_index " +
                           std::to_string(raft.CommitIndex()) + "\n";
                  });
  std::thread([&] {
    std::string e;
    if (!mgmt.Run(&e)) std::cerr << "mgmt: " << e << "\n";
  }).detach();
#else
  NbdServer srv(&store, cfg.nbd_port);
#endif
  std::cout << "[vdrd] serving " << cfg.blocks << " blocks on :" << cfg.nbd_port << "\n";
  if (!srv.Run(&err)) {
    std::cerr << "nbd: " << err << "\n";
    return 1;
  }
  return 0;
}
