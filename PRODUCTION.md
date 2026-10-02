# VDRSD — Production Guide (K3s Only)

> Repo root is `VDRSD-capstone/` (git top-level). All docs flat here.
> Stack: NBD oldstyle + nuRaft + plain-file. Leader-only I/O, connector-owned failover.

## 1. Artifacts

* Image: `vdrd:<tag>` from `Dockerfile.vdrd` (multi-stage, ~140MB target, `RAFT=ON` build-arg).
* Image: `vdr-connector:<tag>` from `Dockerfile.connector` (alpine + nbd-client + `connect.sh`).
* Manifests in `k8s/`: `statefulset.yaml`, `headless-svc.yaml`, `daemon-connector.yaml`, `networkpolicy.yaml` (no ConfigMap — peers are static DNS baked into `tools/k8s-entry.sh`).

## 2. K3s spec (target state)

* `StatefulSet vdrd x3`, `PVC 10Gi` each (`local-path`), `PodAntiAffinity` spread nodes.
* Headless `vdrd-headless:50051 (raft), :50052 (mgmt)` for peer DNS `vdrd-{0,1,2}.vdrd-headless`.
* `DaemonSet vdr-connector` on app nodes: `privileged:true, CAP_SYS_ADMIN`, mounts `/dev`, runs `poll GET :50052/leader → nbd-client $LEADER 10809 /dev/nbd0`.
* `NetworkPolicy`: `:50051` only from StatefulSet, `:10809` only from connector. No ingress.
* Config: peer list `1@vdrd-0.vdrd-headless:50051,...` static (ordinal → `--id` in `k8s-entry.sh`); auth token via Secret (NBD V1 — not yet enforced, see §6).

## 3. Deploy / rollback

```bash
# deploy
k3d cluster create vdrsd -a 3 || k3s install
kubectl apply -f k8s/
kubectl rollout status statefulset/vdrd
curl http://vdrd-0.vdrd-headless:50052/leader  # expect "vdrd-N:50051"
curl http://vdrd-0.vdrd-headless:50052/metrics

# chaos (C1-C4): pod kill, minority partition, majority partition, app-node reboot
kubectl delete pod vdrd-1 && kubectl get pods -w            # C1: re-elect, fsck clean
kubectl exec -it vdrd-2 -- tc qdisc add dev eth0 root netem loss 100%  # C2: minority cut
kubectl exec -it vdrd-2 -- tc qdisc del dev eth0 root       # heal: rejoins via log replay
# C3: partition 2/3 -> writes correctly block; heal, single leader resumes.
# C4: reboot app node -> connector reconnects, remount, data intact.

# rollback
kubectl rollout undo statefulset/vdrd
# data rollback = PVC snapshots (never `rm -rf /data` on quorum loss)
```

## 4. Ops runbook

| Event | Signal | Action |
|---|---|---|
| Leader pod dies | `term` bump, `is_leader` flaps, connector reconnect | Auto. Verify `fsck` clean, `kubectl get pods -w`. |
| Minority partition (1 node) | 1 follower down, majority writable | Serve on majority. Heal net → auto log-replay catch-up (snapshots off V1). |
| Majority partition | Writes hang (client timeout → EIO), `commit_idx` frozen | Correct to block. Heal, verify single leader, resume. |
| Corrupt block (FNV fail) | commit rejects, client EIO, never ACKed | Replace node volume from snapshot, rejoin. |
| Quorum loss (2/3 down) | No leader | Stop writers, restore PVCs, restart sequentially 0→1→2. |

## 5. Observability (minimal)

* Mgmt `:50052` text endpoints: `/leader` (`host:port`), `/health`, `/metrics`
  (`vdr_is_leader`, `vdr_leader_id`, `vdr_commit_index`). Scrape `/metrics`; no `:9090` exporter V1.
* Logs: stdout (`[vdrd]`, `[selftest]`); grep `SELFTEST OK`, `raft up`, `selftest skipped`.

## 6. Security / limits V1

* No NBD TLS (Pod-net trust + NetworkPolicy). Add `STARTTLS` in `nbd_server` only for WAN/multi-tenant.
* No multi-export, no iSCSI multipath, leader-only reads (read scale limit acknowledged).
* Snapshots off (`chk_create_snapshot=false`): WAL grows unbounded — fine at test scale, revisit with real snapshots beyond ~100K logs.
