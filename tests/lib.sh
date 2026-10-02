# Shared by host-side tests: map mgmt /leader "host:port" (a RAFT endpoint,
# possibly cluster-DNS) to a host-reachable NBD "host:port".
# Compose maps NBD 10809+i -> vdrd-i and raft 50051+i; localhost runs use
# 127.0.0.1:50051+i with NBD on 10809+i.
resolve_nbd() {
  ep="$1"
  h="${ep%:*}"
  p="${ep##*:}"
  case "$h" in
    vdrd-*) n="${h##*-}" ;;
    *) n=$((p - 50051)) ;;
  esac
  case "$n" in
    '' | *[!0-9]*) echo "FAIL: cannot map leader endpoint $ep" >&2; return 1 ;;
  esac
  echo "localhost:$((10809 + n))"
}

find_leader() {
  mgmt="${MGMT:-localhost:15052,localhost:15053,localhost:15054}"
  for h in $(echo "$mgmt" | tr ',' ' '); do
    ep=$(curl -sf "http://$h/leader" 2>/dev/null | tr -d ' \r\n' || true)
    if [ -n "$ep" ] && [ "$ep" != "none" ]; then
      echo "$ep"
      return 0
    fi
  done
  echo "FAIL: no leader reported (mgmt: $mgmt)" >&2
  return 1
}
