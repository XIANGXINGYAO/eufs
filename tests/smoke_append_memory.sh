#!/usr/bin/env bash
set -euo pipefail

binary="${1:-./build/eufs}"
mount_dir="${2:-/tmp/eufs-append-mnt}"
log_file="${3:-/tmp/eufs-append.log}"

mkdir -p "$mount_dir"

cleanup() {
  fusermount3 -u "$mount_dir" 2>/dev/null || true
  if [[ -n "${eufs_pid:-}" ]]; then
    wait "$eufs_pid" 2>/dev/null || true
  fi
}
trap cleanup EXIT

umask 022
"$binary" -f "$mount_dir" >"$log_file" 2>&1 &
eufs_pid=$!

for _ in $(seq 1 50); do
  if mountpoint -q "$mount_dir"; then
    break
  fi
  sleep 0.1
done

mountpoint -q "$mount_dir"
printf 'A' >"$mount_dir/a.txt"
exec 3>>"$mount_dir/a.txt"
exec 4>>"$mount_dir/a.txt"
printf 'B' >&3
printf 'C' >&4
exec 3>&-
exec 4>&-
test "$(cat "$mount_dir/a.txt")" = "ABC"

: >"$mount_dir/records.txt"
pids=()
for writer in A B C D; do
  (
    exec 3>>"$mount_dir/records.txt"
    for sequence in $(seq 1 50); do
      printf '%s-%03d\n' "$writer" "$sequence" >&3
    done
    exec 3>&-
  ) &
  pids+=("$!")
done
for pid in "${pids[@]}"; do
  wait "$pid"
done

test "$(wc -l <"$mount_dir/records.txt")" = "200"
actual_digest="$(LC_ALL=C sort "$mount_dir/records.txt" | sha256sum | cut -d' ' -f1)"
expected_digest="$({
  for writer in A B C D; do
    for sequence in $(seq 1 50); do
      printf '%s-%03d\n' "$writer" "$sequence"
    done
  done
} | LC_ALL=C sort | sha256sum | cut -d' ' -f1)"
test "$actual_digest" = "$expected_digest"

printf 'PASS: in-memory append smoke test\n'
