# VDRSD-capstone — Virtual Distributed Replicated Storage Device

Git repo root. All docs flat in this folder only — no `docs/` subfolder.

Stack: NBD oldstyle + nuRaft + plain-file. No follower forwarding, no NBD TLS in V1.

## Docs

* `IMPLEMENTATION_PLAN.md` — phased plan P0-P7, tests + exit gates
* `DEVELOPMENT.md` — local dev: build, docker compose, unit/integration tests
* `PRODUCTION.md` — K3s deploy, ops runbook, observability

## Quickstart (dev)

```bash
cmake -B build && cmake --build build -j && ctest --test-dir build -V
docker compose up --build
```

## Quickstart (prod)

See `PRODUCTION.md`. K3s manifests land at P6.
