#!/usr/bin/env bash
set -euo pipefail

mkfs_binary="${1:-./build-brpc/eufs-mkfs}"
server_binary="${2:-./build-brpc/eufs_object_server}"
load_binary="${3:-./build-brpc/eufs_object_load_client}"
client_binary="${4:-./build-brpc/eufs_object_client}"
eufsck_binary="${5:-./build-brpc/eufsck}"
work_dir="${6:-/tmp/eufs-brpc-overload-matrix}"
listen_addr="${7:-127.0.0.1:8027}"
vars_url="http://$listen_addr/vars"

for binary in "$mkfs_binary" "$server_binary" "$load_binary" \
              "$client_binary" "$eufsck_binary"; do
  if [[ ! -x "$binary" ]]; then
    printf 'required executable is missing: %s\n' "$binary" >&2
    exit 2
  fi
done

if [[ -e "$work_dir" ]]; then
  printf 'work directory already exists: %s\n' "$work_dir" >&2
  exit 2
fi
mkdir -p "$work_dir"

server_pid=""
load_pid=""

cleanup() {
  if [[ -n "$load_pid" ]]; then
    kill "$load_pid" 2>/dev/null || true
    wait "$load_pid" 2>/dev/null || true
  fi
  if [[ -n "$server_pid" ]]; then
    kill -INT "$server_pid" 2>/dev/null || true
    wait "$server_pid" 2>/dev/null || true
  fi
}
trap cleanup EXIT

start_server() {
  local image="$1"
  local log_file="$2"
  local max_queued_writes="$3"
  local max_inflight_bytes="$4"

  "$server_binary" --image="$image" --listen_addr="$listen_addr" \
    --max_queued_write_tasks="$max_queued_writes" \
    --max_inflight_bytes="$max_inflight_bytes" \
    --max_queued_read_tasks=32 --read_workers=4 >"$log_file" 2>&1 &
  server_pid=$!
  for _ in $(seq 1 200); do
    if grep -q 'is serving on port=' "$log_file"; then
      return 0
    fi
    if ! kill -0 "$server_pid" 2>/dev/null; then
      wait "$server_pid" 2>/dev/null || true
      server_pid=""
      cat "$log_file" >&2
      return 1
    fi
    sleep 0.1
  done
  printf 'server readiness timeout: %s\n' "$log_file" >&2
  return 1
}

stop_server() {
  kill -INT "$server_pid"
  wait "$server_pid"
  server_pid=""
}

read_metric() {
  local metric="$1"
  local metrics_file="$2"
  awk -v metric="$metric" \
    '$1 == metric { print $3 + 0; found = 1 } END { if (!found) exit 1 }' \
    "$metrics_file"
}

sample_metrics() {
  curl -fsS "$vars_url" | awk '
    $1 == "eufs_object_service_inflight_payload_bytes" { bytes = $3 }
    $1 == "eufs_object_service_write_queue_depth" { queue = $3 }
    END {
      if (bytes == "" || queue == "") exit 1
      print bytes + 0, queue + 0
    }'
}

verify_manifest() {
  local manifest="$1"
  local payload_size="$2"
  local scenario_dir="$3"
  local expected="$scenario_dir/expected.bin"
  local actual="$scenario_dir/actual.bin"

  head -c "$payload_size" /dev/zero | tr '\0' 'P' >"$expected"
  while IFS=$'\t' read -r index status key request_id latency detail; do
    if [[ "$index" == "index" ]]; then
      continue
    fi
    if [[ "$status" == "PUT_STATUS_OK" ]]; then
      "$client_binary" --server="$listen_addr" --operation=get --key="$key" \
        --output_file="$actual" >"$scenario_dir/get-$index.log"
      cmp "$expected" "$actual"
    elif [[ "$status" == "PUT_STATUS_OVERLOADED" ]]; then
      set +e
      "$client_binary" --server="$listen_addr" --operation=get --key="$key" \
        >"$scenario_dir/get-$index.log" 2>&1
      local get_code=$?
      set -e
      if [[ "$get_code" != "10" ]] ||
         ! grep -q 'status=READ_STATUS_NOT_FOUND' \
           "$scenario_dir/get-$index.log"; then
        printf 'rejected key became reachable: scenario=%s key=%s\n' \
          "$scenario_dir" "$key" >&2
        return 1
      fi
    else
      printf 'unexpected load status: index=%s status=%s detail=%s\n' \
        "$index" "$status" "$detail" >&2
      return 1
    fi
  done <"$manifest"
}

