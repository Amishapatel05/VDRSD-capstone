# VDRSD — Development Guide (Local Only)

> Repo root is `VDRSD-capstone/` (git top-level). All docs live flat here — no subfolders.
> Stack: NBD oldstyle + nuRaft + plain-file. No follower forwarding, no NBD TLS in V1.

## 1. Prereqs

* Docker 24+, `docker compose` plugin, Linux with `nbd` module (`modprobe nbd`), `curl`.
* For native build: `g++, cmake>=3.28`; for `-DVDR_WITH_NURAFT=ON`: `libssl-dev, libz-dev`, network (FetchContent).

## 2. Layout

```text
VDRSD-capstone/
 README.md  IMPLEMENTATION_PLAN.md  DEVELOPMENT.md  PRODUCTION.md  BENCH.md
 CMakeLists.txt  Dockerfile.vdrd  Dockerfile.connector  docker-compose.yml
 src/main.cpp  src/block_store.h  src/nbd_server.h  src/mgmt_http.h
 src/vdr_logstore.h  src/vdr_raft_sm.h  src/vdr_raft.h
 src/block_store_demo.cpp  src/nbd_demo.cpp
 tests/replication.sh  tests/kill_leader.sh  tests/nbd_quorum.sh  tests/fio_bench.sh
 tools/connect.sh  tools/k8s-entry.sh
 k8s/ (P6 manifests)  bench/ (gitignored fio output)  vol0-2/ (gitignored)
```

Per-node files in `/data`: `data.blk`, `raft.wal`, `commit.idx`, `cluster.conf`, `srv_state.bin`.

## 3. Native build + unit tests

```bash
cmake -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo          # stdlib-only
cmake -B build -DVDR_WITH_NURAFT=ON                       # + replication
cmake --build build -j
ctest --test-dir build -V   # block_store_demo + nbd_demo (assert binaries, no gtest)
```

## 4. Docker dev loop

```bash
docker compose up -d --build     # 3 nodes, RAFT=ON, --selftest 20 each boot
./tests/replication.sh           # >=1 SELFTEST OK + live leader + sha equality
./tests/kill_leader.sh           # cycle each node, leader re-elected every round
./tests/nbd_quorum.sh            # 1MiB through leader NBD + quorum sha (needs /dev/nbd)
./tests/fio_bench.sh             # fio numbers -> bench/ (needs fio)
docker compose down -v
```

Volumes `./vol0,./vol1,./vol2` bind to `/data`. Delete to reset cluster. Note:
`--selftest` re-runs on every (re)start with deterministic patterns, so restarts
add more `SELFTEST OK` lines — scripts assert `>=1`, and sha must stay equal.

## 5. Debugging

* Leader: `curl localhost:15052/leader` (also `:15053`, `:15054`) → `vdrd-N:50051`.
* Metrics: `curl localhost:15052/metrics` (`vdr_is_leader`, `vdr_leader_id`, `vdr_commit_index`).
* Logs: `docker compose logs -f | grep -E 'SELFTEST|raft up|selftest skipped'`.
* Corrupt WAL: flip a byte in `vol1/raft.wal` → node fails that record on load;
  commit FNV mismatch → EIO to client, never ACKed. (No `corrupt_blocks` counter V1.)
* NBD stall: `dmesg | tail` on host.

## 6. Dev don'ts

* Don't add follower forwarding — followers RST NBD, connector reconnects.
* Don't add NBD TLS / gRPC — nuRaft asio `:50051` is the only replication transport.
* Don't tune perf before `replication.sh` + `kill_leader.sh` + `nbd_quorum.sh` are green.
