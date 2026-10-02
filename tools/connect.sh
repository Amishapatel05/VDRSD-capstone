#!/bin/sh
# Polls mgmt /leader (plain "host:port") and keeps nbd-client on the leader.
# No follower forwarding: followers RST, we reconnect. See ARCH decision.
# Usage: MGMT_HOSTS="localhost:15052,localhost:15053,localhost:15054" NBD_DEV=/dev/nbd0 ./connect.sh
set -eu
MGMT_HOSTS="${MGMT_HOSTS:-vdrd-0:50052}"
NBD_DEV="${NBD_DEV:-/dev/nbd0}"
CURRENT=""

while true; do
  LEADER=""
  for h in $(echo "$MGMT_HOSTS" | tr ',' ' '); do
    LEADER=$(curl -sf "http://$h/leader" 2>/dev/null | tr -d ' \r\n' || true)
    [ -n "$LEADER" ] && [ "$LEADER" != "none" ] && break
    LEADER=""
  done
  if [ -n "$LEADER" ] && [ "$LEADER" != "$CURRENT" ]; then
    echo "[connector] leader -> $LEADER (was $CURRENT)"
    nbd-client -d "$NBD_DEV" 2>/dev/null || true
    H="${LEADER%:*}"
    P="${LEADER##*:}"
    nbd-client "$H" "$P" "$NBD_DEV" || echo "[connector] connect failed, retrying"
    CURRENT="$LEADER"
  fi
  sleep 1
done