run_scenario() {
  local name="$1"
  local payload_size="$2"
  local max_queued_writes="$3"
  local max_inflight_bytes="$4"
  local expected_rejection="$5"
  local request_id_seed="$6"
  local scenario_dir="$work_dir/$name"
  local image="$scenario_dir/eufs.img"
  local manifest="$scenario_dir/results.tsv"
  local snapshots="$scenario_dir/metric-snapshots.log"
  local final_metrics="$scenario_dir/metrics-final.log"
  mkdir -p "$scenario_dir"

  "$mkfs_binary" --image="$image" --size=128M --inodes=512 \
    --journal-blocks=256 --request-ledger-entries=256 --force \
    >"$scenario_dir/mkfs.log"
  start_server "$image" "$scenario_dir/server-load.log" \
    "$max_queued_writes" "$max_inflight_bytes"

  "$load_binary" --server="$listen_addr" --concurrency=16 --requests=16 \
    --payload_size="$payload_size" --key_prefix="$name-" \
    --request_id_seed="$request_id_seed" --timestamp_base="$request_id_seed" \
    --result_file="$manifest" >"$scenario_dir/load-summary.log" &
  load_pid=$!
  : >"$snapshots"
  while kill -0 "$load_pid" 2>/dev/null; do
    sample_metrics >>"$snapshots" || true
    sleep 0.02
  done
  wait "$load_pid"
  load_pid=""
  sample_metrics >>"$snapshots"

  local ok overloaded
  ok=$(awk -F '\t' '$2 == "PUT_STATUS_OK" { ++count } END { print count + 0 }' \
    "$manifest")
  overloaded=$(awk -F '\t' \
    '$2 == "PUT_STATUS_OVERLOADED" { ++count } END { print count + 0 }' \
    "$manifest")
  if (( ok == 0 || overloaded == 0 || ok + overloaded != 16 )); then
    printf 'scenario %s did not produce both success and overload: ok=%s overloaded=%s\n' \
      "$name" "$ok" "$overloaded" >&2
    return 1
  fi

  curl -fsS "$vars_url" | tr -d '\r' >"$final_metrics"
  local byte_rejections queue_rejections storage_errors final_bytes final_queue
  byte_rejections=$(read_metric \
    eufs_object_service_inflight_byte_rejection_count "$final_metrics")
  queue_rejections=$(read_metric \
    eufs_object_service_write_queue_rejection_count "$final_metrics")
  storage_errors=$(read_metric eufs_object_service_storage_error_count \
    "$final_metrics")
  final_bytes=$(read_metric eufs_object_service_inflight_payload_bytes \
    "$final_metrics")
  final_queue=$(read_metric eufs_object_service_write_queue_depth \
    "$final_metrics")

  if [[ "$storage_errors" != "0" || "$final_bytes" != "0" ||
        "$final_queue" != "0" ]]; then
    printf 'scenario %s ended dirty: storage=%s bytes=%s queue=%s\n' \
      "$name" "$storage_errors" "$final_bytes" "$final_queue" >&2
    return 1
  fi
  if [[ "$expected_rejection" == "queue" ]]; then
    test "$queue_rejections" = "$overloaded"
    test "$byte_rejections" = "0"
  else
    test "$byte_rejections" = "$overloaded"
    test "$queue_rejections" = "0"
  fi

  awk -v max_bytes="$max_inflight_bytes" -v max_queue="$max_queued_writes" '
    $1 > max_bytes { print "observed byte gauge exceeded limit" > "/dev/stderr"; exit 1 }
    $2 > max_queue { print "observed queue gauge exceeded capacity" > "/dev/stderr"; exit 1 }
  ' "$snapshots"
  local observed_peak_bytes observed_peak_queue
  observed_peak_bytes=$(awk '
    $1 > peak { peak = $1 } END { print peak + 0 }
  ' "$snapshots")
  observed_peak_queue=$(awk '
    $2 > peak { peak = $2 } END { print peak + 0 }
  ' "$snapshots")

  stop_server
  start_server "$image" "$scenario_dir/server-verify.log" \
    "$max_queued_writes" "$max_inflight_bytes"
  verify_manifest "$manifest" "$payload_size" "$scenario_dir"
  stop_server
  "$eufsck_binary" "$image" >"$scenario_dir/eufsck.log"

  printf 'scenario=%s ok=%s overloaded=%s byte_rejections=%s queue_rejections=%s observed_peak_bytes=%s observed_peak_queue=%s\n' \
    "$name" "$ok" "$overloaded" "$byte_rejections" "$queue_rejections" \
    "$observed_peak_bytes" "$observed_peak_queue" \
    | tee "$scenario_dir/summary.log"
}

# 小对象几乎不消耗字节额度，只允许写队列容量成为拒绝原因。
run_scenario queue-overload 4096 2 67108864 queue 81001
# 大对象配合 8,000,000 字节额度，只允许字节准入成为拒绝原因。
run_scenario byte-overload 4000000 32 8000000 bytes 81002

printf 'PASS: brpc overload isolation and restart consistency matrix\n'
