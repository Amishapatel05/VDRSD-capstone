// P2: --serve runs BlockStore + NbdServer single-node (no Raft until P3).
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include "block_store.h"
#include "nbd_server.h"

namespace {

struct Config {
  int id = 1;
  std::string peers = "127.0.0.1:50051";
  std::string data_dir = "/data";
  int nbd_port = 10809;
  int raft_port = 50051;
  int mgmt_port = 50052;
  uint64_t blocks = 1048576;  // 4 GiB sparse; costs nothing until written.
  bool serve = false;
};

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
              << "\n[vdrd] pass --serve to export NBD (RaftNode lands in P3).\n";
    return 0;
  }
  BlockStore store;
  std::string err;
  if (!store.Open(cfg.data_dir + "/data.blk", cfg.blocks, &err)) {
    std::cerr << "open store: " << err << "\n";
    return 1;
  }
  std::cout << "[vdrd] serving " << cfg.blocks << " blocks on :" << cfg.nbd_port << "\n";
  NbdServer srv(&store, cfg.nbd_port);
  if (!srv.Run(&err)) {
    std::cerr << "nbd: " << err << "\n";
    return 1;
  }
  return 0;
}
