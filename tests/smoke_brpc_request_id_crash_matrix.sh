#!/usr/bin/env bash
set -euo pipefail

mkfs_binary="${1:-./build/eufs-mkfs}"
server_binary="${2:-./build/eufs_object_server}"
client_binary="${3:-./build/eufs_object_client}"
work_dir="${4:-/tmp/eufs-brpc-request-id-crash-matrix}"
listen_addr="${5:-127.0.0.1:8027}"

if (( $# >= 6 )); then
  stages=("${@:6}")
else
  stages=(ordered-data journal-body control-exposure commit home-blocks checkpoint)
fi

for binary in "$mkfs_binary" "$server_binary" "$client_binary"; do
  if [[ ! -x "$binary" ]]; then
    printf 'required executable is missing: %s\n' "$binary" >&2
    exit 2
  fi
done

for stage in "${stages[@]}"; do
  case "$stage" in
    ordered-data|journal-body|control-exposure|commit|home-blocks|checkpoint) ;;
    *)
      printf 'unknown durable stage: %s\n' "$stage" >&2
      exit 2
      ;;
  esac
done

if [[ -e "$work_dir" ]]; then
  printf 'work directory already exists: %s\n' "$work_dir" >&2
  exit 2
fi
mkdir -p "$work_dir"

server_pid=""

cleanup() {
  if [[ -n "$server_pid" ]]; then
    kill -INT "$server_pid" 2>/dev/null || true
    wait "$server_pid" 2>/dev/null || true
  fi
}
trap cleanup EXIT

start_server() {
  local image="$1"
  local log_file="$2"
  shift 2

  "$server_binary" --image="$image" --listen_addr="$listen_addr" "$@" \
    >"$log_file" 2>&1 &
  server_pid=$!
  for _ in $(seq 1 200); do
    if grep -q 'is serving on port=' "$log_file"; then
      return 0
    fi
    if ! kill -0 "$server_pid" 2>/dev/null; then
      wait "$server_pid" 2>/dev/null || true
      cat "$log_file" >&2
      return 1
    fi
    sleep 0.1
  done
  printf 'server readiness timeout: %s\n' "$log_file" >&2
  return 1
}

wait_for_injected_crash() {
  local stage="$1"
  local log_file="$2"

  for _ in $(seq 1 200); do
    if ! kill -0 "$server_pid" 2>/dev/null; then
      break
    fi
    sleep 0.1
  done
  if kill -0 "$server_pid" 2>/dev/null; then
    printf 'server crash timeout at stage %s\n' "$stage" >&2
    return 1
  fi
  set +e
  wait "$server_pid"
  local exit_code=$?
  set -e
  server_pid=""
  if [[ "$exit_code" != "200" ]]; then
    printf 'server did not exit through the crash observer: rc=%s\n' \
      "$exit_code" >&2
    cat "$log_file" >&2
    return 1
  fi
  grep -q "crash failpoint reached: $stage" "$log_file"
}

stop_server() {
  kill -INT "$server_pid"
  set +e
  wait "$server_pid"
  local exit_code=$?
  set -e
  server_pid=""
  if [[ "$exit_code" != "0" ]]; then
    printf 'server did not stop cleanly: rc=%s\n' "$exit_code" >&2
    return 1
  fi
}

request_id=0123456789abcdeffedcba9876543210
timestamp_ns=4000000000

for stage in "${stages[@]}"; do
  stage_dir="$work_dir/$stage"
  image="$stage_dir/eufs.img"
  crash_log="$stage_dir/server-crash.log"
  first_rpc_log="$stage_dir/first-rpc.log"
  recovery_log="$stage_dir/server-recovery.log"
  before_retry_log="$stage_dir/get-before-retry.log"
  retry_log="$stage_dir/retry.log"
  final_get_log="$stage_dir/get-after-retry.log"
  expected_file="$stage_dir/expected.bin"
  actual_file="$stage_dir/actual.bin"
  payload="payload-$stage"
  mkdir -p "$stage_dir"

  "$mkfs_binary" --image="$image" --size=16M --inodes=256 \
    --journal-blocks=16 --request-ledger-entries=64 --force \
    >"$stage_dir/mkfs.log"

  start_server "$image" "$crash_log" "--crash_after=$stage"
  set +e
  "$client_binary" --server="$listen_addr" --operation=put --key=object \
    --payload="$payload" --timestamp_ns="$timestamp_ns" \
    --request_id="$request_id" --timeout_ms=5000 \
    >"$first_rpc_log" 2>&1
  first_rpc_code=$?
  set -e
  if [[ "$first_rpc_code" != "5" ]]; then
    printf 'stage %s: first RPC did not lose its response: rc=%s\n' \
      "$stage" "$first_rpc_code" >&2
    cat "$first_rpc_log" >&2
    exit 1
  fi
  grep -q 'rpc_error=' "$first_rpc_log"
  wait_for_injected_crash "$stage" "$crash_log"

  start_server "$image" "$recovery_log"
  set +e
  "$client_binary" --server="$listen_addr" --operation=get --key=object \
    >"$before_retry_log" 2>&1
  before_retry_code=$?
  set -e

  if [[ "$stage" == "ordered-data" || "$stage" == "journal-body" ||
        "$stage" == "control-exposure" ]]; then
    expected_recovery=old
    test "$before_retry_code" = "10"
    grep -q 'status=READ_STATUS_NOT_FOUND' "$before_retry_log"
  else
    expected_recovery=committed
    test "$before_retry_code" = "0"
    grep -q 'status=READ_STATUS_OK' "$before_retry_log"
  fi

  before_retry_hash=$(sha256sum "$image" | awk '{print $1}')
  "$client_binary" --server="$listen_addr" --operation=put --key=object \
    --payload="$payload" --timestamp_ns="$timestamp_ns" \
    --request_id="$request_id" --timeout_ms=5000 \
    >"$retry_log" 2>&1
  grep -q 'status=PUT_STATUS_OK' "$retry_log"
  after_retry_hash=$(sha256sum "$image" | awk '{print $1}')

  if [[ "$expected_recovery" == "old" ]]; then
    test "$before_retry_hash" != "$after_retry_hash"
    retry_effect=executed
  else
    test "$before_retry_hash" = "$after_retry_hash"
    retry_effect=replayed
  fi

  "$client_binary" --server="$listen_addr" --operation=get --key=object \
    --output_file="$actual_file" >"$final_get_log" 2>&1
  grep -q 'status=READ_STATUS_OK' "$final_get_log"
  printf '%s' "$payload" >"$expected_file"
  cmp "$expected_file" "$actual_file"
  stop_server

  printf 'stage=%s recovery=%s retry=%s digest=%s\n' \
    "$stage" "$expected_recovery" "$retry_effect" "$after_retry_hash"
done

printf 'PASS: brpc Request-ID crash/retry matrix\n'
