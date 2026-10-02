# VDRSD — Bench Notes (P5)

Run: `docker compose up -d --build && ./tests/fio_bench.sh`. Raw `fio` output lands in `bench/*.txt` (gitignored). Needs `fio`, `nbd-client`, `/dev/nbd`.

## Method

* Target: leader's NBD (`10809`-mapped port, discovered via `:15052/leader`).
* Workloads: 4K randwrite / 4K randread (`iodepth=8`, 30s), 64K seqwrite.
* Every write is a quorum commit (fsync on all 3 nodes), so this measures Raft latency, not disk.

## Results (fill on your box)

| workload | IOPS | p50 (ms) | p99 (ms) | BW |
|---|---|---|---|---|
| 4K randwrite | — | — | — | — |
| 4K randread | — | — | — | — |
| 64K seqwrite | — | — | — | — |

Ballpark for 3-node local SSD V1: 4K sync write p50 5–15ms, 500–1500 IOPS.
If slower: first suspects are per-write `fsync` × 3 nodes and one-log-per-block
(no batching) — group-commit and multi-block logs are the P7+ levers, not new deps.

## Do not tune before

`replication.sh` + `nbd_quorum.sh` + `kill_leader.sh` green. Perf work on a
correctness-failing cluster is wasted motion.
