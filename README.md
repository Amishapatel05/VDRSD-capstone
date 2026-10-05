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
./vdrctl up              # build + start 3-node cluster
./vdrctl status          # containers, leader, metrics, replica hashes
./vdrctl test all        # unit + replication + kill + NBD quorum
./vdrctl failover        # kill leader, verify re-election + convergence
./vdrctl bench           # fio numbers -> bench/ (needs fio)
./vdrctl down            # stop (volumes kept); ./vdrctl menu for interactive
```

Raw equivalents (no wrapper): `cmake -B build -DVDR_WITH_NURAFT=ON && ...`
plus `tests/*.sh` — see `DEVELOPMENT.md`.

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
