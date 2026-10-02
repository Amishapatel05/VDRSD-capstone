#!/bin/sh
# K3s entry wrapper: vdrd-<N> -> --id N+1, static peers over headless DNS.
# Needs: POD_NAME (fieldRef), image built with RAFT=ON.
set -eu
ORD="${POD_NAME##*-}"
ID=$((ORD + 1))
BLOCKS="${BLOCKS:-262144}"
PEERS="1@vdrd-0.vdrd-headless:50051,2@vdrd-1.vdrd-headless:50051,3@vdrd-2.vdrd-headless:50051"
ARGS="--id $ID --data-dir /data --serve --blocks $BLOCKS --peers $PEERS"
if [ -n "${SELFTEST:-}" ]; then
  ARGS="$ARGS --selftest $SELFTEST"
fi
# shellcheck disable=SC2086
exec /app/vdrd $ARGS
