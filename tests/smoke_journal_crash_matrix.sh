#!/usr/bin/env bash
set -euo pipefail

mkfs_binary="${1:-./build/eufs-mkfs}"
daemon_binary="${2:-./build/eufsd}"
work_dir="${3:-/tmp/eufs-journal-crash-matrix}"

if (( $# >= 4 )); then
  stages=("${@:4}")
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
}

verify_state() {
  local mount_dir="$1"
  local expected="$2"
  test -f "$mount_dir/a.txt"
  if [[ "$expected" == "old" ]]; then
    test "$(stat -c %s "$mount_dir/a.txt")" = "0"
  else
    test "$(cat "$mount_dir/a.txt")" = "hello"
    test "$(stat -c %s "$mount_dir/a.txt")" = "5"
  fi
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
  : >"$mount_dir/a.txt"
  stop_daemon "$mount_dir"

  start_daemon "$image" "$mount_dir" "$crash_log" \
    "--crash-after=$stage"
  set +e
  printf 'hello' | dd of="$mount_dir/a.txt" conv=notrunc status=none
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

  if [[ "$stage" == "ordered-data" || "$stage" == "journal-body" ||
        "$stage" == "control-exposure" ]]; then
    expected="old"
  else
    expected="new"
  fi

  start_daemon "$image" "$mount_dir" "$recovery_log" -o ro
  verify_state "$mount_dir" "$expected"
  stop_daemon "$mount_dir"
  recovered_digest=$(sha256sum "$image" | cut -d' ' -f1)

  start_daemon "$image" "$mount_dir" "$second_log" -o ro
  verify_state "$mount_dir" "$expected"
  stop_daemon "$mount_dir"
  second_digest=$(sha256sum "$image" | cut -d' ' -f1)
  test "$recovered_digest" = "$second_digest"

  printf 'stage=%s recovered=%s digest=%s\n' \
    "$stage" "$expected" "$recovered_digest"
done

printf 'PASS: mounted journal crash/recovery matrix\n'
