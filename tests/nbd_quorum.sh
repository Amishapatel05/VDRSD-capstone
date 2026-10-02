#!/bin/sh
# P4 gate (needs /dev/nbd0 + cluster up): write pattern through LEADER's NBD,
# read back, confirm quorum replication via sha equality across volumes.
# Failover half is manual: docker compose kill <leader-node>, rerun this.
set -eu
cd "$(dirname "$0")/.."
MGMT="${MGMT:-localhost:15052,localhost:15053,localhost:15054}"
DEV="${NBD_DEV:-/dev/nbd0}"
LEADER=""
for h in $(echo "$MGMT" | tr ',' ' '); do
  LEADER=$(curl -sf "http://$h/leader" 2>/dev/null | tr -d ' \r\n' || true)
  [ -n "$LEADER" ] && [ "$LEADER" != "none" ] && break
  LEADER=""
done
[ -n "$LEADER" ] || { echo "FAIL: no leader reported"; exit 1; }
H="${LEADER%:*}"
P="${LEADER##*:}"
echo "leader NBD: $H:$P (mgmt: $MGMT)"
sudo modprobe nbd || true
sudo nbd-client -d "$DEV" 2>/dev/null || true
sudo nbd-client "$H" "$P" "$DEV"
head -c 1048576 /dev/urandom > /tmp/vdr_pat.bin
sudo dd if=/tmp/vdr_pat.bin of="$DEV" bs=4096 count=256 conv=fsync status=none
sudo dd if="$DEV" of=/tmp/vdr_got.bin bs=4096 count=256 status=none
if cmp -s /tmp/vdr_pat.bin /tmp/vdr_got.bin; then
  echo "nbd quorum write/read: OK"
else
  echo "FAIL: readback mismatch"
  exit 1
fi
sudo nbd-client -d "$DEV" || true
sleep 3  # let followers apply
H0=$(sha256sum vol0/data.blk | cut -d' ' -f1)
H1=$(sha256sum vol1/data.blk | cut -d' ' -f1)
H2=$(sha256sum vol2/data.blk | cut -d' ' -f1)
if [ "$H0" = "$H1" ] && [ "$H1" = "$H2" ]; then
  echo "quorum replicas identical: OK ($H0)"
else
  echo "FAIL: diverged: $H0 $H1 $H2"
  exit 1
fi
