#!/bin/sh
# P5 bench (needs fio + /dev/nbd + cluster up): 4K randwrite/randread + 64K seq
# against the LEADER's NBD. Results land in bench/*.txt for BENCH.md.
set -eu
cd "$(dirname "$0")/.."
. ./tests/lib.sh
command -v fio >/dev/null || { echo "SKIP: fio not installed"; exit 0; }
DEV="${NBD_DEV:-/dev/nbd0}"
LEADER=$(find_leader) || exit 1
NBD=$(resolve_nbd "$LEADER") || exit 1
H="${NBD%:*}"
P="${NBD##*:}"
echo "leader raft: $LEADER -> NBD $H:$P"
sudo modprobe nbd || true
sudo nbd-client -d "$DEV" 2>/dev/null || true
sudo nbd-client "$H" "$P" "$DEV"
mkdir -p bench
run() {
  name="$1"; shift
  echo "--- $name ---"
  sudo fio --name="$name" --filename="$DEV" --direct=1 --ioengine=libaio --iodepth=8 \
    --runtime=30 --time_based --group_reporting "$@" | tee "bench/$name.txt"
}
run randwrite_4k --rw=randwrite --bs=4k --size=256M
run randread_4k --rw=randread --bs=4k --size=256M
run seqwrite_64k --rw=write --bs=64k --size=512M
sudo nbd-client -d "$DEV" || true
echo "bench files in bench/"
