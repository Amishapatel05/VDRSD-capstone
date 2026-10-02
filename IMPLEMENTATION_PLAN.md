# VDRSD — Phased Implementation Plan (Locked: NBD oldstyle + nuRaft + plain-file)

## Locked decisions (do not revisit in V1)

* **Frontend:** NBD oldstyle handshake, no TLS. Single export `vd0`, 4KiB blocks, default 4GiB.
* **Consensus:** nuRaft lib (asio transport `:50051`). No custom Raft. No follower forwarding.
* **Store:** plain sparse file `data.blk` + nuRaft log via custom `VdrLogStore`. CRC32C per entry.
* **Routing:** leader-only reads/writes. Followers RST NBD. `vdr-connector` sidecar polls `GET :50052/leader` and keeps `nbd-client` on leader.
* **Mgmt:** tiny asio HTTP `:50052` (`/leader`, `/health`), Prometheus `:9090`.
* **Lang/std:** C++17, CMake >= 3.28, `FetchContent` nuRaft.

---

## Phase 0 — Skeleton, toolchain, Docker (0.5 week)

**Goal:** anyone clones and gets a `vdrd --help` binary in Docker.

**Tasks:**
1. `CMakeLists.txt`: `project(vdrd)`, C++17, `-Wall -Wextra`, `FetchContent` nuRaft + asio, `find_package OpenSSL ZLIB`.
2. `src/main.cpp`: arg parse (`--id --peers --data-dir --nbd-port --raft-port --mgmt-port`), init logging (spdlog), stub `BlockStore`, `NbdServer`, `RaftNode`.
3. `Dockerfile.vdrd` multi-stage (builder `ubuntu:24.04` + runtime slim), `.dockerignore`, `docker-compose.yml` with 1x `vdrd` stub.
4. CI smoke: `cmake -B build && cmake --build build && ./vdrd --help`.

**Testing:**
* `docker build -f Dockerfile.vdrd .` passes, image <200MB target.
* `./vdrd --help` exits 0.

**Outcome / exit gate:** `P0_DONE` = clean build locally + in Docker. No logic yet.

---

## Phase 1 — Plain-file BlockStore (0.5–1 week)

**Goal:** crash-safe fixed-block store all later phases depend on.

**Tasks:**
1. `src/block_store.{h,cpp}`: `open(path,size)`, `read(blk)->buf`, `write(blk,buf)`, `fsync()`, sparse prealloc via `ftruncate`, `pwrite/pread`, `shared_mutex` for concurrent reads.
2. CRC32C helper (`crc32c.{h,cpp}` or vendored) — store-side check on read in debug mode.
3. Unit tests `tests/block_store_test.cpp` (gtest): unaligned guard, out-of-range guard, 1M-block sparse check, overwrite + reopen persistence.

**Testing:**
* `ctest`: all green, including reopen-persistence test.
* Manual: `dd`-scale write 1GiB random 4K, `du -h` confirms sparse, reopen checksum match.

**Outcome / exit gate:** `BlockStore` API frozen:
```cpp
Status Write(uint64_t blk, span<uint8_t,4096>);
Status Read(uint64_t blk, span<uint8_t,4096>);
Status Fsync();
```
No Raft/NBD dependency.

---

## Phase 2 — NBD oldstyle server, single node (1 week)

**Goal:** `mkfs.ext4 + mount + fio` works against one `vdrd` with no replication.

**Tasks:**
1. `src/nbd_server.{h,cpp}` (asio TCP): oldstyle handshake (`NBD_MAGIC`, size, flags), loop `NBD_CMD_READ/WRITE/DISC/FLUSH`, single export. Reject unaligned len.
2. Wire `NbdServer -> BlockStore` direct (no Raft yet). `WRITE` = `store.Write + Fsync` then ACK. `FLUSH` = `Fsync`.
3. `tools/connect.sh`: `modprobe nbd; nbd-client <host> 10809 /dev/nbd0`.
4. `Dockerfile.connector` (alpine + nbd-client).

