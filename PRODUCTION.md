# VDRSD — Production Guide (K3s Only)

> Repo root is `VDRSD-capstone/` (git top-level). All docs flat here.
> Stack: NBD oldstyle + nuRaft + plain-file. Leader-only I/O, connector-owned failover.

## 1. Artifacts

* Image: `vdrd:<tag>` from `Dockerfile.vdrd` (multi-stage, ~140MB target).
* Image: `vdr-connector:<tag>` from `Dockerfile.connector` (alpine + nbd-client + `connect.sh`).
* Manifests: applied from this folder at P6 (`statefulset.yaml, headless-svc.yaml, leader-svc.yaml, daemon-connector.yaml, configmap.yaml, networkpolicy.yaml`). Until P6 ships, this doc is the spec — don't pre-create `k8s/`.

## 2. K3s spec (target state)

* `StatefulSet vdrd x3`, `PVC 10Gi` each (`local-path`), `PodAntiAffinity` spread nodes.
* Headless `vdrd-headless:50051 (raft), :50052 (mgmt)` for peer DNS `vdrd-{0,1,2}.vdrd-headless`.
* `DaemonSet vdr-connector` on app nodes: `privileged:true, CAP_SYS_ADMIN`, mounts `/dev`, runs `poll GET :50052/leader → nbd-client $LEADER 10809 /dev/nbd0`.
* `NetworkPolicy`: `:50051` only from StatefulSet, `:10809` only from connector. No ingress.
* Config: peer list `vdrd-0.vdrd-headless:50051,...` via ConfigMap; auth token via Secret (NBD V1).

## 3. Deploy / rollback

```bash
# deploy (P6+)
k3d cluster create vdrsd -a 3 || k3s install
kubectl apply -f statefulset.yaml -f headless-svc.yaml -f leader-svc.yaml -f daemon-connector.yaml -f configmap.yaml -f networkpolicy.yaml
kubectl rollout status statefulset/vdrd
curl http://vdrd-headless:50052/leader   # expect {"leader":<0-2>,"term":N}

# rollback
kubectl rollout undo statefulset/vdrd
# data rollback = PVC snapshots (never `rm -rf /data` on quorum loss)
```

## 4. Ops runbook

| Event | Signal | Action |
|---|---|---|
| Leader pod dies | `term` bump, `is_leader` flaps, connector reconnect | Auto. Verify `fsck` clean, `kubectl get pods -w`. |
| Minority partition (1 node) | 1 follower `health:lagging` | Serve on majority. Heal net → auto snapshot catch-up. |
| Majority partition | Writes EIO/timeout, `commit_idx` frozen | Correct to block. Heal, verify single leader (`term` converges), resume. |
| Corrupt block | `corrupt_blocks>0`, client EIO | Don't ACK. Replace node volume from snapshot, rejoin. |
| Quorum loss (2/3 down) | No leader | Stop writers, restore PVCs, restart sequentially 0→1→2. |

## 5. Observability (minimal)

* Prometheus `:9090`: `term, is_leader, commit_idx, commit_latency_ms_p50/p99, wal_bytes, corrupt_blocks`.
* Alert: `term changes >5/min`, `commit_latency_p99 >500ms`, `corrupt_blocks>0`.
* Logs: `spdlog` → stdout → Loki; grep `leader_change, snapshot_install, crc_fail`.

## 6. Security / limits V1

* No NBD TLS (Pod-net trust + NetworkPolicy). Add `STARTTLS` in `nbd_server` only for WAN/multi-tenant.
* No multi-export, no iSCSI multipath, leader-only reads (read scale limit acknowledged).
* Snapshots = full `data.blk` copy — fine at 4GiB, revisit beyond.
