# VDRSD — Development Guide (Local Only)

> Repo root is `VDRSD-capstone/` (git top-level). All docs live flat here — no subfolders.
> Stack: NBD oldstyle + nuRaft + plain-file. No follower forwarding, no NBD TLS in V1.

## 1. Prereqs

* Docker 24+, `docker compose` plugin, Linux with `nbd` module (`modprobe nbd`).
* For native build: `g++-13, cmake>=3.28, libssl-dev, libz-dev`.

## 2. Layout (flat + code)

```text
VDRSD-capstone/              <- you are here, git repo
 README.md                   <- index (this folder only)
 IMPLEMENTATION_PLAN.md      <- phased plan P0-P7
 DEVELOPMENT.md              <- this file (local dev)
 PRODUCTION.md               <- K3s deploy + ops
 CMakeLists.txt              <- (P0) FetchContent nuRaft
 src/{main,block_store,nbd_server,vdr_logstore,vdr_statemachine,raft_node,mgmt_http}.*
 tests/{block_store_test, replication.sh, kill_leader.sh, fio_bench.sh}
 Dockerfile.vdrd, Dockerfile.connector, docker-compose.yml
 k8s-apply.sh (generates k8s/ only at P6 — not yet, keep flat until then)
```

Rule: don't create `docs/` — keep `*.md` flat in this folder until P6.

## 3. Native build + unit test

```bash
cmake -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j
ctest --test-dir build -V
```

Gates: `block_store_test`, `logstore_test`, `statemachine_test` green.

## 4. Docker dev loop (P0-P5)

```bash
# single node NBD (P2)
docker build -f Dockerfile.vdrd -t vdrd:dev .
docker run --privileged -p 10809:10809 -v ./vol0:/data vdrd:dev --id 1 --data-dir /data
sudo modprobe nbd && sudo nbd-client localhost 10809 /dev/nbd0
sudo mkfs.ext4 /dev/nbd0 && sudo mount /dev/nbd0 /mnt/vd0

# 3-node cluster (P3+)
docker compose up --build
./tests/replication.sh      # write via leader, sha256 match on all 3 vol*/data.blk
./tests/kill_leader.sh      # kill leader, expect re-election <1s, I/O resumes
./tests/fio_bench.sh        # 4k randwrite + 64k seq, records p50/p99
docker compose down -v
```

Volumes: `./vol0,./vol1,./vol2` bind to `/data`. Delete to reset cluster.

## 5. Dev debugging

* Leader: `curl localhost:50052/leader`, health `curl localhost:50052/health`.
* Logs: `docker compose logs -f vdrd-0 | grep -E 'term|commit|corrupt'`.
* Corrupt inject (P4 test): flip byte in `vol1/wal.log` → expect `corrupt_blocks==1`, client EIO, no ghost ACK.
* NBD stall: `dmesg | tail` on host, `fio --rw=randwrite --bs=4k --size=256M`.

## 6. Dev don'ts

* Don't add follower forwarding — followers must RST NBD, connector reconnects.
* Don't add NBD TLS / gRPC — nuRaft asio `:50051` is the only replication transport.
* Don't tune perf before `kill_leader.sh` passes.