**Testing:**
* T1 `nbd-client localhost 10809 /dev/nbd0 && mkfs.ext4 /dev/nbd0 && mount && echo hello > /mnt/f && umount` — pass.
* T2 `fio --rw=randwrite --bs=4k --size=256M` completes, `dmesg` clean.
* T3 kill -9 vdrd mid-write → reopen → committed writes intact (single-node fsync guarantee).

**Outcome / exit gate:** single-node virtual disk demoable. NBD handshake code frozen; later phases only change the commit callback (direct → Raft).

---

## Phase 3 — nuRaft 3-node replication core (1.5–2 weeks)

**Goal:** 3x `vdrd` elect a leader and replicate opaque 4K payloads. No NBD yet.

**Tasks:**
1. `src/vdr_logstore.{h,cpp} : nuraft::log_store`: `append()` with `fsync wal.log`, `read_at()`, `start_idx/next_slot`, `compact()`. Entry payload = `{blk:u64, crc:u32, data:4096}` raw-packed.
2. `src/vdr_statemachine.{h,cpp} : nuraft::state_machine`: `commit(idx,buf)` = crc-check → `BlockStore::Write` → `notify(commit_cv)`; `save_snapshot/load_snapshot` = copy `data.blk` + `last_idx`; `read_logical_snp_obj` passthrough.
3. `src/raft_node.{h,cpp}`: `asio_service`, `raft_params` (election 150–300ms, heartbeat 50ms, `snapshot_distance 10000`), `srv_config` from `--peers vdrd-0:50051,...`, `raft_server::init`, `is_leader()`, `append(payload)->future`.
4. Debug tool `vdr-cli append/read` bypassing NBD to inject blocks directly.

**Testing:**
* T1 single node `append 100 blocks → kill → restart → all 100 readable` (log replay).
* T2 3-node compose: `append via leader → all 3 data.blk identical (sha256 per 1K-block range)`.
* T3 `docker kill leader → new leader elected <1s (check /leader), writes resume, no double-apply (index monotonic)`.
* T4 lagging follower (stop 60s, write 500 blocks, restart) → catches up via delta or snapshot, checksum match.
* gtest: `logstore append/read/compact`, `statemachine commit ordering`, `crc-reject`.

**Outcome / exit gate:** replication proven without NBD in the loop. Raft transport left entirely to nuRaft.

---

## Phase 4 — Commit path: NBD → nuRaft → quorum ACK + Mgmt/Connector (1 week)

**Goal:** synchronous replicated disk: NBD ACK means quorum-committed.

**Tasks:**
1. Rewire `NbdServer` commit callback: `WRITE → if !is_leader: close conn (no forwarding) → else raft->append() → await commit_cv (timeout 2s) → ACK; timeout → NBD EIO`.
2. `READ → if !is_leader: close → else BlockStore::Read` (leader-only linearizable).
3. `src/mgmt_http.{h,cpp}`: `GET /leader {id,endpoint,is_leader,term}`, `GET /health`, `GET /metrics` (prom text: `term, is_leader, commit_idx, commit_latency_ms, corrupt_blocks`).
4. `vdr-connector` loop: `poll /leader every 1s → (re)connect nbd-client on change`. Handle `nbd-client -d` disconnect cleanly.

**Testing:**
* T1 `fio randwrite` via connector → kill leader mid-run → I/O stalls then resumes (<2s gap in fio log), file md5 intact.
* T2 write to follower NBD port directly → connection dropped (not ACKed), no phantom write in leader log.
* T3 `commit_latency_p50` recorded; corrupt-injected block (flip byte in wal) → rejected + `corrupt_blocks==1`, client gets EIO not ghost ACK.
* T4 timeout test: isolate leader from quorum (iptables) → writes block with EIO after 2s, no false ACK.

**Outcome / exit gate:** `RPO=0` demonstrated: every ACKed write survives leader death. This is the capstone demo core.

