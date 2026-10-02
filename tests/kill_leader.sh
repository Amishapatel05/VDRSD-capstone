#!/bin/sh
# P3 gate: kill each node in turn, restart, cluster reconverges (no leader lookup needed).
set -eu
cd "$(dirname "$0")/.."
for i in 0 1 2; do
  echo "--- cycling vdrd-$i ---"
  docker compose stop "vdrd-$i"
  sleep 4
  docker compose start "vdrd-$i"
  sleep 8
  # After each restart a leader must exist (re-election), not just equal files.
  LEADER=""
  for p in 15052 15053 15054; do
    LEADER=$(curl -sf "http://localhost:$p/leader" 2>/dev/null | tr -d ' \r\n' || true)
    [ -n "$LEADER" ] && [ "$LEADER" != "none" ] && break
    LEADER=""
  done
  [ -n "$LEADER" ] || { echo "FAIL: no leader after cycling vdrd-$i"; exit 1; }
  echo "leader after vdrd-$i cycle: $LEADER"
done
H0=$(sha256sum vol0/data.blk | cut -d' ' -f1)
H1=$(sha256sum vol1/data.blk | cut -d' ' -f1)
H2=$(sha256sum vol2/data.blk | cut -d' ' -f1)
if [ "$H0" = "$H1" ] && [ "$H1" = "$H2" ]; then
  echo "reconverge: OK ($H0)"
else
  echo "FAIL: diverged after cycling: $H0 $H1 $H2"
  exit 1
fi
