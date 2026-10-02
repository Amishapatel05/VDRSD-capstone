#!/bin/sh
# P3 gate: 3 nodes elect, leader self-tests 20 replicated writes, all data.blk identical.
set -eu
cd "$(dirname "$0")/.."
docker compose up -d --build
sleep 12
OK=$(docker compose logs --no-log-prefix 2>/dev/null | grep -c "SELFTEST OK" || true)
if [ "$OK" != "1" ]; then
  echo "FAIL: expected exactly 1 SELFTEST OK, got $OK"
  docker compose logs --tail=30
  exit 1
fi
H0=$(sha256sum vol0/data.blk | cut -d' ' -f1)
H1=$(sha256sum vol1/data.blk | cut -d' ' -f1)
H2=$(sha256sum vol2/data.blk | cut -d' ' -f1)
if [ "$H0" = "$H1" ] && [ "$H1" = "$H2" ]; then
  echo "replication: OK ($H0)"
else
  echo "FAIL: diverged: $H0 $H1 $H2"
  exit 1
fi