---

## Phase 5 — Docker-compose 3-node + FIO bench (0.5 week)

**Goal:** one-command local cluster anyone can run.

**Tasks:**
1. `docker-compose.yml`: `vdrd-0,1,2` (volumes `vol0/1/2:/data`, ports `10809-11/50051-53/50052-54`), `fio-tester` image, `connect.sh` entrypoint.
2. `tests/replication.sh`, `tests/kill_leader.sh`, `tests/fio_bench.sh` (4k randread/randwrite, 64k seq).
3. Image size + startup-time notes.

**Testing:**
* `docker compose up --build --abort-on-container-exit` runs T1–T3 green on fresh laptop.
* Bench recorded: target ballpark 4K sync write p50 5–15ms, 500–1500 IOPS (local SSD). Record actual, don't chase.

**Outcome / exit gate:** reproducible local demo + baseline numbers for report.

---

## Phase 6 — K3s manifests + chaos (1 week)

**Goal:** same cluster on real K3s with pod kills and partitions.

**Tasks:**
1. `k8s/`: `statefulset.yaml` (3 repl, anti-affinity, `local-path` PVC 10Gi), `headless-svc.yaml`, `leader-svc.yaml`, `daemon-connector.yaml` (privileged `CAP_SYS_ADMIN`, `/dev` mount), `configmap.yaml` (peers), `networkpolicy.yaml` (50051 only from StatefulSet, 10809 only from connector).
2. Deploy scripts `k8s/apply.sh`, `k8s/port-forward.sh`.
3. Chaos: `kubectl delete pod vdrd-1`, `tc qdisc ... loss/partition` minority vs majority, kubelet restart.

**Testing:**
* C1 rolling pod delete → leader re-elected, ext4 mount survives, `fsck` clean.
* C2 minority partition (1 node isolated) → majority writable, minority NBD drops; heal → auto rejoin + snapshot catch-up.
* C3 majority partition → writes correctly block (EIO/timeout), resume on heal with no split-brain (term check in logs).
* C4 `mkfs.ext4` + kernel reboot of app node → remount + data intact.

**Outcome / exit gate:** HA claim proven on K3s. Manifests frozen.

---

## Phase 7 — Hardening, observability, report (0.5–1 week)

**Goal:** shippable + gradable.

**Tasks:**
1. Snapshot tuning (`snapshot_distance` vs WAL growth), log rotation, `fsync=batch` verification.
2. Grafana dashboard (optional): `commit_latency, term changes, is_leader`.
3. Docs: `README.md` (quickstart compose + K3s), `ARCHITECTURE.md` (this stack + why no forwarding/TLS), `BENCH.md` (FIO numbers), known limits (single export, no TLS, leader-only reads).
4. Final `docker compose` + `k3s` clean-slate run video/logs for submission.

**Testing:** full regression P1→C4 from scratch on clean VM. `git tag v1.0`.

**Outcome / exit gate:** tagged release + report-ready artifacts.

---

## Global test matrix (run per phase)

| Level | Cmd | What gates |
|---|---|---|
| Unit | `ctest` | BlockStore, LogStore, StateMachine, CRC |
| Integration | `docker compose up --abort-on-container-exit` | 3-node replicate, kill-leader, catch-up |
| System | `fio_bench.sh + fsck` | ext4 + FIO + remount |
| Chaos (K3s) | `kubectl delete / tc partition` | election <1s, no split-brain, RPO=0 |
| Bench | `fio` 4k/64k | record p50/p99, IOPS (no perf gate V1) |

## Risks / non-goals V1

* Single export only; no multi-volume, no iSCSI multipath.
* No TLS on NBD; mitigated by NetworkPolicy + private Pod net. Add `STARTTLS` later in `nbd_server` only.
* Leader-only reads cap read throughput — acceptable; add lease-reads later.
* nuRaft snapshot size = full `data.blk` copy — fine at 4GiB, revisit for larger disks.
