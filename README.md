# VDRSD-capstone — Virtual Distributed Replicated Storage Device

Git repo root. All docs flat in this folder only — no `docs/` subfolder.

Stack: NBD oldstyle + nuRaft + plain-file. No follower forwarding, no NBD TLS in V1.

## Docs

* `IMPLEMENTATION_PLAN.md` — phased plan P0-P7, tests + exit gates
* `DEVELOPMENT.md` — local dev: build, docker compose, unit/integration tests
* `PRODUCTION.md` — K3s deploy, ops runbook, observability
* `BENCH.md` — bench method + results table

## Test matrix (run on your box — no toolchain in this snapshot env)

```bash
cmake -B build -DVDR_WITH_NURAFT=ON && cmake --build build -j
./build/block_store_demo && ./build/nbd_demo   # P1+P2 loopback checks
docker compose up -d --build
./tests/replication.sh    # P3: elect + 20 replicated writes + sha equality
./tests/kill_leader.sh    # P3: cycle every node, reconverge
./tests/nbd_quorum.sh     # P4: 1MiB through leader NBD + quorum sha (needs /dev/nbd)
./tests/fio_bench.sh      # P5: fio numbers -> bench/ (needs fio)
```

## Quickstart (dev)

```bash
cmake -B build && cmake --build build -j && ./build/block_store_demo
docker compose up --build
```

## Quickstart (prod)

```bash
kubectl apply -f k8s/ && kubectl rollout status statefulset/vdrd
curl http://vdrd-0.vdrd-headless:50052/leader
```

See `PRODUCTION.md` for chaos + runbook.
