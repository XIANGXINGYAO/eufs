#!/usr/bin/env bash
set -euo pipefail

mkfs_binary="${1:-./build/eufs-mkfs}"
daemon_binary="${2:-./build/eufsd}"
eufsck_binary="${3:-./build/eufsck}"
work_dir="${4:-/tmp/eufs-cow-overwrite-crash-matrix}"
shift $(( $# >= 4 ? 4 : $# ))

if (( $# > 0 )); then
  stages=("$@")
else
  stages=(ordered-data journal-body control-exposure commit home-blocks checkpoint)
fi

for stage in "${stages[@]}"; do
  case "$stage" in
    ordered-data|journal-body|control-exposure|commit|home-blocks|checkpoint) ;;
    *)
      printf 'unknown durable stage: %s\n' "$stage" >&2
      exit 2
      ;;
  esac
done

mkdir -p "$work_dir"

active_mount=""

cleanup() {
  if [[ -n "$active_mount" ]]; then
    cleanup_mount "$active_mount"
  fi
  if [[ -n "${daemon_pid:-}" ]]; then
    wait "$daemon_pid" 2>/dev/null || true
  fi
}
trap cleanup EXIT

cleanup_mount() {
  local mount_dir="$1"
  fusermount3 -u "$mount_dir" 2>/dev/null || true
}

start_daemon() {
  local image="$1"
  local mount_dir="$2"
  local log_file="$3"
  shift 3
  "$daemon_binary" --image "$image" "$@" -f "$mount_dir" \
    >"$log_file" 2>&1 &
  daemon_pid=$!
  active_mount="$mount_dir"
  for _ in $(seq 1 100); do
    if mountpoint -q "$mount_dir"; then
      return 0
    fi
    if ! kill -0 "$daemon_pid" 2>/dev/null; then
      wait "$daemon_pid" || true
      cat "$log_file" >&2
      return 1
    fi
    sleep 0.1
  done
  return 1
}

stop_daemon() {
  local mount_dir="$1"
  cleanup_mount "$mount_dir"
  wait "$daemon_pid"
  unset daemon_pid
  active_mount=""
}

write_initial() {
  python3 - "$1" <<'PY'
import os
import sys

data = bytes(ord('A') + index % 17 for index in range(6000))
fd = os.open(sys.argv[1], os.O_CREAT | os.O_WRONLY, 0o644)
try:
    assert os.write(fd, data) == len(data)
finally:
    os.close(fd)
PY
}

overwrite_file() {
  python3 - "$1" <<'PY'
import os
import sys

payload = bytes(ord('k') + index % 17 for index in range(5000))
fd = os.open(sys.argv[1], os.O_WRONLY)
try:
    try:
        os.pwrite(fd, payload, 3500)
    except OSError:
        sys.exit(1)
finally:
    try:
        os.close(fd)
    except OSError:
        pass
PY
}

verify_state() {
  python3 - "$1" "$2" "$3" "$4" <<'PY'
import sys

path, expected_state, callback_offset, callback_size = sys.argv[1:]
callback_offset = int(callback_offset)
callback_size = int(callback_size)
initial = bytes(ord('A') + index % 17 for index in range(6000))
payload = bytes(ord('k') + index % 17 for index in range(5000))
if expected_state == 'old':
    expected = initial
else:
    assert callback_offset >= 3500
    source_offset = callback_offset - 3500
    assert source_offset + callback_size <= len(payload)
    new = bytearray(initial)
    callback_end = callback_offset + callback_size
    if callback_end > len(new):
        new.extend(b'\0' * (callback_end - len(new)))
    new[callback_offset:callback_end] = payload[
        source_offset:source_offset + callback_size]
    expected = bytes(new)
assert open(path, 'rb').read() == expected
PY
}

for stage in "${stages[@]}"; do
  image="$work_dir/$stage.img"
  mount_dir="$work_dir/$stage.mnt"
  setup_log="$work_dir/$stage.setup.log"
  crash_log="$work_dir/$stage.crash.log"
  recovery_log="$work_dir/$stage.recovery.log"
  second_log="$work_dir/$stage.second.log"
  mkdir -p "$mount_dir"

  "$mkfs_binary" --image "$image" --size 64M --inodes 1024 \
    --journal-blocks 256 --force >/dev/null

  start_daemon "$image" "$mount_dir" "$setup_log"
  write_initial "$mount_dir/a.txt"
  stop_daemon "$mount_dir"

  start_daemon "$image" "$mount_dir" "$crash_log" \
    "--crash-after=$stage"
  set +e
  overwrite_file "$mount_dir/a.txt"
  write_rc=$?
  set -e

  for _ in $(seq 1 100); do
    if ! kill -0 "$daemon_pid" 2>/dev/null; then
      break
    fi
    sleep 0.1
  done
  set +e
  wait "$daemon_pid"
  daemon_rc=$?
  set -e
  unset daemon_pid
  cleanup_mount "$mount_dir"
  test "$write_rc" -ne 0
  test "$daemon_rc" = "200"
  rg -q "crash failpoint reached: $stage" "$crash_log"
  transaction_line=$(rg -m1 \
    'write transaction inode=2 offset=[0-9]+ size=[0-9]+' "$crash_log")
  callback_offset=$(sed -E 's/.* offset=([0-9]+) size=([0-9]+).*/\1/' \
    <<<"$transaction_line")
  callback_size=$(sed -E 's/.* offset=([0-9]+) size=([0-9]+).*/\2/' \
    <<<"$transaction_line")

  if [[ "$stage" == "ordered-data" || "$stage" == "journal-body" ||
        "$stage" == "control-exposure" ]]; then
    expected="old"
  else
    expected="committed-callback"
  fi

  start_daemon "$image" "$mount_dir" "$recovery_log" -o ro
  verify_state "$mount_dir/a.txt" "$expected" "$callback_offset" \
    "$callback_size"
  stop_daemon "$mount_dir"
  "$eufsck_binary" "$image" >"$work_dir/$stage.eufsck.txt"
  recovered_digest=$(sha256sum "$image" | cut -d' ' -f1)

  start_daemon "$image" "$mount_dir" "$second_log" -o ro
  verify_state "$mount_dir/a.txt" "$expected" "$callback_offset" \
    "$callback_size"
  stop_daemon "$mount_dir"
  second_digest=$(sha256sum "$image" | cut -d' ' -f1)
  test "$recovered_digest" = "$second_digest"

  printf 'stage=%s callback_offset=%s callback_size=%s recovered=%s digest=%s\n' \
    "$stage" "$callback_offset" "$callback_size" "$expected" \
    "$recovered_digest"
done

printf 'PASS: mounted multi-block COW overwrite crash/recovery matrix\n'
