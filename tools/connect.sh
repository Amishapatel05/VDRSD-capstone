#!/bin/sh
# Polls mgmt /leader and keeps nbd-client pointed at the leader (no follower forwarding, see ARCH decision).
# Usage: MGMT_HOSTS="vdrd-0:50052,vdrd-1:50052,vdrd-2:50052" NBD_PORT=10809 NBD_DEV=/dev/nbd0 ./connect.sh
set -eu
MGMT_HOSTS="${MGMT_HOSTS:-vdrd-0:50052}"
NBD_PORT="${NBD_PORT:-10809}"
NBD_DEV="${NBD_DEV:-/dev/nbd0}"
CURRENT=""

while true; do
  LEADER=""
  for h in $(echo "$MGMT_HOSTS" | tr ',' ' '); do
    # P4+ mgmt returns {"leader":"host:port"}; until then this fails and we retry.
    LEADER=$(curl -sf "http://$h/leader" || true)
    [ -n "$LEADER" ] && break
  done
  if [ -n "$LEADER" ] && [ "$LEADER" != "$CURRENT" ]; then
    echo "[connector] leader -> $LEADER (was $CURRENT)"
    nbd-client -d "$NBD_DEV" 2>/dev/null || true
    nbd-client "$LEADER" "$NBD_PORT" "$NBD_DEV" || echo "[connector] connect failed, retrying"
    CURRENT="$LEADER"
  fi
  sleep 1
done
